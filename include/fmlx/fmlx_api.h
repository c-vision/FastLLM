/* fmlx_api.h — il layer ② di FastLLM: router, catalogo errori e adapter OpenAI/Anthropic
 * sopra il codec (json + http). Non sopra un motore: il motore arriva da sotto come una
 * funzione che produce EVENTI NEUTRI.
 *
 * IL CONFINE (piano:199 — "il motore non deve conoscere HTTP"). Il layer ① emette
 * `{token_id, text, n, finish_reason}` e basta. Tutto ciò che è forma — che un delta
 * OpenAI si chiama `choices[0].delta.content`, che in Anthropic lo stesso byte è un
 * `content_block_delta` con `text_delta` dentro, che `stop` qui e `end_turn` là — vive
 * QUI. Un adapter che chiamasse `mlx_sample()` e un motore che scrivesse `data:` sono lo
 * stesso errore in due direzioni opposte.
 *
 * LA REGOLA CHE DECIDE IL DESIGN: **mai scoprire un campo ignorato guardando l'output**
 * (piano:474). Un client che manda `logit_bias` e ottiene una risposta coerente ma
 * prodotta *senza* quel bias sta leggendo una menzogna statistica. Quindi i campi che
 * non onoriamo finiscono in `ignored_params[]` nella risposta E in un warning nei log, e
 * la lista di ciò che è "accettato" contro ciò che è "onorato" è scritta qui sotto, non
 * dedotta dal codice.
 *
 * ZERO ALLOCAZIONI anche qui, come nel codec: gli adapter scrivono nella memoria del
 * chiamante (`FmlxJsonW` con `mem != NULL`, `FmlxHttpRes` con buffer fisso) e le stringhe
 * che `FmlxApiChat` conserva sono *slice* nel DOM del chiamante, che DEVE restare vivo
 * finché il chat vive. Le risposte troppo grandi per il buffer sono un errore
 * (`FMLX_API_INTERNAL` con messaggio esplicito), non una risposta tagliata.
 *
 * I `created`/`id` NON sono generati qui. Un adapter che leggesse l'orologio o un RNG
 * renderebbe ogni test non deterministico e nasconderebbe dove nasce un id di traccia; il
 * chiamante passa `FmlxApiMeta` e il test passa valori fissi.
 */
#ifndef FMLX_API_H
#define FMLX_API_H

#include "fmlx_http.h"
#include "fmlx_json.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FMLX_API_MAX_NAME 96u    /* model, role, nome funzione, id */
#define FMLX_API_MAX_STOP 8u
#define FMLX_API_MAX_SYSTEM 8u   /* messaggi `role:system` in una richiesta OpenAI */
#define FMLX_API_MAX_IGNORED 24u /* oltre: la richiesta è malformata, non "i primi 24" */
#define FMLX_API_MAX_WARN 8u
#define FMLX_API_MAX_TOOLCALLS 8u
#define FMLX_API_DEF_MAX_TOKENS 2048 /* piano:510 — il default Anthropic, con warning */

/* ---- 1. Catalogo errori: UNA lista interna, DUE formati su filo (piano:513-518) ---- */

typedef enum {
    FMLX_API_OK = 0,
    FMLX_API_INVALID_REQUEST = 1,   /* 400 */
    FMLX_API_UNAUTHORIZED = 2,      /* 401 */
    FMLX_API_MODEL_NOT_FOUND = 3,   /* 404 — il modello richiesto non è caricato */
    FMLX_API_NOT_FOUND = 4,         /* 404 — la ROTTA non esiste */
    FMLX_API_CONTEXT_LENGTH = 5,    /* 400 — con tokens_requested/tokens_available */
    FMLX_API_RESOURCE_EXHAUSTED = 6,/* 429 — con Retry-After */
    FMLX_API_MEMORY_GUARD = 7,      /* 409 — "non entra in RAM": byte + suggerimento */
    FMLX_API_UPSTREAM_HUB = 8,      /* 502 */
    FMLX_API_INTERNAL = 9,          /* 500 */
    FMLX_API_NOT_SUPPORTED = 10     /* 501 — "non lo stimo, non lo so fare" (piano:494) */
} FmlxApiErr;

const char *fmlx_api_err_str(FmlxApiErr e);
int fmlx_api_http_status(FmlxApiErr e);
/* Il nome della classe per il campo `type`. OpenAI e Anthropic NON usano le stesse
 * stringhe (piano:518): `invalid_request_error` è condiviso, gli altri no. */
const char *fmlx_api_openai_type(FmlxApiErr e);
const char *fmlx_api_anthropic_type(FmlxApiErr e);

/* `FMLX_API_NOT_FOUND` NON è nel piano: §11.5 elenca gli errori dell'elaborazione di una
 * richiesta, e una rotta inesistente non è "la richiesta è sbagliata" né "il modello non
 * c'è". I due 404 hanno cause diverse e la regola di casa (un codice per causa,
 * fmlx_http.h:13) impone di separarli. Anche `rate_limit_exceeded`, `memory_guard_refused`
 * su filo Anthropic (`overloaded_error`) e `upstream_error` sono scelte NOSTRE dove il
 * piano non dice: sono fissate qui, per iscritto, così non derivano da sole. */

typedef struct {
    FmlxApiErr err;
    const char *message;  /* ASCII, in INGLESE: va al client. I log restano in italiano. */
    const char *param;    /* il campo in colpa per OpenAI, o NULL */
    const char *code;     /* sottocodice macchina-leggibile, o NULL = il default del tipo */
    uint64_t tok_requested, tok_available; /* CONTEXT_LENGTH */
    uint64_t need_bytes;                   /* MEMORY_GUARD */
    int retry_after;                       /* RESOURCE_EXHAUSTED in secondi, 0 = assente */
} FmlxApiErrCtx;

/* Scrivono il CORPO completo dell'errore nel writer (radice compresa). */
FmlxApiErr fmlx_api_render_error_openai(const FmlxApiErrCtx *e, FmlxJsonW *w);
FmlxApiErr fmlx_api_render_error_anthropic(const FmlxApiErrCtx *e, FmlxJsonW *w);
/* Il corpo dentro la testata: content-type application/json + Content-Length coerente. */
FmlxApiErr fmlx_api_error_response(const FmlxApiErrCtx *e, int anthropic,
                                   char *mem, size_t cap, size_t *out_len);

/* ---- 2. Il confine col motore: eventi neutri, niente HTTP, niente JSON ---- */

typedef enum {
    FMLX_API_FINISH_STOP = 0,   /* fine turno */
    FMLX_API_FINISH_LENGTH = 1, /* limite token */
    FMLX_API_FINISH_TOOL = 2,   /* chiamata di tool */
    FMLX_API_FINISH_STOPSEQ = 3 /* stop sequence colpita */
} FmlxApiFinish;

/* Le DUE vocabole del filo (piano:501-504): `stop`/`length`/... e
 * `end_turn`/`max_tokens`/... — non esiste un terzo modo di scriverle. */
const char *fmlx_api_finish_openai(FmlxApiFinish f);
const char *fmlx_api_finish_anthropic(FmlxApiFinish f);
/* 1 se `s` è una delle vocabole OpenAI riconosciute e la scrive in `*out`; 0 su valori
 * che nessun client emette, e il chiamante li tratta come errore interno, non come "stop". */
int fmlx_api_finish_from_openai(const char *s, FmlxApiFinish *out);

typedef enum {
    FMLX_API_EV_TEXT = 0,      /* testo del messaggio */
    FMLX_API_EV_THINK = 1,     /* ragionamento: MAI un evento con n==0 (piano:508) */
    FMLX_API_EV_TOOL_NAME = 2, /* apre un tool call: id+name, gli argomenti vengono dopo */
    FMLX_API_EV_TOOL_ARGS = 3, /* frammento di STRINGA JSON per il tool corrente */
    FMLX_API_EV_DONE = 4       /* chiude: finish + conteggi token */
} FmlxApiEvKind;

typedef struct {
    FmlxApiEvKind ev;
    const char *text; /* TEXT/THINK/TOOL_ARGS: NON terminata, `n` fa parte del valore */
    uint32_t n;
    const char *tool_id, *tool_name; /* TOOL_NAME: terminate, o NULL */
    FmlxApiFinish finish;            /* DONE */
    const char *stop_sequence;       /* DONE se finish == STOPSEQ, altrimenti NULL */
    uint64_t prompt_tokens, completion_tokens, cached_tokens; /* DONE */
} FmlxApiEvent;

typedef enum {
    FMLX_API_GEN_MORE = 0, /* un evento prodotto in *ev */
    FMLX_API_GEN_END = 1,  /* generazione finita normalmente (*ev non toccato) */
    FMLX_API_GEN_ERR = 2   /* il motore ha fallito: *err dice cosa dire al client */
} FmlxApiGenSt;

/* Il layer ① implementa questo. Restituisce GLI EVENTI, non i chunk: decidono gli adapter
 * se un evento diventa un `data:` o un campo di un oggetto già chiuso. */
typedef FmlxApiGenSt (*FmlxApiNext)(void *gen_ctx, FmlxApiEvent *ev, FmlxApiErrCtx *err);

/* ---- 3. La richiesta normalizzata: PARAMETRI normali, CONTENUTO com'è arrivato ---- */

/* Cosa è normalizzato e cosa no, e perché. I parametri di campionamento hanno una semantica
 * comune (piano:511: "range e default non coincidono: si normalizza esplicitamente") e
 * vanno normalizzati. La forma dei messaggi NO: `content` OpenAI (stringa o array) e i
 * blocchi Anthropic (text/tool_use/tool_result) sono due strutture diverse e una
 * conversione non implementata è meglio di una conversione dichiarata e approssimativa.
 * L'adapter che riceve una richiesta in un dialetto e risponde nell'ALTRO non esiste:
 * `/v1/chat/completions` risponde in OpenAI, `/v1/messages` in Anthropic. */
typedef struct {
    FmlxSlice model;
    const FmlxJsonValue *messages; /* ARR, o NULL */
    const FmlxJsonValue *system;   /* il campo `system` di Anthropic: STR o ARR, o NULL */
    /* OpenAI non ha un campo system: il ruolo di sistema È un messaggio, e può essercene
     * più d'uno (piano:507). Si tengono i puntatori, non una copia: la memoria del
     * chiamante possiede il DOM. */
    const FmlxJsonValue *sys_msgs[FMLX_API_MAX_SYSTEM];
    uint32_t n_sys_msgs;
    const FmlxJsonValue *tools;    /* ARR, o NULL */
    const FmlxJsonValue *tool_choice;

    double temperature, top_p, presence_penalty, frequency_penalty;
    int64_t max_tokens, seed;
    uint32_t top_k;
    int has_temperature, has_top_p, has_top_k, has_presence, has_frequency;
    int has_max_tokens, has_seed;
    int max_tokens_defaulted; /* 1 = il default:510 applicato: va nei log */

    int stream, stream_usage; /* stream_options.include_usage */
    int want_tool;            /* tool_choice obbligato a una funzione specifica */

    FmlxSlice stop[FMLX_API_MAX_STOP];
    uint32_t n_stop;

    /* piano:474 — ciò che il client ha mandato e che NON onoriamo. Le fette puntano di
     * norma nel DOM del chiamante; quelle COPIATE (un token dell'header `anthropic-beta`,
     * che di DOM non ha) stanno in `ignored_pool`, che è parte della struct e quindi vive
     * quanto lei. Mettere una `char[]` locale in una slice sarebbe il puntatore sospeso che
     * il campo `n` non ti fa vedere. */
    FmlxSlice ignored[FMLX_API_MAX_IGNORED];
    char ignored_pool[FMLX_API_MAX_IGNORED][FMLX_API_MAX_NAME];
    uint32_t n_ignored;
    const char *warn[FMLX_API_MAX_WARN]; /* frasi per i log, in italiano, statiche */
    uint32_t n_warn;
    int overflow; /* 1 = qualcosa è stato perso per i limiti di questa struct: NON risponda 200 */
} FmlxApiChat;

void fmlx_api_chat_init(FmlxApiChat *c);
/* Aggiunge un campo alla lista dei non onorati. Ritorna 0 se `ignored` è piena: il
 * chiamante DEVE rispondere un errore, non dimenticare in silenzio. */
int fmlx_api_ignore(FmlxApiChat *c, FmlxSlice key);
/* Come `fmlx_api_ignore` ma COPIA la C string in una pool della struct: serve per ciò che
 * non arriva dal DOM (gli header). `0` se non c'è posto o se la stringa è troppo lunga. */
int fmlx_api_ignore_copy(FmlxApiChat *c, const char *s);
int fmlx_api_warn(FmlxApiChat *c, const char *msg);

/* Testo di sistema unico per il layer ①: campo `system` di Anthropic (stringa o array di
 * blocchi) e messaggi `role:system` di OpenAI, uniti con `\n\n` (piano:507). Se il nodo è
 * già una sola stringa, `*out` PUNTA nel DOM e `mem` non è toccata; serve memoria solo
 * quando di blocchi ce ne sono più d'uno. `FMLX_API_INTERNAL` se i byte non entrano in
 * `mem`: mai un prompt di sistema tagliato, perché il modello risponderebbe a metà delle
 * istruzioni senza che nessuno se ne accorga. */
FmlxApiErr fmlx_api_join_system(const FmlxApiChat *c, char *mem, size_t cap, FmlxSlice *out);

/* ---- 4. L'accumulatore: la risposta del motore messa in forma ---- */

/* È qui che vive il pezzo delicato del piano: gli argomenti dei tool ARRIVANO come
 * stringa (OpenAI) e vanno emessi come oggetto (Anthropic), "buffer dentro ②, flush a
 * blocco chiuso" (piano:506). `FmlxApiAcc` è quel buffer. */
typedef struct {
    char *text;
    size_t text_cap, text_len;
    char *think;
    size_t think_cap, think_len;
    struct {
        char id[FMLX_API_MAX_NAME];
        char name[FMLX_API_MAX_NAME];
        char *args;
        size_t args_cap, args_len;
        int closed; /* 1 = il blocco è chiuso: gli argomenti sono definitivi */
    } tool[FMLX_API_MAX_TOOLCALLS];
    int ntool;
    FmlxApiFinish finish;
    int have_finish;
    char stop_seq[FMLX_API_MAX_NAME];
    uint64_t prompt_tokens, completion_tokens, cached_tokens;
    int overflow; /* 1 = un buffer non è bastato: la risposta NON è inviable */
} FmlxApiAcc;

void fmlx_api_acc_init(FmlxApiAcc *a, char *text, size_t text_cap, char *think, size_t think_cap,
                       char *args, size_t args_cap_each);

/* Accoda un evento. FMLX_API_INTERNAL se i buffer non bastano (`overflow` posto, gli
 * eventi successivi non cambiano nulla: la risposta è già non inviable). */
FmlxApiErr fmlx_api_acc_push(FmlxApiAcc *a, const FmlxApiEvent *ev);

/* Chiude l'ultimo blocco tool e dice se i suoi argomenti sono JSON valido.
 * Ritorna 1 = valido ed emettibile, 0 = NON emettere il tool_use (piano:505: se il JSON è
 * troncato si risponde `max_tokens` e nessun blocco parziale, MAI un `input` a metà). */
int fmlx_api_acc_tool_ok(const FmlxApiAcc *a, int i);

/* Il passaggio che dà il nome al pezzo: `arguments` (stringa) -> `input` (oggetto) senza
 * manipolare byte a mano — si parsea e si renderizza dal DOM. */
FmlxApiErr fmlx_api_tool_input(FmlxJsonW *w, const char *args, size_t len);

/* ---- 5. Router (piano:453-461) ---- */

typedef enum {
    FMLX_API_ACT_NONE = 0,
    FMLX_API_ACT_MODELS,        /* GET  /v1/models */
    FMLX_API_ACT_MODELS_STATUS, /* GET  /v1/models/status */
    FMLX_API_ACT_LOAD,          /* POST /v1/models/load */
    FMLX_API_ACT_UNLOAD,        /* POST /v1/models/unload */
    FMLX_API_ACT_HEALTH,        /* GET  /health */
    FMLX_API_ACT_METRICS,       /* GET  /metrics */
    FMLX_API_ACT_CHAT,          /* POST /v1/chat/completions */
    FMLX_API_ACT_COMPLETIONS,   /* POST /v1/completions */
    FMLX_API_ACT_RESPONSES,     /* POST /v1/responses */
    FMLX_API_ACT_MESSAGES,      /* POST /v1/messages */
    FMLX_API_ACT_COUNT_TOKENS,  /* POST /v1/messages/count_tokens */
    FMLX_API_ACT_EMBEDDINGS     /* POST /v1/embeddings */
} FmlxApiAction;

/* `*act` = NONE e FMLX_API_NOT_FOUND se la rotta non c'è. Il metodo sbagliato su una rotta
 * che esiste è FMLX_API_INVALID_REQUEST: è un client che sa dov'è la rotta ed è la
 * differenza fra "non la trovo" e "la sto usando male". */
FmlxApiErr fmlx_api_route(FmlxSlice method, FmlxSlice target, FmlxApiAction *act);
int fmlx_api_is_anthropic(FmlxApiAction act);

/* ---- 6. Adapter OpenAI ---- */

typedef struct {
    const char *id;       /* es. "chatcmpl-7f3a..." — generato dal trasporto, non qui */
    int64_t created;      /* unix epoch */
    /* Il prompt GIÀ contato dal layer ① prima di generare: `message_start` Anthropic lo
     * riporta, e 0 deve poter significare "non contato", non "prompt vuoto". */
    uint64_t prompt_tokens_known;
} FmlxApiMeta;

/* Interpreta il corpo (già parseato) come chat OpenAI. Gli errori sono quelli del piano:
 * `invalid_request` con `param` che nomina il campo. */
FmlxApiErr fmlx_api_openai_parse(const FmlxJsonValue *root, FmlxApiChat *c, FmlxApiErrCtx *err);
/* Il corpo di risposta non-stream. */
FmlxApiErr fmlx_api_openai_completion(const FmlxApiChat *c, const FmlxApiMeta *m,
                                      const FmlxApiAcc *a, FmlxJsonW *w);
/* Head + chunk + `data: [DONE]` (piano:471). Scrive SU `w`: la testata SSE la scrive il
 * trasportatore PRIMA, con `fmlx_api_sse_head`. */
FmlxApiErr fmlx_api_openai_stream(const FmlxApiChat *c, const FmlxApiMeta *m, FmlxApiNext next,
                                  void *gen_ctx, FmlxHttpRes *w);

/* `/v1/completions` (testo grezzo): `object:"text_completion"`, `choices[].text`. Stessa
 * disciplina del chat (DONE, [DONE], chiusura chunked) ma il formato è quello legacy. */
FmlxApiErr fmlx_api_openai_completion_text(const FmlxApiChat *c, const FmlxApiMeta *m,
                                           const FmlxApiAcc *a, FmlxJsonW *w);
FmlxApiErr fmlx_api_openai_stream_text(const FmlxApiChat *c, const FmlxApiMeta *m, FmlxApiNext next,
                                       void *gen_ctx, FmlxHttpRes *w);

/* ---- 7. Adapter Anthropic ---- */

FmlxApiErr fmlx_api_anthropic_parse(const FmlxJsonValue *root, FmlxApiChat *c, FmlxApiErrCtx *err);
FmlxApiErr fmlx_api_anthropic_message(const FmlxApiChat *c, const FmlxApiMeta *m,
                                      const FmlxApiAcc *a, FmlxJsonW *w);
/* La sequenza OBBLIGATORIA (piano:489-492): message_start → (content_block_start →
 * content_block_delta… → content_block_stop)* → message_delta → message_stop. */
FmlxApiErr fmlx_api_anthropic_stream(const FmlxApiChat *c, const FmlxApiMeta *m, FmlxApiNext next,
                                     void *gen_ctx, FmlxHttpRes *w);
/* L'evento `error` dopo un `message_start` valido (piano:493). */
FmlxApiErr fmlx_api_anthropic_sse_error(const FmlxApiErrCtx *e, FmlxHttpRes *w);
/* `ping` (piano:491): un evento di keepalive. Chi lo scriva e quando è del trasportatore
 * (solo lui sa da quanto non passa un byte); qui c'è la FORMA, che deve essere identica a
 * come la vede un client Anthropic: `event: ping` + `data: {"type":"ping"}`. */
FmlxApiErr fmlx_api_anthropic_ping(FmlxHttpRes *w);
/* `POST /v1/messages/count_tokens` (piano:494): il conteggio DEVE usare lo stesso render
 * del template del modello. Il render del template vive nel layer ① e non c'è ancora, e
 * una STIMA avrebbe l'aspetto di un numero esatto. Questa funzione riempie `err` con
 * `not_supported` e ritorna quell'errore: rifiutare è l'unico comportamento onesto finché
 * il tokenizzatore non è qui. NON c'è un ramo che conta caratteri diviso tre. */
FmlxApiErr fmlx_api_anthropic_count_tokens(FmlxApiErrCtx *err);
/* `anthropic-version` (piano:486): 0 = assente (il server applica il default e DEVE
 * loggare un warning), 1 = presente e compresa, -1 = presente e non compresa. */
int fmlx_api_anthropic_version(FmlxSlice v);
#define FMLX_API_ANTHROPIC_VERSION_DEFAULT "2023-06-01"
/* `anthropic-beta` (piano:487): i token non nella whitelist finiscono in `ignored_params`
 * con il prefisso del header, così il client vede QUALE bandiera non è attiva. */
void fmlx_api_anthropic_beta(FmlxSlice beta, FmlxApiChat *c);

/* ---- 8. Pezzi condivisi dagli adapter (e dal CLI, più sotto) ---- */

/* Confronto ESATTO slice/letterale: i valori delle API sono sensibili alla maiuscola
 * (`role:"System"` non è un ruolo), a differenza degli header HTTP. */
int fmlx_api_streq(FmlxSlice s, const char *lit);
/* La testata di uno stream: text/event-stream + no-cache + chunked, e lascia `w` pronto
 * per gli `sse_write`. */
FmlxHttpStatus fmlx_api_sse_head(FmlxHttpRes *w);
/* `ignored_params[]` (piano:474) — assente se vuoto: un array vuoto direbbe "il client non
 * ha mandato niente di sconosciuto", che è un'informazione diversa da "non glielo dico". */
FmlxApiErr fmlx_api_write_ignored(FmlxJsonW *w, const FmlxApiChat *c);

#ifdef __cplusplus
}
#endif

#endif /* FMLX_API_H */
