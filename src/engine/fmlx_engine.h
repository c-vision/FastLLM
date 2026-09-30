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
/* Tetto sui token di uscita. NON è un limite "di sicurezza" da tenere basso: i modelli reasoning
 * possono spendere centinaia di migliaia di token nel blocco <think> prima della risposta, quindi
 * il cap deve stare ~262k (256k). Il tetto reale è la KV del modello; qui si lascia spazio. */
#define FMLX_ENGINE_MAX_NEW 262144

/* Default di CONTESTO (capienza KV, in token) applicato all'avvio di OGNI modello e mostrato
 * a video. Sovrascrivibile a caldo con `/prompt <n>`. Un solo punto da cambiare. */
#define FMLX_ENGINE_DEFAULT_CTX 262144
/* DS4 (firma GGUF `deepseek4`): cache KV PREALLOCATE -> contesto più basso per non saturare la
 * RAM col modello da ~91 GB. Rilevato dalla FIRMA del modello, non dal nome. */
#define FMLX_DSV4_DEFAULT_CTX 32768

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

/* Embedding di `text` col modello, L2-normalizzato (come il percorso llama.cpp). Scrive `dim`
 * float in `out` (cap >= dim), scrive in `*out_tokens` i token usati (se non NULL) e ritorna dim,
 * oppure -1 (motore senza CAP_EMBED, testo vuoto, tokenizzazione fallita). IDS-based: tokenizza
 * col tokenizer del modello e chiama `embed_ids`/`embed_text` dell'ABI. */
int fmlx_engine_embed(FmlxEngineModel *m, const char *text, float *out, int cap, int *out_tokens);

/* Dimensione dell'embedding del modello, o -1 se il motore non espone CAP_EMBED. */
int fmlx_engine_embed_dim(FmlxEngineModel *m);

/* Offload esperti MoE da disco (flag `--offload-experts`): va impostato PRIMA del primo load.
 * Il motore streama gli esperti dalla dir del modello invece di materializzarli (per modelli più
 * grandi della RAM). No-op per i modelli dense. */
void fmlx_engine_set_offload_experts(int on);

/* 1 = sessione interattiva (run/chat da terminale): abilita il PROMPT Y/N per l'offload quando il
 * modello supera il 50% della memoria libera+reclamabile e `--offload-experts` non è già dato.
 * `serve` NON lo chiama: un server non deve bloccarsi su una domanda. */
void fmlx_engine_set_interactive(int on);

/* Thinking (ragionamento) dei modelli reasoning: DEFAULT = disattivato (il render Jinja inietta
 * `enable_thinking=false`, che per i Qwen pre-riempie <think></think>). `--think` lo riattiva. */
void fmlx_engine_set_think(int on);

/* IMMAGINI (input multimodale). Il motore MLX non ha vision: il SERVER registra un descrittore
 * che, dato un blocco immagine (url `data:` base64, o http), ritorna una descrizione testuale
 * (via un VLM sulla lane llama.cpp/mtmd). `content_text` lo invoca quando incontra un blocco
 * immagine (OpenAI `image_url` / Anthropic `image`) e inietta la descrizione nel prompt.
 * Ritorna 0 ok, -1 errore (l'immagine viene saltata). */
typedef int (*FmlxImageDescriber)(void *ctx, const char *url, size_t url_len,
                                  char *out, size_t outcap);
void fmlx_engine_set_image_describer(FmlxImageDescriber fn, void *ctx);

/* Imposta la capienza del contesto/KV (token) del modello residente a runtime, per qualunque
 * modello che supporti CAP_KV_MAX. Ritorna 0 se accettato, -2 se il motore non lo supporta. */
int fmlx_engine_set_ctx(FmlxEngineModel *m, int n);

/* Nuova sessione: azzera la cache KV del modello (riapplicando il contesto corrente). */
int fmlx_engine_new_session(FmlxEngineModel *m);

/* Registro opzioni del motore (parametri senza env): imposta `nome` a `valore`. 0 ok. */
int fmlx_engine_opt_set(const char *name, const char *value);
/* Legge un'opzione booleana dal registro. 0 = trovata (in *out), !=0 = nome ignoto. */
int fmlx_engine_opt_get_bool(const char *name, int *out);
int fmlx_engine_opt_get_int(const char *name, int *out);

/* Enumerazione del registro (per `fastllm options`). */
int fmlx_engine_opt_count(void);
int fmlx_engine_opt_at(int i, const char **name, const char **desc, char *value, int cap);
/* Valore d'esempio dell'i-esima opzione (per l'help). */
int fmlx_engine_opt_example(int i, char *buf, int cap);

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

/* Statistiche KV della generazione (token usati / capacità), lette dall'ABI v2 `kv_stats`
 * sulla sessione. 0 se disponibili, -1 se il motore non le espone. Va chiamata PRIMA che la
 * generazione sia liberata, cioè sull'evento DONE del flusso. */
int fmlx_engine_gen_kv_stats(const FmlxEngineGen *g, int *out_used, int *out_cap);

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
