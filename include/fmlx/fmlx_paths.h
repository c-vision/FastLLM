/* fmlx_paths.h — percorsi di ricerca dei modelli, su FILE DI CONFIGURAZIONE.
 *
 * Il problema: `serve`/`run`/`chat` risolvevano un nome modello provando solo (a) un path
 * esplicito, (b) il cache Hugging Face, (c) $FMLX_MODELS_DIR. Chi tiene i modelli altrove (un
 * disco esterno, `~/ai/Models`, una cartella di GGUF) non aveva un default persistente: doveva
 * ripetere il path o esportare una variabile d'ambiente a ogni invocazione.
 *
 * Soluzione: un file di configurazione `$FASTLLM_HOME/paths` (default `~/.fastllm/paths`), una
 * DIRECTORY per riga, `#` a inizio riga = commento, righe vuote ignorate. Questi percorsi sono
 * cercati, IN ORDINE, dopo il path esplicito e il cache HF. Li si gestisce con `fastllm paths`
 * (lista numerata, `add`, `rm <n>` con progressivo numerico 1-based).
 *
 * Il file di config è volutamente di testo e minimale: si ispeziona a mano, si versiona, non
 * dipende dal layer JSON. */
#ifndef FMLX_PATHS_H
#define FMLX_PATHS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FMLX_PATHS_MAX 64          /* percorsi configurabili */
#define FMLX_PATHS_DIR_MAX 1024    /* lunghezza di un percorso */

/* Percorso del file di config: $FASTLLM_HOME/paths (default ~/.fastllm/paths).
 * Ritorna 0 e scrive in `out`; -1 se HOME assente o buffer corto. */
int fmlx_paths_file(char *out, size_t cap);

/* Numero di percorsi configurati (0 se il file non esiste). Non tocca il disco oltre la lettura. */
int fmlx_paths_count(void);

/* Il percorso i-esimo (0-based) in `out`. Ritorna 0, o -1 se fuori range/buffer corto. */
int fmlx_paths_get(int i, char *out, size_t cap);

/* Aggiunge una directory (dedup sul percorso risolto). Non verifica l'esistenza: un disco esterno
 * può essere smontato oggi e montato domani. Ritorna 0, o -1 con motivo in `err`. */
int fmlx_paths_add(const char *dir, char *err, size_t errcap);

/* Rimuove il percorso con INDICE NUMERICO 1-based (il progressivo mostrato da `fastllm paths`).
 * Ritorna 0, o -1 con motivo in `err`. */
int fmlx_paths_rm(int index_1based, char *err, size_t errcap);

#ifdef __cplusplus
}
#endif
#endif /* FMLX_PATHS_H */
