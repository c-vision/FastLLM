/* mlxengine.c — la superficie d'ABI di MLXEngine, primo blocco: GGUF + dequant.
 *
 * Questo file NON implementa niente: ordina. Il lavoro è già fatto, testato e misurato nelle
 * due lane che chiama — `fmlx_gguf` (lettore) e `fmlx_quant` (dequant provato bit a bit contro
 * tre implementazioni indipendenti). Quello che aggiunge è ciò che una libreria condivisa deve
 * avere e una lane interna no: una forma stable degli argomenti, indici che non possono
 * sforare, e codici che distinguono «la chiamata è sbagliata» da «il file è sbagliato».
 *
 * La regola di casa, qui più vera che altrove: nessuna copia di logica. Se domani il dequant
 * avrà un kernel Metal, quel kernel entrerà DENTRO `fmlx_quant` e questo file non cambierà di
 * una riga — è il test a cui serve l'ABI.
 *
 * Un gap dichiarato, non un buco silenzioso: quando `mlxengine_model_open` fallisce non c'è un
 * handle, e il messaggio specifico del lettore (che nomina il byte, non solo il codice) muore
 * con la sua arena. Chi chiama dall'esterno riceve il CODICE, che è distinto per ogni causa;
 * chi scrive un test dentro il repo può scendere al lettore e leggere il messaggio. Se
 * l'apertura dovrà raccontare il particolare anche a freddo, si farà con un canale proprio (un
 * buffer di errore per thread), non con un handle mezzo aperto: un oggetto da cui si può
 * soltanto leggere un errore è un oggetto che chiede di essere chiuso per sbaglio.
 */
#include "mlxengine.h"

#include "fmlx/fmlx_quant.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct MlxEngineModel {
    FmlxGgufFile f;
};

/* NULL se l'handle non c'è o l'indice non indica un tensore. Un solo punto decide «esiste
 * questo tensore», così gli accessor non possono divergere tra loro. */
static const FmlxGgufTensor *tensor_at(const MlxEngineModel *m, uint64_t idx) {
    if (!m || !m->f.tensors) return NULL;
    if (idx >= m->f.n_tensors) return NULL;
    return &m->f.tensors[idx];
}

/* ------------------------------------------------------------------ identificazione */

unsigned mlxengine_abi_version(void) { return MLXENGINE_ABI_VERSION; }

const char *mlxengine_version_string(void) { return MLXENGINE_VERSION_STRING; }

const char *mlxengine_status_str(MlxEngineStatus s) {
    switch (s) {
    case MLXENGINE_ERR_NULL_ARG: return "argomento obbligatorio passato a NULL";
    case MLXENGINE_ERR_INDEX: return "indice di tensore fuori dal range";
    case MLXENGINE_ERR_WINDOW: return "finestra oltre la fine del tensore";
    case MLXENGINE_ERR_MISALIGNED: return "inizio finestra dentro un blocco di quantizzazione";
    case MLXENGINE_ERR_NO_KEY: return "chiave di metadata assente";
    default: return fmlx_gguf_status_str((FmlxGgufStatus)s);
    }
}

/* ------------------------------------------------------------------ aprire e chiudere */

MlxEngineStatus mlxengine_model_open(const char *path, MlxEngineModel **out) {
    MlxEngineModel *m;
    FmlxGgufStatus st;
    if (!path || !out) return MLXENGINE_ERR_NULL_ARG;
    m = (MlxEngineModel *)calloc(1, sizeof *m);
    if (!m) return FMLX_GGUF_ERR_NO_MEM;
    st = fmlx_gguf_open_header(path, &m->f);
    if (st != FMLX_GGUF_OK) {
        free(m);
        return (MlxEngineStatus)st;
    }
    *out = m;
    return FMLX_GGUF_OK;
}

void mlxengine_model_close(MlxEngineModel *m) {
    if (!m) return; /* `free(NULL)` è una noop, e un close su un handle mai aperto deve
                     * comunque essere legale: è il prezzo di un `open` che ritorna un codice. */
    fmlx_gguf_close(&m->f);
    free(m);
}

const char *mlxengine_model_last_error(const MlxEngineModel *m) {
    return fmlx_gguf_last_error(m ? &m->f : NULL);
}

/* ------------------------------------------------------------------ metadati */

uint64_t mlxengine_model_n_tensors(const MlxEngineModel *m) { return m ? m->f.n_tensors : 0; }

uint64_t mlxengine_model_n_kv(const MlxEngineModel *m) { return m ? m->f.n_kv : 0; }

uint64_t mlxengine_model_alignment(const MlxEngineModel *m) { return m ? m->f.alignment : 0; }

MlxEngineStatus mlxengine_model_kv_str(const MlxEngineModel *m, const char *key,
                                       const char **out) {
    if (!m || !key) return MLXENGINE_ERR_NULL_ARG;
    if (out) *out = NULL;
    return fmlx_gguf_kv_str(&m->f, key, out) ? (MlxEngineStatus)FMLX_GGUF_OK
                                             : (MlxEngineStatus)MLXENGINE_ERR_NO_KEY;
}

MlxEngineStatus mlxengine_model_kv_u64(const MlxEngineModel *m, const char *key,
                                       uint64_t *out) {
    if (!m || !key) return MLXENGINE_ERR_NULL_ARG;
    if (out) *out = 0;
    return fmlx_gguf_kv_u64(&m->f, key, out) ? (MlxEngineStatus)FMLX_GGUF_OK
                                             : (MlxEngineStatus)MLXENGINE_ERR_NO_KEY;
}

/* ------------------------------------------------------------------ tensori */

int mlxengine_tensor_find(const MlxEngineModel *m, const char *name) {
    const FmlxGgufTensor *t;
    ptrdiff_t d;
    if (!m || !name) return -1;
    t = fmlx_gguf_find_tensor(&m->f, name);
    if (!t || !m->f.tensors) return -1;
    d = t - m->f.tensors;
    /* Un file può dichiarare più tensori di quanti `int` ne possa contare: meglio -1 («non
     * l'ho trovato») che un indice che somiglia a un «non trovato» ma non lo è. */
    if (d < 0 || d > INT32_MAX) return -1;
    return (int)d;
}

const char *mlxengine_tensor_name(const MlxEngineModel *m, uint64_t idx) {
    const FmlxGgufTensor *t = tensor_at(m, idx);
    return t ? t->name : NULL;
}

int mlxengine_tensor_type(const MlxEngineModel *m, uint64_t idx) {
    const FmlxGgufTensor *t = tensor_at(m, idx);
    return t ? t->ggml_type : -1;
}

uint64_t mlxengine_tensor_numel(const MlxEngineModel *m, uint64_t idx) {
    const FmlxGgufTensor *t = tensor_at(m, idx);
    return t ? t->numel : 0;
}

uint64_t mlxengine_tensor_bytes(const MlxEngineModel *m, uint64_t idx) {
    const FmlxGgufTensor *t = tensor_at(m, idx);
    return t ? t->nbytes : 0;
}

int mlxengine_tensor_is_covered(const MlxEngineModel *m, uint64_t idx) {
    const FmlxGgufTensor *t = tensor_at(m, idx);
    return (t && fmlx_ggml_type_is_covered(t->ggml_type)) ? 1 : 0;
}

uint64_t mlxengine_tensor_block_vals(const MlxEngineModel *m, uint64_t idx) {
    const FmlxGgufTensor *t = tensor_at(m, idx);
    const FmlxGgmlType *ty;
    if (!t) return 0;
    ty = fmlx_ggml_type(t->ggml_type);
    return ty ? ty->block_vals : 0; /* tipo fuori dalla tabella: 0, lo stesso valore di «indice
                                      * che non esiste». Sono due «non lo so», e fonderli qui è
                                      * onesto: nessuno dei due permette una richiesta legale. */
}

MlxEngineStatus mlxengine_tensor_dims(const MlxEngineModel *m, uint64_t idx, uint64_t *dims) {
    const FmlxGgufTensor *t;
    uint32_t i;
    if (!m || !dims) return MLXENGINE_ERR_NULL_ARG;
    t = tensor_at(m, idx);
    if (!t) return MLXENGINE_ERR_INDEX;
    for (i = 0; i < t->n_dims && i < MLXENGINE_MAX_DIMS; i++) dims[i] = t->dims[i];
    return FMLX_GGUF_OK;
}

/* ------------------------------------------------------------------ il cammino caldo */

MlxEngineStatus mlxengine_tensor_dequant(const MlxEngineModel *m, uint64_t idx, uint64_t at,
                                         uint64_t n, float *dst, size_t dst_len) {
    const FmlxGgufTensor *t;
    const FmlxGgmlType *ty;
    const uint8_t *src;
    uint64_t at_bytes = 0, span = 0;
    MlxEngineStatus st;

    if (!m || !dst) return MLXENGINE_ERR_NULL_ARG;
    t = tensor_at(m, idx);
    if (!t) return MLXENGINE_ERR_INDEX;
    if (n == 0) return FMLX_GGUF_OK; /* chiesto niente, scritto niente: non è un errore */

    /* Prima la finestra, che è una proprietà del FILE, e poi `dst_len`, che è una proprietà di
     * chi chiama: se un `n` assurdo producesse «buffer troppo piccolo» su un file che quei
     * pesi non li contiene, il colpevole indicato sarebbe il chiamante. */
    if (at > t->numel || n > t->numel - at) return MLXENGINE_ERR_WINDOW;
    if (dst_len < (size_t)n * sizeof(float)) return FMLX_GGUF_ERR_DST_SMALL;

    ty = fmlx_ggml_type(t->ggml_type);
    if (!ty) return FMLX_GGUF_ERR_TYPE_UNKNOWN;

    /* Il bordo del blocco è una proprietà del FORMATO, non una cortesia: gli indici di un
     * blocco condividono scala (e minimo), quindi mezzo blocco non ha valori corretti da
     * dare. Chiedere `at` in mezzo significa voler leggere dati che non esistono. */
    if (ty->block_vals > 1 && (at % ty->block_vals) != 0) return MLXENGINE_ERR_MISALIGNED;

    st = (MlxEngineStatus)fmlx_ggml_type_bytes(t->ggml_type, at, &at_bytes);
    if (st != FMLX_GGUF_OK) return st;
    st = (MlxEngineStatus)fmlx_ggml_type_bytes(t->ggml_type, n, &span);
    if (st != FMLX_GGUF_OK) return st; /* `n` non multiplo del blocco: NUMEL_BLOCK */

    /* La finestra è una VISTA dentro la mappatura: nessun byte copiato se non nei pesi che
     * servono davvero, e nessuna pagina materializzata che non serva. */
    src = fmlx_gguf_tensor_bytes(&m->f, idx, at_bytes, span);
    if (!src) return MLXENGINE_ERR_WINDOW;
    return (MlxEngineStatus)fmlx_ggml_dequant_row(t->ggml_type, src, n, dst);
}
