/* fmlx_serve_respond.c — la metà pura del server: una richiesta già incorniciata entra, una
 * risposta completa esce. Nessun socket, nessun orologio, nessuna malloc.
 *
 * Questo file contiene l'unica domanda che il progetto può rispondere OGGI senza il motore:
 * «cosa vede un client che prova a parlarci?». Le risposte sono di tre specie, e la distinzione
 * è tutta qui:
 *  - ciò che è VERO e non chiede il motore (`/v1/models`, `/health`) si risponde per davvero,
 *    leggendo il catalogo locale;
 *  - ciò che chiede il motore si RIFIUTA con 501 e il nome del pezzo mancante. Mai un 200 con
 *    il corpo vuoto: sarebbe una risposta coerente a metà, e l'unica persona che lo scopre è
 *    chi ci costruisce sopra (piano §11.2);
 *  - ciò che il client ha sbagliato si dice con 400/401/404, con la stessa validazione che
 *    resterà in piedi quando il motore arriverà.
 *
 * Le stringhe che vanno al client sono in INGLESE (regola di `fmlx_api.h`: il messaggio è del
 * client, i log sono nostri). Per questo il `message` di un errore della lane hf NON viene
 * incollato nel corpo — è in italiano — mentre i messaggi degli adapter SÌ che possono essere
 * incollati: `fmlx_api_openai.c:106,113` li scrive già in inglese, perché sono dell'adapter e
 * l'adapter parla al client. Per gli errori nostri finisce invece in rete il codice NUMERICO
 * della lane, che è macchina-leggibile e non ha lingua.
 */
#include "fmlx/fmlx_serve.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* `strncasecmp`: i nomi di header non distinguono maiuscole da minuscole */

/* Spazio tenuto libero davanti al corpo, perché `fmlx_http_write_head` scrive dall'inizio dello
 * stesso buffer e il corpo va poi spostato dietro la testata: è il pattern di
 * `fmlx_api_error_response` (src/server/fmlx_api.c:131), non una seconda invenzione. 256 byte
 * coprono status-line + Content-Type + Content-Length + `Connection: close` con margine; se un
 * giorno non bastassero, `w.len > RESERVE` lo dice e la risposta diventa un 500 — mai una
 * testata tagliata. */
#define RESERVE 256u

/* Confronto a TEMPO COSTANTE per la chiave di autenticazione: `strcmp` esce al primo byte
 * diverso, e il tempo che ci mette dipende da quanti primi byte erano giusti — è il canale
 * laterale classico del confronto di segreti, e qui il segreto è quello che apre il server.
 * Pochi byte di CPU in cambio dell'assenza di un oracolo che conta i colpi. */
static int ct_eq(const void *a, size_t na, const void *b, size_t nb) {
    const unsigned char *x = a, *y = b;
    unsigned char diff = (unsigned char)(na != nb);
    size_t i, n = na < nb ? na : nb;

    for (i = 0; i < n; i++) diff = (unsigned char)(diff | (x[i] ^ y[i]));
    return diff == 0;
}

const char *fmlx_serve_status_str(FmlxServeSt s) {
    switch (s) {
    case FMLX_SERVE_OK: return "ok";
    case FMLX_SERVE_BUF_SMALL: return "buffer di risposta troppo piccolo";
    case FMLX_SERVE_BAD_ARG: return "argomento non valido";
    }
    return "?"; /* irraggiungibile: nessuno `default`, così -Wextra obbliga a nominare ogni esito */
}

int fmlx_serve_key_from_env(const char **out) {
    const char *v = getenv("FASTMLX_API_KEY");

    if (out) *out = NULL;
    if (!v || !*v) return 0;
    if (out) *out = v;
    return 1;
}

FmlxServeSt fmlx_serve_apply_env(FmlxServeOpts *o, char *msg, size_t msg_len) {
    const char *v;

    if (!o) return FMLX_SERVE_BAD_ARG;
    if (msg && msg_len) msg[0] = '\0';
    if (!o->host) {
        v = getenv("FASTMLX_HOST");
        if (v && *v) o->host = v;
    }
    if (o->port == 0) {
        v = getenv("FASTMLX_PORT");
        if (v && *v) {
            char *end;
            long n;
            /* Solo cifre, e solo nel range di una porta: `strtol` da sola direbbe di sì anche a
             * "8000x", che è un typo da leggere come errore, non come "8000". */
            errno = 0;
            n = strtol(v, &end, 10);
            if (errno || !end || *end || n < 1 || n > 65535) {
                if (msg && msg_len)
                    snprintf(msg, msg_len, "FASTMLX_PORT: '%s' non è una porta 1..65535", v);
                return FMLX_SERVE_BAD_ARG;
            }
            o->port = (int)n;
        }
    }
    if (!o->api_key) fmlx_serve_key_from_env(&o->api_key);
    return FMLX_SERVE_OK;
}

size_t fmlx_serve_body_limit(const FmlxServeOpts *o) {
    return (o && o->max_body) ? o->max_body : FMLX_SERVE_BODY_MAX;
}

/* ---- il filo: testata + corpo già scritto ---- */

/* Il corpo sta a `resp + RESERVE`, lungo `body_len`. `ka` dice se la connessione può restare
 * aperta, e qui si scrive il `Connection: close` di conseguenza: chi responde e chi decide se
 * tenere aperto devono essere la stessa funzione, o la testata prometterebbe una cosa e il loop
 * ne farebbe un'altra. */
static FmlxServeSt finish_ok(FmlxServeBuf *buf, int status, const char *content_type,
                             size_t body_len, int ka, size_t *out_len) {
    FmlxHttpRes w;

    if (buf->resp_cap < RESERVE) return FMLX_SERVE_BUF_SMALL;
    fmlx_http_res_init(&w, buf->resp, buf->resp_cap);
    if (fmlx_http_write_head(&w, status, content_type, body_len,
                             ka ? NULL : "Connection: close") != FMLX_HTTP_OK)
        return FMLX_SERVE_BUF_SMALL;
    if (w.len > RESERVE) return FMLX_SERVE_BUF_SMALL;
    memmove(buf->resp + w.len, buf->resp + RESERVE, body_len);
    *out_len = w.len + body_len;
    return FMLX_SERVE_OK;
}

/* Un errore del catalogo errori, renderizzato nel dialetto giusto. Chiama
 * `fmlx_api_error_response`, che testata e `Content-Length` li scrive insieme: qui non esiste una
 * strada per emettere un 404 con la lunghezza sbagliata. */
static FmlxServeSt refuse(FmlxServeBuf *buf, const FmlxApiErrCtx *e, int anthropic,
                          size_t *out_len) {
    if (fmlx_api_error_response(e, anthropic, buf->resp, buf->resp_cap, out_len) != FMLX_API_OK)
        return FMLX_SERVE_BUF_SMALL;
    return FMLX_SERVE_OK;
}

static FmlxServeSt refuse_simple(FmlxServeBuf *buf, FmlxApiErr err, int anthropic,
                                 const char *code, const char *msg, size_t *out_len) {
    FmlxApiErrCtx e;

    memset(&e, 0, sizeof e);
    e.err = err;
    e.message = msg;
    e.code = code;
    return refuse(buf, &e, anthropic, out_len);
}

/* ---- l'accordo fra la testata e ciò che il loop farà ---- */

#define CLOSE_HDR "Connection: close\r\n" /* 19 byte: la riga che il client deve poter leggere */

/* 1 = la testata [status_end, head_end) ha già una `Connection:`. Il `\r\n` che la cerca è quello
 * CHE PRECEDE il nome, quindi la prima riga di header si trova con la stessa regola di tutte le
 * altre (il suo `\r\n` è la fine della status-line). */
static int has_conn_hdr(const char *h, size_t from, size_t head_end) {
    size_t i;
    for (i = from >= 2 ? from - 2 : 0; i + 13 <= head_end; i++) {
        if (h[i] != '\r' || h[i + 1] != '\n') continue;
        if (!strncasecmp(h + i + 2, "connection:", 11)) return 1;
    }
    return 0;
}

FmlxServeSt fmlx_serve_agree_head(int http10, FmlxServeBuf *buf, size_t *out_len, int ka) {
    size_t n = *out_len, i, status_end = 0, head_end = 0;
    char *p;

    if (!buf || !buf->resp || !out_len || n == 0) return FMLX_SERVE_BAD_ARG;
    p = buf->resp;
    /* Tutto ciò che questo lane emette comincia con questa precisa stringa. Se non fosse vero, la
     * "correzione" scriverebbe sopra un corpo: è un'inconsistenza interna, e si dice con un
     * errore — saltare in silenzio riporterebbe una testata che mente di nuovo. */
    if (n < 13 || memcmp(p, "HTTP/1.1 ", 9) != 0) {
        *out_len = 0;
        return FMLX_SERVE_BAD_ARG;
    }
    if (http10) memcpy(p + 5, "1.0", 3); /* STESSA lunghezza del `1.1`: nessuno spostamento */

    if (ka) return FMLX_SERVE_OK;
    for (i = 9; i + 1 < n; i++)
        if (p[i] == '\r' && p[i + 1] == '\n') {
            status_end = i + 2;
            break;
        }
    for (i = status_end ? status_end : 9; i + 3 < n; i++)
        if (p[i] == '\r' && p[i + 1] == '\n' && p[i + 2] == '\r' && p[i + 3] == '\n') {
            head_end = i;
            break;
        }
    if (!status_end || !head_end) {
        *out_len = 0;
        return FMLX_SERVE_BAD_ARG;
    }
    if (has_conn_hdr(p, status_end, head_end)) return FMLX_SERVE_OK;
    if (n + sizeof CLOSE_HDR - 1 > buf->resp_cap) {
        *out_len = 0;
        return FMLX_SERVE_BUF_SMALL;
    }
    /* Si sposta il RESTO (corpo incluso) di 19 byte e la riga si infila subito dopo la status-line.
     * `Content-Length` non si tocca: conta il corpo, e il corpo è solo più in là. */
    memmove(p + status_end + sizeof CLOSE_HDR - 1, p + status_end, n - status_end);
    memcpy(p + status_end, CLOSE_HDR, sizeof CLOSE_HDR - 1);
    *out_len = n + sizeof CLOSE_HDR - 1;
    return FMLX_SERVE_OK;
}

/* ---- il dialetto di una richiesta che non abbiamo instradato ---- */

/* Una rotta ignota e un 401 non arrivano a un `FmlxApiAction`, quindi il dialetto non si può
 * dedurre dal path. Si deduce da ciò che il client HA DETTO: un client Anthropic manda
 * `anthropic-version` (piano §11.3), un client OpenAI no. È un segnale del mondo, non
 * un'inferenza nostra sul path — e se il client mente, la forma dell'errore resterà una forma che
 * quel protocollo prevede. */
static int dialect_of(const FmlxHttpReq *req) {
    return fmlx_http_has_header(req, "anthropic-version") ? 1 : 0;
}

/* ---- il catalogo, letto una volta per richiesta ---- */

typedef struct {
    FmlxHfStatus st;
    char root[FMLX_HF_PATH_MAX];
    size_t total, listed, refs_only, non_model, ambiguous;
} Cat;

/* Legge il cache. `buf->repos` è l'array del chiamante: qui non si mallocà, e il tetto di ciò che
 * si può elencare è una capacità dichiarata, non una sorpresa. Un cache con più repo della
 * capacità NON viene elencato a metà: `fmlx_hf_catalog` risponde TOO_MANY con il numero VERO, e
 * chi chiama lo traduce in un 500 che dice quanti erano. */
static Cat load_catalog(const FmlxServeOpts *o, FmlxServeBuf *buf) {
    Cat c;
    size_t n = 0, ignored = 0;
    FmlxHfStatus root_st;

    memset(&c, 0, sizeof c);
    root_st = FMLX_HF_OK;
    if (o && o->cache_root) {
        snprintf(c.root, sizeof c.root, "%s", o->cache_root);
    } else {
        /* La stessa regola della lane, chiamata una volta e SOLO per poterla mostrare in
         * `/health`: la radice non è un campo nostro da reinventare. */
        root_st = fmlx_hf_cache_root(c.root, sizeof c.root);
        if (root_st != FMLX_HF_OK) {
            c.st = root_st;
            c.root[0] = '\0';
            return c;
        }
    }
    c.st = fmlx_hf_catalog(o ? o->cache_root : NULL, buf->repos, buf->repos_cap, &n, &ignored);
    c.total = n; /* il numero VERO anche sull'errore: vedi il commento sopra */
    if (c.st != FMLX_HF_OK) return c;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(buf->repos[i].kind, "model") != 0) c.non_model++;
        else if (buf->repos[i].refs_only) c.refs_only++;
        else c.listed++;
        if (buf->repos[i].ambiguous_name) c.ambiguous++;
    }
    return c;
}

/* ---- GET /v1/models ---- */

/* I FILTRI, e perché. `/v1/models` è l'elenco di ciò che questo server può nominare come modello,
 * e il catalogo locale contiene due cose che modello non sono:
 *  - `dataset`/`space`: la lane hf li distingue per prefisso di directory, e un dataset in mezzo
 *    ai modelli è una voce che un client proverà a usare;
 *  - i repo `refs_only`: hanno i ref e ZERO blob — sono puntatori, non byte. Non c'è nulla da
 *    servire, e nasconderlo qui è il motivo per cui il `models` da terminale li marca.
 * Ogni esclusione è CONTATA in `/health`: filtrare non vuol dire tacere. Un cache che c'è e non
 * ha modelli risponde `data: []` (verità); un cache irraggiungibile risponde 500 (colpa).
 *
 * `created` è l'mtime del blob più recente — un FATTO del disco, non un orologio: il piano vieta
 * che questo layer generi timestamp (intestazione di `fmlx_api.h`), e un repo elencato ha per
 * costruzione almeno un blob, quindi un mtime c'è. */
static FmlxServeSt body_models(FmlxServeBuf *buf, const Cat *c, int ka, size_t *out_len) {
    FmlxJsonW jw;
    size_t blen = 0;
    int wrote = 0;

    if (c->st != FMLX_HF_OK)
        return refuse_simple(buf, FMLX_API_INTERNAL, 0, FMLX_SERVE_CODE_CATALOG,
                             "the local model catalog could not be read; /health reports the code",
                             out_len);

    fmlx_jsonw_init(&jw, buf->resp + RESERVE, buf->resp_cap - RESERVE, buf->resp_cap - RESERVE);
    if (fmlx_jsonw_obj_begin(&jw) == FMLX_JSON_OK && fmlx_jsonw_key(&jw, "object") == FMLX_JSON_OK &&
        fmlx_jsonw_cstr(&jw, "list") == FMLX_JSON_OK && fmlx_jsonw_key(&jw, "data") == FMLX_JSON_OK &&
        fmlx_jsonw_arr_begin(&jw) == FMLX_JSON_OK) {
        for (size_t i = 0; i < c->total; i++) {
            const FmlxHfRepo *r = &buf->repos[i];
            if (strcmp(r->kind, "model") != 0 || r->refs_only) continue;
            if (fmlx_jsonw_obj_begin(&jw) || fmlx_jsonw_key(&jw, "id") ||
                fmlx_jsonw_cstr(&jw, r->repo_id) || fmlx_jsonw_key(&jw, "object") ||
                fmlx_jsonw_cstr(&jw, "model") || fmlx_jsonw_key(&jw, "created") ||
                fmlx_jsonw_i64(&jw, (int64_t)r->mtime) || fmlx_jsonw_key(&jw, "owned_by") ||
                fmlx_jsonw_cstr(&jw, "local") || fmlx_jsonw_obj_end(&jw))
                break; /* il writer è in errore: `finish` lo dirà, e qui sotto si risponde 500 */
            wrote++;
        }
        if (wrote == (int)c->listed && fmlx_jsonw_arr_end(&jw) == FMLX_JSON_OK &&
            fmlx_jsonw_obj_end(&jw) == FMLX_JSON_OK &&
            fmlx_jsonw_finish(&jw, &blen) == FMLX_JSON_OK) {
            fmlx_jsonw_free(&jw);
            return finish_ok(buf, 200, "application/json", blen, ka, out_len);
        }
    }
    fmlx_jsonw_free(&jw);
    /* Non entra: NON si manda l'elenco a metà. Un `data` troncato avrebbe l'aspetto esatto di
     * «questo cache ha N modelli» con N sbagliato, che è il risultato silenzioso del piano. */
    return refuse_simple(buf, FMLX_API_INTERNAL, 0, "response_too_large",
                         "the model list does not fit this server's response buffer: raise the "
                         "response capacity or point at a smaller cache",
                         out_len);
}

/* ---- GET /health ---- */

/* `status` è la parola che un orchestratore legge, e qui può dire due cose sole, entrambe vere:
 *  - `degraded` (HTTP 200): il server fa le DUE cose che dichiara e non può generare. Non «ok»,
 *    perché dire ok a chi chiede «qui si lavora?» sarebbe bugiardo per omissione; non 503, perché
 *    far uccidere da un supervisor un processo che risponde correttamente a tutto ciò che sa fare
 *    è il danno che un health check sbagliato fa a chi ce l'ha configurato.
 *  - `unavailable` (HTTP 503): nemmeno il catalogo si legge — il disco non c'è, o l'ambiente punta
 *    nel posto sbagliato. Questo SÌ che è «non sono in grado di fare il mio lavoro».
 * La scelta fra le due è la scelta fra «sono limitato» e «sono rotto», e `/health` è l'unico posto
 * del progetto dove viene detta.
 *
 * Il `root` del cache finisce nel corpo: è il motivo per cui aprire oltre il loopback senza
 * `FASTMLX_API_KEY` è una scelta dell'esercente, e `/health` la dichiara (`auth: disabled`). */
static FmlxServeSt body_health(FmlxServeBuf *buf, const Cat *c, int auth_on, int ka,
                               size_t *out_len) {
    FmlxJsonW jw;
    size_t blen = 0;
    int catalog_ok = c->st == FMLX_HF_OK;
    int ok;

    fmlx_jsonw_init(&jw, buf->resp + RESERVE, buf->resp_cap - RESERVE, buf->resp_cap - RESERVE);
    ok = fmlx_jsonw_obj_begin(&jw) == FMLX_JSON_OK &&
         fmlx_jsonw_key(&jw, "object") == FMLX_JSON_OK &&
         fmlx_jsonw_cstr(&jw, "health") == FMLX_JSON_OK &&
         fmlx_jsonw_key(&jw, "status") == FMLX_JSON_OK &&
         fmlx_jsonw_cstr(&jw, catalog_ok ? "degraded" : "unavailable") == FMLX_JSON_OK &&
         fmlx_jsonw_key(&jw, "reason") == FMLX_JSON_OK &&
         fmlx_jsonw_cstr(&jw, catalog_ok
                              ? "serving /v1/models and /health only: the inference engine (lane 1) "
                                "is not wired, so every generation route answers 501"
                              : "the local catalog cannot be read: no route here has a true answer") ==
             FMLX_JSON_OK &&
         fmlx_jsonw_key(&jw, "lanes") == FMLX_JSON_OK && fmlx_jsonw_obj_begin(&jw) == FMLX_JSON_OK &&
         fmlx_jsonw_key(&jw, "http") == FMLX_JSON_OK &&
         fmlx_jsonw_cstr(&jw, "ready") == FMLX_JSON_OK &&
         fmlx_jsonw_key(&jw, "router") == FMLX_JSON_OK &&
         fmlx_jsonw_cstr(&jw, "ready") == FMLX_JSON_OK &&
         fmlx_jsonw_key(&jw, "catalog") == FMLX_JSON_OK &&
         fmlx_jsonw_cstr(&jw, catalog_ok ? "ready" : "error") == FMLX_JSON_OK &&
         fmlx_jsonw_key(&jw, "auth") == FMLX_JSON_OK &&
         fmlx_jsonw_cstr(&jw, auth_on ? "enabled" : "disabled") == FMLX_JSON_OK &&
         fmlx_jsonw_key(&jw, "engine") == FMLX_JSON_OK &&
         fmlx_jsonw_cstr(&jw, "not_wired") == FMLX_JSON_OK && fmlx_jsonw_obj_end(&jw) == FMLX_JSON_OK &&
         fmlx_jsonw_key(&jw, "cache") == FMLX_JSON_OK && fmlx_jsonw_obj_begin(&jw) == FMLX_JSON_OK &&
         fmlx_jsonw_key(&jw, "root") == FMLX_JSON_OK && fmlx_jsonw_cstr(&jw, c->root) == FMLX_JSON_OK &&
         fmlx_jsonw_key(&jw, "status_code") == FMLX_JSON_OK &&
         fmlx_jsonw_i64(&jw, (int64_t)c->st) == FMLX_JSON_OK &&
         fmlx_jsonw_key(&jw, "repos_total") == FMLX_JSON_OK &&
         fmlx_jsonw_i64(&jw, (int64_t)c->total) == FMLX_JSON_OK &&
         fmlx_jsonw_key(&jw, "repos_listed") == FMLX_JSON_OK &&
         fmlx_jsonw_i64(&jw, (int64_t)c->listed) == FMLX_JSON_OK &&
         fmlx_jsonw_key(&jw, "repos_refs_only") == FMLX_JSON_OK &&
         fmlx_jsonw_i64(&jw, (int64_t)c->refs_only) == FMLX_JSON_OK &&
         fmlx_jsonw_key(&jw, "repos_non_model") == FMLX_JSON_OK &&
         fmlx_jsonw_i64(&jw, (int64_t)c->non_model) == FMLX_JSON_OK &&
         fmlx_jsonw_key(&jw, "repos_ambiguous") == FMLX_JSON_OK &&
         fmlx_jsonw_i64(&jw, (int64_t)c->ambiguous) == FMLX_JSON_OK &&
         fmlx_jsonw_obj_end(&jw) == FMLX_JSON_OK && fmlx_jsonw_obj_end(&jw) == FMLX_JSON_OK &&
         fmlx_jsonw_finish(&jw, &blen) == FMLX_JSON_OK;
    if (ok) {
        fmlx_jsonw_free(&jw);
        return finish_ok(buf, catalog_ok ? 200 : 503, "application/json", blen, ka, out_len);
    }
    fmlx_jsonw_free(&jw);
    return refuse_simple(buf, FMLX_API_INTERNAL, 0, "response_too_large",
                         "the health body does not fit this server's response buffer", out_len);
}

/* ---- il rifiuto di ciò che chiede il motore ---- */

/* UNA frase per azione mancante, non una frase sola per tutte: «not supported» e «manca il
 * registro delle sessioni» sono due informazioni diverse, e la seconda è quella che fa partire il
 * lavoro giusto. Il `code` resta COSTANTE (`FMLX_SERVE_CODE_ENGINE`) perché la macchina deve
 * distinguere «il motore non c'è» da «questa rotta non esiste nemmeno in progetto» senza parsare
 * frasi che possono essere riscritte. */
static const char *missing_route_reason(FmlxApiAction act) {
    switch (act) {
    case FMLX_API_ACT_CHAT: return "chat completion";
    case FMLX_API_ACT_COMPLETIONS: return "text completion";
    case FMLX_API_ACT_RESPONSES: return "the Responses API";
    case FMLX_API_ACT_MESSAGES: return "the Messages API";
    case FMLX_API_ACT_EMBEDDINGS: return "embeddings";
    case FMLX_API_ACT_LOAD: return "loading a model into memory";
    case FMLX_API_ACT_UNLOAD: return "unloading a model from memory";
    case FMLX_API_ACT_MODELS_STATUS: return "which models are loaded, and their state";
    case FMLX_API_ACT_METRICS: return "engine counters";
    case FMLX_API_ACT_COUNT_TOKENS: return "an exact token count";
    case FMLX_API_ACT_NONE: break;
    case FMLX_API_ACT_MODELS: break; /* gestiti sopra: non arrivano fin qui */
    case FMLX_API_ACT_HEALTH: break;
    }
    return "this route";
}

static FmlxServeSt refuse_engine(FmlxServeBuf *buf, FmlxApiAction act, int anthropic,
                                 size_t *out_len) {
    char msg[256];
    FmlxApiErrCtx e;

    memset(&e, 0, sizeof e);
    e.err = FMLX_API_NOT_SUPPORTED;
    e.code = FMLX_SERVE_CODE_ENGINE;
    snprintf(msg, sizeof msg,
             "%s requires the inference engine (lane 1), which is not wired in this build: this "
             "server answers /v1/models and /health only",
             missing_route_reason(act));
    e.message = msg;
    return refuse(buf, &e, anthropic, out_len);
}

/* ---- validazione del corpo: la parte che NON dipende dal motore ---- */

typedef enum {
    V_OK = 0,  /* il corpo va bene (e la rotta resta da cablare: chi chiama risponde 501) */
    V_REPLIED, /* il corpo NON andava bene: il 400 è già scritto nel buffer */
    V_BUF_SMALL /* non c'era posto nemmeno per il 400 */
} VSt;

static VSt bad_body(FmlxServeBuf *buf, int anthropic, const char *msg, size_t *out_len) {
    return refuse_simple(buf, FMLX_API_INVALID_REQUEST, anthropic, FMLX_SERVE_CODE_BODY_INVALID, msg,
                         out_len) == FMLX_SERVE_OK
               ? V_REPLIED
               : V_BUF_SMALL;
}

/* Esegue la parte di interpretazione che nel layer ② ESISTE già per il dialetto giusto, e niente
 * di più. `/v1/completions`, `/v1/responses`, `/v1/embeddings`, `load` e `unload` non hanno un
 * parser nel layer ②: coniare qui una loro validazione significherebbe inventare regole che il
 * layer ② dovrà disfare, quindi si esige solo che il corpo sia un oggetto JSON e lo si rimanda al
 * 501. */
static VSt validate(FmlxApiAction act, const char *body, size_t body_len, int anthropic,
                    FmlxServeBuf *buf, size_t *out_len) {
    FmlxJsonDoc doc;
    FmlxJsonStatus js;
    const FmlxJsonValue *root;
    char msg[192];

    fmlx_json_doc_init(&doc);
    js = fmlx_json_parse(body, body_len, 0, &doc);
    if (js != FMLX_JSON_OK) {
        VSt r;
        /* `doc.err_msg` è in italiano ed è nostro: su filo vanno la forma inglese e l'offset, che
         * è la parte che un client può usare davvero. */
        snprintf(msg, sizeof msg, "request body is not valid JSON (offset %zu)", doc.err_off);
        fmlx_json_doc_free(&doc);
        r = bad_body(buf, anthropic, msg, out_len);
        return r;
    }
    root = fmlx_json_root(&doc);
    if (!root || root->type != FMLX_JSON_OBJ) {
        fmlx_json_doc_free(&doc);
        return bad_body(buf, anthropic, "request body must be a JSON object", out_len);
    }

    if (act == FMLX_API_ACT_CHAT || act == FMLX_API_ACT_MESSAGES) {
        FmlxApiChat chat;
        FmlxApiErrCtx e;
        FmlxApiErr api;
        VSt r;

        fmlx_api_chat_init(&chat);
        memset(&e, 0, sizeof e);
        api = act == FMLX_API_ACT_CHAT ? fmlx_api_openai_parse(root, &chat, &e)
                                       : fmlx_api_anthropic_parse(root, &chat, &e);
        if (api != FMLX_API_OK) {
            r = refuse(buf, &e, act == FMLX_API_ACT_MESSAGES ? 1 : 0, out_len) == FMLX_SERVE_OK
                    ? V_REPLIED
                    : V_BUF_SMALL;
            fmlx_json_doc_free(&doc);
            return r;
        }
    }
    fmlx_json_doc_free(&doc);
    return V_OK;
}

/* 1 = la rotta ha un corpo da leggere. È la colonna `is_post` della tabella del router
 * (src/server/fmlx_api.c:483): `models/status` e `metrics` sono le uniche GET che arrivano fin
 * qui, e pretenderne un corpo sarebbe un 400 inventato su una richiesta perfetta. */
static int route_has_body(FmlxApiAction act) {
    switch (act) {
    case FMLX_API_ACT_CHAT:
    case FMLX_API_ACT_COMPLETIONS:
    case FMLX_API_ACT_RESPONSES:
    case FMLX_API_ACT_MESSAGES:
    case FMLX_API_ACT_COUNT_TOKENS:
    case FMLX_API_ACT_EMBEDDINGS:
    case FMLX_API_ACT_LOAD:
    case FMLX_API_ACT_UNLOAD: return 1;
    case FMLX_API_ACT_NONE: break;
    case FMLX_API_ACT_MODELS: break;
    case FMLX_API_ACT_MODELS_STATUS: break;
    case FMLX_API_ACT_HEALTH: break;
    case FMLX_API_ACT_METRICS: break;
    }
    return 0;
}

/* ---- il dispatch ---- */

static FmlxServeSt respond_one(const FmlxHttpReq *req, const char *body, size_t body_len,
                               const FmlxServeOpts *o, const FmlxServeBuf *cbuf, size_t *out_len,
                               int *keep_alive) {
    FmlxServeBuf buf;
    FmlxApiAction act = FMLX_API_ACT_NONE;
    FmlxApiErr rt;
    FmlxServeSt st;
    VSt v;
    Cat cat;
    char msg[288];
    const char *want = NULL;
    int ant, ka, auth_on;

    if (!req || !cbuf || !cbuf->resp || !out_len || !keep_alive) return FMLX_SERVE_BAD_ARG;
    *out_len = 0;
    *keep_alive = 0;
    buf = *cbuf;
    if (buf.resp_cap < RESERVE + 64) return FMLX_SERVE_BUF_SMALL;
    ka = fmlx_http_keep_alive(req);

    /* L'autenticazione PRIMA di tutto, rotta inclusa: «questa rotta esiste» è già informazione, e
     * un 401 non deve mai diventare un sondaggio gratuito delle route. La chiave viene dagli
     * argomenti (che il CLI ha già risolto dall'ambiente) o dall'ambiente in modo diretto: NON è
     * una bandiera, per la stessa ragione di `HF_TOKEN`. */
    if (o && o->api_key && o->api_key[0]) want = o->api_key;
    else
        fmlx_serve_key_from_env(&want);
    auth_on = want != NULL;
    if (want) {
        FmlxSlice got = fmlx_http_get_header(req, "authorization");
        FmlxSlice xkey = fmlx_http_get_header(req, "x-api-key");
        size_t wn = strlen(want);
        int ok = 0;
        if (got.n > 7 && memcmp(got.p, "Bearer ", 7) == 0)
            ok = ct_eq(got.p + 7, got.n - 7, want, wn);
        if (!ok && xkey.n) ok = ct_eq(xkey.p, xkey.n, want, wn);
        if (!ok) {
            st = refuse_simple(&buf, FMLX_API_UNAUTHORIZED, dialect_of(req), FMLX_SERVE_CODE_AUTH,
                               "this server requires an API key: send \"Authorization: Bearer "
                               "<key>\" or \"x-api-key: <key>\"",
                               out_len);
            /* Un non autorizzato non tiene occupata una connessione, e il `close` va SCRITTO oltre
             * che restituito: testata e comportamento del loop devono dire la stessa cosa. */
            *keep_alive = 0;
            return st;
        }
    }

    rt = fmlx_api_route(req->method, req->target, &act);
    if (rt != FMLX_API_OK) {
        if (rt == FMLX_API_NOT_FOUND)
            snprintf(msg, sizeof msg, "no route %.*s on this server", (int)req->target.n,
                     req->target.p);
        else
            snprintf(msg, sizeof msg, "%.*s is not the method this route takes",
                     (int)req->method.n, req->method.p);
        st = refuse_simple(&buf,
                           rt == FMLX_API_NOT_FOUND ? FMLX_API_NOT_FOUND : FMLX_API_INVALID_REQUEST,
                           dialect_of(req), rt == FMLX_API_NOT_FOUND ? NULL : "wrong_method", msg,
                           out_len);
        *keep_alive = st == FMLX_SERVE_OK ? ka : 0;
        return st;
    }
    ant = fmlx_api_is_anthropic(act);

    if (act == FMLX_API_ACT_MODELS || act == FMLX_API_ACT_HEALTH) {
        cat = load_catalog(o, &buf);
        st = act == FMLX_API_ACT_MODELS ? body_models(&buf, &cat, ka, out_len)
                                        : body_health(&buf, &cat, auth_on, ka, out_len);
        *keep_alive = st == FMLX_SERVE_OK ? ka : 0;
        return st;
    }

    /* `count_tokens` ha già il SUO rifiuto scritto nel layer ②, con la ragione vera («il
     * conteggio deve usare lo stesso render del template del modello, e il render non c'è»):
     * riusarlo è la regola della sorgente unica, non pigrizia. Qui non si riscrive una frase
     * diversa sullo stesso fatto. */
    if (act == FMLX_API_ACT_COUNT_TOKENS) {
        FmlxApiErrCtx e;
        memset(&e, 0, sizeof e);
        fmlx_api_anthropic_count_tokens(&e);
        st = refuse(&buf, &e, 1, out_len);
        *keep_alive = st == FMLX_SERVE_OK ? ka : 0;
        return st;
    }

    if (route_has_body(act)) {
        if (body_len > fmlx_serve_body_limit(o)) {
            st = refuse_simple(&buf, FMLX_API_INVALID_REQUEST, ant, FMLX_SERVE_CODE_BODY_LARGE,
                               "request body is larger than this server accepts", out_len);
            *keep_alive = 0;
            return st;
        }
        if (ant) {
            FmlxSlice hv = fmlx_http_get_header(req, "anthropic-version");
            if (fmlx_api_anthropic_version(hv) < 0) {
                snprintf(msg, sizeof msg, "anthropic-version %.*s is not understood", (int)hv.n,
                         hv.p);
                st = refuse_simple(&buf, FMLX_API_INVALID_REQUEST, 1, FMLX_SERVE_CODE_VERSION, msg,
                                   out_len);
                *keep_alive = st == FMLX_SERVE_OK ? ka : 0;
                return st;
            }
        }
        v = validate(act, body, body_len, ant, &buf, out_len);
        if (v == V_REPLIED) {
            *keep_alive = ka;
            return FMLX_SERVE_OK; /* il corpo era sbagliato: il 400 è già nel buffer */
        }
        if (v == V_BUF_SMALL) return FMLX_SERVE_BUF_SMALL;
    }

    st = refuse_engine(&buf, act, ant, out_len);
    *keep_alive = st == FMLX_SERVE_OK ? ka : 0;
    return st;
}

/* L'UNICO punto in cui una risposta di questa lane esce: qui la testata viene messa d'accordo con
 * ciò che il loop farà (versione e `Connection: close`), e qui un esito che non è OK azzera
 * `*out_len`. Il secondo obbligo è il contratto dell'header («su BUF_SMALL/BAD_ARG non c'è nulla da
 * inviare»), che senza questo punto dipenderebbe da come si è comportata ogni singola `refuse`. */
FmlxServeSt fmlx_serve_respond(const FmlxHttpReq *req, const char *body, size_t body_len,
                               const FmlxServeOpts *o, const FmlxServeBuf *cbuf, size_t *out_len,
                               int *keep_alive) {
    FmlxServeBuf m;
    FmlxServeSt st;

    st = respond_one(req, body, body_len, o, cbuf, out_len, keep_alive);
    if (st != FMLX_SERVE_OK) {
        *out_len = 0;
        return st;
    }
    m = *cbuf;
    st = fmlx_serve_agree_head(req->http10, &m, out_len, *keep_alive);
    if (st != FMLX_SERVE_OK) *out_len = 0;
    return st;
}
