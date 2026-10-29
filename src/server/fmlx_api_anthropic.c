/* fmlx_api_anthropic.c — il dialetto Anthropic: `/v1/messages`.
 *
 * È il file dove i due protocolli si toccano davvero, e dove si vede se il layer ② serve.
 * Il motore emette UN testo e GLI ARGOMENTI DI UN TOOL COME STRINGA (sono token, e un token
 * è un frammento di testo); Anthropic vuole BLOCCHI TIPIZZATI in un ordine preciso, con
 * l'oggetto dentro `input`. Nessuna delle due cose è ricavabile dall'altra a byte: la
 * conversione è questo file, e il suo cuore è `fmlx_api_tool_input`.
 *
 * L'ORDINE DEGLI EVENTI NON È UNA SUGGERIMENTO (piano:489-492):
 *   message_start → (content_block_start → content_block_delta… → content_block_stop)* →
 *   message_delta → message_stop, e in errore `error` DOPO un `message_start` valido.
 * Un client Anthropic apre i blocchi per indice e li chiude quando arriva lo `stop`: saltare
 * un `content_block_stop` lascia il client in attesa per sempre, che è il modo in cui "funziona
 * su curl e si blocca su SDK".
 *
 * GLI ARGOMENTI DEI TOOL SI BUFFERIZZANO QUI (piano:506) e si emettono a blocco chiuso. È ciò
 * che rende applicabile piano:505: se il JSON è troncato, nessun `tool_use` parte e
 * `stop_reason` diventa `max_tokens`. Anthropic vera emette `input_json_delta` frammento per
 * frammento; noi emettiamo UN solo `input_json_delta` per blocco quando è completo. È una
 * divergenza VOLUTA e scritta: frammentare vorrebbe dire promettere di poter ritirare un byte
 * già partito, e non si può. Il tipo dell'evento e la forma del payload restano canonici, e i
 * client che assemblano `partial_json` non vedono la differenza.
 */
#include "fmlx/fmlx_api.h"

#include <stdio.h>
#include <string.h>

#define ANT_EV_CAP 8192u
#define ANT_TEXT_CAP 8192u
#define ANT_THINK_CAP 4096u
#define ANT_ARGS_CAP 2048u

static const char *ANT_HONORED[] = {"model",     "messages",  "system",
                                    "max_tokens", "temperature", "top_p",
                                    "top_k",      "stop_sequences", "stream",
                                    "tools",      "tool_choice", "metadata",
                                    "thinking"};
#define ANT_HONORED_N ((int)(sizeof ANT_HONORED / sizeof ANT_HONORED[0]))

/* Le beta che conosciamo (piano:487): un token sconosciuto è ignorato E loggato, non è un
 * errore. Sono le due che il piano nomina; aggiungerne una qui è l'unico modo di
 * "supportarla", e il gate è il test. */
static const char *ANT_BETA_OK[] = {"fine-grained-tool-streaming-2025-05-14",
                                    "interleaved-thinking-2025-05-14"};

static int is_honored(const char *k, size_t n) {
    for (int i = 0; i < ANT_HONORED_N; i++)
        if (strlen(ANT_HONORED[i]) == n && !memcmp(ANT_HONORED[i], k, n)) return 1;
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
    if (n > 0xffffffffu) return 0;
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

/* ---- Header --------------------------------------------------------------- */

int fmlx_api_anthropic_version(FmlxSlice v) {
    if (v.n == 0) return 0; /* assente: default + warning, NON un rifiuto */
    static const char OK0[] = "2023-06-01";
    static const char OK1[] = "2025-04-15"; /* quella dei tool streaming a grana fine */
    if (v.n == sizeof OK0 - 1 && !memcmp(v.p, OK0, v.n)) return 1;
    if (v.n == sizeof OK1 - 1 && !memcmp(v.p, OK1, v.n)) return 1;
    return -1;
}

void fmlx_api_anthropic_beta(FmlxSlice beta, FmlxApiChat *c) {
    if (!c || beta.n == 0) return;
    /* La lista è separata da virgole e può avere spazi: `anthropic-beta: a, b`. Il codec
     * HTTP ha già tolto gli spazi attorno al VALORE, non dentro la lista. */
    size_t s = 0;
    for (size_t i = 0; i <= beta.n; i++) {
        if (i != beta.n && beta.p[i] != ',') continue;
        while (s < i && (beta.p[s] == ' ' || beta.p[s] == '\t')) s++;
        size_t e = i;
        while (e > s && (beta.p[e - 1] == ' ' || beta.p[e - 1] == '\t')) e--;
        if (e > s) {
            int known = 0;
            for (size_t k = 0; k < sizeof ANT_BETA_OK / sizeof ANT_BETA_OK[0]; k++) {
                size_t kn = strlen(ANT_BETA_OK[k]);
                if (kn == e - s && !memcmp(ANT_BETA_OK[k], beta.p + s, kn)) { known = 1; break; }
            }
            /* `anthropic-beta: <token>` entra in `ignored_params` con il prefisso: il client
             * legge UNA lista sola e il prefisso dice che la colpa è dell'header, non del
             * corpo — che è l'unica differenza che può usare per correggersi. */
            if (!known) {
                char name[FMLX_API_MAX_NAME];
                int n = snprintf(name, sizeof name, "anthropic-beta:%.*s", (int)(e - s),
                                 beta.p + s);
                /* `ignore_copy`, non `ignore`: la slice di un buffer di stack morirebbe al
                 * ritorno e `ignored_params` leggerebbe sporcizia dal frame del caller. */
                if (n > 0 && (size_t)n < sizeof name) fmlx_api_ignore_copy(c, name);
            }
        }
        s = i + 1;
    }
}

/* ---- Parse ---------------------------------------------------------------- */

FmlxApiErr fmlx_api_anthropic_parse(const FmlxJsonValue *root, FmlxApiChat *c,
                                    FmlxApiErrCtx *err) {
    if (!c) return FMLX_API_INTERNAL;
    fmlx_api_chat_init(c);
    if (!root || root->type != FMLX_JSON_OBJ) {
        set_err(err, FMLX_API_INVALID_REQUEST, "the request body must be a JSON object", NULL);
        return FMLX_API_INVALID_REQUEST;
    }

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

    const FmlxJsonValue *v = fmlx_json_obj_get(root, "model");
    if (!str_of(v, &c->model) || c->model.n == 0) {
        set_err(err, FMLX_API_INVALID_REQUEST, "model is required and must be a string", "model");
        return FMLX_API_INVALID_REQUEST;
    }
    v = fmlx_json_obj_get(root, "messages");
    if (!v || v->type != FMLX_JSON_ARR || fmlx_json_arr_n(v) == 0) {
        set_err(err, FMLX_API_INVALID_REQUEST, "messages is required and must be non-empty",
                "messages");
        return FMLX_API_INVALID_REQUEST;
    }
    c->messages = v;
    v = fmlx_json_obj_get(root, "system");
    if (v && v->type != FMLX_JSON_STR && v->type != FMLX_JSON_ARR) {
        set_err(err, FMLX_API_INVALID_REQUEST, "system must be a string or an array of blocks",
                "system");
        return FMLX_API_INVALID_REQUEST;
    }
    c->system = v;

    /* `max_tokens` è OBBLIGATORIO nel protocollo Anthropic. Il piano:510 dice cosa fare
     * quando manca: default interno (FMLX_API_DEF_MAX_TOKENS) E un warning, non un errore e non "nessun limite".
     * Nessun limite è la richiesta che manda il server in out-of-memory su un modello da
     * 128k di contesto. */
    v = fmlx_json_obj_get(root, "max_tokens");
    if (!v || fmlx_json_is_null(v)) {
        c->max_tokens = FMLX_API_DEF_MAX_TOKENS;
        c->max_tokens_defaulted = 1;
        c->has_max_tokens = 1;
        fmlx_api_warn(c, "max_tokens assente: applicato il default del motore (262144; il tetto reale e' la KV) (piano:510)");
    } else {
        if (!fmlx_json_as_i64(v, &c->max_tokens) || c->max_tokens <= 0) {
            set_err(err, FMLX_API_INVALID_REQUEST,
                    "max_tokens is required and must be a positive integer", "max_tokens");
            return FMLX_API_INVALID_REQUEST;
        }
        c->has_max_tokens = 1;
    }

    v = fmlx_json_obj_get(root, "temperature");
    if (v && !fmlx_json_is_null(v)) {
        double t;
        if (!fmlx_json_as_f64(v, &t) || t < 0.0) {
            set_err(err, FMLX_API_INVALID_REQUEST, "temperature must be a number >= 0",
                    "temperature");
            return FMLX_API_INVALID_REQUEST;
        }
        /* L'intervallo Anthropic è [0, 1] e OpenAI arriva a 2 (piano:511: "range e default
         * NON coincidono: si normalizza esplicitamente e si logga il valore effettivo"). Il
         * valore effettivo è 1.0 e il warning lo dice: la normalizzazione silenziosa è
         * esattamente ciò che il piano vieta. */
        if (t > 1.0) {
            fmlx_api_warn(c, "temperature oltre 1.0: limitata a 1.0 (intervallo Anthropic)");
            t = 1.0;
        }
        c->temperature = t;
        c->has_temperature = 1;
    }
    v = fmlx_json_obj_get(root, "top_p");
    if (v && !fmlx_json_is_null(v)) {
        double p;
        if (!fmlx_json_as_f64(v, &p) || p <= 0.0 || p > 1.0) {
            set_err(err, FMLX_API_INVALID_REQUEST, "top_p must be a number in (0, 1]", "top_p");
            return FMLX_API_INVALID_REQUEST;
        }
        c->top_p = p;
        c->has_top_p = 1;
    }
    v = fmlx_json_obj_get(root, "top_k");
    if (v && !fmlx_json_is_null(v)) {
        int64_t k;
        if (!fmlx_json_as_i64(v, &k) || k < 0 || k > 1000000) {
            set_err(err, FMLX_API_INVALID_REQUEST, "top_k must be a small non-negative integer",
                    "top_k");
            return FMLX_API_INVALID_REQUEST;
        }
        c->top_k = (uint32_t)k;
        c->has_top_k = 1;
    }
    v = fmlx_json_obj_get(root, "stream");
    if (v && !fmlx_json_is_null(v)) {
        int b;
        if (!fmlx_json_as_bool(v, &b)) {
            set_err(err, FMLX_API_INVALID_REQUEST, "stream must be a boolean", "stream");
            return FMLX_API_INVALID_REQUEST;
        }
        c->stream = b;
    }
    /* `thinking`: {type:"enabled",budget_tokens:N} accende il ragionamento; "disabled" lo spegne
     * (default Anthropic = spento). budget_tokens/altri campi restano nel DOM. */
    v = fmlx_json_obj_get(root, "thinking");
    if (v && !fmlx_json_is_null(v)) {
        if (v->type != FMLX_JSON_OBJ) {
            set_err(err, FMLX_API_INVALID_REQUEST, "thinking must be an object", "thinking");
            return FMLX_API_INVALID_REQUEST;
        }
        const FmlxJsonValue *ty = fmlx_json_obj_get(v, "type");
        const char *tp = NULL;
        size_t tn = 0;
        if (ty && fmlx_json_as_str(ty, &tp, &tn)) {
            c->has_think = 1;
            c->think = (tn == 7 && !memcmp(tp, "enabled", 7));
        }
    }
    v = fmlx_json_obj_get(root, "stop_sequences");
    if (v && !fmlx_json_is_null(v)) {
        if (v->type != FMLX_JSON_ARR || fmlx_json_arr_n(v) > FMLX_API_MAX_STOP) {
            set_err(err, FMLX_API_INVALID_REQUEST, "stop_sequences must be an array of strings",
                    "stop_sequences");
            return FMLX_API_INVALID_REQUEST;
        }
        for (size_t i = 0; i < fmlx_json_arr_n(v); i++)
            if (!str_of(fmlx_json_arr_at(v, i), &c->stop[i])) {
                set_err(err, FMLX_API_INVALID_REQUEST,
                        "each stop sequence must be a string", "stop_sequences");
                return FMLX_API_INVALID_REQUEST;
            }
        c->n_stop = (uint32_t)fmlx_json_arr_n(v);
    }
    v = fmlx_json_obj_get(root, "tools");
    if (v && v->type == FMLX_JSON_ARR && fmlx_json_arr_n(v)) c->tools = v;
    v = fmlx_json_obj_get(root, "tool_choice");
    if (v && !fmlx_json_is_null(v)) {
        c->tool_choice = v;
        if (v->type == FMLX_JSON_OBJ) {
            FmlxSlice t;
            if (str_of(fmlx_json_obj_get(v, "type"), &t) && fmlx_api_streq(t, "tool"))
                c->want_tool = 1;
        }
    }
    if (c->overflow) {
        set_err(err, FMLX_API_INTERNAL, "request has more fields than this server tracks", NULL);
        return FMLX_API_INTERNAL;
    }
    return FMLX_API_OK;
}

/* ---- Blocchi di contenuto -------------------------------------------------- */

/* L'ordine dei blocchi è quello di Anthropic: prima il ragionamento, poi il testo, poi i
 * tool. Sono INDICI STABILI fra lo stream e la risposta non-stream: un client che ha
 * assemblato `content[1]` dallo stream deve trovare lo stesso oggetto nella risposta JSON. */
typedef struct {
    int think, text;      /* indice del blocco, o -1 se non c'è */
    int tool0;            /* indice del primo tool_use */
} BlkMap;

static BlkMap map_blocks(const FmlxApiAcc *a) {
    BlkMap m = {-1, -1, -1};
    int i = 0;
    if (a->think_len) m.think = i++;
    if (a->text_len) m.text = i++;
    m.tool0 = i;
    return m;
}

/* Quanti tool si POSSONO emettere: tutti, o nessuno (piano:505). "Nessuno" include il caso
 * di uno valido e uno troncato — emettere solo il valido manderebbe al client una chiamata
 * a metà del ragionamento del modello, che è il fallimento silenzioso che il piano nomina
 * quando dice che qui è dove i progetti muoiono. */
static int count_tools(const FmlxApiAcc *a) {
    int ok = 0;
    for (int i = 0; i < a->ntool; i++)
        if (fmlx_api_acc_tool_ok(a, i)) ok++;
    return (ok == a->ntool) ? ok : 0;
}

static FmlxApiFinish ant_finish(const FmlxApiAcc *a, int tools_ok) {
    if (a->have_finish && a->finish == FMLX_API_FINISH_TOOL && tools_ok == 0)
        return FMLX_API_FINISH_LENGTH;
    return a->have_finish ? a->finish : FMLX_API_FINISH_STOP;
}

static FmlxApiErr write_block(FmlxJsonW *w, const FmlxApiAcc *a, int i, const BlkMap m) {
    if (i == m.think) {
        if (fmlx_jsonw_obj_begin(w)) return FMLX_API_INTERNAL;
        if (fmlx_jsonw_key(w, "type") || fmlx_jsonw_cstr(w, "thinking")) return FMLX_API_INTERNAL;
        if (fmlx_jsonw_key(w, "thinking") ||
            fmlx_jsonw_str(w, a->think, a->think_len)) return FMLX_API_INTERNAL;
        /* `signature` è il campo che Anthropic usa per firmare il ragionamento con la SUA
         * chiave. La nostra firma non può esistere: il campo c'è (i client lo leggono per
         * validare il blocco) e il valore è vuoto, e qui è scritto il perché invece di
         * lasciarlo sembrare un dettaglio di implementazione. */
        if (fmlx_jsonw_key(w, "signature") || fmlx_jsonw_cstr(w, "")) return FMLX_API_INTERNAL;
        return fmlx_jsonw_obj_end(w) ? FMLX_API_INTERNAL : FMLX_API_OK;
    }
    if (i == m.text) {
        if (fmlx_jsonw_obj_begin(w)) return FMLX_API_INTERNAL;
        if (fmlx_jsonw_key(w, "type") || fmlx_jsonw_cstr(w, "text")) return FMLX_API_INTERNAL;
        if (fmlx_jsonw_key(w, "text") || fmlx_jsonw_str(w, a->text, a->text_len))
            return FMLX_API_INTERNAL;
        return fmlx_jsonw_obj_end(w) ? FMLX_API_INTERNAL : FMLX_API_OK;
    }
    int t = i - m.tool0;
    if (t < 0 || t >= a->ntool) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_obj_begin(w)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "type") || fmlx_jsonw_cstr(w, "tool_use")) return FMLX_API_INTERNAL;
    char id[FMLX_API_MAX_NAME + 8];
    if (a->tool[t].id[0]) snprintf(id, sizeof id, "%s", a->tool[t].id);
    else snprintf(id, sizeof id, "toolu_%d", t);
    if (fmlx_jsonw_key(w, "id") || fmlx_jsonw_cstr(w, id)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "name") || fmlx_jsonw_cstr(w, a->tool[t].name))
        return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "input")) return FMLX_API_INTERNAL;
/* IL PONTE: la stringa del motore diventa un oggetto nel filo, senza toccare byte a
 * mano — si parsea e si renderizza dal DOM. */
    if (fmlx_api_tool_input(w, a->tool[t].args, a->tool[t].args_len)) return FMLX_API_INTERNAL;
    return fmlx_jsonw_obj_end(w) ? FMLX_API_INTERNAL : FMLX_API_OK;
}

FmlxApiErr fmlx_api_anthropic_message(const FmlxApiChat *c, const FmlxApiMeta *m,
                                      const FmlxApiAcc *a, FmlxJsonW *w) {
    if (!c || !m || !a || !w) return FMLX_API_INTERNAL;
    if (a->overflow) return FMLX_API_INTERNAL;
    int tools_ok = count_tools(a);
    BlkMap map = map_blocks(a);

    if (fmlx_jsonw_obj_begin(w)) return FMLX_API_INTERNAL;
    char id[FMLX_API_MAX_NAME + 8];
    if (m->id) snprintf(id, sizeof id, "%s", m->id);
    else snprintf(id, sizeof id, "msg_%lld", (long long)m->created);
    if (fmlx_jsonw_key(w, "id") || fmlx_jsonw_cstr(w, id)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "type") || fmlx_jsonw_cstr(w, "message")) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "role") || fmlx_jsonw_cstr(w, "assistant")) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "model") || fmlx_jsonw_str(w, c->model.p, c->model.n))
        return FMLX_API_INTERNAL;

    if (fmlx_jsonw_key(w, "content") || fmlx_jsonw_arr_begin(w)) return FMLX_API_INTERNAL;
    if (map.think >= 0 && write_block(w, a, map.think, map)) return FMLX_API_INTERNAL;
    if (map.text >= 0 && write_block(w, a, map.text, map)) return FMLX_API_INTERNAL;
    for (int t = 0; t < tools_ok; t++)
        if (write_block(w, a, map.tool0 + t, map)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_arr_end(w)) return FMLX_API_INTERNAL;

    FmlxApiFinish fin = ant_finish(a, tools_ok);
    if (fmlx_jsonw_key(w, "stop_reason") || fmlx_jsonw_cstr(w, fmlx_api_finish_anthropic(fin)))
        return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "stop_sequence")) return FMLX_API_INTERNAL;
    if (fin == FMLX_API_FINISH_STOPSEQ && a->stop_seq[0] ? fmlx_jsonw_cstr(w, a->stop_seq)
                                                        : fmlx_jsonw_null(w))
        return FMLX_API_INTERNAL;

    if (fmlx_jsonw_key(w, "usage") || fmlx_jsonw_obj_begin(w)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "input_tokens") || fmlx_jsonw_u64(w, a->prompt_tokens))
        return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "output_tokens") || fmlx_jsonw_u64(w, a->completion_tokens))
        return FMLX_API_INTERNAL;
    /* `cache_read_input_tokens` = i token serviti dal prefisso (piano:509: la cache Anthropic
     * è mappata sul L2 disk-KV). Sono DUE numeri diversi da `input_tokens`: sommarli o
     * riportare lo stesso valore nelle due colonne direbbe "tutto il prompt era in cache" a
     * chi sta misurando un costo. */
    if (fmlx_jsonw_key(w, "cache_read_input_tokens") || fmlx_jsonw_u64(w, a->cached_tokens))
        return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "cache_creation_input_tokens") || fmlx_jsonw_u64(w, 0))
        return FMLX_API_INTERNAL;
    if (fmlx_jsonw_obj_end(w)) return FMLX_API_INTERNAL;
    return fmlx_jsonw_obj_end(w) ? FMLX_API_INTERNAL : FMLX_API_OK;
}

/* ---- Stream ---------------------------------------------------------------- */

static FmlxApiErr ev_begin(FmlxJsonW *j, char *buf, size_t cap, const char *type) {
    fmlx_jsonw_init(j, buf, cap, cap);
    if (fmlx_jsonw_obj_begin(j)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(j, "type") || fmlx_jsonw_cstr(j, type)) return FMLX_API_INTERNAL;
    return FMLX_API_OK;
}

static FmlxApiErr ev_end(FmlxJsonW *j, FmlxHttpRes *w, const char *event) {
    if (fmlx_jsonw_obj_end(j)) return FMLX_API_INTERNAL;
    size_t n = 0;
    if (fmlx_jsonw_finish(j, &n)) return FMLX_API_INTERNAL;
    FmlxSlice d = {j->buf, (uint32_t)n};
    return fmlx_http_sse_write(w, event, d) == FMLX_HTTP_OK ? FMLX_API_OK : FMLX_API_INTERNAL;
}

static FmlxApiErr send_block_start(FmlxHttpRes *w, int index, const char *block_json) {
    char buf[ANT_EV_CAP];
    FmlxJsonW j;
    if (ev_begin(&j, buf, sizeof buf, "content_block_start")) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(&j, "index") || fmlx_jsonw_i64(&j, index)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(&j, "content_block")) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_raw(&j, block_json, strlen(block_json))) return FMLX_API_INTERNAL;
    return ev_end(&j, w, "content_block_start");
}

static FmlxApiErr send_block_stop(FmlxHttpRes *w, int index) {
    char buf[ANT_EV_CAP];
    FmlxJsonW j;
    if (ev_begin(&j, buf, sizeof buf, "content_block_stop")) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(&j, "index") || fmlx_jsonw_i64(&j, index)) return FMLX_API_INTERNAL;
    return ev_end(&j, w, "content_block_stop");
}

/* Un delta con un solo campo testuale: `text_delta{text}`, `thinking_delta{thinking}`,
 * `signature_delta{signature}`, `input_json_delta{partial_json}` (piano:490). */
static FmlxApiErr send_delta(FmlxHttpRes *w, int index, const char *delta_type,
                             const char *field, const char *text, size_t n) {
    char buf[ANT_EV_CAP];
    FmlxJsonW j;
    if (ev_begin(&j, buf, sizeof buf, "content_block_delta")) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(&j, "index") || fmlx_jsonw_i64(&j, index)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(&j, "delta") || fmlx_jsonw_obj_begin(&j)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(&j, "type") || fmlx_jsonw_cstr(&j, delta_type)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(&j, field)) return FMLX_API_INTERNAL;
    if (text) {
        if (fmlx_jsonw_str(&j, text, n)) return FMLX_API_INTERNAL;
    } else if (fmlx_jsonw_cstr(&j, "")) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_obj_end(&j)) return FMLX_API_INTERNAL;
    return ev_end(&j, w, "content_block_delta");
}

/* Chiusura di un blocco di ragionamento: Anthropic emitte `signature_delta` PRIMA dello
 * `content_block_stop` (piano:490-491), e i client che validano la firma si aspettano che
 * quel delta CI SIA. La firma è vuota per lo stesso motivo del campo `signature` nella
 * risposta non-stream: non abbiamo la chiave con cui Anthropic firma il thinking. */
static FmlxApiErr close_block(FmlxHttpRes *w, int index, int kind) {
    if (kind == 0 && send_delta(w, index, "signature_delta", "signature", NULL, 0) != FMLX_API_OK)
        return FMLX_API_INTERNAL;
    return send_block_stop(w, index);
}

FmlxApiErr fmlx_api_anthropic_sse_error(const FmlxApiErrCtx *e, FmlxHttpRes *w) {
    if (!e || !w) return FMLX_API_INTERNAL;
    char buf[ANT_EV_CAP];
    FmlxJsonW j;
    if (ev_begin(&j, buf, sizeof buf, "error")) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(&j, "error") || fmlx_jsonw_obj_begin(&j)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(&j, "type") || fmlx_jsonw_cstr(&j, fmlx_api_anthropic_type(e->err)))
        return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(&j, "message") ||
        fmlx_jsonw_cstr(&j, e->message ? e->message : fmlx_api_err_str(e->err)))
        return FMLX_API_INTERNAL;
    if (fmlx_jsonw_obj_end(&j)) return FMLX_API_INTERNAL;
    return ev_end(&j, w, "error");
}

FmlxApiErr fmlx_api_anthropic_ping(FmlxHttpRes *w) {
    if (!w) return FMLX_API_INTERNAL;
    static const char P[] = "{\"type\":\"ping\"}";
    FmlxSlice d = {P, sizeof P - 1};
    return fmlx_http_sse_write(w, "ping", d) == FMLX_HTTP_OK ? FMLX_API_OK : FMLX_API_INTERNAL;
}

FmlxApiErr fmlx_api_anthropic_count_tokens(FmlxApiErrCtx *err) {
    set_err(err, FMLX_API_NOT_SUPPORTED,
            "count_tokens requires the model's exact chat template render, which this "
            "server does not estimate",
            NULL);
    return FMLX_API_NOT_SUPPORTED;
}

FmlxApiErr fmlx_api_anthropic_stream(const FmlxApiChat *c, const FmlxApiMeta *m, FmlxApiNext next,
                                     void *gen_ctx, FmlxHttpRes *w) {
    if (!c || !m || !next || !w) return FMLX_API_INTERNAL;
    char text[ANT_TEXT_CAP], think[ANT_THINK_CAP];
    char args[FMLX_API_MAX_TOOLCALLS * ANT_ARGS_CAP];
    FmlxApiAcc a;
    fmlx_api_acc_init(&a, text, sizeof text, think, sizeof think, args, ANT_ARGS_CAP);

    /* 1. `message_start` PRIMA di tutto: anche l'errore arriva dopo (piano:493), perché un
     *    client Anthropic apre il messaggio qui e ignora tutto ciò che non ha un messaggio
     *    aperto da chiudere. */
    {
        char buf[ANT_EV_CAP];
        FmlxJsonW j;
        if (ev_begin(&j, buf, sizeof buf, "message_start")) return FMLX_API_INTERNAL;
        if (fmlx_jsonw_key(&j, "message") || fmlx_jsonw_obj_begin(&j)) return FMLX_API_INTERNAL;
        char id[FMLX_API_MAX_NAME + 8];
        if (m->id) snprintf(id, sizeof id, "%s", m->id);
        else snprintf(id, sizeof id, "msg_%lld", (long long)m->created);
        if (fmlx_jsonw_key(&j, "id") || fmlx_jsonw_cstr(&j, id)) return FMLX_API_INTERNAL;
        if (fmlx_jsonw_key(&j, "type") || fmlx_jsonw_cstr(&j, "message")) return FMLX_API_INTERNAL;
        if (fmlx_jsonw_key(&j, "role") || fmlx_jsonw_cstr(&j, "assistant"))
            return FMLX_API_INTERNAL;
        if (fmlx_jsonw_key(&j, "model") || fmlx_jsonw_str(&j, c->model.p, c->model.n))
            return FMLX_API_INTERNAL;
        if (fmlx_jsonw_key(&j, "content") || fmlx_jsonw_arr_begin(&j) || fmlx_jsonw_arr_end(&j))
            return FMLX_API_INTERNAL;
        if (fmlx_jsonw_key(&j, "stop_reason") || fmlx_jsonw_null(&j)) return FMLX_API_INTERNAL;
        if (fmlx_jsonw_key(&j, "stop_sequence") || fmlx_jsonw_null(&j)) return FMLX_API_INTERNAL;
        /* `input_tokens` qui è il prompt GIÀ contato dal layer ① prima di generare: è il
         * motivo per cui `message_start` può riportarlo e `usage` finale no (solo output).
         * 0 significa "non contato", e il campo `input_tokens_known` in `FmlxApiMeta` lo
         * distingue da un prompt vuoto. */
        if (fmlx_jsonw_key(&j, "usage") || fmlx_jsonw_obj_begin(&j)) return FMLX_API_INTERNAL;
        if (fmlx_jsonw_key(&j, "input_tokens") || fmlx_jsonw_u64(&j, m->prompt_tokens_known))
            return FMLX_API_INTERNAL;
        if (fmlx_jsonw_key(&j, "output_tokens") || fmlx_jsonw_u64(&j, 0)) return FMLX_API_INTERNAL;
        /* DUE chiusure qui, non una: `usage` e il messaggio dentro `message_start`. La terza
         * — la cornice dell'evento — è quella di `ev_end`. Chiuderne una sole lasciava la
         * radice aperta e `finish` rifiutava l'evento: lo stream Anthropic nasceva vuoto. */
        if (fmlx_jsonw_obj_end(&j) || fmlx_jsonw_obj_end(&j)) return FMLX_API_INTERNAL;
        if (ev_end(&j, w, "message_start")) return FMLX_API_INTERNAL;
    }

    FmlxApiErr err = FMLX_API_OK;
    int next_index = 0;
    int open_kind = -1, open_index = -1; /* -1 nessuno, 0 thinking, 1 text */
    int done_seen = 0;

    for (;;) {
        /* CANCELLAZIONE: client disconnesso (`w->err`) -> stop immediato della generazione. */
        if (w && w->err != FMLX_HTTP_OK) { err = FMLX_API_INTERNAL; break; }
        FmlxApiEvent ev;
        memset(&ev, 0, sizeof ev);
        FmlxApiErrCtx ec;
        memset(&ec, 0, sizeof ec);
        FmlxApiGenSt gs = next(gen_ctx, &ev, &ec);
        if (gs == FMLX_API_GEN_ERR) {
            /* Su un filo già iniziato l'unica uscita onesta è `error` + il chunk terminale,
             * nell'ordine: si chiude il blocco aperto (un `content_block_start` senza stop è
             * un messaggio rotto), poi l'errore col SUO codice, poi `0\r\n\r\n` — che va
             * scritto comunque, o il client resta attaccato al socket. Il codice del motore
             * sale al trasporto SOLO se il filo è stato coerente: dire "upstream ha detto no"
             * su una risposta mai chiusa sarebbe metà della verità. */
            FmlxApiErr wire = FMLX_API_OK;
            if (open_index >= 0) wire = send_block_stop(w, open_index);
            if (wire == FMLX_API_OK) wire = fmlx_api_anthropic_sse_error(&ec, w);
            if (wire == FMLX_API_OK && fmlx_http_sse_end(w) != FMLX_HTTP_OK)
                wire = FMLX_API_INTERNAL;
            if (wire != FMLX_API_OK) return wire;
            return ec.err ? ec.err : FMLX_API_INTERNAL;
        }
        if (gs == FMLX_API_GEN_END) break;
        if (fmlx_api_acc_push(&a, &ev) != FMLX_API_OK) {
            err = FMLX_API_INTERNAL;
            break;
        }
        if (ev.ev == FMLX_API_EV_DONE) {
            done_seen = 1;
            break;
        }
        if (ev.ev == FMLX_API_EV_TOOL_NAME || ev.ev == FMLX_API_EV_TOOL_ARGS) {
            /* Gli argomenti NON partono ora: restano nel buffer del blocco (piano:506) e
             * vengono scritti tutti insieme alla chiusura. */
            continue;
        }
        int kind = (ev.ev == FMLX_API_EV_THINK) ? 0 : 1;
        if (open_kind != kind) {
            if (open_index >= 0 && close_block(w, open_index, open_kind) != FMLX_API_OK) {
                err = FMLX_API_INTERNAL;
                break;
            }
            open_index = next_index++;
            open_kind = kind;
            const char *blk = kind == 0 ? "{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"\"}"
                                        : "{\"type\":\"text\",\"text\":\"\"}";
            if (send_block_start(w, open_index, blk) != FMLX_API_OK) {
                err = FMLX_API_INTERNAL;
                break;
            }
        }
        if (send_delta(w, open_index, kind == 0 ? "thinking_delta" : "text_delta",
                       kind == 0 ? "thinking" : "text", ev.text, ev.n) != FMLX_API_OK) {
            err = FMLX_API_INTERNAL;
            break;
        }
    }

    if (err == FMLX_API_OK && open_index >= 0) {
        if (close_block(w, open_index, open_kind) != FMLX_API_OK) err = FMLX_API_INTERNAL;
        open_index = -1;
    }

    /* 2. I blocchi tool, solo se VALIDI, tutti insieme alla fine: è il prezzo (accettato, e
     *    scritto sopra) del buffering, ed è ciò che rende possibile piano:505. */
    int tools_ok = 0;
    if (err == FMLX_API_OK) {
        tools_ok = count_tools(&a);
        for (int t = 0; t < tools_ok && err == FMLX_API_OK; t++) {
            int idx = next_index++;
            char blk[FMLX_API_MAX_NAME * 2 + 48];
            int n = snprintf(blk, sizeof blk, "{\"type\":\"tool_use\",\"id\":\"%s\",\"name\":\"%s\","
                                              "\"input\":{}}",
                             a.tool[t].id[0] ? a.tool[t].id : "toolu", a.tool[t].name);
            if (n <= 0 || (size_t)n >= sizeof blk) {
                err = FMLX_API_INTERNAL; /* l'id/name non entra: meglio un errore che un JSON spezzato */
                break;
            }
            if (send_block_start(w, idx, blk) != FMLX_API_OK) {
                err = FMLX_API_INTERNAL;
                break;
            }
            if (send_delta(w, idx, "input_json_delta", "partial_json", a.tool[t].args,
                           a.tool[t].args_len) != FMLX_API_OK) {
                err = FMLX_API_INTERNAL;
                break;
            }
            if (send_block_stop(w, idx) != FMLX_API_OK) err = FMLX_API_INTERNAL;
        }
    }

    /* 3. `message_delta` con il finish e il conteggio di uscita, poi `message_stop`. */
    if (err == FMLX_API_OK) {
        FmlxApiFinish fin = ant_finish(&a, tools_ok);
        char buf[ANT_EV_CAP];
        FmlxJsonW j;
        if (ev_begin(&j, buf, sizeof buf, "message_delta")) err = FMLX_API_INTERNAL;
        else if (fmlx_jsonw_key(&j, "delta") || fmlx_jsonw_obj_begin(&j))
            err = FMLX_API_INTERNAL;
        else {
            /* Qui l'azione è `err = `, non `return`: un `return` in mezzo allo stream lascerebbe
             * la connessione APERTA senza `message_stop` né `0\r\n\r\n` — il client resta appeso
             * invece di ricevere l'errore. */
            if (fmlx_jsonw_key(&j, "stop_reason") ||
                fmlx_jsonw_cstr(&j, fmlx_api_finish_anthropic(fin)))
                err = FMLX_API_INTERNAL;
            else if (fmlx_jsonw_key(&j, "stop_sequence")) err = FMLX_API_INTERNAL;
            else if (fin == FMLX_API_FINISH_STOPSEQ && a.stop_seq[0]
                         ? fmlx_jsonw_cstr(&j, a.stop_seq)
                         : fmlx_jsonw_null(&j))
                err = FMLX_API_INTERNAL;
            else if (fmlx_jsonw_obj_end(&j)) err = FMLX_API_INTERNAL;
            else if (fmlx_jsonw_key(&j, "usage") || fmlx_jsonw_obj_begin(&j))
                err = FMLX_API_INTERNAL;
            else if (fmlx_jsonw_key(&j, "output_tokens") ||
                     fmlx_jsonw_u64(&j, a.completion_tokens))
                err = FMLX_API_INTERNAL;
            else if (fmlx_jsonw_obj_end(&j)) err = FMLX_API_INTERNAL;
            else if (ev_end(&j, w, "message_delta")) err = FMLX_API_INTERNAL;
        }
    }
    if (err == FMLX_API_OK) {
        char buf[ANT_EV_CAP];
        FmlxJsonW j;
        if (ev_begin(&j, buf, sizeof buf, "message_stop") == FMLX_API_OK)
            err = ev_end(&j, w, "message_stop");
        else
            err = FMLX_API_INTERNAL;
    }
    if (err == FMLX_API_OK && !done_seen) return FMLX_API_INTERNAL;
    if (err == FMLX_API_OK && fmlx_http_sse_end(w) != FMLX_HTTP_OK) return FMLX_API_INTERNAL;
    return err;
}
