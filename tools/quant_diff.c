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
 *      src/lane_gguf/fmlx_quant.c src/lane_gguf/fmlx_quant_ext.c \
 *      src/lane_gguf/fmlx_quant_iq.c \
 *      "$G/native/gmb_moe_runtime/src/gmb_moe_ggml.c" \
 *      -L "$L/build/bin" -lggml-base -Wl,-rpath,"$L/build/bin" -o build/quant_diff
 * Uso: build/quant_diff <file.gguf> [...]      oppure   build/quant_diff --synthetic
 *      Il giro include SEMPRE anche `grid_sweep()`: ogni indice di ogni codebook (2048 voci
 *      di iq1s_grid, 1024 di iq2s_grid, ...) battuto uno per uno contro la tabella compilata
 *      nella dylib. È la sola prova che vede una PERMUTAZIONE dello header generato, che
 *      `gen_codebooks.py --check` non vede (confronta le copie sorgente, non il file scritto)
 *      e che nessuna verifica di forma vede (le voci restano distinte e in alfabeto).
 *      exit 0 = nessun ns. bit divergente, 1 = un ns. bit diverge (lavoro di questo repo),
 *      3 = noi esatti e i TESTIMONI in disaccordo tra loro (bug esterno, da aprire dove
 *      quel codice vive — oggi: i due convertitori f16 di GMBasic sugli NaN),
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
uint32_t ggc_f16_bits(uint16_t h);

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

/* I due numeri NON sono uno solo, e tenerli insieme era il difetto: finché il tipo 1
 * (F16) divergeva dai due testimoni di GMBasic, il gate chiudeva 1 e la riga diceva
 * "DIVERGENZE" — un rosso PERMANENTE che assomiglia a un nostro bug e che, a forza di
 * vederlo, smette di significare niente. `ours` è quello che dobbiamo noi; `ext` è il
 * disaccordo tra codici che vivono altrove, da APRIRE come bug dove quel codice sta. */
struct verdict {
    uint64_t ours; /* ns. bit divergono, e nessuna autorità ci dà ragione */
    uint64_t ext;  /* chi sbaglia è un testimone (o due): noi coincidiamo con ggml */
    uint64_t calls;
};

static void compare(int type, const uint8_t *bytes, uint64_t elems, const char *label,
                    int verbose, struct verdict *v) {
    struct witness w[3];
    FmlxGgufStatus s;
    int eq[3];
    int i, j, n_ok = 0;

    v->calls++;
    s = fmlx_ggml_dequant_row(type, bytes, elems, mine);
    if (s != FMLX_GGUF_OK) {
        printf("    %s tipo %d: IL NS. DEQUANT RIFIUTA (%" PRIu64 " elem) -> %s\n", label, type,
               elems, fmlx_gguf_status_str(s));
        v->ours++;
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
        eq[i] = -1;
        if (!w[i].ok) continue;
        eq[i] = cmp_bufs(mine, w[i].buf, elems, w[i].tag, label, &first, &maxdiff) ? 1 : 0;
        if (verbose || !eq[i]) {
            printf("    %s tipo %d vs %s: %s", label, type, w[i].tag,
                   eq[i] ? "bit-exact" : "DIVERSO");
            if (!eq[i]) printf("  max|diff|=%.9g", (double)maxdiff);
            printf("\n");
        }
    }
    /* Classificazione: C è l'autorità (è il codice che i GGUF li SCRIVE). Se i ns. bit
     * coincidono con C e un testimone secondario dice altro, l'outlier è LUI. Se C non
     * copre il tipo, la divergenza resta nostra: non abbiamo modo di assolverci. */
    for (i = 0; i < 2; i++) {
        if (eq[i] != 0) continue;
        if (eq[2] == 1) v->ext++;
        else v->ours++;
    }
    if (eq[2] == 0) v->ours++;
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
                v->ext++;
            }
        }
    }
}

static void one_tensor(const FmlxGgufFile *f, uint64_t idx, int type, uint64_t elems,
                       struct verdict *v) {
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
    compare(type, bytes, elems, label, 1, v);
}

/* Ritorna 0 = nulla di nostro diverge, 2 = il file non si è nemmeno aperto (un path
 * sbagliato NON è un verdetto sui bit: mischiare i due significati ha già fatto uscire
 * "ESITO: DIVERGENZE" su un argomento che non era un file). */
static int one(const char *path, struct verdict *v) {
    FmlxGgufFile f;
    uint64_t i, before = v->ours;
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
        one_tensor(&f, i, type, elems, v);
    }
    fmlx_gguf_close(&f);
    return v->ours > before ? 1 : 0;
}

/* Ogni tipo coperto, anche su blocchi FINITI: i file locali non contengono tutti i tipi
 * (Q4_0, Q5_K, I8, I16 mancano del tutto), e un confronto differenziale non ha bisogno di
 * dati veri — basta che le due implementazioni leggano gli STESSI byte. Byte pseudo-casuali
 * con seed fisso = deterministico; per gli IQ questo esplora anche gli indici di griglia,
 * MA NON LI COPRE TUTTI: per quelli c'è grid_sweep() qui sotto. */
static void synthetic(struct verdict *v) {
    unsigned char bytes[SAMPLE_ELEMS * 4];
    uint64_t seed = 20260922u;
    size_t i;
    int t;
    printf("synthetic (blocchi pseudo-casuali, seed %" PRIu32 ")\n", (uint32_t)seed);
    for (i = 0; i < sizeof bytes; i++) {
        seed = seed * 1103515245u + 12345u;
        bytes[i] = (unsigned char)(seed >> 16);
    }
    /* La lista dei tipi ERA un `static const int types[]` mantenuto a mano, e ha già
     * morso: aggiungendo IQ4_NL/IQ4_XS/MXFP4/NVFP4 alla tabella e alla copertura, questo
     * giro li ha saltati in silenzio — l'ASSE 2 (forma, derivato dalla tabella) li contava
     * e l'asse 1 (bit) no. Un elenco di verità scritto a mano accanto alla tabella che
     * dovrebbe sostituirsi è un SECONDO posto dove divergere: ora si itera la tabella. */
    for (t = 0; t <= FMLX_GGML_TYPE_MAX; t++) {
        const FmlxGgmlType *ty = fmlx_ggml_type(t);
        uint64_t bs, elems;
        if (!ty || !ty->usable) continue;
        if (!fmlx_ggml_type_is_covered(t)) continue;   /* non coperto: niente da confrontare */
        bs = ty->block_vals;
        elems = SAMPLE_ELEMS - (SAMPLE_ELEMS % bs);
        if (elems == 0) continue;
        compare(t, bytes, elems, "synthetic", 1, v);
    }
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
static int coverage_audit(struct verdict *v) {
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
    v->ours += (uint64_t)bad; /* un'incoerenza di copertura è nostra, non di un testimone */
    return bad ? 1 : 0;
}

/* Il convertitore f16->f32 è USATO DA OGNI dequant (ogni `d` è un half), e i test sui blocchi
 * lo coprono solo per i pattern che capitano. Qui si girano TUTTI I 65536 pattern e si
 * confrontano i BIT del risultato con quelli di ggml: un convertitore che diverge sugli NaN
 * darebbe risultati "nan vs nan" ovunque e in nessun posto — esattamente il tipo di difetto
 * che un soffitto verde non vede. */
static void f16_exhaustive(struct verdict *v) {
    uint32_t h, bad = 0;
    int shown = 0;
    for (h = 0; h < 65536u; h++) {
        uint16_t s = (uint16_t)h;
        uint32_t mine_bits, gg_bits;
        float f = fmlx_ggml_f16_to_f32(s);
        memcpy(&mine_bits, &f, 4);
        gg_bits = ggc_f16_bits(s);
        if (mine_bits != gg_bits) {
            bad++;
            if (shown < 6) {
                printf("    f16 0x%04x: nostri 0x%08x, ggml 0x%08x\n", s, mine_bits, gg_bits);
                shown++;
            }
        }
    }
    printf("f16->f32 esaustivo: 65536 pattern, %u bit-diversi%s\n", bad,
           bad ? " (mostrati i primi 6)" : "");
    v->ours += bad;
}

/* ================= grid_sweep: OGNI indice di griglia, uno per uno =================
 *
 * Perché esiste, ed è la parte CHE NON SI PUÒ FARE CON I BYTE CASUALI: le tabelle di
 * `fmlx_iqgrid_tables.h` e `fmlx_iq1s_tables.h` sono codebook ALLENATI — non c'è una
 * specifica da ricalcolare, e i due testi ggml che le contano sono due VERSIONI dello
 * STESSO autore, non due autori. Il generatore (tools/gen_codebooks.py) può solo dire che
 * concordano; qui si esegue il testimone che vale: la copia COMPILATA dentro
 * libggml-base, quella che i GGUF li ha scritti.
 *
 * Il seed pseudo-casuale del giro `synthetic` copre una frazione degli indici e non dice
 * QUANTO: su 2048 voci di `iq1s_grid` ne aveva toccate qualche centinaio. Lo sweep invece
 * costruisce un blocco per volta in cui i CAMPI DI INDICE enumerano la tabella, e i campi
 * SEGNO/moltiplicatore girano con loro — così ogni voce è letta almeno una volta, e ogni
 * bit di `ksigns` è applicato almeno una volta.
 *
 * `d` è 1.0f (0x3c00) e le scale sono nibble esatti: i prodotti restano numeri binari
 * esatti, quindi una voce di tabella diversa di UNO cambia i BIT del risultato — non un
 * "quasi uguale" che un confronto a tolleranza ingoierebbe.
 *
 * Uscita: una riga di conto per tabella. Le divergenze, se ci sono, le stampa `compare()`
 * in modalità quieta (che è il solo motivo per cui gli sweep non inondano il terminale). */

/* Scrive `width` bit di `val` a partire da `bitpos` in un campo little-endian, ORandoli:
 * stesso ordine di bit con cui i dequant leggono (`ld32` + shift su little-endian). */
static void put_bits(uint8_t *p, uint32_t bitpos, uint32_t width, uint32_t val) {
    uint32_t i;
    for (i = 0; i < width; i++) {
        if (val & (1u << i)) {
            p[(bitpos + i) / 8] |= (uint8_t)(1u << ((bitpos + i) % 8));
        }
    }
}

/* Ogni builder riempie UN BLOCCO con gli indici base, base+1, ... e ritorna quanti ne
 * ha scritti (0 = non ha scritto nulla, e il pilota lo tratta come errore, non come "fatto").
 * I campi che NON sono l'indice (segni, scale, moltiplicatori, segno del delta) girano con
 * la base, perché la prova che interessa include anche l'aritmetica che li usa. */

static uint32_t sw_iq2_xxs(uint8_t *b, uint32_t base, uint32_t n_grid) {
    uint32_t k;
    memset(b, 0, 66);
    b[0] = 0x00; b[1] = 0x3c;                         /* d = 1.0f */
    for (k = 0; k < 32 && base + k < n_grid; k++) {   /* 8 gruppi da 4 indici da 8 bit */
        uint32_t ib = k / 4, li = k % 4;
        b[2 + 8 * ib + li] = (uint8_t)(base + k);
        put_bits(b + 2 + 8 * ib + 4, 7 * li, 7, (base + k) & 127u); /* indice dei segni */
        if (li == 0) put_bits(b + 2 + 8 * ib + 4, 28, 4, base & 15u); /* ls, una per gruppo */
    }
    return k;
}

static uint32_t sw_iq2_xs(uint8_t *b, uint32_t base, uint32_t n_grid) {
    uint32_t k;
    memset(b, 0, 74);
    b[0] = 0x00; b[1] = 0x3c;
    for (k = 0; k < 8; k++) b[66 + k] = (uint8_t)(base + k); /* due scale per byte */
    for (k = 0; k < 32 && base + k < n_grid; k++) {          /* 9 bit griglia + 7 di segno */
        uint32_t q = (base + k) & 511u, sg = (base + k) & 127u, v = q | (sg << 9);
        b[2 + 2 * k] = (uint8_t)v;
        b[3 + 2 * k] = (uint8_t)(v >> 8);
    }
    return k;
}

static uint32_t sw_iq3_xxs(uint8_t *b, uint32_t base, uint32_t n_grid) {
    uint32_t k;
    memset(b, 0, 98);
    b[0] = 0x00; b[1] = 0x3c;
    for (k = 0; k < 64 && base + k < n_grid; k++) {   /* 64 indici da 8 bit per blocco */
        uint32_t ib = k / 8, l = k % 8;
        b[2 + k] = (uint8_t)(base + k);
        put_bits(b + 66 + 4 * ib, 7 * (l / 2), 7, (base + k) & 127u);
        if (l == 0) put_bits(b + 66 + 4 * ib, 28, 4, base & 15u);
    }
    return k;
}

/* IQ3_S: l'indice è 8 bit in `qs` + UN bit in `qh`, e il bit di `qh` che corrisponde a
 * ciascuna delle 64 posizioni è 2*l per la griglia 1 e 2*l+1 per la griglia 2 (vengono dai
 * due shift diversi `(qh << (8-2l))` e `(qh << (7-2l))` di ggml-quants.c:2551). */
static uint32_t sw_iq3_s(uint8_t *b, uint32_t base, uint32_t n_grid) {
    uint32_t s;
    memset(b, 0, 110);
    b[0] = 0x00; b[1] = 0x3c;
    for (s = 0; s < 32; s++) b[74 + s] = (uint8_t)(base + s);   /* segni */
    for (s = 0; s < 4; s++) b[106 + s] = (uint8_t)(base + s);   /* scale (due nibble) */
    for (s = 0; s < 64 && base + s < n_grid; s++) {
        uint32_t h = s / 16, half = (s % 16) / 8, rem = s % 8, x = base + s;
        b[2 + 16 * h + 8 * half + rem] = (uint8_t)(x & 0xFFu);
        put_bits(b + 66 + 2 * h + half, 2 * (rem / 2) + (rem % 2), 1, (x >> 8) & 1u);
    }
    return s;
}

/* IQ2_S: 10 bit per indice (griglia da 1024), e i due bit alti stanno in `qh[ib]` alle
 * posizioni 2l e 2l+1 — lo shift dipendente da l di ggml-quants.c:2489. */
static uint32_t sw_iq2_s(uint8_t *b, uint32_t base, uint32_t n_grid) {
    uint32_t s;
    memset(b, 0, 82);
    b[0] = 0x00; b[1] = 0x3c;
    for (s = 0; s < 32; s++) b[34 + s] = (uint8_t)(base + s);   /* segni (dentro qs, in ggml) */
    for (s = 0; s < 8; s++) b[74 + s] = (uint8_t)(base + s);   /* scale */
    for (s = 0; s < 32 && base + s < n_grid; s++) {
        uint32_t ib = s / 4, l = s % 4, x = base + s;
        b[2 + s] = (uint8_t)(x & 0xFFu);
        put_bits(b + 66 + ib, 2 * l, 2, (x >> 8) & 3u);
    }
    return s;
}

/* IQ1_S: 11 bit per indice (2048 voci), tre per volta in `qh[ib]` a 3*l; i bit 12..14
 * scelgono il moltiplicatore e il 15 il segno del delta, e anche quelli girano. */
static uint32_t sw_iq1_s(uint8_t *b, uint32_t base, uint32_t n_grid) {
    uint32_t s;
    memset(b, 0, 50);
    b[0] = 0x00; b[1] = 0x3c;
    for (s = 0; s < 8; s++) {
        put_bits(b + 34 + 2 * s, 12, 3, (base + s) & 7u);         /* moltiplicatore */
        put_bits(b + 34 + 2 * s, 15, 1, ((base + s) >> 3) & 1u);  /* segno del delta */
    }
    for (s = 0; s < 32 && base + s < n_grid; s++) {
        uint32_t ib = s / 4, l = s % 4, x = base + s;
        b[2 + s] = (uint8_t)(x & 0xFFu);
        put_bits(b + 34 + 2 * ib, 3 * l, 3, (x >> 8) & 7u);
    }
    return s;
}

/* IQ1_M: nessun `d` nel blocco — la scala è ricomposta da quattro half e qui va FORZATA a
 * 1.0f, perché i nibble che la compongono sono gli STESSI bit (12..15 di ogni parola) che
 * il builder usa per variare: senza questo vincolo `d` diventerebbe un NaN a caso e lo
 * sweep misurerebbe NaN contro NaN. 0x3c00 vuole 0,0,C,3 in quei quattro nibble. */
static uint32_t sw_iq1_m(uint8_t *b, uint32_t base, uint32_t n_grid) {
    uint32_t s;
    memset(b, 0, 56);
    for (s = 0; s < 8; s++) b[48 + s] = (uint8_t)(base + s); /* scale/moltiplicatori */
    b[49] &= 0x0F; b[51] &= 0x0F;
    b[53] = (uint8_t)(0xC0 | (b[53] & 0x0Fu));
    b[55] = (uint8_t)(0x30 | (b[55] & 0x0Fu));
    for (s = 0; s < 32 && base + s < n_grid; s++) {
        uint32_t ib = s / 4, l = s % 4, x = base + s;
        b[s] = (uint8_t)(x & 0xFFu);
        put_bits(b + 32 + 2 * ib + l / 2, 4 * (l % 2), 3, (x >> 8) & 7u);       /* indice */
        put_bits(b + 32 + 2 * ib + l / 2, 4 * (l % 2) + 3, 1, (x >> 11) & 1u);  /* delta */
    }
    return s;
}

/* kvalues_iq4nl: sedici voci, due nibble per byte. Un blocco le copre TUTTE in entrambe le
 * posizioni (la codebook NON è simmetrica, -127..+113: basso e alto non sono lo stesso
 * giro), quindi la base non avanza e il giro finisce al primo blocco. */
static uint32_t sw_iq4_nl(uint8_t *b, uint32_t base, uint32_t n_grid) {
    uint32_t j;
    (void)base;
    memset(b, 0, 18);
    b[0] = 0x00; b[1] = 0x3c;                    /* d = 1.0f */
    for (j = 0; j < 16; j++) b[2 + j] = (uint8_t)((j & 15u) | (((j + 8) & 15u) << 4));
    return n_grid;
}

/* IQ4_XS: otto sottoblocchi da 32 con scala propria (6 bit spaccati tra scales_h e
 * scales_l). Le 128 byte di `qs` coprono i 16 valori otto volte, e le scale girano. */
static uint32_t sw_iq4_xs(uint8_t *b, uint32_t base, uint32_t n_grid) {
    uint32_t j;
    (void)base;
    memset(b, 0, 136);
    b[0] = 0x00; b[1] = 0x3c;
    for (j = 0; j < 128; j++) b[8 + j] = (uint8_t)((j & 15u) | (((j + 8) & 15u) << 4));
    b[2] = 0x05; b[4] = 0x97;                    /* scales_h / scales_l: non tutte uguali */
    return n_grid;
}

static uint32_t sw_mxfp4(uint8_t *b, uint32_t base, uint32_t n_grid) {
    uint32_t j;
    (void)base;
    memset(b, 0, 36);
    b[0] = 127;                                  /* E8M0: 2^0, che è mezzo per convenzione */
    for (j = 0; j < 16; j++) b[1 + j] = (uint8_t)((j & 15u) | (((j + 8) & 15u) << 4));
    return n_grid;
}

static uint32_t sw_nvfp4(uint8_t *b, uint32_t base, uint32_t n_grid) {
    static const uint8_t sc[4] = {0x38, 0x39, 0x20, 0x21};
    uint32_t j;
    (void)base;
    memset(b, 0, 36);
    /* Quattro scale UE4M3 NON degenere: i due codici degeneri (0 e 0x7F, che valgono
     * entrambi 0.0f) azzererebbero un sottoblocco intero e lo sweep non misurerebbe
     * nulla su quei 16 nibble; sono coperti dal giro synthetic sui byte casuali. */
    for (j = 0; j < 4; j++) b[j] = sc[j];
    for (j = 0; j < 32; j++) b[4 + j] = (uint8_t)((j & 15u) | (((j + 8) & 15u) << 4));
    return n_grid;
}

struct sweep {
    int type;
    const char *grid;
    uint32_t n_grid;
    uint32_t block_bytes;
    uint32_t elems_per_block;
    uint32_t (*fill)(uint8_t *blk, uint32_t base, uint32_t n_grid);
};

static const struct sweep sweeps[] = {
    {16, "iq2xxs_grid",   256,  66, 256, sw_iq2_xxs},
    {17, "iq2xs_grid",    512,  74, 256, sw_iq2_xs},
    {18, "iq3xxs_grid",   256,  98, 256, sw_iq3_xxs},
    {21, "iq3s_grid",     512, 110, 256, sw_iq3_s},
    {22, "iq2s_grid",    1024,  82, 256, sw_iq2_s},
    {19, "iq1s_grid",    2048,  50, 256, sw_iq1_s},
    {29, "iq1s_grid",    2048,  56, 256, sw_iq1_m},
    {20, "kvalues_iq4nl",  16,  18,  32, sw_iq4_nl},
    {23, "kvalues_iq4nl",  16, 136, 256, sw_iq4_xs},
    {39, "kvalues_fp4",    16,  17,  32, sw_mxfp4},
    {40, "kvalues_fp4",    16,  36,  64, sw_nvfp4},
};

static void grid_sweep(struct verdict *v) {
    size_t si;
    printf("sweep delle griglie (OGNI indice di codebook contro la tabella compilata in ggml)\n");
    for (si = 0; si < sizeof sweeps / sizeof *sweeps; si++) {
        const struct sweep *s = &sweeps[si];
        uint64_t before = v->ours + v->ext;
        uint32_t base = 0, written = 0, blocks = 0;
        unsigned char blk[136];
        if (s->block_bytes > sizeof blk) {
            printf("    %s: blocco da %" PRIu32 " byte troppo grande per il buffer da %zu: "
                   "sweep NON eseguito\n", s->grid, s->block_bytes, sizeof blk);
            v->ours++;
            continue;
        }
        while (base < s->n_grid) {
            uint32_t wrote;
            memset(blk, 0, sizeof blk);
            wrote = s->fill(blk, base, s->n_grid);
            if (wrote == 0) {
                printf("    %s: il builder ha scritto ZERO indici alla base %" PRIu32
                       " — sweep interrotto (un giro muto non è un passaggio)\n", s->grid, base);
                v->ours++;
                break;
            }
            if (wrote > s->n_grid - base) {
                printf("    %s: il builder ha scritto %" PRIu32 " indici oltre le %" PRIu32
                       " voci a base %" PRIu32 " — sweep interrotto\n", s->grid, wrote,
                       s->n_grid - base, base);
                v->ours++;
                break;
            }
            compare(s->type, blk, s->elems_per_block, s->grid, 0, v);
            base += wrote;
            written += wrote;
            blocks++;
        }
        printf("    %-16s tipo %2d: %" PRIu32 "/%" PRIu32 " indici battuti in %" PRIu32
               " blocchi, %s\n", s->grid, s->type, written, s->n_grid, blocks,
               (v->ours + v->ext) == before ? "nessun bit divergente" : "DIVERGENZE SOPRA");
    }
}

int main(int argc, char **argv) {
    struct verdict v;
    int i, io = 0;
    memset(&v, 0, sizeof v);
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
        if (one(argv[i], &v) == 2) io = 1;
    }
    synthetic(&v);
    grid_sweep(&v);
    f16_exhaustive(&v);
    coverage_audit(&v);
    if (io) {
        printf("ESITO: almeno un file NON è stato aperto — nessun verdetto sui bit "
               "(le righe qui sopra dicono quali)\n");
        return 2;
    }
    if (v.ours) {
        printf("ESITO: %" PRIu64 " DIVERGENZE NOSTRE su %" PRIu64
               " confronti — lavoro di questo repo\n", v.ours, v.calls);
        return 1;
    }
    if (v.ext) {
        printf("ESITO: nessun ns. bit divergente su %" PRIu64
               " confronti, ma %" PRIu64 " disaccordi TRA TESTIMONI (A/B di GMBasic contro "
               "ggml): bug da aprire nel vault GMBasic, non qui. Chiuso 3 = VERDE PER NOI.\n",
               v.calls, v.ext);
        return 3;
    }
    printf("ESITO: nessun bit divergente (%" PRIu64 " confronti)\n", v.calls);
    return 0;
}
