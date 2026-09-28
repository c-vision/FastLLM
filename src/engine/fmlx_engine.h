/* fmlx_engine.h — la lane del MOTORE: il ponte fra l'ABI comune `llmengine` (LLMEnv) e il
 * confine a eventi neutri del layer ② (`fmlx_api.h`).
 *
 * COSA C'È E COSA NON C'È. Qui vive tutto ciò che serve a trasformare una richiesta
 * normalizzata in testo generato: risoluzione del modello, cache residente, tokenizer
 * (`llmtoken`), chat template, prefill e decode via vtable MLX. NON c'è HTTP e NON c'è JSON
 * di protocollo: gli eventi escono come `FmlxApiEvent` e li veste l'adapter del dialetto
 * (OpenAI/Anthropic) nel layer ②. È il confine che `fmlx_api.h` prescrive: il motore non
 * conosce HTTP, l'adapter non conosce il motore.
 *
 * L'ABI MLX è IDS-ONLY (`llmengine_llm_engine_mlx()`): non espone tokenize/detokenize, quindi
 * la tokenizzazione sta QUI e usa `LLMToken`. Il prompt per `/v1/chat/completions` e
 * `/v1/messages` passa dal chat template del modello (`tokenizer_config.json`); per
 * `/v1/completions` è testo grezzo.
 *
 * Nessuna allocazione statica di modello: il modello è caricato alla prima richiesta per nome
 * e RESTA residente (la cache è la ragione per cui una conversazione non ri-paga il load).
 */
#ifndef FMLX_ENGINE_H
#define FMLX_ENGINE_H

#include "fmlx/fmlx_api.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Limiti condivisi con il responder: il buffer che copia il nome del modello dal DOM e il tetto
 * sui token di uscita (un `max_tokens` oltre il tetto viene riportato al tetto, non ignorato). */
#define FMLX_ENGINE_NAME_MAX 256
#define FMLX_ENGINE_MAX_NEW 8192

/* Prefix cache (CAP_PREFIX del runner MLX). `MATCH` = quanti token di prefisso comuni servono
 * perché il riuso valga il prefill risparmiato; `SAVE` = lunghezza minima del prompt perché
 * valga la pena salvarne la KV. Entrambe sovrascrivibili via ambiente (FMLX_PREFIX_MIN_MATCH,
 * FMLX_PREFIX_MIN_SAVE_LEN): sono politiche di memoria, non costanti del linguaggio. */
#define FMLX_PREFIX_MIN_MATCH 8
#define FMLX_PREFIX_MIN_SAVE_LEN 64

/* Un modello residente (handle opaco). */
typedef struct FmlxEngineModel FmlxEngineModel;

/* Una generazione in corso, usabile come `FmlxApiNext` via `fmlx_engine_next`. */
typedef struct FmlxEngineGen FmlxEngineGen;

/* Risolve `name` (repo id HF, path assoluto/relativo, o nome sotto $FMLX_MODELS_DIR) e carica
 * il modello nella cache residente. `cache_root` è la radice del cache HF da usare (NULL = la
 * regola d'ambiente): la STESSA che `/v1/models` elenca, così `--cache` non mente. Ritorna NULL
 * e scrive la causa in `err`. Il secondo chiamante con lo stesso nome riusa il modello in RAM. */
FmlxEngineModel *fmlx_engine_model(const char *cache_root, const char *name, char *err,
                                   size_t errcap);

/* Conteggio token ESATTO del prompt di una chat (stesso render del template usato per
 * generare). `*out` = numero di token. Usato da `/v1/messages/count_tokens`. */
int fmlx_engine_count_tokens(FmlxEngineModel *m, const FmlxApiChat *c, int *out, char *err,
                             size_t errcap);

/* Avvia una generazione di chat. `c` è la richiesta normalizzata del layer ②; il sistema è
 * ricavato con `fmlx_api_join_system`. Ritorna NULL + `err` su fallimento (modello
 * irraggiungibile, prompt non tokenizzabile, decode fallito). */
FmlxEngineGen *fmlx_engine_chat(FmlxEngineModel *m, const FmlxApiChat *c, char *err, size_t errcap);

/* Avvia una generazione da PROMPT GREZZO (`/v1/completions`): nessun chat template. `c` serve
 * solo per le stop sequences (può essere NULL). */
FmlxEngineGen *fmlx_engine_prompt(FmlxEngineModel *m, const char *prompt, int max_tokens,
                                  float temperature, float top_p, int top_k, float presence,
                                  float frequency, int64_t seed, const FmlxApiChat *c, char *err,
                                  size_t errcap);

/* Iteratore compatibile con `FmlxApiNext`. Genera UN token per chiamata (decode incrementale),
 * emette TEXT a pezzi (boundary UTF-8 rispettati) e un solo DONE finale. `ctx` è il
 * `FmlxEngineGen*`. */
FmlxApiGenSt fmlx_engine_next(void *ctx, FmlxApiEvent *ev, FmlxApiErrCtx *err);

/* 1 se QUESTA generazione ha riusato la prefix cache (il prefisso del prompt era già in KV,
 * quindi niente prefill del prefisso), 0 se il prefill è stato completo. È il dato che rende
 * onesto il contatore `prefix_cache_hits_total` di `/metrics`. */
int fmlx_engine_gen_prefix_hit(const FmlxEngineGen *g);

void fmlx_engine_gen_free(FmlxEngineGen *g);

/* Libera la cache residente (per i test e per uno shutdown pulito). */
void fmlx_engine_shutdown(void);

/* Introspezione della cache residente (per `/v1/models/status`, `load`, `unload`). */
int fmlx_engine_resident_count(void);
const char *fmlx_engine_resident_name(int i); /* NULL fuori range */
/* Scarica il modello residente con quel nome. Ritorna 1 se c'era, 0 se non residente. */
int fmlx_engine_unload(const char *name);

#ifdef __cplusplus
}
#endif
#endif /* FMLX_ENGINE_H */
