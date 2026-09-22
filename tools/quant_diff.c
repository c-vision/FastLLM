/* quant_diff.c — verifica DIFFERENZIALE del dequant di FastMLX contro TRE implementazioni
 * indipendenti, sui tensori REALI dei GGUF di questa
 * macchina. Nessun download, nessun modello in memoria: si dequantizza una finestra per
 * tipo e si confrontano i bit.
 *
 * Perché esiste: i formati dei blocchi ggml sono documentati per forma, non per formula,
 * e la memoria non è una fonte (vedi la lezione "mai citare un valore a memoria"). Qui la
 * fonte è ESEGUIBILE:
 *   A) GMBasic/native/gmb_host_ai_mlx/gmb_ggml.h         (gmb_ggml_dequant_row)
 *   B) GMBasic/native/gmb_moe_runtime/src/gmb_moe_ggml.c (gmb_moe_ggml_dequant_row)
 *   C) llama.cpp/ggml, la libreria già costruita su questa macchina: `dequantize_row_*` e
 *      `ggml_row_size`, raggiunti tramite tools/quant_diff_ggml.c
 * A e B sono due code diverse che girano su modelli veri: se divergono tra loro, la
 * divergenza è SEGNALATA (bug da aprire nel vault GMBasic), non assorbita. C è in più il
 * codice che i file GGUF li SCRIVE, quindi è autorità anche sui NUMERI di enum e sulle
 * dimensioni di blocco: sono i suoi `_Static_assert` contro `ggml.h` ad aver scoperto che la
 * nostra tabella posizionale aveva spostato TQ1_0/TQ2_0/MXFP4/NVFP4/Q1_0 di uno.
 *
 * Build (i path sono fuori repo di proposito: è uno strumento di confronto, non una
 * dipendenza di build del progetto — vedi CMakeLists.txt):
 *   G=/Users/gaetano/Documents/Projects/GMBasic
 *   L=/Users/gaetano/llamacpp
 *   cc -Wall -Wextra -Werror -std=gnu17 -I include -I "$G/native/gmb_host_ai_mlx" \
 *      -I "$G/native/gmb_moe_runtime/include" -I "$G/native/gmb_moe_runtime/src" \
 *      -I "$L/ggml/src" -I "$L/ggml/include" \
 *      tools/quant_diff.c tools/quant_diff_ggml.c src/lane_gguf/fmlx_gguf.c \
 *      src/lane_gguf/fmlx_quant.c "$G/native/gmb_moe_runtime/src/gmb_moe_ggml.c" \
 *      -L "$L/build/bin" -lggml-base -Wl,-rpath,"$L/build/bin" -o build/quant_diff
 * Uso: build/quant_diff <file.gguf> [...]      oppure   build/quant_diff --synthetic
 *      exit 0 = nessun bit divergente, 1 = anche UN solo bit diverge,
 *      2 = almeno un file non si è aperto (NON è un verdetto sui bit).
 */
#include "fmlx/fmlx_gguf.h"
#include "fmlx/fmlx_quant.h"

#include "gmb_ggml.h"    /* A */
#include "gmb_moe_ggml.h" /* B */

/* C: firme in tools/quant_diff_ggml.c */
int ggc_block_vals(int type);
int ggc_dequant_row(int type, const uint8_t *bytes, int64_t n, float *dst);
uint64_t ggc_row_bytes(int type, int64_t n);
int ggc_type_count(void);
uint64_t ggc_row_bytes_any(int type, int64_t n);

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLE_ELEMS 8192u

static float mine[SAMPLE_ELEMS], refA[SAMPLE_ELEMS], refB[SAMPLE_ELEMS], refC[SAMPLE_ELEMS];

/* Ritorna 1 se il confronto è lecito (stessa forma dei bit). */
static int cmp_bufs(const float *x, const float *y, uint64_t n, const char *what,
                    const char *tensor, uint64_t *first_diff, float *maxdiff) {
    uint64_t i;
    for (i = 0; i < n; i++) {
        if (memcmp(&x[i], &y[i], sizeof(float)) != 0) {
            double a = (double)x[i], b = (double)y[i];
            double d = a > b ? a - b : b - a;
            if (*maxdiff < (float)d) *maxdiff = (float)d;
            if (!*first_diff) {
                *first_diff = i + 1;
                printf("    DIVERGENZA %s su \"%s\"[%llu]: %.9g vs %.9g\n", what, tensor,
                       (unsigned long long)i, a, b);
            }
        }
    }
    return memcmp(x, y, (size_t)n * sizeof(float)) == 0;
}

struct witness {
    const char *tag;
    int ok; /* 1 = questo codice sa dequantizzare `type` e ha scritto il buffer */
    float *buf;
};

static void compare(int type, const uint8_t *bytes, uint64_t elems, const char *label,
                    uint64_t *bad) {
    struct witness w[3];
    FmlxGgufStatus s;
    int i, j, n_ok = 0;

    s = fmlx_ggml_dequant_row(type, bytes, elems, mine);
    if (s != FMLX_GGUF_OK) {
        printf("    %s tipo %d: IL NS. DEQUANT RIFIUTA (%" PRIu64 " elem) -> %s\n", label, type,
               elems, fmlx_gguf_status_str(s));
        (*bad)++;
        return;
    }
    w[0].tag = "A (gmb_ggml.h)";
    w[0].ok = gmb_ggml_dequant_row(type, bytes, (int64_t)elems, refA);
    w[0].buf = refA;
    w[1].tag = "B (gmb_moe_ggml.c)";
    w[1].ok = (gmb_moe_ggml_dequant_row((uint32_t)type, bytes, refB, (size_t)elems) == 0);
    w[1].buf = refB;
    w[2].tag = "C (ggml)";
    w[2].ok = ggc_dequant_row(type, bytes, (int64_t)elems, refC);
    w[2].buf = refC;

    for (i = 0; i < 3; i++) n_ok += w[i].ok != 0;
    if (!n_ok) {
        printf("    %s tipo %d: NESSUNA implementazione di riferimento lo copre (gap "
               "dichiarato, non confrontabile)\n", label, type);
        return;
    }
    for (i = 0; i < 3; i++) {
        uint64_t first = 0;
        float maxdiff = 0;
        int eq;
        if (!w[i].ok) continue;
        eq = cmp_bufs(mine, w[i].buf, elems, w[i].tag, label, &first, &maxdiff);
        printf("    %s tipo %d vs %s: %s", label, type, w[i].tag, eq ? "bit-exact" : "DIVERSO");
        if (!eq) {
            printf("  max|diff|=%.9g", (double)maxdiff);
            (*bad)++;
        }
        printf("\n");
    }
    /* Testimone contro testimone: se due di loro divergono, il disaccordo NON è nostro da
     * assorbire — è un bug da aprire dove il codice vive. */
    for (i = 0; i < 3; i++) {
        for (j = i + 1; j < 3; j++) {
            uint64_t first = 0;
            float maxdiff = 0;
            if (!w[i].ok || !w[j].ok) continue;
            if (!cmp_bufs(w[i].buf, w[j].buf, elems, "testimone", label, &first, &maxdiff)) {
                printf("    !! I TESTIMONI DIVERGONO tra loro (%s vs %s, tipo %d): bug da "
                       "aprire dove quei codici vivono, non una scelta di questo repo\n",
                       w[i].tag, w[j].tag, type);
                (*bad)++;
            }
        }
    }
}

static void one_tensor(const FmlxGgufFile *f, uint64_t idx, int type, uint64_t elems,
                       uint64_t *bad) {
    const FmlxGgufTensor *t = &f->tensors[idx];
    uint64_t need = 0;
    const uint8_t *bytes;
    char label[160];

    if (fmlx_ggml_type_bytes(type, elems, &need) != FMLX_GGUF_OK) return;
    bytes = fmlx_gguf_tensor_bytes(f, idx, 0, need);
    if (!bytes) {
        printf("    (finestra da %" PRIu64 " byte non disponibile su \"%s\")\n", need, t->name);
        return;
    }
    snprintf(label, sizeof label, "\"%s\"", t->name);
    compare(type, bytes, elems, label, bad);
}

/* Ritorna 0 = nessun bit divergente, 1 = divergenze, 2 = il file non si è nemmeno aperto
 * (un path sbagliato NON è un verdetto sui bit: mischiare i due significati ha già fatto
 * uscire "ESITO: DIVERGENZE" su un argomento che non era un file). */
static int one(const char *path) {
    FmlxGgufFile f;
    uint64_t i, bad = 0;
    int seen[64];
    memset(seen, 0, sizeof seen);
    if (fmlx_gguf_open_header(path, &f) != FMLX_GGUF_OK) {
        printf("%s: testata rifiutata (%s)\n", path, fmlx_gguf_last_error(&f));
        fmlx_gguf_close(&f);
        return 2;
    }
    printf("%s: %" PRIu64 " tensori\n", path, f.n_tensors);
    for (i = 0; i < f.n_tensors; i++) {
        int type = f.tensors[i].ggml_type;
        uint64_t bs, elems;
        if (type < 0 || type >= 64 || seen[type]) continue;
        if (!fmlx_ggml_type_is_covered(type)) continue;
        bs = fmlx_ggml_type(type)->block_vals;
        elems = SAMPLE_ELEMS - (SAMPLE_ELEMS % bs);
        if (elems == 0 || f.tensors[i].numel < elems) continue;
        seen[type] = 1;
        one_tensor(&f, i, type, elems, &bad);
    }
    fmlx_gguf_close(&f);
    return bad ? 1 : 0;
}

/* Ogni tipo coperto, anche su blocchi FINITI: i file locali non contengono tutti i tipi
 * (Q4_0, Q5_K, I8, I16 mancano del tutto), e un confronto differenziale non ha bisogno di
 * dati veri — basta che le due implementazioni leggano gli STESSI byte. Byte pseudo-casuali
 * con seed fisso = deterministico; per gli IQ questo esplora anche gli indici di griglia. */
static int synthetic(void) {
    static const int types[] = { 0, 1, 2, 8, 10, 12, 13, 14, 16, 24, 25, 26, 30 };
    unsigned char bytes[SAMPLE_ELEMS * 4];
    uint64_t bad = 0, seed = 20260922u;
    size_t i;
    printf("synthetic (blocchi pseudo-casuali, seed %" PRIu32 ")\n", (uint32_t)seed);
    for (i = 0; i < sizeof bytes; i++) {
        seed = seed * 1103515245u + 12345u;
        bytes[i] = (unsigned char)(seed >> 16);
    }
    for (i = 0; i < sizeof types / sizeof *types; i++) {
        const FmlxGgmlType *ty = fmlx_ggml_type(types[(int)i]);
        uint64_t bs, elems;
        if (!ty) {
            printf("    tipo %d: assente dalla tabella, il test è sbagliato PRIMA dei dati\n",
                   types[(int)i]);
            bad++;
            continue;
        }
        bs = ty->block_vals;
        elems = SAMPLE_ELEMS - (SAMPLE_ELEMS % bs);
        if (!ty->usable || elems == 0) continue;
        compare(types[(int)i], bytes, elems, "synthetic", &bad);
    }
    return bad ? 1 : 0;
}

/* Audit della REGOLA di copertura, su DUE assi che prima non c'erano.
 *
 * ASSE 1 — i BIT: `fmlx_ggml_type_is_covered` non può mai asserire una copertura che nessun
 * testimone conferma. Il caso opposto (un testimone copre, noi no) NON è un fallimento: è un
 * GAP LISTATO, il lavoro che si può fare restando dentro la regola.
 *
 * ASSE 2 — la FORMA: per OGNI valore di enum, gli elementi per blocco e i byte per riga della
 * ns. tabella devono coincidere con quelli di ggml. È l'asse che ha beccato i cinque tipi
 * spostati di uno: quei tipi non hanno un solo tensore sui file di questa macchina, quindi
 * nessun confronto bit-a-bit li avrebbe mai toccati. */
static int coverage_audit(void) {
    static unsigned char bytes[SAMPLE_ELEMS * 4];
    uint64_t seed = 20260923u;
    size_t i;
    int t, bad = 0, seen = 0, gaps = 0, forms = 0;

    printf("audit di copertura (bit + forma) contro i testimoni A, B, C\n");
    for (i = 0; i < sizeof bytes; i++) {
        seed = seed * 1103515245u + 12345u;
        bytes[i] = (unsigned char)(seed >> 16);
    }
    /* i tipi fuori dalla ns. tabella ma conosciuti da ggml si confrontano lo stesso: è il
     * senso di avere un'autorità per i NUMERI, non solo per i float */
    for (t = 0; t < ggc_type_count(); t++) {
        const FmlxGgmlType *ty = fmlx_ggml_type(t);
        uint64_t n = 2048, need = 0, gg;
        int cbv = ggc_block_vals(t);

        forms++;
        if (ty && cbv && (uint64_t)cbv != ty->block_vals) {
            printf("    tipo %d (%s): elementi per blocco %llu, ggml dice %d — la tabella e "
                   "l'autorità non descrivono lo stesso formato\n", t, ty->name,
                   (unsigned long long)ty->block_vals, cbv);
            bad++;
        }
        if (ty) {
            n -= n % ty->block_vals;
            if (n) (void)fmlx_ggml_type_bytes(t, n, &need);
        } else if (cbv) {
            n -= n % (uint64_t)cbv;
        }
        gg = 0;
        if (ty && n && need) {
            /* si bussa a ggml SOLO per i tipi che anche noi riteniamo ben formati: i ritirati
             * Q4_2/Q4_3 sono un buco nominato qui e un GGML_ASSERT là — una chiamata che
             * abortisce sarebbe un verdetto peggiore di nessun verdetto */
            gg = ggc_row_bytes_any(t, (int64_t)n);
        }
        if (ty && n && gg && need && gg != need) {
            printf("    tipo %d (%s): %" PRIu64 " byte per %" PRIu64 " elementi, ggml ne dice "
                   "%" PRIu64 " — i byte sbagliati spostano la data section di TUTTI i tensori\n",
                   t, ty->name, need, n, gg);
            bad++;
        }
    }
    for (t = 0; t <= FMLX_GGML_TYPE_MAX; t++) {
        const FmlxGgmlType *ty = fmlx_ggml_type(t);
        uint64_t n, need;
        int aok, bok, cok, ours, witnesses;
        if (!ty || !ty->usable) continue;
        n = 2048u - (2048u % ty->block_vals);
        if (n == 0 || fmlx_ggml_type_bytes(t, n, &need) != FMLX_GGUF_OK || need > sizeof bytes) {
            continue;
        }
        aok = gmb_ggml_dequant_row(t, bytes, (int64_t)n, refA) ? 1 : 0;
        bok = (gmb_moe_ggml_dequant_row((uint32_t)t, bytes, refB, (size_t)n) == 0) ? 1 : 0;
        cok = ggc_dequant_row(t, bytes, (int64_t)n, refC);
        ours = (fmlx_ggml_dequant_row(t, bytes, n, mine) == FMLX_GGUF_OK) ? 1 : 0;
        witnesses = aok + bok + cok;
        seen++;
        if (ours && !witnesses) {
            printf("    tipo %d (%s): NOI asseriamo di coprirlo e NESSUN testimone lo fa — "
                   "copertura senza secondo codice\n", t, ty->name);
            bad++;
        }
        if (fmlx_ggml_type_is_covered(t) != ours) {
            printf("    tipo %d (%s): is_covered=%d ma il dequant risponde %d — le due facce "
                   "della stessa affermazione divergono\n", t, ty->name,
                   fmlx_ggml_type_is_covered(t), ours);
            bad++;
        }
        if (!ours && witnesses) {
            printf("    tipo %d (%s): gap COLMABILE, %d testimone/i pronto/i (A=%d B=%d C=%d)\n", t,
                   ty->name, witnesses, aok, bok, cok);
            gaps++;
        }
    }
    printf("    %d tipi confrontati sui bit, %d forme confrontate con ggml, %d incoerenze, "
           "%d gap colmabili\n", seen, forms, bad, gaps);
    return bad ? 1 : 0;
}

int main(int argc, char **argv) {
    int i, div = 0, io = 0;
    if (argc < 2) {
        fprintf(stderr, "uso: %s <--synthetic | <file.gguf> [...]>\n", argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "--synthetic")) {
        if (argc != 2) {
            fprintf(stderr, "--synthetic si usa da solo\n");
            return 2;
        }
        argc = 1; /* nessun file: si passa direttamente al verdetto */
    }
    for (i = 2; i < argc; i++) {
        int r = one(argv[i]);
        if (r == 2) io = 1;
        else div |= r;
    }
    div |= synthetic();
    div |= coverage_audit();
    if (io) {
        printf("ESITO: almeno un file NON è stato aperto — nessun verdetto sui bit "
               "(le righe qui sopra dicono quali)\n");
        return 2;
    }
    printf("%s\n", div ? "ESITO: DIVERGENZE" : "ESITO: nessun bit divergente");
    return div ? 1 : 0;
}
