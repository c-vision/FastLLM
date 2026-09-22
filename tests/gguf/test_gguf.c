/* test_gguf.c — suite della lane GGUF.
 *
 * Nessuna fixture richiede un download: i file validi si scrivono col writer di fixture
 * (`fmlx_gguf_write_fixture`), i file MALATI si costruiscono byte per byte qui sotto.
 * Corrisponde ai casi (a)–(f) del piano §8, più i casi di corruzione che nei lettori di
 * casa condividono tutti lo stesso `false`.
 */
#include "fmlx/fmlx_gguf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails, checks;

#define CHECK(cond, ...) do {                                     \
    checks++;                                                     \
    if (!(cond)) { fails++;                                       \
        printf("  FAIL %s:%d  ", __FILE__, __LINE__);             \
        printf(__VA_ARGS__);                                      \
        printf("\n");                                             \
    }                                                             \
} while (0)

#define OK(name) printf("  ok   %s\n", name)

/* ------------------------------------------------------------------ builder byte-level */

typedef struct {
    unsigned char *b;
    size_t n, cap;
} Buf;

static void buf_init(Buf *x) { x->b = NULL; x->n = x->cap = 0; }
static void buf_free(Buf *x) { free(x->b); buf_init(x); }
static void buf_put(Buf *x, const void *p, size_t n) {
    if (x->n + n > x->cap) {
        size_t nc = (x->cap ? x->cap * 2 : 256);
        while (nc < x->n + n) nc *= 2;
        x->b = (unsigned char *)realloc(x->b, nc);
        if (!x->b) { fprintf(stderr, "OOM nel builder\n"); exit(2); }
        x->cap = nc;
    }
    memcpy(x->b + x->n, p, n);
    x->n += n;
}
static void buf_u(Buf *x, uint64_t v, int bytes) {
    unsigned char t[8];
    int i;
    for (i = 0; i < bytes; i++) t[i] = (unsigned char)(v >> (8 * i));
    buf_put(x, t, (size_t)bytes);
}
static void buf_str(Buf *x, const char *s) {
    size_t n = strlen(s);
    buf_u(x, n, 8);
    buf_put(x, s, n);
}
/* header + 1 KV "general.alignment" (u32, se with_align) + 1 tensor-info scritto a mano */
static void buf_tensor(Buf *x, const char *name, uint64_t numel, uint32_t type, uint64_t offset) {
    buf_str(x, name);
    buf_u(x, 2, 4);          /* n_dims */
    buf_u(x, numel, 8);
    buf_u(x, 1, 8);
    buf_u(x, type, 4);
    buf_u(x, offset, 8);
}

/* Chiude la testata al multiplo di `align` (come fa il formato) e aggiunge `n` byte di
 * data section: senza di quelli ogni fixture risulterebbe solo DATA_SHORT. */
static void pad_to_and_data(Buf *x, uint64_t align, uint64_t n) {
    static const char zeros[256];
    uint64_t i;
    while (x->n % align) buf_u(x, 0, 1);
    for (i = 0; i < n;) {
        uint64_t chunk = n - i < sizeof zeros ? n - i : sizeof zeros;
        buf_put(x, zeros, (size_t)chunk);
        i += chunk;
    }
}

/* ------------------------------------------------------------------ casi */

static const char *tmp_dir(void) {
    const char *d = getenv("TMPDIR");
    return (d && *d) ? d : "/tmp";
}

static char fixture_path[512];
static const char *fx(const char *tag) {
    snprintf(fixture_path, sizeof fixture_path, "%s/fmlx_test_%s.gguf", tmp_dir(), tag);
    return fixture_path;
}

/* (a) allineamento dichiarato dall'autore del modello VIOLATO da chi hardcode 32: qui si
 *     asserisce che il ns. lettore lo LEGGE e posiziona la data section lì. Due valori,
 *     64 e 4096: con 4096 un lettore che hardcoded 32 sbaglia di sicuro (la testata non è
 *     mai multipla di 4096), con 64 si vede il caso minimo. */
static void alignment_case(uint64_t al) {
    FmlxGgufFile f;
    FmlxGgufKV kv;
    FmlxGgufTensor t;
    unsigned char *blob;
    uint64_t nb = 0;
    char tag[32];

    memset(&kv, 0, sizeof kv);
    kv.key = (char *)"general.alignment";
    kv.type = 4 /* u32 */;
    kv.u = al;
    memset(&t, 0, sizeof t);
    t.name = (char *)"blk.0.w";
    t.numel = 4096;
    t.ggml_type = 0; /* F32 */
    CHECK(fmlx_ggml_type_bytes(0, 4096, &nb) == FMLX_GGUF_OK && nb == 16384, "F32 4096 -> %llu",
          (unsigned long long)nb);
    /* La fixture DEVE avere i dati: senza la data section l'apertura fallirebbe per
     * DATA_SHORT e il test non misurerebbe l'allineamento. */
    blob = (unsigned char *)calloc(1, (size_t)nb);
    CHECK(blob != NULL, "blob fixture");
    if (!blob) return;
    snprintf(tag, sizeof tag, "align%llu", (unsigned long long)al);
    CHECK(fmlx_gguf_write_fixture(fx(tag), 3, al, &kv, 1, &t, 1, blob, nb) == FMLX_GGUF_OK,
          "scrittura fixture alignment=%llu fallita", (unsigned long long)al);
    free(blob);

    if (fmlx_gguf_open_header(fx(tag), &f) != FMLX_GGUF_OK) {
        CHECK(0, "apertura alignment=%llu: %s", (unsigned long long)al,
              fmlx_gguf_last_error(&f));
        fmlx_gguf_close(&f);
        unlink(fx(tag));
        return;
    }
    CHECK(f.alignment == al, "alignment letto = %llu, atteso %llu",
          (unsigned long long)f.alignment, (unsigned long long)al);
    CHECK(f.data_start % al == 0, "data_start %llu non multiplo di %llu",
          (unsigned long long)f.data_start, (unsigned long long)al);
    CHECK(f.data_start > 32, "data_start %llu: il padding non è stato applicato",
          (unsigned long long)f.data_start);
    CHECK(f.n_tensors == 1 && f.n_kv == 1, "conteggi %llu/%llu",
          (unsigned long long)f.n_tensors, (unsigned long long)f.n_kv);
    CHECK(f.tensors && f.tensors[0].name && strcmp(f.tensors[0].name, "blk.0.w") == 0,
          "nome tensore \"%s\"", (f.tensors && f.tensors[0].name) ? f.tensors[0].name : "(null)");
    CHECK(f.tensors[0].offset == 0 && f.tensors[0].nbytes == nb,
          "tensore offset %llu nbytes %llu", (unsigned long long)f.tensors[0].offset,
          (unsigned long long)f.tensors[0].nbytes);
    fmlx_gguf_close(&f);
    unlink(fx(tag));
}

static void test_alignment_honored(void) {
    alignment_case(64);
    alignment_case(4096);
    OK("general.alignment onorato (64 e 4096)");
}

/* (b) cap: 4097 tensori (il cap di casa) DEVONO aprirsi, con il conteggio giusto. */
static void test_no_tensor_cap(void) {
    enum { N = 4097 };
    FmlxGgufFile f;
    FmlxGgufTensor *ts = (FmlxGgufTensor *)calloc(N, sizeof(FmlxGgufTensor));
    char (*names)[16] = (char (*)[16])malloc(N * 16);
    uint64_t i, off = 0;
    CHECK(ts && names, "allocazione fixture");
    if (!ts || !names) return;
    for (i = 0; i < N; i++) {
        uint64_t nb = 0;
        snprintf(names[i], 16, "t%llu", (unsigned long long)i);
        ts[i].name = names[i];
        ts[i].numel = 32;
        ts[i].ggml_type = 0; /* F32: 128 byte */
        fmlx_ggml_type_bytes(0, 32, &nb);
        ts[i].offset = off; /* il writer ricalcola: qui basta per la size del blob */
        off += nb;
    }
    /* Il blob serve davvero: senza i dati l'apertura si fermerebbe a DATA_SHORT e non
     * misurerebbe il cap. Qui è zero-filled, tanto il test legge la sola testata. */
    {
        unsigned char *blob = (unsigned char *)calloc((size_t)off, 1);
        FmlxGgufStatus st;
        CHECK(blob != NULL, "blob da %llu byte", (unsigned long long)off);
        st = fmlx_gguf_write_fixture(fx("cap4097"), 3, 32, NULL, 0, ts, N, blob, off);
        free(blob);
        CHECK(st == FMLX_GGUF_OK, "scrittura fixture da %d tensori: %d", N, (int)st);
    }
    if (fmlx_gguf_open_header(fx("cap4097"), &f) != FMLX_GGUF_OK) {
        CHECK(0, "4097 tensori rifiutati (%s): il cap deve non esistere",
              fmlx_gguf_last_error(&f));
        fmlx_gguf_close(&f);
    } else {
        CHECK(f.n_tensors == N, "letti %llu tensori, attesi %d",
              (unsigned long long)f.n_tensors, N);
        CHECK(f.tensors[N - 1].name && strcmp(f.tensors[N - 1].name, "t4096") == 0,
              "ultimo tensore = \"%s\"", f.tensors[N - 1].name ? f.tensors[N - 1].name : "(null)");
        fmlx_gguf_close(&f);
    }
    free(names);
    free(ts);
    unlink(fx("cap4097"));
    OK("nessun cap sui tensori (4097 letti)");
}

/* (c) BF16: 2 byte per elemento, MAI 0. È il caso che nei file a disco esiste già. */
static void test_bf16_not_zero(void) {
    uint64_t nb = 1;
    CHECK(fmlx_ggml_type_bytes(30, 512, &nb) == FMLX_GGUF_OK, "BF16 rifiutato");
    CHECK(nb == 1024, "BF16 512 elementi -> %llu byte, attesi 1024 (0 = bug di casa)",
          (unsigned long long)nb);
    nb = 1;
    CHECK(fmlx_ggml_type_bytes(26, 256, &nb) == FMLX_GGUF_OK && nb == 1024,
          "I32 256 -> %llu, attesi 1024", (unsigned long long)nb);
    OK("BF16 e I32 hanno dimensione reale");
}

/* (d) numel non multiplo del blocco: ERRORE, non troncamento. */
static void test_block_truncation_is_error(void) {
    uint64_t nb = 1;
    CHECK(fmlx_ggml_type_bytes(12, 100, &nb) == FMLX_GGUF_ERR_NUMEL_BLOCK,
          "Q4_K con numel=100: atteso errore, got %d (%llu byte)", (int)0,
          (unsigned long long)nb);
    CHECK(nb == 0, "in errore *out deve restare 0, got %llu", (unsigned long long)nb);
    CHECK(fmlx_ggml_type_bytes(12, 2560, &nb) == FMLX_GGUF_OK && nb == 1440,
          "Q4_K 2560 -> %llu, attesi 1440", (unsigned long long)nb);
    OK("numel fuori blocco è errore esplicito");
}

/* (e) tipo sconosciuto / ritirato: ERRORE con il NUMERO del tipo nel messaggio. */
static void test_unknown_type_errors(void) {
    uint64_t nb = 7;
    CHECK(fmlx_ggml_type_bytes(999, 256, &nb) == FMLX_GGUF_ERR_TYPE_UNKNOWN, "tipo 999 non errore");
    CHECK(fmlx_ggml_type_bytes(31, 256, &nb) == FMLX_GGUF_ERR_TYPE_UNKNOWN,
          "tipo reserved 31 non errore");
    CHECK(fmlx_ggml_type_bytes(4, 256, &nb) == FMLX_GGUF_ERR_TYPE_REMOVED,
          "Q4_2 (ritirato) non errore distinto");
    CHECK(fmlx_ggml_type(4) != NULL, "Q4_2 deve restare NOMINATO in tabella (usable=0)");
    OK("tipi sconosciuti/ritirati => errori distinti");
}

/* (f) parità della tabella contro una sorgente indipendente.
 * Valori di riferimento: /Users/gaetano/ds4/gguf-tools/quants.h:20-54 (enum per valore)
 * + quants.c:39-74 ({nome, block_vals, block_bytes}). Sono gli STESSI valori che la
 * tabella di casa (gmb_gguf_writer.c:49-62) viola su 18 e 19.
 *
 * Nota di onestà, perché questa lista HA GIA' MENTITO: i valori qui sotto sono una
 * TRASCRIZIONE, e la prima trascrizione spostava TQ1_0/TQ2_0/MXFP4/NVFP4/Q1_0 di uno
 * (33/34/38/39/40 invece di 34/35/39/40/41) — il test concordava col codice perché entrambi
 * venivano dalla mia stessa lettura, e "concordano" non è "sono indipendenti". La parità che
 * vale qualcosa è quella del gate: `tools/quant_diff_ggml.c` mette `_Static_assert` contro i
 * macro di `ggml.h`, quindi lì un numero sbagliato FERMA LA COMPILAZIONE invece di passare. */
static const struct {
    int t;
    const char *name;
    uint64_t bv, bb;
} REF[] = {
    { 0, "F32", 1, 4 }, { 1, "F16", 1, 2 }, { 2, "Q4_0", 32, 18 }, { 3, "Q4_1", 32, 20 },
    { 6, "Q5_0", 32, 22 }, { 7, "Q5_1", 32, 24 }, { 8, "Q8_0", 32, 34 }, { 9, "Q8_1", 32, 36 },
    { 10, "Q2_K", 256, 84 }, { 11, "Q3_K", 256, 110 }, { 12, "Q4_K", 256, 144 },
    { 13, "Q5_K", 256, 176 }, { 14, "Q6_K", 256, 210 }, { 15, "Q8_K", 256, 292 },
    { 16, "IQ2_XXS", 256, 66 }, { 17, "IQ2_XS", 256, 74 },
    { 18, "IQ3_XXS", 256, 98 },  /* casa scrive 34: 65% in meno */
    { 19, "IQ1_S", 256, 50 },    /* casa scrive 144: 188% in più */
    { 20, "IQ4_NL", 32, 18 }, { 21, "IQ3_S", 256, 110 }, { 22, "IQ2_S", 256, 82 },
    { 23, "IQ4_XS", 256, 136 }, { 24, "I8", 1, 1 }, { 25, "I16", 1, 2 }, { 26, "I32", 1, 4 },
    { 27, "I64", 1, 8 }, { 28, "F64", 1, 8 }, { 29, "IQ1_M", 256, 56 }, { 30, "BF16", 1, 2 },
    { 34, "TQ1_0", 256, 54 }, { 35, "TQ2_0", 256, 66 }, { 39, "MXFP4", 32, 17 },
    { 40, "NVFP4", 64, 36 }, { 41, "Q1_0", 128, 18 },
};

/* I buchi dell'enum DEVONO restare buchi: se 31/32/33/36/37/38 acquistarono un nome, la
 * tabella fu di nuovo compattata e cinque tipi sono letti con la forma sbagliata. */
static const int REF_HOLES[] = { 31, 32, 33, 36, 37, 38 };

static void test_type_table_parity(void) {
    size_t i;
    for (i = 0; i < sizeof REF / sizeof *REF; i++) {
        const FmlxGgmlType *ty = fmlx_ggml_type(REF[i].t);
        char expect_bytes[64];
        uint64_t nb = 0;
        /* il numero di byte di UN blocco, via la funzione pubblica */
        snprintf(expect_bytes, sizeof expect_bytes, "%llu", (unsigned long long)REF[i].bb);
        CHECK(ty != NULL, "tipo %d (%s) assente dalla ns. tabella", REF[i].t, REF[i].name);
        if (!ty) continue;
        CHECK(strcmp(ty->name, REF[i].name) == 0, "tipo %d: nome \"%s\", atteso \"%s\"", REF[i].t,
              ty->name, REF[i].name);
        CHECK(ty->block_vals == REF[i].bv, "tipo %d (%s): block_vals %llu, atteso %llu", REF[i].t,
              REF[i].name, (unsigned long long)ty->block_vals, (unsigned long long)REF[i].bv);
        CHECK(ty->block_bytes == REF[i].bb, "tipo %d (%s): block_bytes %llu, atteso %s", REF[i].t,
              REF[i].name, (unsigned long long)ty->block_bytes, expect_bytes);
        CHECK(fmlx_ggml_type_bytes(REF[i].t, REF[i].bv, &nb) == FMLX_GGUF_OK && nb == REF[i].bb,
              "tipo %d: un blocco -> %llu byte, attesi %llu", REF[i].t,
              (unsigned long long)nb, (unsigned long long)REF[i].bb);
    }
    for (i = 0; i < sizeof REF_HOLES / sizeof *REF_HOLES; i++) {
        CHECK(fmlx_ggml_type(REF_HOLES[i]) == NULL,
              "il valore %d dell'enum è un BUCO in ggml (tipo ritirato): la ns. tabella gli ha "
              "dato un nome, cioè è stata di nuovo compattata", REF_HOLES[i]);
    }
    OK("tabella tipi in parità con la sorgente di riferimento");
}

/* ------------------------------------------------------------------ corruzioni: codici DISTINTI
 *
 * Nei lettori di casa questi casi convergono tutti nello stesso `false`/`0`. Qui si
 * asserisce che ognuno ha il SUO codice, e che nessun caso ha esito 0.
 */
static void expect_code(const unsigned char *b, size_t n, FmlxGgufStatus want, const char *what) {
    FmlxGgufFile f;
    FmlxGgufStatus got = fmlx_gguf_parse(b, n, &f);
    CHECK(got == want, "%s: atteso codice %d (%s), got %d (%s) — messaggio: %s", what, (int)want,
          fmlx_gguf_status_str(want), (int)got, fmlx_gguf_status_str(got),
          fmlx_gguf_last_error(&f));
    CHECK(got != FMLX_GGUF_OK || want == FMLX_GGUF_OK, "%s: esito 0 su input malato", what);
    fmlx_gguf_close(&f);
    /* il messaggio deve restare leggibile anche DOPO close? no: close lo libera.
     * Verifichiamo che PRIMA di close ci fosse, per ogni errore. */
}

static void test_corruption_codes(void) {
    Buf x;
    char msg[256];

    /* magic errata */
    buf_init(&x);
    buf_u(&x, 0x11223344u, 4);
    buf_u(&x, 3, 4);
    buf_u(&x, 0, 8);
    buf_u(&x, 0, 8);
    expect_code(x.b, x.n, FMLX_GGUF_ERR_NOT_GGUF, "magic diversa");
    buf_free(&x);

    /* versione 1 */
    buf_init(&x);
    buf_u(&x, FMLX_GGUF_MAGIC, 4);
    buf_u(&x, 1, 4);
    buf_u(&x, 0, 8);
    buf_u(&x, 0, 8);
    expect_code(x.b, x.n, FMLX_GGUF_ERR_VERSION, "versione 1");
    buf_free(&x);

    /* file troncato a metà tensor-info */
    buf_init(&x);
    buf_u(&x, FMLX_GGUF_MAGIC, 4);
    buf_u(&x, 3, 4);
    buf_u(&x, 1, 8);
    buf_u(&x, 0, 8);
    buf_str(&x, "w");
    buf_u(&x, 2, 4);
    buf_u(&x, 32, 8);
    expect_code(x.b, x.n, FMLX_GGUF_ERR_TRUNCATED, "tensor-info troncato");
    buf_free(&x);

    /* n_dims = 9 (oltre il massimo) */
    buf_init(&x);
    buf_u(&x, FMLX_GGUF_MAGIC, 4);
    buf_u(&x, 3, 4);
    buf_u(&x, 1, 8);
    buf_u(&x, 0, 8);
    buf_str(&x, "w");
    buf_u(&x, 9, 4);
    expect_code(x.b, x.n, FMLX_GGUF_ERR_NDIMS, "n_dims = 9");
    buf_free(&x);

    /* dim = 0 */
    buf_init(&x);
    buf_u(&x, FMLX_GGUF_MAGIC, 4);
    buf_u(&x, 3, 4);
    buf_u(&x, 1, 8);
    buf_u(&x, 0, 8);
    buf_str(&x, "w");
    buf_u(&x, 2, 4);
    buf_u(&x, 0, 8);
    buf_u(&x, 1, 8);
    expect_code(x.b, x.n, FMLX_GGUF_ERR_NEG_DIM, "dim[0] = 0");
    buf_free(&x);

    /* tipo 999 dentro un header altrimenti valido: errore, non 0 byte */
    buf_init(&x);
    buf_u(&x, FMLX_GGUF_MAGIC, 4);
    buf_u(&x, 3, 4);
    buf_u(&x, 1, 8);
    buf_u(&x, 0, 8);
    buf_str(&x, "w");
    buf_u(&x, 2, 4);
    buf_u(&x, 256, 8);
    buf_u(&x, 1, 8);
    buf_u(&x, 999, 4);
    buf_u(&x, 0, 8);
    expect_code(x.b, x.n, FMLX_GGUF_ERR_TYPE_UNKNOWN, "ggml_type 999");
    /* e il messaggio deve contenere il numero del tipo */
    {
        FmlxGgufFile f;
        if (fmlx_gguf_parse(x.b, x.n, &f) != FMLX_GGUF_OK) {
            const char *m = fmlx_gguf_last_error(&f);
            snprintf(msg, sizeof msg, "%s", m ? m : "");
            CHECK(strstr(msg, "999") != NULL, "il messaggio non nomina il tipo: \"%s\"", msg);
            fmlx_gguf_close(&f);
        }
    }
    buf_free(&x);

    /* general.alignment = 3 (non potenza di 2) */
    buf_init(&x);
    buf_u(&x, FMLX_GGUF_MAGIC, 4);
    buf_u(&x, 3, 4);
    buf_u(&x, 0, 8);
    buf_u(&x, 1, 8);
    buf_str(&x, "general.alignment");
    buf_u(&x, 4, 4);
    buf_u(&x, 3, 4);
    expect_code(x.b, x.n, FMLX_GGUF_ERR_BAD_ALIGN, "alignment = 3");
    buf_free(&x);

    /* tensori sovrapposti */
    buf_init(&x);
    buf_u(&x, FMLX_GGUF_MAGIC, 4);
    buf_u(&x, 3, 4);
    buf_u(&x, 2, 8);
    buf_u(&x, 0, 8);
    buf_tensor(&x, "a", 32, 0, 0);   /* F32 32 = 128 byte */
    buf_tensor(&x, "b", 32, 0, 64);   /* si sovrappone ad a */
    expect_code(x.b, x.n, FMLX_GGUF_ERR_OVERLAP, "tensori sovrapposti");
    buf_free(&x);

    /* dati oltre la fine del file */
    buf_init(&x);
    buf_u(&x, FMLX_GGUF_MAGIC, 4);
    buf_u(&x, 3, 4);
    buf_u(&x, 1, 8);
    buf_u(&x, 0, 8);
    buf_tensor(&x, "a", 32, 0, 1000000);
    expect_code(x.b, x.n, FMLX_GGUF_ERR_DATA_SHORT, "offset oltre la fine");
    buf_free(&x);

    /* nome duplicato */
    buf_init(&x);
    buf_u(&x, FMLX_GGUF_MAGIC, 4);
    buf_u(&x, 3, 4);
    buf_u(&x, 2, 8);
    buf_u(&x, 0, 8);
    buf_tensor(&x, "dup", 32, 0, 0);
    buf_tensor(&x, "dup", 32, 0, 128);
    expect_code(x.b, x.n, FMLX_GGUF_ERR_NAME_DUP, "nome duplicato");
    buf_free(&x);

    /* n_tensors enorme: con la GUARDIA diventa COUNT e non una richiesta da 4 GB.
     * Il tensore 4098 (oltre il cap di casa) è nel test (b): lì si asserisce OK. */
    buf_init(&x);
    buf_u(&x, FMLX_GGUF_MAGIC, 4);
    buf_u(&x, 3, 4);
    buf_u(&x, 4000000000ull, 8);
    buf_u(&x, 0, 8);
    expect_code(x.b, x.n, FMLX_GGUF_ERR_COUNT, "n_tensors impossibile per la size");
    buf_free(&x);
    OK("11 corruzioni diverse => 11 codici diversi");
}

/* round-trip: header + dati reali, lettura di un tensore de-quantizzabile a byte */
static void test_roundtrip_read(void) {
    FmlxGgufFile f;
    FmlxGgufTensor ts[2];
    unsigned char blob[256 + 64];
    uint64_t i;
    unsigned char got[256];
    memset(ts, 0, sizeof ts);
    memset(blob, 0, sizeof blob);
    for (i = 0; i < sizeof blob; i++) blob[i] = (unsigned char)(i * 7 + 1);
    ts[0].name = (char *)"attn_q.w";
    ts[0].numel = 32;             /* F32 -> 128 byte */
    ts[0].ggml_type = 0;
    ts[1].name = (char *)"attn_k.w";
    ts[1].numel = 16;             /* F32 -> 64 byte, dopo il primo */
    ts[1].ggml_type = 0;
    CHECK(fmlx_gguf_write_fixture(fx("rt"), 3, 32, NULL, 0, ts, 2, blob, sizeof blob)
              == FMLX_GGUF_OK,
          "scrittura fixture round-trip");
    CHECK(fmlx_gguf_open_header(fx("rt"), &f) == FMLX_GGUF_OK, "apertura round-trip: %s",
          fmlx_gguf_last_error(&f));
    if (f.n_tensors == 2) {
        CHECK(f.tensors[0].offset == 0 && f.tensors[0].nbytes == 128,
              "tensore 0 offset %llu nbytes %llu", (unsigned long long)f.tensors[0].offset,
              (unsigned long long)f.tensors[0].nbytes);
        CHECK(f.tensors[1].offset == 128 && f.tensors[1].nbytes == 64,
              "tensore 1 offset %llu nbytes %llu", (unsigned long long)f.tensors[1].offset,
              (unsigned long long)f.tensors[1].nbytes);
        CHECK(fmlx_gguf_find_tensor(&f, "attn_k.w") == &f.tensors[1], "lookup per nome");
        memset(got, 0, sizeof got);
        CHECK(fmlx_gguf_read_tensor(&f, 0, got, 128) == FMLX_GGUF_OK, "lettura tensore 0");
        CHECK(memcmp(got, blob, 128) == 0, "byte del tensore 0 non coincidenti");
        CHECK(fmlx_gguf_read_tensor(&f, 1, got, 128) == FMLX_GGUF_OK, "lettura tensore 1");
        CHECK(memcmp(got, blob + 128, 64) == 0, "byte del tensore 1 non coincidenti");
        CHECK(fmlx_gguf_read_tensor(&f, 1, got, 63) == FMLX_GGUF_ERR_DST_SMALL,
              "buffer troppo piccolo non rilevato");
        CHECK(fmlx_gguf_read_tensor(&f, 99, got, 128) == FMLX_GGUF_ERR_COUNT,
              "indice fuori range non rilevato");
    }
    fmlx_gguf_close(&f);
    unlink(fx("rt"));
    OK("round-trip header+dati e lettura byte-esatta");
}

/* GGUF v2 (conteggi u32) deve aprirsi: il formato vecchio non è un errore */
static void test_version2_counts(void) {
    Buf x;
    FmlxGgufFile f;
    buf_init(&x);
    buf_u(&x, FMLX_GGUF_MAGIC, 4);
    buf_u(&x, 2, 4);
    buf_u(&x, 1, 4);          /* n_tensors u32 */
    buf_u(&x, 0, 4);          /* n_kv u32 */
    buf_tensor(&x, "v2w", 32, 0, 0);
    pad_to_and_data(&x, 32, 128); /* data section: 32 F32 = 128 byte, altrimenti è DATA_SHORT */
    CHECK(fmlx_gguf_parse(x.b, x.n, &f) == FMLX_GGUF_OK, "v2 rifiutata: %s",
          fmlx_gguf_last_error(&f));
    if (f.n_tensors) {
        CHECK(f.version == 2 && f.n_tensors == 1, "v2 letta come v%u n=%llu", f.version,
              (unsigned long long)f.n_tensors);
        fmlx_gguf_close(&f);
    }
    buf_free(&x);
    OK("GGUF v2 (conteggi u32) supportato");
}

/* Ordine NON crescente degli offset: in GGUF è LEGALE (le tensor-info non promettono
 * l'ordine dei dati). Un lettore che lo rifiuta scarta file veri. */
static void test_unordered_offsets_are_ok(void) {
    Buf x;
    FmlxGgufFile f;
    buf_init(&x);
    buf_u(&x, FMLX_GGUF_MAGIC, 4);
    buf_u(&x, 3, 4);
    buf_u(&x, 2, 8);
    buf_u(&x, 0, 8);
    buf_tensor(&x, "z.late", 32, 0, 256);  /* dichiarato per primo, ma dopo */
    buf_tensor(&x, "a.early", 32, 0, 0);
    pad_to_and_data(&x, 32, 384);          /* 256 + 128 byte di dati */
    CHECK(fmlx_gguf_parse(x.b, x.n, &f) == FMLX_GGUF_OK, "offset non crescenti rifiutati: %s",
          fmlx_gguf_last_error(&f));
    if (f.n_tensors) {
        CHECK(f.n_tensors == 2, "letti %llu tensori", (unsigned long long)f.n_tensors);
        CHECK(fmlx_gguf_find_tensor(&f, "z.late") == &f.tensors[0], "lookup sul primo info");
        fmlx_gguf_close(&f);
    }
    buf_free(&x);

    /* Stesso file, ma il secondo tensore INVADE il primo: qui OVERLAP deve scattare. */
    buf_init(&x);
    buf_u(&x, FMLX_GGUF_MAGIC, 4);
    buf_u(&x, 3, 4);
    buf_u(&x, 2, 8);
    buf_u(&x, 0, 8);
    buf_tensor(&x, "z.late", 32, 0, 256);
    buf_tensor(&x, "a.early", 32, 0, 300); /* [300,428) dentro [256,384) */
    expect_code(x.b, x.n, FMLX_GGUF_ERR_OVERLAP, "tensori fuori ordine e sovrapposti");
    buf_free(&x);
    OK("ordine degli offset non è un requisito, la sovrapposizione sì");
}

/* (g) il fallimento di APERTURA deve avere un codice E un motivo nominato. Un `%s` che
 *     stampa "(null)" è come i lettori di casa chiudono il discorso: "false", e basta. */
static void test_io_errors_are_named(void) {
    char missing[512], empty[512];
    FmlxGgufFile f;
    const char *msg;
    FILE *fp;
    FmlxGgufStatus st;

    snprintf(missing, sizeof missing, "%s/fmlx_test_assente.gguf", tmp_dir());
    unlink(missing); /* se esiste, non è più il caso "file che non c'è" */
    st = fmlx_gguf_open_header(missing, &f);
    CHECK(st == FMLX_GGUF_ERR_IO, "file assente: esito %d atteso ERR_IO", (int)st);
    msg = fmlx_gguf_last_error(&f);
    CHECK(msg && *msg, "file assente: messaggio vuoto");
    CHECK(msg && strstr(msg, missing) != NULL, "il messaggio non nomina il path: \"%s\"",
          msg ? msg : "(null)");
    CHECK(msg && strstr(msg, "No such file") != NULL,
          "manca il motivo di sistema nel messaggio: \"%s\"", msg ? msg : "(null)");
    fmlx_gguf_close(&f); /* close() su un'apertura fallita deve essere lecita */

    snprintf(empty, sizeof empty, "%s/fmlx_test_vuoto.gguf", tmp_dir());
    fp = fopen(empty, "wb");
    CHECK(fp != NULL, "fixture vuota non creabile");
    if (fp) fclose(fp);
    st = fmlx_gguf_open_header(empty, &f);
    CHECK(st == FMLX_GGUF_ERR_IO, "file vuoto: esito %d atteso ERR_IO", (int)st);
    msg = fmlx_gguf_last_error(&f);
    CHECK(msg && strstr(msg, empty) != NULL, "file vuoto: messaggio \"%s\"", msg ? msg : "(null)");
    fmlx_gguf_close(&f);
    unlink(empty);

    /* e mai NULL, nemmeno con la struttura stessa assente */
    CHECK(fmlx_gguf_last_error(NULL) != NULL, "last_error(NULL) ritorna NULL");
    CHECK(fmlx_gguf_last_error(NULL)[0] != '\0', "last_error(NULL) è vuota");
    OK("i fallimenti di apertura nominano path e motivo");
}

int main(void) {
    /* stdout non bloccato: se un CHECK fallisce, la riga deve apparire anche se il
     * processo muore subito dopo (il default è block-buffered quando l'output è un pipe). */
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("lane_gguf:\n");
    test_alignment_honored();
    test_no_tensor_cap();
    test_bf16_not_zero();
    test_block_truncation_is_error();
    test_unknown_type_errors();
    test_type_table_parity();
    test_corruption_codes();
    test_roundtrip_read();
    test_version2_counts();
    test_unordered_offsets_are_ok();
    test_io_errors_are_named();
    printf("\n%d verifiche, %d fallite\n", checks, fails);
    if (fails) {
        printf("ESITO: FALLITO\n");
        return 1;
    }
    printf("ESITO: OK\n");
    return 0;
}
