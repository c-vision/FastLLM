/* fmlx_gguf.h — lettore GGUF di riferimento (lane_gguf), ABI C pubblica.
 *
 * Un SOLO lettore di record in FastLLM. Derivato nel design dal lettore GGUF di ds4
 * (motore C indipendente, senza ggml) e scritto contro le specifiche GGUF v2/v3/v4.
 *
 * Regole volute, che sono la ragione per cui questo file esiste (piano §8):
 *   - NESSUN cap sul numero di tensori o di KV (solo memoria);
 *   - `general.alignment` rispettato (32 byte è il DEFAULT, non un hardcode);
 *   - ogni rifiuto ha un CODICE DISTINTO e un messaggio: "file troncato", "tipo ignoto",
 *     "tensori sovrapposti", "versione non supportata" non condividono mai lo stesso
 *     `false`/`0`;
 *   - MAI un byte-count silenzioso: un tipo sconosciuto o un `numel` non multiplo del
 *     blocco sono ERRORI (è il difetto dei lettori di casa, vedi §8 del piano).
 *
 * Nessun legame di progetto: questo header non conosce MLX, llama.cpp né GMBasic.
 */
#ifndef FMLX_GGUF_H
#define FMLX_GGUF_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FMLX_GGUF_MAGIC 0x46554747u /* "GGUF" little-endian */
#define FMLX_GGUF_ALIGN_DEFAULT 32u
#define FMLX_GGUF_MAX_DIMS 8u /* oltre: errore distintivo (GGUF v3 usa 4, v2<=4) */

/* Codice di esito. 0 = ok. Ogni fallimento ha il SUO codice: mai un 0 indistinto. */
typedef enum {
    FMLX_GGUF_OK = 0,
    FMLX_GGUF_ERR_NOT_GGUF = 1,        /* magic diversa */
    FMLX_GGUF_ERR_VERSION = 2,         /* versione non 2/3/4 */
    FMLX_GGUF_ERR_TRUNCATED = 3,       /* file finito dentro header/KV/tensor-info */
    FMLX_GGUF_ERR_IMPLAUSIBLE = 4,     /* lunghezza stringa/array assurda => header corrotto */
    FMLX_GGUF_ERR_KV_TYPE = 5,         /* tipo di valore KV sconosciuto */
    FMLX_GGUF_ERR_NDIMS = 6,           /* n_dims 0 o > FMLX_GGUF_MAX_DIMS */
    FMLX_GGUF_ERR_BAD_ALIGN = 7,       /* general.alignment non potenza di 2 / 0 */
    FMLX_GGUF_ERR_NEG_DIM = 8,         /* dimensione <= 0 */
    FMLX_GGUF_ERR_TYPE_UNKNOWN = 9,    /* ggml_type fuori tabella: Errore, non 0 byte */
    FMLX_GGUF_ERR_TYPE_REMOVED = 10,   /* Q4_2/Q4_3, tipi ritirati upstream */
    FMLX_GGUF_ERR_NUMEL_BLOCK = 11,    /* numel non multiplo del blocco */
    FMLX_GGUF_ERR_OVERLAP = 12,        /* intervalli di dati che si sovrappongono */
    FMLX_GGUF_ERR_DATA_SHORT = 13,     /* dati dichiarati oltre la fine del file */
    /* 14 libero: un tempo era ERR_ORDER ("offset non crescente"). Sbagliato: GGUF non
     * richiede che le tensor-info siano nell'ordine degli offset. Ciò che è vietato è la
     * SOVRAPPOSIZIONE degli intervalli (ERR_OVERLAP), non l'ordine. */
    FMLX_GGUF_ERR_NO_MEM = 15,
    FMLX_GGUF_ERR_IO = 16,
    FMLX_GGUF_ERR_NAME_DUP = 17,       /* due tensori con lo stesso nome */
    FMLX_GGUF_ERR_COUNT = 18,          /* n_tensors/n_kv incoerente con la size del file */
    FMLX_GGUF_ERR_DST_SMALL = 19,      /* buffer di destinazione piu piccolo del tensore */
    /* 20: il tipo ESISTE ed è legittimo, ma questa lane non ha ancora il suo dequant.
     * È un gap dichiarato, non uno stub: chi chiama riceve un errore, mai f32 a zero. */
    FMLX_GGUF_ERR_NOT_COVERED = 20
} FmlxGgufStatus;

const char *fmlx_gguf_status_str(FmlxGgufStatus s);

/* ---- Tabella dei tipi ggml: UNICA, keyed per VALORE di enum, con riferimento verificabile ---- */

#define FMLX_GGML_TYPE_MAX 41 /* = GGML_TYPE_COUNT-1 di ggml.h:432 (COUNT 42) */
/* I NUMERI qui sono il formato, non un dettaglio di implementazione: i valori 31/32/33 e
 * 36/37/38 sono BUCHI (tipi ritirati) e vanno lasciati vuoti — comprimerli sposta TQ1_0,
 * TQ2_0, MXFP4, NVFP4 e Q1_0 di uno, che significa leggere i byte di un tipo come se fossero
 * l'altro. Il gate fermò la compilazione su esattamente questo (docs/quant-coverage.md §5). */

typedef struct {
    const char *name;
    uint64_t block_vals;  /* elementi per blocco (1 = non bloccato) */
    uint64_t block_bytes; /* byte per blocco */
    int standard;         /* 1 = ggml upstream 0..30, 0 = estensione di casa */
    int usable;           /* 0 = ritirate (Q4_2/Q4_3) */
} FmlxGgmlType;

/* Restituisce NULL se il valore non è nella tabella (mai un finto "tipo 0"). */
const FmlxGgmlType *fmlx_ggml_type(int ggml_type);

/* Byte di un tensore. Ritorna FMLX_GGUF_OK e scrive in *out, oppure un codice d'errore.
 * `numel` NON multiplo di block_vals è un errore: mai troncare in silenzio. */
FmlxGgufStatus fmlx_ggml_type_bytes(int ggml_type, uint64_t numel, uint64_t *out);

/* ---- Record ---- */

typedef struct {
    char *name;
    uint32_t n_dims;
    uint64_t dims[FMLX_GGUF_MAX_DIMS];
    int32_t ggml_type;
    uint64_t offset;  /* relativo all'inizio della data section */
    uint64_t nbytes;  /* calcolato alla parse: 0 solo se il tensore è davvero vuoto */
    uint64_t numel;
} FmlxGgufTensor;

/* Valore KV. Le stringhe sono copiate nel arena del file (possedute da FmlxGgufFile). */
typedef struct {
    char *key;
    uint32_t type; /* GGUF metadata type: 0 u8 .. 12 f64 */
    uint64_t u;    /* intero/bool (per f32/f64: bit pattern) */
    double f;      /* valore float se type 6/12 */
    char *s;       /* stringa, o NULL */
    /* array: elementi consecutivi, stesso tipo scalare/float (annidati non supportati -> errore) */
    uint32_t elem_type;
    uint64_t arr_len;
    uint64_t *arr_u;
    double *arr_f;
    char **arr_s;
} FmlxGgufKV;

typedef struct {
    const uint8_t *data; /* mappato o letto: buffer posseduto da chi apre, vedi owns_buffer */
    size_t len;
    uint32_t version;
    uint64_t n_tensors;
    uint64_t n_kv;
    uint64_t alignment;         /* general.alignment, default 32 */
    uint64_t data_start;        /* offset assoluto della data section (già allineato) */
    FmlxGgufTensor *tensors;    /* n_tensors elementi */
    FmlxGgufKV *kvs;            /* n_kv elementi */
    char *last_error;           /* messaggio specifico; arena del file */
    int owns_buffer;            /* 1 = `data` è stata allocata da noi e va liberata */
    void *arena;
} FmlxGgufFile;

/* Parse di un buffer già in memoria (fixture sintetiche, test, mmap). */
FmlxGgufStatus fmlx_gguf_parse(const uint8_t *data, size_t len, FmlxGgufFile *out);

/* Legge la TESTATA da file (header + KV + tensor info) con mmap PROT_READ|MAP_PRIVATE: le
 * pagine dei pesi NON vengono materializzate, restano su disco e il SO le porta via quando
 * (e se) si leggono. `out->data` punta alla mappatura, quindi `fmlx_gguf_read_tensor()`
 * funziona subito; chi vuole solo i metadati non tocca mai la data section. */
FmlxGgufStatus fmlx_gguf_open_header(const char *path, FmlxGgufFile *out);

/* Copia in `dst` (che deve avere >= nbytes byte) i byte del tensore `idx`. */
FmlxGgufStatus fmlx_gguf_read_tensor(const FmlxGgufFile *f, uint64_t idx, void *dst, size_t dst_len);

/* Vista in sola lettura di `len` byte del tensore `idx` a partire dall'offset `at` dentro
 * il tensore: NULL se la finestra esce dal tensore o dal buffer. Con un file aperto da
 * `fmlx_gguf_open_header` la vista è dentro la mmap, quindi NON copia e NON materializza
 * le pagine che non si toccano (è così che si dequantizza a pezzi un file da 90 GB).
 * La validità della vista dura fino a fmlx_gguf_close(). */
const uint8_t *fmlx_gguf_tensor_bytes(const FmlxGgufFile *f, uint64_t idx, uint64_t at,
                                      uint64_t len);

void fmlx_gguf_close(FmlxGgufFile *f);

/* Lookup. */
const FmlxGgufTensor *fmlx_gguf_find_tensor(const FmlxGgufFile *f, const char *name);
const FmlxGgufKV *fmlx_gguf_find_kv(const FmlxGgufFile *f, const char *key);
int fmlx_gguf_kv_str(const FmlxGgufFile *f, const char *key, const char **out);
int fmlx_gguf_kv_u64(const FmlxGgufFile *f, const char *key, uint64_t *out);

/* Messaggio dell'ultimo fallimento (NULL se nessuno). */
const char *fmlx_gguf_last_error(const FmlxGgufFile *f);

/* Writer minimale, usato SOLO dai test per produrre fixture sintetiche senza scaricare
 * nulla. Non è la superficie pubblica del progetto. */
FmlxGgufStatus fmlx_gguf_write_fixture(const char *path, uint32_t version, uint64_t alignment,
                                       const FmlxGgufKV *kvs, uint64_t n_kv,
                                       const FmlxGgufTensor *tensors, uint64_t n_tensors,
                                       const uint8_t *blob, size_t blob_len);

#ifdef __cplusplus
}
#endif

#endif /* FMLX_GGUF_H */
