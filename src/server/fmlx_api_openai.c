/* fmlx_api_openai.c — il dialetto OpenAI: `/v1/chat/completions` in ingresso e in uscita.
 *
 * Due contratti governano questo file.
 *
 * 1. **Nessun campo accettato in silenzio** (piano:474). Ogni chiave del corpo che non
 *    onoriamo finisce in `ignored_params[]` della risposta. La lista di ciò che è onorato
 *    è HONORED qui sotto e corrisponde a piano:471-473, campo per campo: `n` è onorato solo
 *    nel valore 1, perché `n:2` con una sola scelta in risposta sarebbe proprio l'output
 *    incoerente che il piano vieta.
 *
 * 2. **Un tipo sbagliato è un errore, non un valore approssimato.** `max_tokens: 1.5`
 *    ritorna `invalid_request` con `param:"max_tokens"` (è il caso scritto in
 *    fmlx_json.h:8-10: il parser distingue INT da REAL, e qui la distinzione diventa il
 *    protocollo). `temperature:"0.7"` (stringa) idem.
 *
 * Gli argomenti dei tool: OpenAI li vuole STRINGA (`function.arguments`), Anthropic OGGETTO.
 * La stringa è il formato nativo di questo file; il ponte è `fmlx_api_tool_input`.
 */
#include "fmlx/fmlx_api.h"

#include <string.h>
#include <stdio.h>

#define OAI_CHUNK_CAP 8192u  /* un frame di stream: un token, non una risposta intera */
#define OAI_TEXT_CAP 8192u   /* il testo accumulato dallo stream */
#define OAI_THINK_CAP 4096u
#define OAI_ARGS_CAP 2048u   /* glia rgomenti di UN singolo tool call */

/* Le chiavi che questo adapter LEGGE e ONORA. Tutto il resto del corpo è ignoto →
 * `ignored_params`. `response_format` è onorato per i due tipi che sappiamo servire
 * (`json_object`/`json_schema`) con l'iniezione di un'istruzione nel prompt e la
 * post-validazione: NON è grammatica nativa (l'ABI non la espone) e va detto.
 * `logprobs` è ONORATO dall'ABI v2 (logprob del token scelto); `top_logprobs:K>0` è ONORATO
 * dall'ABI v3 `token_topk` (le K alternative per token, con `logprobs:true` obbligatorio).
 * `logit_bias` e `user` restano ignorati e DICHIARATI. */
static const char *HONORED[] = {"messages",      "model",     "stream",
                                "stream_options", "temperature", "top_p",
                                "max_tokens",     "max_completion_tokens", "stop",
                                "n",              "presence_penalty", "frequency_penalty",
                                "tools",          "tool_choice", "seed",
                                "response_format", "logprobs", "top_logprobs"};
#define HONORED_N ((int)(sizeof HONORED / sizeof HONORED[0]))

static int is_honored(const char *k, size_t n) {
    for (int i = 0; i < HONORED_N; i++)
        if (strlen(HONORED[i]) == n && !memcmp(HONORED[i], k, n)) return 1;
    return 0;
}

static int already_ignored(const FmlxApiChat *c, const char *k, size_t n) {
    for (uint32_t i = 0; i < c->n_ignored; i++)
        if (c->ignored[i].n == n && !memcmp(c->ignored[i].p, k, n)) return 1;
    return 0;
}

static int str_of(const FmlxJsonValue *v, FmlxSlice *out) {
    const char *p;
    size_t n;
    if (!fmlx_json_as_str(v, &p, &n)) return 0;
    if (n > 0xffffffffu) return 0; /* un content da 4 GB non è un messaggio */
    out->p = p;
    out->n = (uint32_t)n;
    return 1;
}

static void set_err(FmlxApiErrCtx *err, FmlxApiErr e, const char *msg, const char *param) {
    if (!err) return;
    memset(err, 0, sizeof *err);
    err->err = e;
    err->message = msg;
    err->param = param;
}

/* Un numero che il protocollo vuole come reale. `as_f64` accetta INT e REAL (4 va bene),
 * rifiuta stringhe e bool. */
static int num(const FmlxJsonValue *v, double *out) { return fmlx_json_as_f64(v, out); }

/* Un numero che il protocollo vuole come INTERO: qui `as_f64` non basta, e la differenza è
 * il caso `max_tokens: 1.5` che fmlx_json.h:8 promette di non troncare. */
static int integer(const FmlxJsonValue *v, int64_t *out) { return fmlx_json_as_i64(v, out); }

/* `logprobs`/`top_logprobs`, comuni a chat e completions. `logprobs:true` chiede la logprob del
 * token scelto (ABI v2). `top_logprobs:K>0` chiede le K alternative per token (ABI v3 `token_topk`)
 * e, come in OpenAI, esige `logprobs:true`; K oltre il tetto servibile è un 400 esplicito. Il motore
 * che non espone `token_topk` non è un problema di parsing: lo scopre il layer ① al decode. */
FmlxApiErr fmlx_api_openai_logprobs(const FmlxJsonValue *root, FmlxApiChat *c, FmlxApiErrCtx *err) {
    const FmlxJsonValue *v;

    if (!root || !c) return FMLX_API_INTERNAL;
    v = fmlx_json_obj_get(root, "logprobs");
    if (v && !fmlx_json_is_null(v)) {
        int b = 0;
        if (!fmlx_json_as_bool(v, &b)) {
            set_err(err, FMLX_API_INVALID_REQUEST, "logprobs must be a boolean", "logprobs");
            return FMLX_API_INVALID_REQUEST;
        }
        c->want_logprobs = b;
    }
    v = fmlx_json_obj_get(root, "top_logprobs");
    if (v && !fmlx_json_is_null(v)) {
        int64_t k = 0;
        if (!integer(v, &k) || k < 0) {
            set_err(err, FMLX_API_INVALID_REQUEST, "top_logprobs must be a non-negative integer",
                    "top_logprobs");
            return FMLX_API_INVALID_REQUEST;
        }
        if (k > 0 && !c->want_logprobs) {
            set_err(err, FMLX_API_INVALID_REQUEST,
                    "top_logprobs requires logprobs:true", "top_logprobs");
            return FMLX_API_INVALID_REQUEST;
        }
        if (k > (int64_t)FMLX_API_MAX_TOP_K) {
            set_err(err, FMLX_API_INVALID_REQUEST, "top_logprobs must be at most 20", "top_logprobs");
            return FMLX_API_INVALID_REQUEST;
        }
        c->want_top_logprobs = (int)k;
    }
    return FMLX_API_OK;
}

FmlxApiErr fmlx_api_openai_parse(const FmlxJsonValue *root, FmlxApiChat *c, FmlxApiErrCtx *err) {
    if (!c) return FMLX_API_INTERNAL;
    fmlx_api_chat_init(c);
    if (!root || root->type != FMLX_JSON_OBJ) {
        set_err(err, FMLX_API_INVALID_REQUEST, "the request body must be a JSON object", NULL);
        return FMLX_API_INVALID_REQUEST;
    }

    /* --- passée 1: i campi sconosciuti, prima di validare. Un corpo con DUE problemi
     * (campo ignoto + temperatura fuori range) deve dare l'errore di validazione, che è
     * quello che blocca la richiesta; `ignored_params` sopravvive e si riporta se la
     * richiesta diventa valida. --- */
    for (size_t i = 0; i < fmlx_json_obj_n(root); i++) {
        const FmlxJsonValue *k = fmlx_json_obj_key_at(root, i);
        const char *kp;
        size_t kn;
        if (!fmlx_json_as_str(k, &kp, &kn)) continue;
        if (is_honored(kp, kn) || already_ignored(c, kp, kn)) continue;
        if (!fmlx_api_ignore(c, (FmlxSlice){kp, (uint32_t)kn})) {
            set_err(err, FMLX_API_INVALID_REQUEST, "too many unrecognized parameters", NULL);
            return FMLX_API_INVALID_REQUEST;
        }
    }

    const FmlxJsonValue *v;

    v = fmlx_json_obj_get(root, "model");
    if (!str_of(v, &c->model) || c->model.n == 0) {
        set_err(err, FMLX_API_INVALID_REQUEST, "model is required and must be a non-empty string",
                "model");
        return FMLX_API_INVALID_REQUEST;
    }

    v = fmlx_json_obj_get(root, "messages");
    if (!v || v->type != FMLX_JSON_ARR || fmlx_json_arr_n(v) == 0) {
        set_err(err, FMLX_API_INVALID_REQUEST, "messages is required and must be a non-empty array",
                "messages");
        return FMLX_API_INVALID_REQUEST;
    }
    c->messages = v;
    /* `system` OpenAI NON è un campo: è un messaggio con role system/developer. Li isoliamo
     * qui perché il layer ① riceve un prompt di sistema e una conversazione separati, e
     * più messaggi di sistema si concatenano con `\n\n` (piano:507): `join_system` lo fa,
     * qui si raccolgono solo i puntatori. */
    for (size_t i = 0; i < fmlx_json_arr_n(v); i++) {
        const FmlxJsonValue *m = fmlx_json_arr_at(v, i);
        const FmlxJsonValue *r = fmlx_json_obj_get(m, "role");
        FmlxSlice role;
        if (!str_of(r, &role)) continue;
        if (!fmlx_api_streq(role, "system") && !fmlx_api_streq(role, "developer")) continue;
        if (c->n_sys_msgs >= FMLX_API_MAX_SYSTEM) {
            set_err(err, FMLX_API_INVALID_REQUEST, "too many system messages", "messages");
            return FMLX_API_INVALID_REQUEST;
        }
        c->sys_msgs[c->n_sys_msgs++] = fmlx_json_obj_get(m, "content");
    }

    v = fmlx_json_obj_get(root, "stream");
    if (v && !fmlx_json_is_null(v)) {
        int b;
        if (fmlx_json_as_bool(v, &b)) c->stream = b;
        else {
            set_err(err, FMLX_API_INVALID_REQUEST, "stream must be a boolean", "stream");
            return FMLX_API_INVALID_REQUEST;
        }
    }
    v = fmlx_json_obj_get(root, "stream_options");
    if (v && v->type == FMLX_JSON_OBJ) {
        const FmlxJsonValue *iu = fmlx_json_obj_get(v, "include_usage");
        int b;
        if (iu && fmlx_json_as_bool(iu, &b)) c->stream_usage = b;
    }

    v = fmlx_json_obj_get(root, "temperature");
    if (v && !fmlx_json_is_null(v)) {
        if (!num(v, &c->temperature) || c->temperature < 0.0 || c->temperature > 2.0) {
            set_err(err, FMLX_API_INVALID_REQUEST, "temperature must be a number in [0, 2]",
                    "temperature");
            return FMLX_API_INVALID_REQUEST;
        }
        c->has_temperature = 1;
    }
    v = fmlx_json_obj_get(root, "top_p");
    if (v && !fmlx_json_is_null(v)) {
        if (!num(v, &c->top_p) || c->top_p <= 0.0 || c->top_p > 1.0) {
            set_err(err, FMLX_API_INVALID_REQUEST, "top_p must be a number in (0, 1]", "top_p");
            return FMLX_API_INVALID_REQUEST;
        }
        c->has_top_p = 1;
    }
    v = fmlx_json_obj_get(root, "presence_penalty");
    if (v) {
        if (!num(v, &c->presence_penalty) || c->presence_penalty < -2.0 || c->presence_penalty > 2.0) {
            set_err(err, FMLX_API_INVALID_REQUEST,
                    "presence_penalty must be a number in [-2, 2]", "presence_penalty");
            return FMLX_API_INVALID_REQUEST;
        }
        c->has_presence = 1;
    }
    v = fmlx_json_obj_get(root, "frequency_penalty");
    if (v) {
        if (!num(v, &c->frequency_penalty) || c->frequency_penalty < -2.0 ||
            c->frequency_penalty > 2.0) {
            set_err(err, FMLX_API_INVALID_REQUEST,
                    "frequency_penalty must be a number in [-2, 2]", "frequency_penalty");
            return FMLX_API_INVALID_REQUEST;
        }
        c->has_frequency = 1;
    }
    v = fmlx_json_obj_get(root, "seed");
    if (v && !fmlx_json_is_null(v)) {
        if (!integer(v, &c->seed)) {
            set_err(err, FMLX_API_INVALID_REQUEST, "seed must be an integer", "seed");
            return FMLX_API_INVALID_REQUEST;
        }
        c->has_seed = 1;
    }

    /* I due nomi del limite (piano:510): `max_completion_tokens` è quello che OpenAI usa
     * oggi per i modelli reasoning, `max_tokens` è il vecchio. Se il client manda ENTRAMBI
     * con valori diversi vince il nuovo e il vecchio va in `ignored_params`: è letteralmente
     * un campo che non abbiamo onorato, e dirlo è l'unico comportamento non menzognero. */
    const FmlxJsonValue *mt = fmlx_json_obj_get(root, "max_tokens");
    const FmlxJsonValue *mct = fmlx_json_obj_get(root, "max_completion_tokens");
    const FmlxJsonValue *pick = mct ? mct : mt;
    if (pick && !fmlx_json_is_null(pick)) {
        if (!integer(pick, &c->max_tokens) || c->max_tokens <= 0) {
            set_err(err, FMLX_API_INVALID_REQUEST,
                    "max_tokens must be a positive integer",
                    pick == mct ? "max_completion_tokens" : "max_tokens");
            return FMLX_API_INVALID_REQUEST;
        }
        c->has_max_tokens = 1;
        if (mct && mt) {
            int64_t other;
            if (integer(mt, &other) && other != c->max_tokens)
                fmlx_api_ignore(c, (FmlxSlice){"max_tokens", 10});
        }
    }

    v = fmlx_json_obj_get(root, "n");
    if (v && !fmlx_json_is_null(v)) {
        int64_t n;
        if (!integer(v, &n) || n < 1) {
            set_err(err, FMLX_API_INVALID_REQUEST, "n must be a positive integer", "n");
            return FMLX_API_INVALID_REQUEST;
        }
        if (n != 1) {
            set_err(err, FMLX_API_INVALID_REQUEST,
                    "n greater than 1 is not supported: this server returns one choice", "n");
            return FMLX_API_INVALID_REQUEST;
        }
    }

    v = fmlx_json_obj_get(root, "stop");
    if (v && !fmlx_json_is_null(v)) {
        if (v->type == FMLX_JSON_STR) {
            if (!str_of(v, &c->stop[0])) {
                set_err(err, FMLX_API_INVALID_REQUEST, "stop must be a string or an array", "stop");
                return FMLX_API_INVALID_REQUEST;
            }
            c->n_stop = 1;
        } else if (v->type == FMLX_JSON_ARR) {
            if (fmlx_json_arr_n(v) > FMLX_API_MAX_STOP) {
                set_err(err, FMLX_API_INVALID_REQUEST, "too many stop sequences", "stop");
                return FMLX_API_INVALID_REQUEST;
            }
            for (size_t i = 0; i < fmlx_json_arr_n(v); i++)
                if (!str_of(fmlx_json_arr_at(v, i), &c->stop[i])) {
                    set_err(err, FMLX_API_INVALID_REQUEST,
                            "each stop sequence must be a string", "stop");
                    return FMLX_API_INVALID_REQUEST;
                }
            c->n_stop = (uint32_t)fmlx_json_arr_n(v);
        } else {
            set_err(err, FMLX_API_INVALID_REQUEST, "stop must be a string or an array", "stop");
            return FMLX_API_INVALID_REQUEST;
        }
    }

    v = fmlx_json_obj_get(root, "tools");
    if (v && v->type == FMLX_JSON_ARR && fmlx_json_arr_n(v)) c->tools = v;
    v = fmlx_json_obj_get(root, "tool_choice");
    if (v && !fmlx_json_is_null(v)) {
        c->tool_choice = v;
        if (v->type == FMLX_JSON_OBJ) {
            const FmlxJsonValue *fn = fmlx_json_obj_get(v, "function");
            if (fn && fmlx_json_obj_get(fn, "name")) c->want_tool = 1;
        }
    }

    /* `response_format`: i due tipi onorabili. `json_schema` esige uno `schema` oggetto,
     * altrimenti non c'è niente da chiedere al modello. Un tipo ignoto è un errore, non un
     * "testo": il client crede di avere JSON e riceve prosa. */
    v = fmlx_json_obj_get(root, "response_format");
    if (v && !fmlx_json_is_null(v)) {
        const FmlxJsonValue *ty;
        const char *tp = NULL;
        size_t tn = 0;
        if (v->type != FMLX_JSON_OBJ || !(ty = fmlx_json_obj_get(v, "type")) ||
            !fmlx_json_as_str(ty, &tp, &tn)) {
            set_err(err, FMLX_API_INVALID_REQUEST,
                    "response_format must be an object with a string type", "response_format");
            return FMLX_API_INVALID_REQUEST;
        }
        if (tn == 11 && !memcmp(tp, "json_object", 11)) {
            c->response_format = v;
            c->rf_json_object = 1;
        } else if (tn == 11 && !memcmp(tp, "json_schema", 11)) {
            const FmlxJsonValue *js = fmlx_json_obj_get(v, "json_schema");
            const FmlxJsonValue *sch = js ? fmlx_json_obj_get(js, "schema") : NULL;
            if (!js || js->type != FMLX_JSON_OBJ || !sch || sch->type != FMLX_JSON_OBJ) {
                set_err(err, FMLX_API_INVALID_REQUEST,
                        "response_format.json_schema.schema must be a JSON object",
                        "response_format");
                return FMLX_API_INVALID_REQUEST;
            }
            c->response_format = v;
            c->rf_json_schema = 1;
        } else if (!(tn == 4 && !memcmp(tp, "text", 4))) {
            set_err(err, FMLX_API_INVALID_REQUEST,
                    "unsupported response_format.type: use text, json_object or json_schema",
                    "response_format");
            return FMLX_API_INVALID_REQUEST;
        }
    }

    /* `logprobs` (ABI v2): onorato — il motore emette, per token, la logprob del token scelto.
     * `top_logprobs > 0` resta un 400: l'ABI v2 espone la logprob del token scelto, non la
     * distribuzione top-k, quindi non c'è modo onesto di servirla. Vedi `fmlx_api_openai_logprobs`. */
    {
        FmlxApiErr lerr = fmlx_api_openai_logprobs(root, c, err);
        if (lerr != FMLX_API_OK) return lerr;
    }
    /* Con i tool attivi il testo passa dal riconoscitore (bufferizzato): la corrispondenza
     * token↔logprob non è più esatta, quindi la combinazione è rifiutata invece che mentita. */
    if (c->want_logprobs && fmlx_api_tool_mode(c, NULL) != FMLX_API_TOOL_NONE) {
        set_err(err, FMLX_API_INVALID_REQUEST,
                "logprobs cannot be combined with tool calls on this server", "logprobs");
        return FMLX_API_INVALID_REQUEST;
    }

    if (c->overflow) {
        set_err(err, FMLX_API_INTERNAL, "request has more fields than this server tracks", NULL);
        return FMLX_API_INTERNAL;
    }
    return FMLX_API_OK;
}

/* ---- Risposta non-stream ------------------------------------------------------ */

/* Quanti `tool_calls` SARANNO emessi: TUTTI o nessuno. Non è una rifinitura — `content:
 * null` vuol dire "la risposta sta nella chiamata", e dirlo quando la chiamata viene
 * scartata produce un messaggio SENZA contenuto e SENZA chiamata: il client non ha più
 * niente da leggere e nessun errore da vedere. Serve prima di scrivere `content`, quindi
 * il conteggio sta qui e non dentro `write_tool_calls`. */
static int tools_emitted(const FmlxApiAcc *a) {
    int valid = 0;
    for (int i = 0; i < a->ntool; i++)
        if (fmlx_api_acc_tool_ok(a, i)) valid++;
    return valid == a->ntool ? valid : 0;
}

static FmlxApiErr write_tool_calls(FmlxJsonW *w, const FmlxApiAcc *a, int *emitted) {
    /* Prima si conta, poi si scrive: un `"tool_calls": []` accanto a `finish_reason:"length"`
     * direbbe al client "c'era una chiamata e non te la mostro", che è peggio di non
     * menzionare il campo. */
    int valid = tools_emitted(a);
    *emitted = valid;
    if (valid == 0) return FMLX_API_OK;
    if (fmlx_jsonw_key(w, "tool_calls") || fmlx_jsonw_arr_begin(w)) return FMLX_API_INTERNAL;
    for (int i = 0; i < a->ntool; i++) {
        /* MAI un `arguments` a metà (piano:505): una stringa JSON troncata è un input che il
         * client non può passare alla funzione. In OpenAI `arguments` è testuale e
         * "troncare" sembrerebbe legale; non lo è, perché il client fa lo stesso
         * `JSON.parse`. La regola del piano è applicata a entrambi i dialetti. */
        if (fmlx_jsonw_obj_begin(w)) return FMLX_API_INTERNAL;
        char id[FMLX_API_MAX_NAME + 8];
        if (a->tool[i].id[0]) snprintf(id, sizeof id, "%s", a->tool[i].id);
        else snprintf(id, sizeof id, "call_%d", i);
        if (fmlx_jsonw_key(w, "id") || fmlx_jsonw_cstr(w, id)) return FMLX_API_INTERNAL;
        if (fmlx_jsonw_key(w, "type") || fmlx_jsonw_cstr(w, "function")) return FMLX_API_INTERNAL;
        if (fmlx_jsonw_key(w, "function") || fmlx_jsonw_obj_begin(w)) return FMLX_API_INTERNAL;
        if (fmlx_jsonw_key(w, "name") || fmlx_jsonw_cstr(w, a->tool[i].name))
            return FMLX_API_INTERNAL;
        if (fmlx_jsonw_key(w, "arguments")) return FMLX_API_INTERNAL;
        if (fmlx_jsonw_str(w, a->tool[i].args, a->tool[i].args_len)) return FMLX_API_INTERNAL;
        if (fmlx_jsonw_obj_end(w) || fmlx_jsonw_obj_end(w)) return FMLX_API_INTERNAL;
        (*emitted)++;
    }
    if (fmlx_jsonw_arr_end(w)) return FMLX_API_INTERNAL;
    return FMLX_API_OK;
}

/* Se il motore ha chiesto un tool ma gli argomenti non sono JSON, la risposta non può
 * fingersi finita bene: diventa `length` (piano:505), che è l'unico valore che dice al
 * client "non eseguire nulla di quello che hai letto". */
static FmlxApiFinish effective_finish(const FmlxApiAcc *a, int tools_ok) {
    if (a->have_finish && a->finish == FMLX_API_FINISH_TOOL && tools_ok == 0)
        return FMLX_API_FINISH_LENGTH;
    /* Nessun DONE dal motore: `STOP` di default sarebbe inventarsi un finish_reason. Il
     * chiamante che non chiude la generazione ha un bug nel layer ①, e lo scopre dal
     * `have_finish` che NON è posto — qui si ritorna STOP solo perché il tipo non ha un
     * "non lo so"; il corpo della risposta non viene comunque emesso se `overflow` è posto. */
    return a->have_finish ? a->finish : FMLX_API_FINISH_STOP;
}

/* L'array `top_logprobs` di UNA voce: `{token, logprob, bytes}` per ogni alternativa, ordinate
 * per logprob decrescente. Vuoto quando il motore non ne ha prodotte (campo comunque presente). */
static FmlxApiErr write_top_entries(FmlxJsonW *w, const FmlxApiTopLogprob *top, int n_top) {
    if (fmlx_jsonw_key(w, "top_logprobs") || fmlx_jsonw_arr_begin(w)) return FMLX_API_INTERNAL;
    for (int i = 0; i < n_top; i++) {
        const FmlxApiTopLogprob *t = &top[i];
        if (t->n == 0) continue; /* voce non prodotta (o non rappresentabile): si omette */
        if (fmlx_jsonw_obj_begin(w)) return FMLX_API_INTERNAL;
        if (fmlx_jsonw_key(w, "token") || fmlx_jsonw_str(w, t->token, t->n)) return FMLX_API_INTERNAL;
        if (fmlx_jsonw_key(w, "logprob") || fmlx_jsonw_f64(w, (double)t->logprob))
            return FMLX_API_INTERNAL;
        if (fmlx_jsonw_key(w, "bytes") || fmlx_jsonw_arr_begin(w)) return FMLX_API_INTERNAL;
        for (uint32_t b = 0; b < t->n; b++)
            if (fmlx_jsonw_i64(w, (unsigned char)t->token[b])) return FMLX_API_INTERNAL;
        if (fmlx_jsonw_arr_end(w) || fmlx_jsonw_obj_end(w)) return FMLX_API_INTERNAL;
    }
    return fmlx_jsonw_arr_end(w) == FMLX_JSON_OK ? FMLX_API_OK : FMLX_API_INTERNAL;
}

/* Una voce di `choices[].logprobs.content[]` (OpenAI chat): token, logprob, bytes, top-k.
 * `bytes` è l'UTF-8 del token (il writer rifiuta byte non validi, e qui il testo è già UTF-8);
 * `top`/`n_top` sono le alternative dall'ABI v3 (vuoto se il motore non le espone). */
static FmlxApiErr write_chat_lp_entry(FmlxJsonW *w, const char *tok, uint32_t n, float lp,
                                      const FmlxApiTopLogprob *top, int n_top) {
    if (fmlx_jsonw_obj_begin(w)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "token") || fmlx_jsonw_str(w, tok, n)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "logprob") || fmlx_jsonw_f64(w, (double)lp)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "bytes") || fmlx_jsonw_arr_begin(w)) return FMLX_API_INTERNAL;
    for (uint32_t i = 0; i < n; i++)
        if (fmlx_jsonw_i64(w, (unsigned char)tok[i])) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_arr_end(w)) return FMLX_API_INTERNAL;
    if (write_top_entries(w, top, n_top) != FMLX_API_OK) return FMLX_API_INTERNAL;
    return fmlx_jsonw_obj_end(w) == FMLX_JSON_OK ? FMLX_API_OK : FMLX_API_INTERNAL;
}

/* `logprobs` di una risposta chat: `{"content":[...]}` se il client l'ha chiesto e il motore
 * ha prodotto token con logprob; altrimenti `null` (campo comunque presente nel contratto). */
static FmlxApiErr write_chat_logprobs(FmlxJsonW *w, const FmlxApiChat *c, const FmlxApiAcc *a) {
    if (fmlx_jsonw_key(w, "logprobs")) return FMLX_API_INTERNAL;
    if (!c->want_logprobs || a->n_logprobs == 0)
        return fmlx_jsonw_null(w) == FMLX_JSON_OK ? FMLX_API_OK : FMLX_API_INTERNAL;
    if (fmlx_jsonw_obj_begin(w) || fmlx_jsonw_key(w, "content") || fmlx_jsonw_arr_begin(w))
        return FMLX_API_INTERNAL;
    int k = c->want_top_logprobs;
    if (k > a->top_k) k = a->top_k;
    for (int i = 0; i < a->n_logprobs; i++) {
        const FmlxApiTopLogprob *top = (k > 0) ? a->top + (size_t)i * a->top_k : NULL;
        if (write_chat_lp_entry(w, a->text + a->log_off[i], a->log_len[i], a->logprob[i], top, k))
            return FMLX_API_INTERNAL;
    }
    if (fmlx_jsonw_arr_end(w) || fmlx_jsonw_obj_end(w)) return FMLX_API_INTERNAL;
    return FMLX_API_OK;
}

FmlxApiErr fmlx_api_openai_completion(const FmlxApiChat *c, const FmlxApiMeta *m,
                                      const FmlxApiAcc *a, FmlxJsonW *w) {
    if (!c || !m || !a || !w) return FMLX_API_INTERNAL;
    if (a->overflow) {
        /* Un token non entrato nel buffer = una risposta monca. Mai inviarla: il client la
         * parserebbe e la crederebbe completa. */
        return FMLX_API_INTERNAL;
    }

    int tools_ok = 0;
    if (fmlx_jsonw_obj_begin(w)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "id") || fmlx_jsonw_cstr(w, m->id ? m->id : "chatcmpl-unknown"))
        return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "object") || fmlx_jsonw_cstr(w, "chat.completion"))
        return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "created") || fmlx_jsonw_i64(w, m->created)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "model") || fmlx_jsonw_str(w, c->model.p, c->model.n))
        return FMLX_API_INTERNAL;

    if (fmlx_jsonw_key(w, "choices") || fmlx_jsonw_arr_begin(w)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_obj_begin(w)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "index") || fmlx_jsonw_i64(w, 0)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "message") || fmlx_jsonw_obj_begin(w)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "role") || fmlx_jsonw_cstr(w, "assistant")) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "content")) return FMLX_API_INTERNAL;
    if (a->text_len == 0 && tools_emitted(a) > 0) {
        /* `content: null` con i tool call è il formato reale di OpenAI; una stringa vuota
         * accanto a un tool call fa vedere al client DUE risposte diverse. Il test è su ciò
         * che VIENE EMESSO, non su ciò che il motore ha iniziato: un tool scartato per
         * argomenti troncati non autorizza un `null` senza chiamata. */
        if (fmlx_jsonw_null(w)) return FMLX_API_INTERNAL;
    } else if (fmlx_jsonw_str(w, a->text, a->text_len)) return FMLX_API_INTERNAL;
    if (write_tool_calls(w, a, &tools_ok)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_obj_end(w)) return FMLX_API_INTERNAL;
    if (write_chat_logprobs(w, c, a) != FMLX_API_OK) return FMLX_API_INTERNAL;
    FmlxApiFinish fin = effective_finish(a, tools_ok);
    if (fmlx_jsonw_key(w, "finish_reason") || fmlx_jsonw_cstr(w, fmlx_api_finish_openai(fin)))
        return FMLX_API_INTERNAL;
    if (fmlx_jsonw_obj_end(w) || fmlx_jsonw_arr_end(w)) return FMLX_API_INTERNAL;

    if (fmlx_jsonw_key(w, "usage") || fmlx_jsonw_obj_begin(w)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "prompt_tokens") || fmlx_jsonw_u64(w, a->prompt_tokens))
        return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "completion_tokens") || fmlx_jsonw_u64(w, a->completion_tokens))
        return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "total_tokens") ||
        fmlx_jsonw_u64(w, a->prompt_tokens + a->completion_tokens))
        return FMLX_API_INTERNAL;
    if (fmlx_jsonw_obj_end(w)) return FMLX_API_INTERNAL;
    if (fmlx_api_write_ignored(w, c)) return FMLX_API_INTERNAL;
    return fmlx_jsonw_obj_end(w) ? FMLX_API_INTERNAL : FMLX_API_OK;
}

/* ---- Stream ------------------------------------------------------------------ */

/* Un frame: `{"id","object":"chat.completion.chunk","created","model","choices":[{index,
 * delta, logprobs:null, finish_reason:null}]}` e, SULL'ULTIMO, `finish_reason` valorizzato
 * + `usage` se chiesto, poi il letterale `data: [DONE]` (piano:469-471). */
static FmlxApiErr openai_chunk(FmlxHttpRes *w, const FmlxApiMeta *m, const FmlxApiChat *c,
                               const char *delta_json, size_t dn, const char *finish,
                               const FmlxApiAcc *a, int usage, const FmlxApiEvent *tev) {
    char buf[OAI_CHUNK_CAP];
    FmlxJsonW j;
    fmlx_jsonw_init(&j, buf, sizeof buf, sizeof buf);
    if (fmlx_jsonw_obj_begin(&j)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(&j, "id") || fmlx_jsonw_cstr(&j, m->id ? m->id : "chatcmpl-unknown"))
        return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(&j, "object") || fmlx_jsonw_cstr(&j, "chat.completion.chunk"))
        return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(&j, "created") || fmlx_jsonw_i64(&j, m->created)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(&j, "model") || fmlx_jsonw_str(&j, c->model.p, c->model.n))
        return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(&j, "choices") || fmlx_jsonw_arr_begin(&j)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_obj_begin(&j)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(&j, "index") || fmlx_jsonw_i64(&j, 0)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(&j, "delta")) return FMLX_API_INTERNAL;
    if (delta_json && *delta_json) {
        /* Il delta è costruito fuori e arrivato come JSON già formato: `raw` lo VALIDA col
         * parser prima di incorporarlo, quindi un `{"content":` rotto non esce mai. */
        if (fmlx_jsonw_raw(&j, delta_json, dn)) return FMLX_API_INTERNAL;
    } else if (fmlx_jsonw_obj_begin(&j) || fmlx_jsonw_obj_end(&j)) return FMLX_API_INTERNAL;
    /* `logprobs` del frame: l'oggetto `{"content":[voce]}` quando l'evento porta la logprob
     * del token; `null` (assente di valore) sui frame di ruolo e finale. */
    if (fmlx_jsonw_key(&j, "logprobs")) return FMLX_API_INTERNAL;
    if (tev && tev->has_logprob) {
        if (fmlx_jsonw_obj_begin(&j) || fmlx_jsonw_key(&j, "content") || fmlx_jsonw_arr_begin(&j))
            return FMLX_API_INTERNAL;
        if (write_chat_lp_entry(&j, tev->text, tev->n, tev->logprob, tev->top, tev->n_top))
            return FMLX_API_INTERNAL;
        if (fmlx_jsonw_arr_end(&j) || fmlx_jsonw_obj_end(&j)) return FMLX_API_INTERNAL;
    } else if (fmlx_jsonw_null(&j)) {
        return FMLX_API_INTERNAL;
    }
    if (fmlx_jsonw_key(&j, "finish_reason")) return FMLX_API_INTERNAL;
    if (finish ? fmlx_jsonw_cstr(&j, finish) : fmlx_jsonw_null(&j)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_obj_end(&j) || fmlx_jsonw_arr_end(&j)) return FMLX_API_INTERNAL;
    if (usage && a) {
        if (fmlx_jsonw_key(&j, "usage") || fmlx_jsonw_obj_begin(&j)) return FMLX_API_INTERNAL;
        if (fmlx_jsonw_key(&j, "prompt_tokens") || fmlx_jsonw_u64(&j, a->prompt_tokens))
            return FMLX_API_INTERNAL;
        if (fmlx_jsonw_key(&j, "completion_tokens") || fmlx_jsonw_u64(&j, a->completion_tokens))
            return FMLX_API_INTERNAL;
        if (fmlx_jsonw_key(&j, "total_tokens") ||
            fmlx_jsonw_u64(&j, a->prompt_tokens + a->completion_tokens))
            return FMLX_API_INTERNAL;
        if (fmlx_jsonw_obj_end(&j)) return FMLX_API_INTERNAL;
    }
    /* La radice si chiude qui e NON prima: `usage` è un membro DEL frame, quindi arriva dopo
     * la `choices`. Dimenticare questa chiusura è stato il fallimento dell'intera lane di
     * streaming: `finish` rifiuta un contenitore aperto (giustamente) e il chiamante vedeva
     * un `internal` senza colpevoli. */
    if (fmlx_jsonw_obj_end(&j)) return FMLX_API_INTERNAL;
    size_t n = 0;
    if (fmlx_jsonw_finish(&j, &n)) return FMLX_API_INTERNAL;
    FmlxSlice d = {buf, (uint32_t)n};
    return fmlx_http_sse_write(w, NULL, d) == FMLX_HTTP_OK ? FMLX_API_OK : FMLX_API_INTERNAL;
}

/* Il frammento di delta per un evento. Costruito in `out` come oggetto JSON. */
/* Il frammento di delta per un evento. Costruito in `out` come oggetto JSON.
 * Ritorna uno `FmlxApiErr` e non un `int`: "ok/ko" con un codice d'errore in mezzo è il
 * modo in cui un fallimento del writer viene scambiato per un successo. */
static FmlxApiErr delta_for(FmlxJsonW *j, const FmlxApiEvent *ev, int tool_index) {
    if (fmlx_jsonw_obj_begin(j)) return FMLX_API_INTERNAL;
    switch (ev->ev) {
    case FMLX_API_EV_TEXT:
        if (fmlx_jsonw_key(j, "content") || fmlx_jsonw_str(j, ev->text, ev->n))
            return FMLX_API_INTERNAL;
        break;
    case FMLX_API_EV_THINK:
        /* `reasoning_content` è l'estensione de-fatto (piano:466, 469). Campo ASSENTE se il
         * modello non produce thinking: l'evento con n==0 non arriva nemmeno qui. */
        if (fmlx_jsonw_key(j, "reasoning_content") || fmlx_jsonw_str(j, ev->text, ev->n))
            return FMLX_API_INTERNAL;
        break;
    case FMLX_API_EV_TOOL_NAME: {
        if (fmlx_jsonw_key(j, "tool_calls") || fmlx_jsonw_arr_begin(j)) return FMLX_API_INTERNAL;
        if (fmlx_jsonw_obj_begin(j)) return FMLX_API_INTERNAL;
        if (fmlx_jsonw_key(j, "index") || fmlx_jsonw_i64(j, tool_index)) return FMLX_API_INTERNAL;
        char id[FMLX_API_MAX_NAME + 8];
        if (ev->tool_id && ev->tool_id[0]) snprintf(id, sizeof id, "%s", ev->tool_id);
        else snprintf(id, sizeof id, "call_%d", tool_index);
        if (fmlx_jsonw_key(j, "id") || fmlx_jsonw_cstr(j, id)) return FMLX_API_INTERNAL;
        if (fmlx_jsonw_key(j, "type") || fmlx_jsonw_cstr(j, "function")) return FMLX_API_INTERNAL;
        if (fmlx_jsonw_key(j, "function") || fmlx_jsonw_obj_begin(j)) return FMLX_API_INTERNAL;
        if (ev->tool_name && (fmlx_jsonw_key(j, "name") || fmlx_jsonw_cstr(j, ev->tool_name)))
            return FMLX_API_INTERNAL;
        /* `arguments:""` APRE il campo: i client che accumulano i frammenti si aspettano il
         * contenitore esistente dal primo frame del tool. */
        if (fmlx_jsonw_key(j, "arguments") || fmlx_jsonw_cstr(j, "")) return FMLX_API_INTERNAL;
        if (fmlx_jsonw_obj_end(j) || fmlx_jsonw_obj_end(j) || fmlx_jsonw_arr_end(j))
            return FMLX_API_INTERNAL;
        break;
    }
    case FMLX_API_EV_TOOL_ARGS: {
        if (fmlx_jsonw_key(j, "tool_calls") || fmlx_jsonw_arr_begin(j)) return FMLX_API_INTERNAL;
        if (fmlx_jsonw_obj_begin(j)) return FMLX_API_INTERNAL;
        if (fmlx_jsonw_key(j, "index") || fmlx_jsonw_i64(j, tool_index)) return FMLX_API_INTERNAL;
        if (fmlx_jsonw_key(j, "function") || fmlx_jsonw_obj_begin(j)) return FMLX_API_INTERNAL;
        if (fmlx_jsonw_key(j, "arguments") || fmlx_jsonw_str(j, ev->text, ev->n))
            return FMLX_API_INTERNAL;
        if (fmlx_jsonw_obj_end(j) || fmlx_jsonw_obj_end(j) || fmlx_jsonw_arr_end(j))
            return FMLX_API_INTERNAL;
        break;
    }
    case FMLX_API_EV_DONE:
        /* Il DONE non produce un delta: il `finish_reason` sta sul frame che il chiamante
         * scrive dopo. Un oggetto vuoto è ciò che il protocollo mostra, non un `null`. */
        if (fmlx_jsonw_obj_begin(j) || fmlx_jsonw_obj_end(j)) return FMLX_API_INTERNAL;
        return FMLX_API_OK;
    }
    if (fmlx_jsonw_obj_end(j)) return FMLX_API_INTERNAL;
    return FMLX_API_OK;
}

FmlxApiErr fmlx_api_openai_stream(const FmlxApiChat *c, const FmlxApiMeta *m, FmlxApiNext next,
                                  void *gen_ctx, FmlxHttpRes *w) {
    if (!c || !m || !next || !w) return FMLX_API_INTERNAL;
    /* I buffer stanno SULLO STACK del thread che serve la richiesta: il layer ② non
     * alloca. `args` è per-SINGOLO tool (`acc_init` moltiplica per l'indice): darne uno
     * condiviso da 4 KB a 8 tool avrebbe scritto fuori per 28 KB al secondo tool. */
    char text[OAI_TEXT_CAP], think[OAI_THINK_CAP];
    char args[FMLX_API_MAX_TOOLCALLS * OAI_ARGS_CAP];
    FmlxApiAcc a;
    fmlx_api_acc_init(&a, text, sizeof text, think, sizeof think, args, OAI_ARGS_CAP);

    /* Il primo frame porta il ruolo e nessun contenuto: è ciò che rende il chunk stream
     * riconosciuto dai client che aprono il messaggio alla prima delta. */
    const char ROLE[] = "{\"role\":\"assistant\"}";
    if (openai_chunk(w, m, c, ROLE, sizeof ROLE - 1, NULL, NULL, 0, NULL))
        return FMLX_API_INTERNAL;

    FmlxApiErr err = FMLX_API_OK;
    int tool_index = -1;
    int done_seen = 0;
    for (;;) {
        /* CANCELLAZIONE: se un write precedente è fallito (client disconnesso, `w->err`),
         * si smette di generare SUBITO invece di completare fino a max_tokens. */
        if (w && w->err != FMLX_HTTP_OK) { err = FMLX_API_INTERNAL; break; }
        FmlxApiEvent ev;
        memset(&ev, 0, sizeof ev);
        FmlxApiErrCtx ec;
        memset(&ec, 0, sizeof ec);
        FmlxApiGenSt gs = next(gen_ctx, &ev, &ec);
        if (gs == FMLX_API_GEN_ERR) {
            err = ec.err ? ec.err : FMLX_API_INTERNAL;
            break;
        }
        if (gs == FMLX_API_GEN_END) break;
        if (fmlx_api_acc_push(&a, &ev) != FMLX_API_OK) {
            err = FMLX_API_INTERNAL;
            break;
        }
        if (ev.ev == FMLX_API_EV_TOOL_NAME) tool_index = a.ntool - 1;
        if (ev.ev == FMLX_API_EV_DONE) {
            done_seen = 1;
            break;
        }
        /* Il writer del delta si RI-CREATO a ogni giro con `jsonw_init`, non "azzerato a
         * mano": i campi interni del writer sono il suo stato, e toccarli da fuori è il
         * modo in cui si resetta mezzo contratto (il flag `err` avvelenato, per esempio). */
        char d[OAI_CHUNK_CAP];
        FmlxJsonW dj;
        fmlx_jsonw_init(&dj, d, sizeof d, sizeof d);
        /* `!= FMLX_API_OK`, non `== 0`: `delta_for` è passato da `int` (1 = ok) a
         * `FmlxApiErr` (0 = ok) e la vecchia prova sarebbe rimasta valida SINTATTICAMENTE,
         * trattando il successo come il fallimento del primo delta. */
        if (delta_for(&dj, &ev, tool_index < 0 ? 0 : tool_index) != FMLX_API_OK) {
            err = FMLX_API_INTERNAL;
            break;
        }
        size_t dn = 0;
        if (fmlx_jsonw_finish(&dj, &dn)) {
            err = FMLX_API_INTERNAL;
            break;
        }
        if (openai_chunk(w, m, c, d, dn, NULL, NULL, 0, ev.has_logprob ? &ev : NULL)) {
            err = FMLX_API_INTERNAL;
            break;
        }
    }
    if (err == FMLX_API_OK) {
        /* Invariante dello stream: `finish_reason` non è MAI `tool_calls` se gli argomenti
         * raccolti non sono JSON valido. Un byte già partito non si ritira, quindi la
         * protezione del piano:505 diventa un vincolo sul finish_reason: il client che vede
         * `length` sa che non deve eseguire nessuna chiamata. */
        int tools_ok = 0;
        for (int i = 0; i < a.ntool; i++)
            if (fmlx_api_acc_tool_ok(&a, i)) tools_ok++;
        FmlxApiFinish fin = effective_finish(&a, tools_ok);
        if (openai_chunk(w, m, c, NULL, 0, fmlx_api_finish_openai(fin), &a,
                         c->stream_usage && done_seen, NULL))
            err = FMLX_API_INTERNAL;
    }
    if (err == FMLX_API_OK) {
        static const char DONE[] = "[DONE]";
        FmlxSlice s = {DONE, 6};
        if (fmlx_http_sse_write(w, NULL, s) != FMLX_HTTP_OK) err = FMLX_API_INTERNAL;
    }
    /* Il chunk terminale `0\r\n\r\n` va scritto ANCHE dopo un errore del motore: `[DONE]` è
     * il segnale di protocollo che dice "risposta finita bene" e quello NON va emesso, ma
     * una risposta `Transfer-Encoding: chunked` che non si chiude lascia il client attaccato
     * al socket ad aspettare byte che non arrivano. */
    if (fmlx_http_sse_end(w) != FMLX_HTTP_OK && err == FMLX_API_OK) err = FMLX_API_INTERNAL;
    return err;
}
