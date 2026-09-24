/* mlxengine.h — ABI pubblica di MLXEngine (deliverable ① del piano fondativo).
 *
 * Che cos'è: il confine stabile attraverso cui un programma usa i motori — la CLI `fastmlx`,
 * il server, e domani GMBasic via P/Invoke. Si distribuisce come libreria condivisa
 * (`libmlxengine.1.dylib`): ciò che è firmato qui non si cambia più, si aggiunge. Un
 * chiamante legato a `MLXENGINE_ABI_VERSION 1` deve poter continuare a funzionare anche
 * quando la dylib sarà la numero 2.
 *
 * Che cosa NON è (ancora): tutto il motore. La superficie di oggi è il lettore GGUF e il
 * dequant dei tipi provati bit a bit contro tre implementazioni indipendenti
 * (`tools/quant_diff.c`). I blocchi che mancano — pesi packed, cache KV, attenzione, MoE,
 * generazione — entrano QUI, uno per volta, e ciascuno col proprio gate di parità: non entra
 * nulla che sia solo "plausibile".
 *
 * Una sola sorgente. I tipi di dato e i codici del FORMATO sono quelli di
 * `fmlx/fmlx_gguf.h`, citati e non ricopiati. Duplicarli avrebbe due effetti prevedibili:
 * divergono al primo cambiamento, e chi lega la dylib non ha più modo di sapere quale dei due
 * è vero.
 */
#ifndef MLXENGINE_H
#define MLXENGINE_H

#include "fmlx/fmlx_gguf.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MLXENGINE_ABI_VERSION 1u
#define MLXENGINE_VERSION_STRING "1.0.0"

/* Esito delle chiamate: 0 = OK. I valori sotto i 32 SONO `FmlxGgufStatus` (ciò che non va
 * nel FILE); i 64.. sono `MlxEngineError` (ciò che non va nella CHIAMATA). La distanza non
 * è un capriccio di numerazione: chi lega una dylib non deve poter leggere "argomento
 * sbagliato" come "GGUF corrotto", perché la reazione giusta nei due casi è opposta — nel
 * primo si corregge il programma, nel secondo si riscrivono i byte. */
typedef int MlxEngineStatus;

typedef enum {
    MLXENGINE_ERR_NULL_ARG = 64,   /* puntatore obbligatorio passato a NULL          */
    MLXENGINE_ERR_INDEX = 65,      /* indice di tensore oltre `n_tensors`            */
    MLXENGINE_ERR_WINDOW = 66,     /* la finestra [at, at+n) esce dal tensore        */
    MLXENGINE_ERR_MISALIGNED = 67, /* `at` cade dentro un blocco: non dequantizz.    */
    MLXENGINE_ERR_NO_KEY = 68      /* la chiave KV non esiste (NON è un file guasto) */
} MlxEngineError;

/* Un modello aperto. L'handle è opaco di proposito: i campi del lettore (`FmlxGgufFile`)
 * sono la sua memoria di lavoro, e esporli significherebbe non poterli più cambiare senza
 * ricompilare i chiamanti. */
typedef struct MlxEngineModel MlxEngineModel;

/* ---- identificazione ------------------------------------------------------------ */

/* Il numero d'ABI, non la versione del pacchetto: 1 anche quando la dylib sarà la 1.7. */
unsigned mlxengine_abi_version(void);
const char *mlxengine_version_string(void);
/* Testo di un esito, sempre non-NULL: anche per un codice che non conosce. */
const char *mlxengine_status_str(MlxEngineStatus s);

/* ---- aprire e chiudere ------------------------------------------------------------ */

/* Apre un GGUF SENZA materializzare i pesi: la mappatura è `PROT_READ|MAP_PRIVATE`, le
 * pagine restano su disco e il SO le porta in RAM quando — e solo se — si leggono. È la
 * stessa disciplina di `mlx-lm` con lo swap su disco, ed è l'unico modo di aprire i file da
 * ~90 GB che non entrerebbero mai in RAM. Non c'è una bandiera "swap: on/off" proprio perché
 * non esiste l'alternativa da nascondere: chi vuole le pagine residenti le tocca, e il modo
 * di toccarle è dequantizzare.
 *
 * Ritorna OK e scrive in `*out` l'handle, o ritorna un codice SENZA toccare `*out`. */
MlxEngineStatus mlxengine_model_open(const char *path, MlxEngineModel **out);
void mlxengine_model_close(MlxEngineModel *m);
/* Il messaggio specifico dell'ultimo fallimento su questo handle. MAI NULL: se non c'è un
 * messaggio, dice che non ce n'è uno (un `%s` che stampa "(null)" è il modo in cui i motivi
 * spariscono dai log). */
const char *mlxengine_model_last_error(const MlxEngineModel *m);

/* ---- metadati -------------------------------------------------------------------- */

/* Contenitori validi anche con handle NULL (ritornano 0), perché "quanti tensori" su un
 * modello che non c'è è una domanda a cui si può rispondere, non un errore. */
uint64_t mlxengine_model_n_tensors(const MlxEngineModel *m);
uint64_t mlxengine_model_n_kv(const MlxEngineModel *m);
MlxEngineStatus mlxengine_model_kv_str(const MlxEngineModel *m, const char *key,
                                       const char **out);
MlxEngineStatus mlxengine_model_kv_u64(const MlxEngineModel *m, const char *key,
                                       uint64_t *out);
/* Allineamento dichiarato (`general.alignment`, 32 se il file non lo dice). */
uint64_t mlxengine_model_alignment(const MlxEngineModel *m);

/* ---- tensori --------------------------------------------------------------------- */

/* Indice del tensore, o -1 se non c'è. Il nome è quello del file, senza copie. */
int mlxengine_tensor_find(const MlxEngineModel *m, const char *name);

/* I quattro accessor qui sotto: NULL/0 su handle non aperto o indice fuori dal range. Un 0
 * qui significa "non lo so", non "il tensore ha zero dimensioni". */
const char *mlxengine_tensor_name(const MlxEngineModel *m, uint64_t idx);
int mlxengine_tensor_type(const MlxEngineModel *m, uint64_t idx);
uint64_t mlxengine_tensor_numel(const MlxEngineModel *m, uint64_t idx);
uint64_t mlxengine_tensor_bytes(const MlxEngineModel *m, uint64_t idx);
/* 1 se questa build sa dequantizzare il tipo del tensore. Domanda da fare PRIMA di copiare
 * byte: i tipi non coperti sono gap dichiarati, non zero silenziosi. */
int mlxengine_tensor_is_covered(const MlxEngineModel *m, uint64_t idx);

/* Elementi per blocco di quantizzazione: 1 sui tipi non bloccati (F32, F16, BF16, I32), 32 o
 * 256 sui quantizzati, 0 se l'indice non esiste.
 *
 * Questa funzione esiste perché un chiamante ESTERNO l'ha chiesta: `mlxengine_tensor_dequant`
 * rifiuta una finestra che non cada sui bordi del blocco, e senza il numero del blocco un
 * programma che non ha i ns. sorgenti non può calcolarsi una richiesta legale — resterebbe con
 * «ho chiesto 8 pesi di un Q4_K e mi ha detto di no», che è un errore dell'ABI travestito da
 * errore del file. */
uint64_t mlxengine_tensor_block_vals(const MlxEngineModel *m, uint64_t idx);

#define MLXENGINE_MAX_DIMS FMLX_GGUF_MAX_DIMS
/* Copia le dimensioni in `dims` (che deve avere `MLXENGINE_MAX_DIMS` posti) e ritorna OK,
 * oppure un codice d'errore: non dice quante ne ha scritte perché `n_dims` è già dentro il
 * tensore e una seconda fonte del numero sarebbe un modo di divergere. */
MlxEngineStatus mlxengine_tensor_dims(const MlxEngineModel *m, uint64_t idx, uint64_t *dims);

/* ---- il cammino caldo: una finestra di pesi in F32 -------------------------------- */

/* Dequantizza `n` elementi del tensore `idx` a partire dall'elemento `at`, in `dst` (che
 * deve avere posto per `n` float). Legge la finestra direttamente dentro la mappatura:
 * nessuna copia intermedia, nessuna pagina toccata che non serva — è così che un
 * super-blocco alla volta si percorre un file più grande della RAM.
 *
 * Tre rifiuti distinti, e sono la ragione per cui la funzione esiste invece di un `memcpy`:
 * `at` deve cadere sul bordo di un blocco (un blocco a metà non è dequantizzabile: i suoi
 * indici condividono la scala), `n` deve esserne multiplo, `dst_len` deve coprire `n` float.
 * Ognuno arriva con il SUO codice. */
MlxEngineStatus mlxengine_tensor_dequant(const MlxEngineModel *m, uint64_t idx, uint64_t at,
                                         uint64_t n, float *dst, size_t dst_len);

#ifdef __cplusplus
}
#endif

#endif /* MLXENGINE_H */
