/* test_engine.c — l'ABI di MLXEngine, provata da fuori.
 *
 * Questo file si comporta come un chiamante che ha SOLO `mlxengine.h` e la dylib: non guarda
 * mai dentro `FmlxGgufFile` se non in un punto, e quel punto è una scelta — aprire lo stesso
 * file due volte, una attraverso il confine e una no, e confrontare i BIT. È l'unico modo di
 * provare che l'ABI non aggiunge e non perde niente: le asserzioni sui valori «calcolati a
 * mano dal formato» provano la correttezza, questo prova lafedeltà del confine.
 *
 * Le tre cose che un'ABI deve tenere ferme, e che qui si misurano:
 *   1. i codici d'errore sono DISTINTI e ciascuno arriva per la sua causa — mai uno al posto
 *      di un altro (è la ragione per cui il lettore ha diciannove codici e non uno);
 *   2. gli argomenti nulli e gli indici fuori range non toccano la memoria di nessuno;
 *   3. `at` è in ELEMENTI e le finestre non materializzano ciò che non serve.
 *
 * Nessun file reale, nessuna rete: la fixture è un GGUF v3 con due tensori (F32 e Q8_0) e i
 * valori attesi vengono dal FORMATO (scala f16 × indici int8), non chiesti in prestito
 * all'implementazione che si vuole provare.
 */
#include "mlxengine.h"

#include "fmlx/fmlx_gguf.h"
#include "fmlx/fmlx_quant.h"

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

static const char *tmp_dir(void) {
    const char *d = getenv("TMPDIR");
    return (d && *d) ? d : "/tmp";
}

static char fixture_path[512];
static const char *fx(const char *tag) {
    snprintf(fixture_path, sizeof fixture_path, "%s/fmlx_engine_%s.gguf", tmp_dir(), tag);
    return fixture_path;
}

/* ---- la fixture: due tensori, e gli attesi scritti a mano ----------------------------- */

#define NF32 8
static const float f32_vals[NF32] = { 1.5f, -2.25f, 0.0f, 1.0f, 1000.0f, -0.5f, 3.5f, 42.0f };

#define NQ 96 /* tre blocchi da 32 */
#define Q8_BLOCK_BYTES 34u
/* Scale dei tre blocchi come bit pattern f16: 1,0 · 2,0 · 0,5. */
static const uint16_t q8_scales[3] = { 0x3c00u, 0x4000u, 0x3800u };
static float q8_expected[NQ];

/* Q8_0: blocco da 34 byte = scala f16 + 32 int8. I byte e l'atteso si costruiscono nello
 * stesso punto, così un errore di conto resta visibile su entrambe le sponde. */
static size_t build_q8(uint8_t *out) {
    int b, i;
    size_t n = 0;
    for (b = 0; b < 3; b++) {
        uint16_t h = q8_scales[b];
        float s = fmlx_ggml_f16_to_f32(h);
        out[n++] = (uint8_t)(h & 0xffu);
        out[n++] = (uint8_t)(h >> 8);
        for (i = 0; i < 32; i++) {
            int8_t q = (int8_t)(i - 16);
            out[n++] = (uint8_t)q;
            q8_expected[b * 32 + i] = s * (float)q;
        }
    }
    return n;
}

static char arch_str[] = "mlxengine-test";

static int write_engine_fixture(void) {
    FmlxGgufKV kv[3];
    FmlxGgufTensor t[2];
    uint8_t *blob, *p;
    size_t qn;
    uint64_t f32_bytes = (uint64_t)NF32 * sizeof(float);
    FmlxGgufStatus st;

    blob = (uint8_t *)malloc(f32_bytes + 3u * Q8_BLOCK_BYTES);
    if (!blob) { CHECK(0, "malloc fixture"); return 0; }
    memcpy(p = blob, f32_vals, f32_bytes);
    qn = build_q8(p + f32_bytes);

    memset(kv, 0, sizeof kv);
    kv[0].key = (char *)"general.architecture";
    kv[0].type = 8 /* STRING */;
    kv[0].s = arch_str;
    kv[1].key = (char *)"general.alignment";
    kv[1].type = 4 /* U32 */;
    kv[1].u = 32;
    kv[2].key = (char *)"test.block_count";
    kv[2].type = 4;
    kv[2].u = 4;

    memset(t, 0, sizeof t);
    t[0].name = (char *)"blk.0.w";
    t[0].numel = NF32;
    t[0].ggml_type = 0; /* F32 */
    t[1].name = (char *)"blk.0.q";
    t[1].numel = NQ;
    t[1].ggml_type = 8; /* Q8_0 */

    st = fmlx_gguf_write_fixture(fx("abi"), 3, 32, kv, 3, t, 2, blob, f32_bytes + qn);
    free(blob);
    CHECK(st == FMLX_GGUF_OK, "scrittura fixture: %s", fmlx_gguf_status_str(st));
    return st == FMLX_GGUF_OK;
}

/* ---- 1. identificazione e codici ------------------------------------------------------ */

static void case_identity(void) {
    static const int call_codes[5] = { MLXENGINE_ERR_NULL_ARG, MLXENGINE_ERR_INDEX,
                                       MLXENGINE_ERR_WINDOW, MLXENGINE_ERR_MISALIGNED,
                                       MLXENGINE_ERR_NO_KEY };
    int i, una_dice_ok = 0;
    CHECK(mlxengine_abi_version() == 1u, "abi version = %u", mlxengine_abi_version());
    CHECK(mlxengine_version_string() && *mlxengine_version_string(), "versione vuota");
    CHECK(strcmp(mlxengine_status_str(0), "ok") == 0, "status_str(0) = \"%s\"",
          mlxengine_status_str(0));
    /* Ogni codice d'ABI ha un testo SUO, e nessuno di loro dice «ok»: se due cause
     * condividessero la frase, il log non raccontarebbe più quale delle due è accaduta. */
    for (i = 0; i < 5; i++) {
        const char *s = mlxengine_status_str(call_codes[i]);
        CHECK(s && *s && strcmp(s, "codice di errore sconosciuto") != 0,
              "codice d'ABI %d senza testo proprio", call_codes[i]);
        if (s && strcmp(s, "ok") == 0) una_dice_ok = 1;
    }
    CHECK(!una_dice_ok, "un codice d'errore dice \"ok\"");
    /* I codici del FORMATO restano leggibili attraverso l'ABI: 20 è NOT_COVERED del lettore. */
    CHECK(strstr(mlxengine_status_str(20), "coperto") != NULL, "codice del formato perduto: \"%s\"",
          mlxengine_status_str(20));
    CHECK(mlxengine_status_str(9999) != NULL, "un codice inventato ritorna NULL");
}

/* ---- 2. argomenti sbagliati, prima ancora di aprire ----------------------------------- */

static void case_bad_args(void) {
    MlxEngineModel *m = (MlxEngineModel *)0x1; /* sentinella: deve restare intatta */
    uint64_t dims[MLXENGINE_MAX_DIMS];
    float dst[4];

    CHECK(mlxengine_model_open(NULL, &m) == MLXENGINE_ERR_NULL_ARG, "path NULL non rifiutato");
    CHECK(mlxengine_model_open(fx("abi"), NULL) == MLXENGINE_ERR_NULL_ARG,
          "out NULL non rifiutato");
    CHECK(m == (MlxEngineModel *)0x1, "model_open ha toccato *out pur fallendo");

    /* Handle NULL: le domande a cui si può rispondere rispondono, le altre rifiutano. Nessuna
     * di queste chiamate deve leggere memoria. */
    CHECK(mlxengine_model_n_tensors(NULL) == 0, "n_tensors(NULL) != 0");
    CHECK(mlxengine_model_n_kv(NULL) == 0, "n_kv(NULL) != 0");
    CHECK(mlxengine_model_alignment(NULL) == 0, "alignment(NULL) != 0");
    CHECK(mlxengine_tensor_name(NULL, 0) == NULL, "tensor_name(NULL) != NULL");
    CHECK(mlxengine_tensor_type(NULL, 0) == -1, "tensor_type(NULL) != -1");
    CHECK(mlxengine_tensor_numel(NULL, 3) == 0, "tensor_numel(NULL) != 0");
    CHECK(mlxengine_tensor_bytes(NULL, 3) == 0, "tensor_bytes(NULL) != 0");
    CHECK(mlxengine_tensor_is_covered(NULL, 3) == 0, "is_covered(NULL) != 0");
    CHECK(mlxengine_tensor_block_vals(NULL, 3) == 0, "block_vals(NULL) != 0");
    CHECK(mlxengine_tensor_find(NULL, "blk.0.w") == -1, "tensor_find(NULL) != -1");
    CHECK(mlxengine_model_kv_str(NULL, "x", NULL) == MLXENGINE_ERR_NULL_ARG, "kv_str(NULL)");
    CHECK(mlxengine_model_kv_u64(NULL, "x", NULL) == MLXENGINE_ERR_NULL_ARG, "kv_u64(NULL)");
    CHECK(mlxengine_tensor_dims(NULL, 0, dims) == MLXENGINE_ERR_NULL_ARG, "dims(NULL)");
    CHECK(mlxengine_tensor_dequant(NULL, 0, 0, 1, dst, sizeof dst) == MLXENGINE_ERR_NULL_ARG,
          "dequant(NULL)");
    mlxengine_model_close(NULL); /* dev'essere legale, non un crash */
    CHECK(mlxengine_model_last_error(NULL) != NULL, "last_error(NULL) deve dire qualcosa");
}

/* ---- 4. il cammino caldo: valori dal formato, fedeltà del confine, quattro rifiuti ---- */

static void case_dequant(MlxEngineModel *m, const char *path) {
    float via_abi[NQ], via_int[NQ], small[8];
    uint64_t nb = 0;
    int i;
    MlxEngineStatus st;

    /* F32: valori esatti. È il modo più diretto di accorgersi che l'ABI scambia i tensori o
     * legge dall'offset sbagliato. */
    memset(via_abi, 0x5a, sizeof via_abi);
    st = mlxengine_tensor_dequant(m, 0, 0, NF32, via_abi, sizeof via_abi);
    CHECK(st == FMLX_GGUF_OK, "dequant F32: %s", mlxengine_status_str(st));
    for (i = 0; i < NF32; i++)
        CHECK(memcmp(&via_abi[i], &f32_vals[i], sizeof(float)) == 0, "F32[%d] = %g, atteso %g", i,
              via_abi[i], f32_vals[i]);

    /* Una finestra in mezzo al tensore: `at` conta gli ELEMENTI, non i byte. Se l'ABI
     * confondesse le due unità, il primo valore sarebbe il quarto float quattro volte. */
    st = mlxengine_tensor_dequant(m, 0, 4, 2, via_abi, 2 * sizeof(float));
    CHECK(st == FMLX_GGUF_OK && via_abi[0] == 1000.0f && via_abi[1] == -0.5f,
          "finestra F32 at=4: %g,%g (%s)", via_abi[0], via_abi[1], mlxengine_status_str(st));

    /* Q8_0, tutto il tensore: l'atteso viene dal formato, blocco per blocco. */
    st = mlxengine_tensor_dequant(m, 1, 0, NQ, via_abi, sizeof via_abi);
    CHECK(st == FMLX_GGUF_OK, "dequant Q8_0 totale: %s", mlxengine_status_str(st));
    for (i = 0; i < NQ; i++)
        CHECK(via_abi[i] == q8_expected[i], "Q8_0[%d] = %.9g, atteso %.9g", i, via_abi[i],
              q8_expected[i]);

    /* E qui la fedeltà del confine: lo STESSO file aperto dalla parte di dentro, la finestra
     * letta come byte crudi (34 byte per blocco, il blocco 1 comincia a byte 34) e dequantizzata
     * dalla funzione interna. Due strade, stessi bit — o l'ABI mente su qualcosa. */
    CHECK(fmlx_ggml_type_bytes(8, NQ, &nb) == FMLX_GGUF_OK && nb == 3u * Q8_BLOCK_BYTES,
          "byte Q8_0 per %d elementi: %llu", NQ, (unsigned long long)nb);
    {
        FmlxGgufFile raw;
        if (fmlx_gguf_open_header(path, &raw) != FMLX_GGUF_OK) {
            CHECK(0, "apertura interna della stessa fixture: %s", fmlx_gguf_last_error(&raw));
            fmlx_gguf_close(&raw);
        } else {
            const uint8_t *win = fmlx_gguf_tensor_bytes(&raw, 1, Q8_BLOCK_BYTES, Q8_BLOCK_BYTES);
            if (!win) {
                CHECK(0, "la vista interna sulla finestra è NULL");
            } else {
                CHECK(fmlx_ggml_dequant_row(8, win, 32, via_int) == FMLX_GGUF_OK,
                      "dequant interno della finestra fallito");
                memset(via_abi, 0, 32 * sizeof(float));
                st = mlxengine_tensor_dequant(m, 1, 32, 32, via_abi, sizeof via_abi);
                CHECK(st == FMLX_GGUF_OK, "dequant ABI della finestra: %s",
                      mlxengine_status_str(st));
                CHECK(memcmp(via_abi, via_int, 32 * sizeof(float)) == 0,
                      "l'ABI non dà gli stessi bit della via interna");
                CHECK(memcmp(via_abi, &q8_expected[32], 32 * sizeof(float)) == 0,
                      "la finestra at=32 non coincide col blocco 1");
            }
            fmlx_gguf_close(&raw);
        }
    }

    /* I quattro rifiuti, ognuno col SUO codice. */
    CHECK(mlxengine_tensor_dequant(m, 1, 8, 32, via_abi, sizeof via_abi) ==
              MLXENGINE_ERR_MISALIGNED, "at dentro un blocco: non è ERR_MISALIGNED");
    CHECK(mlxengine_tensor_dequant(m, 1, 0, 16, via_abi, sizeof via_abi) ==
              FMLX_GGUF_ERR_NUMEL_BLOCK, "n metà blocco: non è ERR_NUMEL_BLOCK");
    CHECK(mlxengine_tensor_dequant(m, 1, 64, 64, via_abi, sizeof via_abi) == MLXENGINE_ERR_WINDOW,
          "finestra oltre la fine: non è ERR_WINDOW");

    /* E qui `block_vals` guadagna il suo posto nell'ABI: una richiesta costruita SOLO con il
     * numero che l'ABI stesso restituisce deve essere accettata, e mezza no. Se un giorno il
     * numero esposto e il rifiuto interno divergessero, questo è il punto che grida — un
     * chiamante esterno non ha altro modo di accorgersene. */
    {
        uint64_t bv = mlxengine_tensor_block_vals(m, 1);
        CHECK(bv == 32, "block_vals(1) = %llu, atteso 32", (unsigned long long)bv);
        memset(via_abi, 0x5a, sizeof via_abi);
        st = mlxengine_tensor_dequant(m, 1, 0, 2 * bv, via_abi, sizeof via_abi);
        CHECK(st == FMLX_GGUF_OK, "2 blocchi chiesti da block_vals: %s",
              mlxengine_status_str(st));
        CHECK(mlxengine_tensor_dequant(m, 1, 0, bv / 2, via_abi, sizeof via_abi) ==
                  FMLX_GGUF_ERR_NUMEL_BLOCK, "metà del blocco esposto non rifiutato");
        CHECK(mlxengine_tensor_dequant(m, 1, bv, bv, via_abi, sizeof via_abi) == FMLX_GGUF_OK,
              "finestra a bordo del blocco esposto non accettata");
    }

    memset(small, 0, sizeof small);
    CHECK(mlxengine_tensor_dequant(m, 1, 0, 32, small, sizeof small) == FMLX_GGUF_ERR_DST_SMALL,
          "32 float chiesti in uno spazio da 8: non è ERR_DST_SMALL");
    for (i = 0; i < 8; i++) CHECK(small[i] == 0.0f, "il rifiuto ha scritto in dst");
    /* `n = 0` non è un errore, e non deve toccare dst. */
    memset(small, 0x33, sizeof small);
    CHECK(mlxengine_tensor_dequant(m, 1, 0, 0, small, 0) == FMLX_GGUF_OK, "n=0 rifiutato");
    for (i = 0; i < 8; i++)
        CHECK(((uint8_t *)small)[i] == 0x33, "n=0 ha scritto in dst");
}

/* ---- 3. il modello vero --------------------------------------------------------------- */

static void case_open(const char *path) {
    MlxEngineModel *m = NULL;
    const char *s = NULL;
    uint64_t u = 0, dims[MLXENGINE_MAX_DIMS];
    MlxEngineStatus st;
    int i;

    /* Il file che non c'è: il codice dice IO, e `*out` resta dov'era — un modello fantasma
     * non deve lasciare un handle da chiudere. */
    {
        char missing[512];
        MlxEngineModel *sentinel = (MlxEngineModel *)0x2;
        snprintf(missing, sizeof missing, "%s/fmlx_engine_absente.gguf", tmp_dir());
        unlink(missing);
        st = mlxengine_model_open(missing, &sentinel);
        CHECK(st == FMLX_GGUF_ERR_IO, "file assente: esito %d (%s)", (int)st,
              mlxengine_status_str(st));
        CHECK(sentinel == (MlxEngineModel *)0x2, "model_open ha toccato *out su file assente");
    }

    st = mlxengine_model_open(path, &m);
    if (st != FMLX_GGUF_OK || !m) {
        CHECK(0, "apertura fixture: %s", mlxengine_status_str(st));
        return;
    }

    CHECK(mlxengine_model_n_tensors(m) == 2, "n_tensors = %llu",
          (unsigned long long)mlxengine_model_n_tensors(m));
    CHECK(mlxengine_model_n_kv(m) == 3, "n_kv = %llu", (unsigned long long)mlxengine_model_n_kv(m));
    CHECK(mlxengine_model_alignment(m) == 32, "alignment = %llu",
          (unsigned long long)mlxengine_model_alignment(m));

    CHECK(mlxengine_model_kv_str(m, "general.architecture", &s) == FMLX_GGUF_OK && s &&
              strcmp(s, "mlxengine-test") == 0, "general.architecture = \"%s\"",
          s ? s : "(null)");
    u = 0;
    CHECK(mlxengine_model_kv_u64(m, "test.block_count", &u) == FMLX_GGUF_OK && u == 4,
          "test.block_count = %llu", (unsigned long long)u);

    /* Chiave ASSENTE ≠ file guasto, ed è il caso che un chiamante tratta al contrario: un
     * metadata opzionale si ignora, un header rotto si riporta. */
    s = (const char *)0x9;
    st = mlxengine_model_kv_str(m, "general.non_c_esiste", &s);
    CHECK(st == MLXENGINE_ERR_NO_KEY, "chiave assente: esito %d", (int)st);
    CHECK(s == NULL, "chiave assente ha scritto in *out");

    i = mlxengine_tensor_find(m, "blk.0.q");
    CHECK(i == 1, "tensor_find(blk.0.q) = %d", i);
    CHECK(mlxengine_tensor_find(m, "blk.9.z") == -1, "nome inventato trovato");
    CHECK(mlxengine_tensor_find(m, NULL) == -1, "nome NULL trovato");

    CHECK(mlxengine_tensor_name(m, 0) && strcmp(mlxengine_tensor_name(m, 0), "blk.0.w") == 0,
          "nome[0] = \"%s\"", mlxengine_tensor_name(m, 0));
    CHECK(mlxengine_tensor_type(m, 0) == 0, "type[0] = %d", mlxengine_tensor_type(m, 0));
    CHECK(mlxengine_tensor_numel(m, 0) == NF32, "numel[0] = %llu",
          (unsigned long long)mlxengine_tensor_numel(m, 0));
    CHECK(mlxengine_tensor_bytes(m, 0) == (uint64_t)NF32 * sizeof(float), "bytes[0] = %llu",
          (unsigned long long)mlxengine_tensor_bytes(m, 0));
    CHECK(mlxengine_tensor_is_covered(m, 0) == 1, "F32 dichiarato non coperto");
    CHECK(mlxengine_tensor_is_covered(m, 1) == 1, "Q8_0 dichiarato non coperto");

    /* Il blocco, com'è visto da fuori: 1 sui tipi non bloccati, 32 su Q8_0, 0 dove il tensore
     * non c'è. Sono le TRE risposte possibili e le uniche tre che un chiamante può usare per
     * calcolarsi una richiesta legale senza avere i ns. sorgenti in mano. */
    CHECK(mlxengine_tensor_block_vals(m, 0) == 1, "block_vals F32 = %llu",
          (unsigned long long)mlxengine_tensor_block_vals(m, 0));
    CHECK(mlxengine_tensor_block_vals(m, 1) == 32, "block_vals Q8_0 = %llu",
          (unsigned long long)mlxengine_tensor_block_vals(m, 1));
    CHECK(mlxengine_tensor_block_vals(m, 99) == 0, "block_vals fuori range != 0");

    memset(dims, 0x7f, sizeof dims);
    CHECK(mlxengine_tensor_dims(m, 0, dims) == FMLX_GGUF_OK && dims[0] == NF32 && dims[1] == 1,
          "dims[0] = %llu,%llu", (unsigned long long)dims[0], (unsigned long long)dims[1]);

    /* Indice fuori range su handle VERO: altra cosa da handle NULL, e il codice lo dice. */
    CHECK(mlxengine_tensor_name(m, 2) == NULL, "nome fuori range non NULL");
    CHECK(mlxengine_tensor_dims(m, 2, dims) == MLXENGINE_ERR_INDEX, "dims fuori range");
    CHECK(mlxengine_tensor_dequant(m, 77, 0, 1, (float *)dims, sizeof dims) ==
              MLXENGINE_ERR_INDEX, "dequant su indice folle non rifiutato con ERR_INDEX");

    case_dequant(m, path);
    mlxengine_model_close(m);
}

int main(void) {
    case_identity();
    case_bad_args();
    if (write_engine_fixture()) case_open(fx("abi"));
    unlink(fx("abi"));
    printf("test_engine: %d verifiche, %d fallite\n", checks, fails);
    return fails ? 1 : 0;
}
