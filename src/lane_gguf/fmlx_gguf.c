/* fmlx_gguf.c — lettore GGUF di riferimento (lane_gguf).
 *
 * Vede il contratto in include/fmlx/fmlx_gguf.h: nessun cap, `general.alignment` onorato,
 * ogni rifiuto con il SUO codice, nessun byte-count silenzioso.
 *
 * Formato (GGUF v2/v3/v4):
 *   magic u32 "GGUF" | version u32 | n_tensors | n_kv      (u32 in v2, u64 in v3/v4)
 *   n_kv volte: key string, type u32, value
 *   n_tensors volte: name string, n_dims u32, dims u64[n_dims], type u32, offset u64
 *   padding a `general.alignment` | data section (gli `offset` sono relativi ad essa)
 *   string = u64 len + byte | array = elem_type u32, n u64, elementi
 */
#include "fmlx/fmlx_gguf.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* ------------------------------------------------------------------ tabella tipi */

static const FmlxGgmlType TYPES[FMLX_GGML_TYPE_MAX + 1] = {
    /* name              block_vals  block_bytes  standard  usable */
    { "F32",       1,   4, 1, 1 },
    { "F16",       1,   2, 1, 1 },
    { "Q4_0",     32,  18, 1, 1 },
    { "Q4_1",     32,  20, 1, 1 },
    { "Q4_2",     32,  18, 1, 0 }, /* ritirati upstream: rifiutati, non coperti a caso */
    { "Q4_3",     32,  20, 1, 0 },
    { "Q5_0",     32,  22, 1, 1 },
    { "Q5_1",     32,  24, 1, 1 },
    { "Q8_0",     32,  34, 1, 1 },
    { "Q8_1",     32,  36, 1, 1 },
    { "Q2_K",    256,  84, 1, 1 },
    { "Q3_K",    256, 110, 1, 1 },
    { "Q4_K",    256, 144, 1, 1 },
    { "Q5_K",    256, 176, 1, 1 },
    { "Q6_K",    256, 210, 1, 1 },
    { "Q8_K",    256, 292, 1, 1 },
    { "IQ2_XXS", 256,  66, 1, 1 },
    { "IQ2_XS",  256,  74, 1, 1 },
    { "IQ3_XXS", 256,  98, 1, 1 },
    { "IQ1_S",   256,  50, 1, 1 },
    { "IQ4_NL",   32,  18, 1, 1 },
    { "IQ3_S",   256, 110, 1, 1 },
    { "IQ2_S",   256,  82, 1, 1 },
    { "IQ4_XS",  256, 136, 1, 1 },
    { "I8",        1,   1, 1, 1 },
    { "I16",       1,   2, 1, 1 },
    { "I32",       1,   4, 1, 1 },
    { "I64",       1,   8, 1, 1 },
    { "F64",       1,   8, 1, 1 },
    { "IQ1_M",   256,  56, 1, 1 },
    { "BF16",      1,   2, 1, 1 },
    /* I BUCHI SONO PARTE DEL FORMATO, non uno spazio da comprimere: ggml.h:421-431 lascia
     * 31/32/33 (Q4_0_4_4, Q4_0_4_8, Q4_0_8_8 ritirati) e 36/37/38 (IQ4_NL_*_*) senza nomi, e
     * TQ1_0 sta a 34, TQ2_0 a 35, MXFP4 a 39, NVFP4 a 40, Q1_0 a 41 (confermato anche da
     * ds4/gguf-tools/quants.h:48-52, che usa inizializzatori designati `[TIPO] = {...}` — la
     * forma che una copia posizionale distrugge). Compattare questi righe spostava CINQUE tipi
     * e faceva leggere i byte di un MXFP4 come NVFP4: nessun file locale lo esercita (gli
     * istogrammi dei 7 GGUF di casa non contengono TQ/MX/NV/Q1_0), quindi il difetto era
     * invisibile finché una sorgente davvero indipendente — `_Static_assert` contro ggml.h —
     * non ha fermato la compilazione. */
    { "reserved-31", 0, 0, 0, 0 },
    { "reserved-32", 0, 0, 0, 0 },
    { "reserved-33", 0, 0, 0, 0 },
    { "TQ1_0",   256,  54, 0, 1 },
    { "TQ2_0",   256,  66, 0, 1 },
    { "reserved-36", 0, 0, 0, 0 },
    { "reserved-37", 0, 0, 0, 0 },
    { "reserved-38", 0, 0, 0, 0 },
    { "MXFP4",    32,  17, 0, 1 },
    { "NVFP4",    64,  36, 0, 1 },
    { "Q1_0",    128,  18, 0, 1 },
};

const FmlxGgmlType *fmlx_ggml_type(int t) {
    if (t < 0 || t > FMLX_GGML_TYPE_MAX) return NULL;
    if (TYPES[t].block_vals == 0) return NULL; /* reserved / mai definito */
    return &TYPES[t];
}

FmlxGgufStatus fmlx_ggml_type_bytes(int t, uint64_t numel, uint64_t *out) {
    const FmlxGgmlType *ty;
    if (out) *out = 0;
    if (t == 4 || t == 5) return FMLX_GGUF_ERR_TYPE_REMOVED;
    ty = fmlx_ggml_type(t);
    if (!ty) return FMLX_GGUF_ERR_TYPE_UNKNOWN;
    if (numel % ty->block_vals) return FMLX_GGUF_ERR_NUMEL_BLOCK;
    if (out) *out = (numel / ty->block_vals) * ty->block_bytes;
    return FMLX_GGUF_OK;
}

const char *fmlx_gguf_status_str(FmlxGgufStatus s) {
    switch (s) {
    case FMLX_GGUF_OK: return "ok";
    case FMLX_GGUF_ERR_NOT_GGUF: return "magic non GGUF";
    case FMLX_GGUF_ERR_VERSION: return "versione GGUF non supportata";
    case FMLX_GGUF_ERR_TRUNCATED: return "file troncato nell'header";
    case FMLX_GGUF_ERR_IMPLAUSIBLE: return "header implausibile";
    case FMLX_GGUF_ERR_KV_TYPE: return "tipo di metadata KV sconosciuto";
    case FMLX_GGUF_ERR_NDIMS: return "n_dims fuori range";
    case FMLX_GGUF_ERR_BAD_ALIGN: return "general.alignment non valido";
    case FMLX_GGUF_ERR_NEG_DIM: return "dimensione di tensore = 0";
    case FMLX_GGUF_ERR_TYPE_UNKNOWN: return "ggml_type sconosciuto";
    case FMLX_GGUF_ERR_TYPE_REMOVED: return "ggml_type ritirato upstream";
    case FMLX_GGUF_ERR_NUMEL_BLOCK: return "numel non multiplo del blocco di quantizzazione";
    case FMLX_GGUF_ERR_OVERLAP: return "intervalli di dati sovrapposti";
    case FMLX_GGUF_ERR_DATA_SHORT: return "dati dichiarati oltre la fine del file";
    case FMLX_GGUF_ERR_NO_MEM: return "memoria esaurita";
    case FMLX_GGUF_ERR_IO: return "errore di I/O";
    case FMLX_GGUF_ERR_NAME_DUP: return "nome di tensore duplicato";
    case FMLX_GGUF_ERR_COUNT: return "conteggi incoerenti con la dimensione del file";
    case FMLX_GGUF_ERR_DST_SMALL: return "buffer di destinazione troppo piccolo";
    case FMLX_GGUF_ERR_NOT_COVERED:
        return "tipo valido ma non coperto da questa lane (gap dichiarato)";
    default: return "codice di errore sconosciuto";
    }
}

/* ------------------------------------------------------------------ arena di proprietà */

typedef struct {
    void **items;
    size_t n, cap;
    int mmapped;
    size_t map_len;
} Ctx;

static int own(Ctx *c, void *p) {
    if (!p) return 0;
    if (c->n == c->cap) {
        size_t nc = c->cap ? c->cap * 2 : 64;
        void **ni = (void **)realloc(c->items, nc * sizeof(void *));
        if (!ni) return 0;
        c->items = ni;
        c->cap = nc;
    }
    c->items[c->n++] = p;
    return 1;
}

/* Copia e registra; in caso di fallimento libera e restituisce NULL. */
static char *dupn(Ctx *c, const char *s, size_t n) {
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    if (!own(c, p)) { free(p); return NULL; }
    return p;
}

/* Alloca azzzerato e registra. */
static void *zalloc_owned(Ctx *c, size_t n, size_t sz) {
    void *p = calloc(n, sz);
    if (!p) return NULL;
    if (!own(c, p)) { free(p); return NULL; }
    return p;
}

static void ctx_free(Ctx *c) {
    size_t i;
    if (!c) return;
    for (i = 0; i < c->n; i++) free(c->items[i]);
    free(c->items);
    c->items = NULL;
    c->n = c->cap = 0;
}

/* Registra il motivo di un fallimento IO quando l'arena ancora non esiste (il file non si
 * è proprio aperto). Senza questo il chiamante legge `last_error == NULL`: il codice è
 * distinto, ma il "perché" no — e iolettori di casa fermano lì l'onestà. */
static void set_io_error(FmlxGgufFile *out, const char *what, const char *path, int err) {
    Ctx *c = (Ctx *)out->arena;
    char buf[512];
    int k;
    if (!c) {
        c = (Ctx *)calloc(1, sizeof *c);
        if (!c) return; /* nessun messaggio: resta solo il codice, che c'è già */
        out->arena = c;
    }
    if (err) k = snprintf(buf, sizeof buf, "%s di \"%s\": %s", what, path, strerror(err));
    else k = snprintf(buf, sizeof buf, "%s di \"%s\"", what, path);
    if (k < 0) return;
    if (k >= (int)sizeof buf) k = (int)sizeof buf - 1;
    out->last_error = dupn(c, buf, (size_t)k);
}

/* ------------------------------------------------------------------ lettore con bound-check */

typedef struct {
    const uint8_t *p;
    size_t len, at;
    Ctx *c;
    char *err;
    FmlxGgufStatus code;
    int failed;
} R;

static void fail(R *r, FmlxGgufStatus s, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    if (r->failed) return; /* il PRIMO errore è quello utile: non viene sovrascritto */
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    r->failed = 1;
    r->code = s;
    r->err = dupn(r->c, buf, strlen(buf));
}

static int need(R *r, size_t n) {
    if (r->failed) return 0;
    if (r->len - r->at < n) {
        fail(r, FMLX_GGUF_ERR_TRUNCATED,
             "servono %zu byte all'offset %zu, il buffer ne ha %zu", n, r->at, r->len);
        return 0;
    }
    return 1;
}

static uint64_t rd_u(R *r, int bytes) {
    uint64_t v = 0;
    int i;
    if (!need(r, (size_t)bytes)) return 0;
    for (i = 0; i < bytes; i++) v |= (uint64_t)r->p[r->at + (size_t)i] << (8 * i);
    r->at += (size_t)bytes;
    return v;
}

#define rd_u32(r) ((uint32_t)rd_u((r), 4))
#define rd_u64(r) rd_u((r), 8)

static char *rd_str(R *r) {
    uint64_t n;
    char *s;
    if (r->failed) return NULL;
    n = rd_u64(r);
    if (r->failed) return NULL;
    if (n > (1u << 28)) {
        fail(r, FMLX_GGUF_ERR_IMPLAUSIBLE, "lunghezza stringa assurda: %llu byte all'offset %zu",
             (unsigned long long)n, r->at - 8);
        return NULL;
    }
    if (!need(r, (size_t)n)) return NULL;
    s = dupn(r->c, (const char *)(r->p + r->at), (size_t)n);
    if (!s) { fail(r, FMLX_GGUF_ERR_NO_MEM, "copia stringa di %llu byte", (unsigned long long)n); return NULL; }
    r->at += (size_t)n;
    return s;
}

/* ------------------------------------------------------------------ valori KV */

#define KV_U8 0
#define KV_I8 1
#define KV_U16 2
#define KV_I16 3
#define KV_U32 4
#define KV_I32 5
#define KV_F32 6
#define KV_BOOL 7
#define KV_STRING 8
#define KV_ARRAY 9
#define KV_U64 10
#define KV_I64 11
#define KV_F64 12

static int kv_scalar_size(uint32_t t) {
    switch (t) {
    case KV_U8: case KV_I8: case KV_BOOL: return 1;
    case KV_U16: case KV_I16: return 2;
    case KV_U32: case KV_I32: case KV_F32: return 4;
    case KV_U64: case KV_I64: case KV_F64: return 8;
    default: return -1;
    }
}

static double bits_to_double(uint64_t raw, int bytes) {
    if (bytes == 4) {
        union { uint32_t u; float f; } a;
        a.u = (uint32_t)raw;
        return (double)a.f;
    } else {
        union { uint64_t u; double f; } a;
        a.u = raw;
        return a.f;
    }
}

/* Legge uno scalare (riempiendo *u e/o *f) oppure una stringa. */
static void rd_scalar(R *r, uint32_t type, uint64_t *u, double *f, char **s) {
    int ssz;
    if (u) *u = 0;
    if (f) *f = 0;
    if (s) *s = NULL;
    if (type == KV_STRING) {
        char *v = rd_str(r);
        if (s) *s = v;
        return;
    }
    ssz = kv_scalar_size(type);
    if (ssz < 0) {
        fail(r, FMLX_GGUF_ERR_KV_TYPE, "tipo KV %u sconosciuto", type);
        return;
    }
    {
        uint64_t raw = rd_u(r, ssz);
        if (r->failed) return;
        if (u) *u = raw;
        if (f) *f = (type == KV_F32) ? bits_to_double(raw, 4)
                                     : (type == KV_F64) ? bits_to_double(raw, 8)
                                                        : (double)(int64_t)raw;
    }
}

static void rd_value(R *r, uint32_t type, FmlxGgufKV *kv) {
    kv->type = type;
    if (type != KV_ARRAY) { rd_scalar(r, type, &kv->u, &kv->f, &kv->s); return; }

    kv->elem_type = rd_u32(r);
    kv->arr_len = rd_u64(r);
    if (r->failed) return;
    if (kv->elem_type == KV_ARRAY) {
        fail(r, FMLX_GGUF_ERR_KV_TYPE, "array di array non supportato (tipo elemento %u)",
             kv->elem_type);
        return;
    }
    if (kv->arr_len == 0) return;

    if (kv->elem_type == KV_STRING) {
        uint64_t i;
        if (kv->arr_len > (r->len - r->at) / 8) {
            fail(r, FMLX_GGUF_ERR_IMPLAUSIBLE,
                 "array di %llu stringhe in %zu byte rimanenti: header corrotto",
                 (unsigned long long)kv->arr_len, r->len - r->at);
            return;
        }
        kv->arr_s = (char **)zalloc_owned(r->c, (size_t)kv->arr_len, sizeof(char *));
        if (!kv->arr_s) { fail(r, FMLX_GGUF_ERR_NO_MEM, "array di stringhe"); return; }
        for (i = 0; i < kv->arr_len && !r->failed; i++) kv->arr_s[i] = rd_str(r);
        return;
    }
    {
        int ssz = kv_scalar_size(kv->elem_type);
        uint64_t i;
        int is_float = (kv->elem_type == KV_F32 || kv->elem_type == KV_F64);
        if (ssz < 0) {
            fail(r, FMLX_GGUF_ERR_KV_TYPE, "tipo elemento array %u sconosciuto", kv->elem_type);
            return;
        }
        if (kv->arr_len > (r->len - r->at) / (size_t)ssz) {
            fail(r, FMLX_GGUF_ERR_IMPLAUSIBLE,
                 "array di %llu elementi x%d byte eccede i %zu rimanenti: header corrotto",
                 (unsigned long long)kv->arr_len, ssz, r->len - r->at);
            return;
        }
        if (is_float) {
            kv->arr_f = (double *)zalloc_owned(r->c, (size_t)kv->arr_len, sizeof(double));
            if (!kv->arr_f) { fail(r, FMLX_GGUF_ERR_NO_MEM, "array di float"); return; }
            for (i = 0; i < kv->arr_len && !r->failed; i++)
                rd_scalar(r, kv->elem_type, NULL, &kv->arr_f[i], NULL);
        } else {
            kv->arr_u = (uint64_t *)zalloc_owned(r->c, (size_t)kv->arr_len, sizeof(uint64_t));
            if (!kv->arr_u) { fail(r, FMLX_GGUF_ERR_NO_MEM, "array di interi"); return; }
            for (i = 0; i < kv->arr_len && !r->failed; i++) kv->arr_u[i] = rd_u(r, ssz);
        }
    }
}

/* ------------------------------------------------------------------ parse */

static uint64_t align_up(uint64_t x, uint64_t a) {
    if (a == 0 || (a & (a - 1))) return x; /* validato altrove: qui non si pezza */
    return (x + a - 1) & ~(a - 1);
}

/* Pulizia in caso di fallimento: le stringhe/array restano nella Ctx (che porta anche il
 * messaggio), quindi NON si libera la Ctx qui. Chi riceve un errore chiama close(). */
static void abandon(FmlxGgufFile *out) {
    out->tensors = NULL;
    out->kvs = NULL;
    out->n_tensors = 0;
    out->n_kv = 0;
}

/* Intervalli di byte dei tensori, per i controlli di disposizione (vedi parse). */
typedef struct {
    const char *name;
    uint64_t start, end;
} Seg;

static int seg_cmp_offset(const void *a, const void *b) {
    const Seg *x = (const Seg *)a, *y = (const Seg *)b;
    if (x->start != y->start) return x->start < y->start ? -1 : 1;
    if (x->end != y->end) return x->end < y->end ? -1 : 1;
    return strcmp(x->name ? x->name : "", y->name ? y->name : "");
}

static int seg_cmp_name(const void *a, const void *b) {
    const Seg *x = (const Seg *)a, *y = (const Seg *)b;
    int c = strcmp(x->name ? x->name : "", y->name ? y->name : "");
    if (c) return c;
    if (x->start != y->start) return x->start < y->start ? -1 : 1;
    return 0;
}

FmlxGgufStatus fmlx_gguf_parse(const uint8_t *data, size_t len, FmlxGgufFile *out) {
    Ctx *c;
    R r;
    uint64_t i;
    int wide;

    if (!data || !out) return FMLX_GGUF_ERR_IO;
    memset(out, 0, sizeof *out);
    c = (Ctx *)calloc(1, sizeof(Ctx));
    if (!c) return FMLX_GGUF_ERR_NO_MEM;
    out->arena = c;
    out->data = data;
    out->len = len;
    out->alignment = FMLX_GGUF_ALIGN_DEFAULT;

    memset(&r, 0, sizeof r);
    r.p = data;
    r.len = len;
    r.c = c;

    if (!need(&r, 20)) goto failed; /* magic+version+2 conteggi minimi (v2) */
    {
        uint32_t magic = (uint32_t)rd_u(&r, 4);
        uint64_t ver;
        if (magic != FMLX_GGUF_MAGIC) {
            fail(&r, FMLX_GGUF_ERR_NOT_GGUF, "magic = 0x%08x, atteso 0x%08x (\"GGUF\")", magic,
                 FMLX_GGUF_MAGIC);
            goto failed;
        }
        ver = rd_u(&r, 4);
        if (ver != 2 && ver != 3 && ver != 4) {
            fail(&r, FMLX_GGUF_ERR_VERSION, "versione %llu: supportate 2, 3 e 4",
                 (unsigned long long)ver);
            goto failed;
        }
        out->version = (uint32_t)ver;
        wide = (ver >= 3);
        r.at = 8;
        out->n_tensors = rd_u(&r, wide ? 8 : 4);
        out->n_kv = rd_u(&r, wide ? 8 : 4);
        /* Non è un cap: è il minimo perché il conteggio possa essere vero. Ogni
         * tensor-info costa >= 32 byte (nome vuoto + n_dims + 1 dima + tipo + offset),
         * ogni KV >= 13 (chiave vuota + tipo + 1 byte di valore). */
        {
            size_t remaining = len - r.at;
            if (out->n_tensors > remaining / 32 + 1) {
                fail(&r, FMLX_GGUF_ERR_COUNT,
                     "n_tensors = %llu non sostenibile in %zu byte di header",
                     (unsigned long long)out->n_tensors, remaining);
                goto failed;
            }
            if (out->n_kv > remaining / 13 + 1) {
                fail(&r, FMLX_GGUF_ERR_COUNT,
                     "n_kv = %llu non sostenibile in %zu byte di header",
                     (unsigned long long)out->n_kv, remaining);
                goto failed;
            }
        }
    }

    /* --- KV (vanno letti prima: `general.alignment` decide dove inizia la data section) --- */
    if (out->n_kv) {
        out->kvs = (FmlxGgufKV *)zalloc_owned(c, (size_t)out->n_kv, sizeof(FmlxGgufKV));
        if (!out->kvs) { fail(&r, FMLX_GGUF_ERR_NO_MEM, "array dei KV"); goto failed; }
        for (i = 0; i < out->n_kv; i++) {
            FmlxGgufKV *kv = &out->kvs[i];
            kv->key = rd_str(&r);
            if (r.failed) goto failed;
            rd_value(&r, rd_u32(&r), kv);
            if (r.failed) goto failed;
            if (strcmp(kv->key, "general.alignment") == 0) {
                if (kv->type == KV_ARRAY || kv->type == KV_STRING) {
                    fail(&r, FMLX_GGUF_ERR_BAD_ALIGN,
                         "general.alignment dichiarato come tipo %u, atteso scalare", kv->type);
                    goto failed;
                }
                if (kv->u == 0 || (kv->u & (kv->u - 1))) {
                    fail(&r, FMLX_GGUF_ERR_BAD_ALIGN,
                         "general.alignment = %llu: deve essere una potenza di 2 > 0",
                         (unsigned long long)kv->u);
                    goto failed;
                }
                out->alignment = kv->u;
            }
        }
    }

    /* --- tensor info --- */
    if (out->n_tensors) {
        out->tensors = (FmlxGgufTensor *)zalloc_owned(c, (size_t)out->n_tensors,
                                                     sizeof(FmlxGgufTensor));
        if (!out->tensors) { fail(&r, FMLX_GGUF_ERR_NO_MEM, "array dei tensori"); goto failed; }
        for (i = 0; i < out->n_tensors; i++) {
            FmlxGgufTensor *t = &out->tensors[i];
            uint64_t d;
            t->name = rd_str(&r);
            if (r.failed) goto failed;
            t->n_dims = rd_u32(&r);
            if (r.failed) goto failed;
            if (t->n_dims == 0 || t->n_dims > FMLX_GGUF_MAX_DIMS) {
                fail(&r, FMLX_GGUF_ERR_NDIMS, "tensore \"%s\": n_dims = %u, ammessi 1..%u",
                     t->name ? t->name : "?", t->n_dims, FMLX_GGUF_MAX_DIMS);
                goto failed;
            }
            t->numel = 1;
            for (d = 0; d < t->n_dims; d++) {
                t->dims[d] = rd_u64(&r);
                if (r.failed) goto failed;
                if (t->dims[d] == 0) {
                    fail(&r, FMLX_GGUF_ERR_NEG_DIM, "tensore \"%s\": dim[%llu] = 0",
                         t->name ? t->name : "?", (unsigned long long)d);
                    goto failed;
                }
                t->numel *= t->dims[d];
            }
            t->ggml_type = (int32_t)rd_u32(&r);
            t->offset = rd_u64(&r);
            if (r.failed) goto failed;
            {
                FmlxGgufStatus ts = fmlx_ggml_type_bytes(t->ggml_type, t->numel, &t->nbytes);
                if (ts != FMLX_GGUF_OK) {
                    const FmlxGgmlType *ty = fmlx_ggml_type(t->ggml_type);
                    fail(&r, ts, "tensore \"%s\": tipo %d (%s), numel %llu -> %s",
                         t->name ? t->name : "?", t->ggml_type,
                         ty ? ty->name : "fuori tabella", (unsigned long long)t->numel,
                         fmlx_gguf_status_str(ts));
                    goto failed;
                }
            }
            /* offset + nbytes può traboccare con un header malato: 10^19 + 10^19 ridiventerebbe
             * piccolo e il controllo di sovrapposizione lo leggerebbe come "tensore in fondo". */
            if (t->offset > (uint64_t)-1 - t->nbytes) {
                fail(&r, FMLX_GGUF_ERR_IMPLAUSIBLE,
                     "tensore \"%s\": offset %llu + %llu byte traboccano un uint64",
                     t->name ? t->name : "?", (unsigned long long)t->offset,
                     (unsigned long long)t->nbytes);
                goto failed;
            }
        }
    }

    /* --- coerenza della disposizione, poi presenza dei dati -------------------------
     *
     * L'ordine delle tensor-info NON è significativo in GGUF: un autore può elencare i
     * tensori in qualunque ordine. Quindi qui si ORDINANO gli intervalli per offset e si
     * cerca una SOVRAPPOSIONE (vietata), non un ordine decrescente (lecito). Lo stesso
     * array ordinato per nome trova i duplicati senza il doppio ciclo O(n²).
     * Prima i difetti di disposizione, poi la lunghezza dei dati: un file con i tensori
     * sovrapposti E la data section assente deve dire "sovrapposti", non "dati corti". */
    out->data_start = align_up(r.at, out->alignment);
    if (out->n_tensors) {
        Seg *sg = (Seg *)zalloc_owned(c, (size_t)out->n_tensors, sizeof(Seg));
        if (!sg) { fail(&r, FMLX_GGUF_ERR_NO_MEM, "array degli intervalli"); goto failed; }
        for (i = 0; i < out->n_tensors; i++) {
            FmlxGgufTensor *t = &out->tensors[i];
            sg[i].name = t->name;
            sg[i].start = t->offset;
            sg[i].end = t->offset + t->nbytes;
        }
        qsort(sg, (size_t)out->n_tensors, sizeof(Seg), seg_cmp_offset);
        /* Sweep con la FINE massima vista fin qui. Dopo l'ordinamento per `start` è
         * equivalente al confronto col predecessore (se un intervallo ne tocca uno più
         * antico, tocca anche chi lo precede in ordine di start): si tiene la forma che
         * non ha bisogno della dimostrazione per essere leggibile. */
        {
            size_t run = 0;
            for (i = 1; i < out->n_tensors; i++) {
                if (sg[i].start < sg[run].end) {
                    fail(&r, FMLX_GGUF_ERR_OVERLAP,
                         "tensori \"%s\" e \"%s\": intervalli [%llu,%llu) e [%llu,%llu) "
                         "sovrapposti",
                         sg[run].name ? sg[run].name : "?", sg[i].name ? sg[i].name : "?",
                         (unsigned long long)sg[run].start, (unsigned long long)sg[run].end,
                         (unsigned long long)sg[i].start, (unsigned long long)sg[i].end);
                    goto failed;
                }
                if (sg[i].end > sg[run].end) run = i;
            }
        }
        qsort(sg, (size_t)out->n_tensors, sizeof(Seg), seg_cmp_name);
        for (i = 1; i < out->n_tensors; i++) {
            if (sg[i].name && sg[i - 1].name && strcmp(sg[i].name, sg[i - 1].name) == 0) {
                fail(&r, FMLX_GGUF_ERR_NAME_DUP, "nome di tensore duplicato: \"%s\"",
                     sg[i].name);
                goto failed;
            }
        }
        for (i = 0; i < out->n_tensors; i++) {
            if (out->data_start > len || sg[i].end > len - out->data_start) {
                fail(&r, FMLX_GGUF_ERR_DATA_SHORT,
                     "tensore \"%s\" finisce all'offset %llu della data section, che inizia a "
                     "%llu in un file da %zu byte",
                     sg[i].name ? sg[i].name : "?", (unsigned long long)sg[i].end,
                     (unsigned long long)out->data_start, len);
                goto failed;
            }
        }
    }
    return FMLX_GGUF_OK;

failed:
    if (r.err) out->last_error = r.err;
    else {
        char b[96];
        snprintf(b, sizeof b, "%s (messaggio non allocato)",
                 fmlx_gguf_status_str(r.failed ? r.code : FMLX_GGUF_ERR_IO));
        out->last_error = dupn(c, b, strlen(b));
    }
    abandon(out);
    return r.failed ? r.code : FMLX_GGUF_ERR_IO;
}

/* ------------------------------------------------------------------ apertura da file */

FmlxGgufStatus fmlx_gguf_open_header(const char *path, FmlxGgufFile *out) {
    int fd;
    off_t sz;
    void *m;
    Ctx *c;
    FmlxGgufStatus st;

    if (!path || !out) return FMLX_GGUF_ERR_IO;
    memset(out, 0, sizeof *out);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        set_io_error(out, "apertura", path, errno);
        return FMLX_GGUF_ERR_IO;
    }
    sz = lseek(fd, 0, SEEK_END);
    if (sz <= 0) {
        set_io_error(out, "lettura dimensione", path, sz == 0 ? 0 : errno);
        close(fd);
        return FMLX_GGUF_ERR_IO;
    }
    /* PROT_READ + MAP_PRIVATE: le pagine restano scaricabili dal SO, e la parse tocca
     * solo la testata. I byte dei tensori non vengono materializzati qui. */
    m = mmap(NULL, (size_t)sz, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) {
        set_io_error(out, "mmap", path, errno);
        return FMLX_GGUF_ERR_IO;
    }
    st = fmlx_gguf_parse((const uint8_t *)m, (size_t)sz, out);
    c = (Ctx *)out->arena;
    if (st == FMLX_GGUF_OK) {
        c->mmapped = 1;
        c->map_len = (size_t)sz;
        out->owns_buffer = 1;
        return FMLX_GGUF_OK;
    }
    /* In errore: il messaggio vive nella Ctx, che close() deve poter liberare. Si marca
     * il mapping come owned così close() fa munmap e ctx_free in un colpo solo. */
    if (c) {
        c->mmapped = 1;
        c->map_len = (size_t)sz;
        out->owns_buffer = 1;
    } else {
        munmap(m, (size_t)sz);
    }
    return st;
}

FmlxGgufStatus fmlx_gguf_read_tensor(const FmlxGgufFile *f, uint64_t idx, void *dst,
                                     size_t dst_len) {
    const FmlxGgufTensor *t;
    if (!f || !dst) return FMLX_GGUF_ERR_IO;
    if (idx >= f->n_tensors || !f->tensors) return FMLX_GGUF_ERR_COUNT;
    t = &f->tensors[idx];
    if (dst_len < t->nbytes) return FMLX_GGUF_ERR_DST_SMALL;
    if (!f->data) return FMLX_GGUF_ERR_IO;
    memcpy(dst, f->data + f->data_start + t->offset, (size_t)t->nbytes);
    return FMLX_GGUF_OK;
}

const uint8_t *fmlx_gguf_tensor_bytes(const FmlxGgufFile *f, uint64_t idx, uint64_t at,
                                      uint64_t len) {
    const FmlxGgufTensor *t;
    if (!f || !f->data) return NULL;
    if (idx >= f->n_tensors || !f->tensors) return NULL;
    t = &f->tensors[idx];
    if (at > t->nbytes || len > t->nbytes - at) return NULL;
    return f->data + f->data_start + t->offset + at;
}

void fmlx_gguf_close(FmlxGgufFile *f) {
    Ctx *c;
    if (!f) return;
    c = (Ctx *)f->arena;
    if (c) {
        /* Gli array di tensore/KV sono nella Ctx: niente free separato. */
        if (c->mmapped && f->data) munmap((void *)f->data, c->map_len);
        else if (f->owns_buffer) free((void *)f->data);
        ctx_free(c);
        free(c);
    }
    memset(f, 0, sizeof *f);
}

/* ------------------------------------------------------------------ lookup */

const FmlxGgufTensor *fmlx_gguf_find_tensor(const FmlxGgufFile *f, const char *name) {
    uint64_t i;
    if (!f || !name || !f->tensors) return NULL;
    for (i = 0; i < f->n_tensors; i++)
        if (f->tensors[i].name && strcmp(f->tensors[i].name, name) == 0) return &f->tensors[i];
    return NULL;
}

const FmlxGgufKV *fmlx_gguf_find_kv(const FmlxGgufFile *f, const char *key) {
    uint64_t i;
    if (!f || !key || !f->kvs) return NULL;
    for (i = 0; i < f->n_kv; i++)
        if (f->kvs[i].key && strcmp(f->kvs[i].key, key) == 0) return &f->kvs[i];
    return NULL;
}

int fmlx_gguf_kv_str(const FmlxGgufFile *f, const char *key, const char **out) {
    const FmlxGgufKV *kv = fmlx_gguf_find_kv(f, key);
    if (out) *out = NULL;
    if (!kv || kv->type != KV_STRING || !kv->s) return 0;
    if (out) *out = kv->s;
    return 1;
}

int fmlx_gguf_kv_u64(const FmlxGgufFile *f, const char *key, uint64_t *out) {
    const FmlxGgufKV *kv = fmlx_gguf_find_kv(f, key);
    if (out) *out = 0;
    if (!kv || kv->type == KV_ARRAY || kv->type == KV_STRING) return 0;
    if (out) *out = kv->u;
    return 1;
}

const char *fmlx_gguf_last_error(const FmlxGgufFile *f) {
    if (!f) return "struttura non inizializzata (f == NULL)";
    /* MAI NULL: un `%s` che stampa "(null)" è il modo in cui i motivi spariscono. */
    return f->last_error ? f->last_error : "nessun messaggio registrato (out non inizializzato?)";
}

/* ------------------------------------------------------------------ writer di fixture */

static int wr_u(FILE *fp, uint64_t v, int bytes) {
    unsigned char b[8];
    int i;
    for (i = 0; i < bytes; i++) b[i] = (unsigned char)((v >> (8 * i)) & 0xFF);
    return fwrite(b, 1, (size_t)bytes, fp) == (size_t)bytes ? 0 : -1;
}

static int wr_str(FILE *fp, const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (wr_u(fp, n, 8)) return -1;
    return n ? (fwrite(s, 1, n, fp) == n ? 0 : -1) : 0;
}

/* Scrive un GGUF coerente (offset e nbytes calcolati dalla tabella, data section allineata).
 * Serve ai test per produrre fixture senza scaricare nulla; non è superficie pubblica d'uso. */
FmlxGgufStatus fmlx_gguf_write_fixture(const char *path, uint32_t version, uint64_t alignment,
                                       const FmlxGgufKV *kvs, uint64_t n_kv,
                                       const FmlxGgufTensor *tensors, uint64_t n_tensors,
                                       const uint8_t *blob, size_t blob_len) {
    FILE *fp;
    uint64_t i, off = 0;
    long hdr;
    uint64_t pad;
    int wide;
    static const char zeros[4096];

    if (!path) return FMLX_GGUF_ERR_IO;
    if (!alignment) alignment = FMLX_GGUF_ALIGN_DEFAULT;
    fp = fopen(path, "wb");
    if (!fp) return FMLX_GGUF_ERR_IO;
    wide = (version >= 3);
    if (wr_u(fp, FMLX_GGUF_MAGIC, 4)) goto io;
    if (wr_u(fp, version, 4)) goto io;
    if (wr_u(fp, n_tensors, wide ? 8 : 4)) goto io;
    if (wr_u(fp, n_kv, wide ? 8 : 4)) goto io;
    for (i = 0; i < n_kv; i++) {
        const FmlxGgufKV *kv = &kvs[i];
        if (wr_str(fp, kv->key)) goto io;
        if (wr_u(fp, kv->type, 4)) goto io;
        if (kv->type == KV_STRING) { if (wr_str(fp, kv->s)) goto io; }
        else if (kv->type == KV_ARRAY) {
            uint64_t j;
            if (wr_u(fp, kv->elem_type, 4)) goto io;
            if (wr_u(fp, kv->arr_len, 8)) goto io;
            for (j = 0; j < kv->arr_len; j++) {
                if (kv->elem_type == KV_STRING) { if (wr_str(fp, kv->arr_s[j])) goto io; }
                else if (wr_u(fp, kv->arr_u[j], kv_scalar_size(kv->elem_type))) goto io;
            }
        } else if (wr_u(fp, kv->u, kv_scalar_size(kv->type))) goto io;
    }
    /* gli offset si ricalcolano qui: la fixture è sempre autoconsistente */
    for (i = 0; i < n_tensors; i++) {
        const FmlxGgufTensor *t = &tensors[i];
        uint64_t nb;
        if (fmlx_ggml_type_bytes(t->ggml_type, t->numel, &nb) != FMLX_GGUF_OK) { fclose(fp); return FMLX_GGUF_ERR_TYPE_UNKNOWN; }
        if (wr_str(fp, t->name)) goto io;
        if (wr_u(fp, 2, 4)) goto io;          /* n_dims: le fixture sono matrici [numel, 1] */
        if (wr_u(fp, t->numel, 8)) goto io;
        if (wr_u(fp, 1, 8)) goto io;
        if (wr_u(fp, (uint64_t)(uint32_t)t->ggml_type, 4)) goto io;
        if (wr_u(fp, off, 8)) goto io;
        off += nb;
    }
    hdr = ftell(fp);
    pad = align_up((uint64_t)hdr, alignment) - (uint64_t)hdr;
    while (pad) {
        size_t chunk = pad > sizeof zeros ? sizeof zeros : (size_t)pad;
        if (fwrite(zeros, 1, chunk, fp) != chunk) goto io;
        pad -= chunk;
    }
    if (blob && blob_len && fwrite(blob, 1, blob_len, fp) != blob_len) goto io;
    if (fclose(fp)) return FMLX_GGUF_ERR_IO;
    return FMLX_GGUF_OK;
io:
    fclose(fp);
    return FMLX_GGUF_ERR_IO;
}
