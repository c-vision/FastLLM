/* test_api.c — il layer ② si prova su TRE cose: chi comanda su un byte, in che ORDINE
 * escono gli eventi, e cosa NON deve mai uscire.
 *
 * "Chi comanda su un byte": `max_tokens: 1.5` non deve diventare 1, e un campo sconosciuto
 * non deve svanire (piano:474). "In che ordine": un client Anthropic apre i blocchi per
 * indice e aspetta lo `stop`; se l'ordine di piano:489-492 non è quello, la SDK si appende.
 * "Cosa non deve uscire": un `tool_use` con l'input troncato (piano:505) — e qui il test è
 * una NEGAZIONE, perché l'oggetto fasullo deve mancare, non avere la forma giusta.
 *
 * Il motore non esiste: la generazione è coperta da un `FmlxApiNext` scriptato, che è il
 * test double del confine, non uno stub di produzione. Se un domani il layer ① violerà il
 * confine (DONE che non chiude, args senza tool aperto) questo file è il posto che lo dice.
 */
#include "fmlx/fmlx_api.h"

#include <stdio.h>
#include <string.h>

static int checks, failures;

#define CK(cond, ...)                                                                            \
    do {                                                                                         \
        checks++;                                                                                \
        if (!(cond)) {                                                                           \
            failures++;                                                                          \
            printf("  FALLIMENTO %s:%d: ", __FILE__, __LINE__);                                  \
            printf(__VA_ARGS__);                                                                 \
            printf("\n");                                                                        \
        }                                                                                        \
    } while (0)

#define HAS(hay, needle) (strstr((hay), (needle)) != NULL)

static FmlxSlice SL(const char *s) {
    FmlxSlice x = {s, (uint32_t)(s ? strlen(s) : 0)};
    return x;
}

/* ---- DOM in ingresso ------------------------------------------------------- */

static const FmlxJsonValue *body(const char *src, FmlxJsonDoc *d) {
    fmlx_json_doc_init(d);
    if (fmlx_json_parse(src, strlen(src), 0, d) != FMLX_JSON_OK) {
        printf("  (il JSON di test non è parseabile: %s)\n", d->err_msg);
        return NULL;
    }
    return fmlx_json_root(d);
}

/* ---- Il motore scriptato --------------------------------------------------- */

typedef struct {
    const FmlxApiEvent *ev;
    int n, i;
} Script;

static FmlxApiGenSt gen_next(void *ctx, FmlxApiEvent *out, FmlxApiErrCtx *err) {
    Script *s = ctx;
    (void)err;
    if (s->i >= s->n) return FMLX_API_GEN_END;
    *out = s->ev[s->i++];
    return FMLX_API_GEN_MORE;
}

/* Il motore che fallisce: porta SUO codice d'errore, e il layer ② lo deve propagare. */
static FmlxApiGenSt gen_err(void *ctx, FmlxApiEvent *out, FmlxApiErrCtx *err) {
    (void)ctx;
    (void)out;
    memset(err, 0, sizeof *err);
    err->err = FMLX_API_UPSTREAM_HUB;
    err->message = "the engine stopped";
    return FMLX_API_GEN_ERR;
}

static FmlxApiEvent EVT_TXT(const char *t) {
    FmlxApiEvent e = {.ev = FMLX_API_EV_TEXT, .text = t, .n = (uint32_t)strlen(t)};
    return e;
}
static FmlxApiEvent EVT_THK(const char *t) {
    FmlxApiEvent e = {.ev = FMLX_API_EV_THINK, .text = t, .n = (uint32_t)strlen(t)};
    return e;
}
static FmlxApiEvent EVT_TOOL(const char *id, const char *name) {
    FmlxApiEvent e = {.ev = FMLX_API_EV_TOOL_NAME, .tool_id = id, .tool_name = name};
    return e;
}
static FmlxApiEvent EVT_ARGS(const char *frag) {
    FmlxApiEvent e = {.ev = FMLX_API_EV_TOOL_ARGS, .text = frag, .n = (uint32_t)strlen(frag)};
    return e;
}
static FmlxApiEvent EVT_DONE(FmlxApiFinish f, uint64_t pt, uint64_t ct, uint64_t cached) {
    FmlxApiEvent e = {.ev = FMLX_API_EV_DONE,
                      .finish = f,
                      .prompt_tokens = pt,
                      .completion_tokens = ct,
                      .cached_tokens = cached};
    return e;
}

/* ============================================================================ */

static void t_router(void) {
    printf("== 1. rotta (piano:453-461)\n");
    FmlxApiAction act;
    CK(fmlx_api_route(SL("GET"), SL("/v1/models"), &act) == FMLX_API_OK &&
           act == FMLX_API_ACT_MODELS,
       "GET /v1/models non riconosciuto (act=%d)", (int)act);
    CK(fmlx_api_route(SL("GET"), SL("/v1/models?limit=10"), &act) == FMLX_API_OK &&
           act == FMLX_API_ACT_MODELS,
       "la query non deve cambiare la rotta");
    CK(fmlx_api_route(SL("POST"), SL("/v1/chat/completions"), &act) == FMLX_API_OK &&
           act == FMLX_API_ACT_CHAT, "POST chat/completions");
    CK(fmlx_api_route(SL("POST"), SL("/v1/messages"), &act) == FMLX_API_OK &&
           act == FMLX_API_ACT_MESSAGES, "POST messages");
    CK(fmlx_api_route(SL("POST"), SL("/v1/messages/count_tokens"), &act) == FMLX_API_OK &&
           act == FMLX_API_ACT_COUNT_TOKENS,
       "count_tokens deve vincere su /v1/messages: è un'altra rotta");
    CK(fmlx_api_is_anthropic(FMLX_API_ACT_COUNT_TOKENS) && fmlx_api_is_anthropic(FMLX_API_ACT_MESSAGES),
       "le due rotte Anthropic");
    CK(!fmlx_api_is_anthropic(FMLX_API_ACT_CHAT), "chat non è Anthropic");
    CK(fmlx_api_route(SL("GET"), SL("/v1/chat/completions"), &act) == FMLX_API_INVALID_REQUEST,
       "GET su una rotta POST deve essere 400, non 404");
    CK(fmlx_api_route(SL("POST"), SL("/v1/nope"), &act) == FMLX_API_NOT_FOUND,
       "rotta inesistente: il codice separato (404 non-400)");
    CK(fmlx_api_http_status(FMLX_API_NOT_FOUND) == 404 &&
           fmlx_api_http_status(FMLX_API_MODEL_NOT_FOUND) == 404,
       "entrambi 404 ma cause diverse");
    CK(fmlx_api_http_status(FMLX_API_MEMORY_GUARD) == 409 &&
           fmlx_api_http_status(FMLX_API_NOT_SUPPORTED) == 501 &&
           fmlx_api_http_status(FMLX_API_RESOURCE_EXHAUSTED) == 429,
       "la mappa dei codici di piano:515-517");
}

static void t_errors(void) {
    printf("== 2. un catalogo, due formati (piano:513-518)\n");
    char buf[2048];
    size_t n = 0;
    FmlxApiErrCtx e;
    memset(&e, 0, sizeof e);
    e.err = FMLX_API_INVALID_REQUEST;
    e.message = "temperature out of range";
    e.param = "temperature";
    CK(fmlx_api_error_response(&e, 0, buf, sizeof buf, &n) == FMLX_API_OK, "risposta openai");
    buf[n < sizeof buf - 1 ? n : sizeof buf - 1] = '\0';
    CK(HAS(buf, "HTTP/1.1 400 Bad Request"), "status-line: %.90s", buf);
    CK(HAS(buf, "Content-Length: "), "il corpo non-stream deve avere una lunghezza");
    CK(HAS(buf, "{\"error\":{\"message\":\"temperature out of range\","
                "\"type\":\"invalid_request_error\",\"param\":\"temperature\"}}"),
       "corpo OpenAI esatto: %s", strchr(buf, '{') ? strchr(buf, '{') : "(nessuno)");

    CK(fmlx_api_error_response(&e, 1, buf, sizeof buf, &n) == FMLX_API_OK, "risposta anthropic");
    buf[n < sizeof buf - 1 ? n : sizeof buf - 1] = '\0';
    CK(HAS(buf, "\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\","
                "\"message\":\"temperature out of range\"}"),
       "corpo Anthropic esatto: %s", strchr(buf, '{') ? strchr(buf, '{') : "(nessuno)");
    CK(!HAS(buf, "\"param\""), "Anthropic non ha il campo param: copiarlo sarebbe un'altra API");

    memset(&e, 0, sizeof e);
    e.err = FMLX_API_CONTEXT_LENGTH;
    e.message = "prompt too long";
    e.tok_requested = 200000;
    e.tok_available = 131072;
    fmlx_api_error_response(&e, 0, buf, sizeof buf, &n);
    buf[n < sizeof buf - 1 ? n : sizeof buf - 1] = '\0';
    CK(HAS(buf, "\"tokens_requested\":200000") && HAS(buf, "\"tokens_available\":131072"),
       "i numeri devono esserci (piano:516): %s", strchr(buf, '{') ? strchr(buf, '{') : "");

    memset(&e, 0, sizeof e);
    e.err = FMLX_API_MEMORY_GUARD;
    e.need_bytes = 118111600640ull;
    fmlx_api_error_response(&e, 1, buf, sizeof buf, &n);
    buf[n < sizeof buf - 1 ? n : sizeof buf - 1] = '\0';
    CK(HAS(buf, "\"required_bytes\":118111600640") && HAS(buf, "--stream-cache-experts"),
       "il caso 'non entra in RAM' deve dire byte E suggerimento (piano:517): %s",
       strchr(buf, '{') ? strchr(buf, '{') : "");
    CK(HAS(buf, "\"message\":\"memory_guard_refused\""),
       "messaggio di default = il nome del codice, mai vuoto");

    memset(&e, 0, sizeof e);
    e.err = FMLX_API_RESOURCE_EXHAUSTED;
    e.message = "slow down";
    e.retry_after = 30;
    fmlx_api_error_response(&e, 0, buf, sizeof buf, &n);
    buf[n < sizeof buf - 1 ? n : sizeof buf - 1] = '\0';
    CK(HAS(buf, "Retry-After: 30"), "429 senza Retry-After è un client che riprova subito");
    CK(HAS(buf, "HTTP/1.1 429 Too Many Requests"), "reason phrase per il 429");
}

static void t_openai_parse(void) {
    printf("== 3. parse OpenAI\n");
    FmlxJsonDoc d;
    FmlxApiChat c;
    FmlxApiErrCtx e;

    const FmlxJsonValue *r = body(
        "{\"model\":\"gpt-x\",\"messages\":[{\"role\":\"system\",\"content\":\"a\"},"
        "{\"role\":\"developer\",\"content\":\"b\"},{\"role\":\"user\",\"content\":\"c\"}],"
        "\"stream\":true,\"stream_options\":{\"include_usage\":true},\"temperature\":0.7,"
        "\"max_completion_tokens\":512,\"stop\":[\"x\",\"y\"],\"seed\":7,\"n\":1,"
        "\"tools\":[{\"type\":\"function\"}],\"logit_bias\":{\"1\":2},\"user\":\"u\","
        "\"reasoning_effort\":\"low\"}",
        &d);
    CK(r != NULL, "corpo di test valido");
    memset(&e, 0xDE, sizeof e);
    CK(fmlx_api_openai_parse(r, &c, &e) == FMLX_API_OK, "parse della richiesta reale");
    CK(c.stream && c.stream_usage, "stream + include_usage");
    CK(c.has_temperature && c.temperature > 0.69 && c.temperature < 0.71, "temperature 0.7");
    CK(c.has_max_tokens && c.max_tokens == 512, "max_completion_tokens onorato");
    CK(c.has_seed && c.seed == 7, "seed");
    CK(c.n_stop == 2 && fmlx_api_streq(c.stop[0], "x"), "stop come array");
    CK(c.n_sys_msgs == 2, "system E developer sono entrambi di sistema (piano:507), %u",
       c.n_sys_msgs);
    CK(c.tools != NULL, "tools");
    CK(c.n_ignored == 2, "logit_bias + user in ignored (reasoning_effort e' ora onorato), %u", c.n_ignored);
    CK(fmlx_api_streq(c.ignored[1], "user"), "l'ultimo campo ignorato e' user");
    CK(c.has_think && c.think, "reasoning_effort=low accende il thinking (bd3e2fc)");

    r = body("{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"c\"},"
             "{\"role\":\"system\",\"content\":\"S1\"},{\"role\":\"system\",\"content\":\"S2\"}]}",
             &d);
    CK(fmlx_api_openai_parse(r, &c, &e) == FMLX_API_OK, "due messaggi system");
    char mem[256];
    FmlxSlice sys;
    CK(fmlx_api_join_system(&c, mem, sizeof mem, &sys) == FMLX_API_OK && sys.n == 6 &&
           !memcmp(sys.p, "S1\n\nS2", 6),
       "i system multipli si uniscono con \\n\\n (piano:507): %.*s", (int)sys.n, sys.p);

    r = body("{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"c\"},"
             "{\"role\":\"system\",\"content\":\"UNO\"}]}",
             &d);
    fmlx_api_openai_parse(r, &c, &e);
    CK(fmlx_api_join_system(&c, mem, sizeof mem, &sys) == FMLX_API_OK && sys.n == 3 &&
           sys.p != mem,
       "un solo blocco: la fetta resta nel DOM, nessuna copia");

    /* --- i rifiuti, ognuno con la CAUSA sua --- */
    r = body("{\"model\":\"m\",\"messages\":[],\"max_tokens\":1.5}", &d);
    CK(fmlx_api_openai_parse(r, &c, &e) == FMLX_API_INVALID_REQUEST &&
           e.param && !strcmp(e.param, "messages"),
       "messages vuota rifiutata");
    r = body("{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"c\"}],"
             "\"max_tokens\":1.5}",
             &d);
    CK(fmlx_api_openai_parse(r, &c, &e) == FMLX_API_INVALID_REQUEST && e.param &&
           !strcmp(e.param, "max_tokens"),
       "`max_tokens: 1.5` NON deve diventare 1 (è il caso scritto in fmlx_json.h:8)");
    r = body("{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"c\"}],"
             "\"temperature\":\"0.7\"}",
             &d);
    CK(fmlx_api_openai_parse(r, &c, &e) == FMLX_API_INVALID_REQUEST, "temperature stringa");
    r = body("{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"c\"}],"
             "\"temperature\":3}",
             &d);
    CK(fmlx_api_openai_parse(r, &c, &e) == FMLX_API_INVALID_REQUEST, "temperature oltre 2");
    r = body("{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"c\"}],\"n\":2}", &d);
    CK(fmlx_api_openai_parse(r, &c, &e) == FMLX_API_INVALID_REQUEST && e.param &&
           !strcmp(e.param, "n"),
       "n:2 deve essere un rifiuto, non UNA scelta in risposta");
    r = body("{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"c\"}],\"stream\":1}",
             &d);
    CK(fmlx_api_openai_parse(r, &c, &e) == FMLX_API_INVALID_REQUEST, "stream non booleano");
    r = body("{\"model\":\"\",\"messages\":[{\"role\":\"user\",\"content\":\"c\"}]}", &d);
    CK(fmlx_api_openai_parse(r, &c, &e) == FMLX_API_INVALID_REQUEST && e.param &&
           !strcmp(e.param, "model"),
       "model vuoto");
    r = body("{\"messages\":[]}", &d);
    CK(fmlx_api_openai_parse(r, &c, &e) == FMLX_API_INVALID_REQUEST, "model assente");
    r = body("[1,2]", &d);
    CK(fmlx_api_openai_parse(r, &c, &e) == FMLX_API_INVALID_REQUEST, "radice non oggetto");
    CK(FMLX_API_MAX_STOP == 8u, "il limite di stop sequence esiste");
    r = body("{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"c\"}],"
             "\"stop\":[\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\",\"8\",\"9\"]}",
             &d);
    CK(fmlx_api_openai_parse(r, &c, &e) == FMLX_API_INVALID_REQUEST, "9 stop sequence > 8");
    r = body("{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"c\"}],"
             "\"max_tokens\":100,\"max_completion_tokens\":200}",
             &d);
    CK(fmlx_api_openai_parse(r, &c, &e) == FMLX_API_OK && c.max_tokens == 200 &&
           c.n_ignored == 1 && fmlx_api_streq(c.ignored[0], "max_tokens"),
       "due limiti diversi: vince il nuovo e l'altro è DICHIARATO ignorato");
    fmlx_json_doc_free(&d);
}

static void t_anthropic_parse(void) {
    printf("== 4. parse Anthropic\n");
    FmlxJsonDoc d;
    FmlxApiChat c;
    FmlxApiErrCtx e;

    const FmlxJsonValue *r =
        body("{\"model\":\"claude-test\",\"system\":[{\"type\":\"text\",\"text\":\"sei\"},"
             "{\"type\":\"text\",\"text\":\"utile\"}],\"messages\":[{\"role\":\"user\","
             "\"content\":\"ciao\"}],\"max_tokens\":64,\"temperature\":0.5,\"top_k\":40,"
             "\"top_p\":0.9,\"stop_sequences\":[\"END\"],\"stream\":false,"
             "\"tools\":[{\"name\":\"f\"}],\"service_tier\":\"standard\"}",
             &d);
    CK(fmlx_api_anthropic_parse(r, &c, &e) == FMLX_API_OK, "corpo Anthropic reale");
    CK(c.max_tokens == 64 && !c.max_tokens_defaulted, "max_tokens esplicito");
    CK(c.top_k == 40 && c.has_top_k, "top_k esiste solo qui");
    CK(c.n_stop == 1 && fmlx_api_streq(c.stop[0], "END"), "stop_sequences");
    CK(c.n_ignored == 1 && fmlx_api_streq(c.ignored[0], "service_tier"),
       "service_tier non è del piano: ignorato e DETTO");
    char mem[256];
    FmlxSlice sys;
    CK(fmlx_api_join_system(&c, mem, sizeof mem, &sys) == FMLX_API_OK && sys.n == 10 &&
           !memcmp(sys.p, "sei\n\nutile", 10),
       "i blocchi system di Anthropic si uniscono come i messaggi: %.*s", (int)sys.n, sys.p);
    CK(sys.p == mem, "qui la copia SERVE: due blocchi non sono contigui nel DOM");

    r = body("{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"c\"}]}", &d);
    CK(fmlx_api_anthropic_parse(r, &c, &e) == FMLX_API_OK && c.max_tokens == FMLX_API_DEF_MAX_TOKENS &&
           c.max_tokens_defaulted && c.n_warn == 1,
       "max_tokens obbligatorio: default del motore E warning (piano:510)");

    r = body("{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"c\"}],"
             "\"max_tokens\":64,\"temperature\":1.5}",
             &d);
    CK(fmlx_api_anthropic_parse(r, &c, &e) == FMLX_API_OK && c.temperature == 1.0 &&
           c.n_warn == 1,
       "1.5 su Anthropic è FUORI intervallo: normalizzato a 1.0 e LOGGATO, non taciuto "
       "(piano:511)");

    r = body("{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"c\"}],"
             "\"max_tokens\":0}",
             &d);
    CK(fmlx_api_anthropic_parse(r, &c, &e) == FMLX_API_INVALID_REQUEST, "max_tokens 0");
    r = body("{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"c\"}],"
             "\"max_tokens\":64,\"top_k\":-1}",
             &d);
    CK(fmlx_api_anthropic_parse(r, &c, &e) == FMLX_API_INVALID_REQUEST, "top_k negativo");
    r = body("{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"c\"}],"
             "\"max_tokens\":64,\"system\":42}",
             &d);
    CK(fmlx_api_anthropic_parse(r, &c, &e) == FMLX_API_INVALID_REQUEST, "system non testo");

    /* gli header */
    CK(fmlx_api_anthropic_version(SL("")) == 0, "versione assente = 0, non errore");
    CK(fmlx_api_anthropic_version(SL("2023-06-01")) == 1, "versione nota");
    CK(fmlx_api_anthropic_version(SL("2099-01-01")) == -1, "versione non nota");
    fmlx_api_chat_init(&c);
    fmlx_api_anthropic_beta(SL("interleaved-thinking-2025-05-14, magic-thing"), &c);
    CK(c.n_ignored == 1 && fmlx_api_streq(c.ignored[0], "anthropic-beta:magic-thing"),
       "la beta nota passa, l'altra è dichiarata: %u '%.*s'", c.n_ignored,
       (int)c.ignored[0].n, c.ignored[0].p);
    CK(c.ignored[0].p != NULL && c.ignored[0].n < FMLX_API_MAX_NAME &&
           c.ignored[0].p == c.ignored_pool[0],
       "la slice di un header COPIA in pool: un buffer di stack sarebbe un puntatore sospeso");
    fmlx_json_doc_free(&d);
}

static void t_acc_bridge(void) {
    printf("== 5. accumulatore e ponte arguments <-> input (piano:505-506)\n");
    /* La capienza è PER-TOOL: `acc_init` calcola `args + i * cap`, quindi il buffer deve
     * essere FMLX_API_MAX_TOOLCALLS volte tanto. Darne 3 di 8 sarebbe lo stesso errore di
     * misura che il test serve a scoprire. */
    char text[64], think[64], args[FMLX_API_MAX_TOOLCALLS * 32];
    FmlxApiAcc a;
    fmlx_api_acc_init(&a, text, sizeof text, think, sizeof think, args, 32);

    const FmlxApiEvent seq[] = {
        EVT_THK("penso"), EVT_TXT("ciao "), EVT_TXT("mondo"),
        EVT_TOOL("toolu_9", "get_weather"), EVT_ARGS("{\"city\":"), EVT_ARGS("\"Roma\"}"),
        EVT_DONE(FMLX_API_FINISH_TOOL, 11, 5, 3)};
    int ok = 1;
    for (size_t i = 0; i < sizeof seq / sizeof seq[0]; i++)
        if (fmlx_api_acc_push(&a, &seq[i]) != FMLX_API_OK) ok = 0;
    CK(ok && !a.overflow, "la sequenza entra nei buffer");
    CK(a.think_len == 5 && !memcmp(a.think, "penso", 5), "thinking accumulato (%zu byte)",
       a.think_len);
    /* 5 + 5: "ciao " e "mondo". Le misure si CONTANO, non si ricordano. */
    CK(a.text_len == 10 && !memcmp(a.text, "ciao mondo", 10), "i delta si uniscono (%zu byte)",
       a.text_len);
    CK(a.ntool == 1 && !a.overflow, "un tool");
    CK(a.tool[0].args_len == 15 && !memcmp(a.tool[0].args, "{\"city\":\"Roma\"}", 15),
       "gli argomenti bufferizzati dai due frame: %.*s", (int)a.tool[0].args_len, a.tool[0].args);
    CK(fmlx_api_acc_tool_ok(&a, 0) == 1, "il JSON completo è emettibile");
    CK(a.tool[0].closed, "DONE chiude i blocchi");

    /* il ponte: stringa -> oggetto */
    char out[128];
    FmlxJsonW w;
    fmlx_jsonw_init(&w, out, sizeof out, sizeof out);
    CK(fmlx_api_tool_input(&w, a.tool[0].args, a.tool[0].args_len) == FMLX_API_OK, "render");
    size_t n = 0;
    CK(fmlx_jsonw_finish(&w, &n) == FMLX_JSON_OK && n == 15 && !memcmp(out, "{\"city\":\"Roma\"}", 15),
       "`input` oggetto esatto: %.*s", (int)n, out);
    fmlx_jsonw_free(&w);

    /* TRONCATO: il caso che il piano nomina come quello che uccide i progetti */
    fmlx_api_acc_init(&a, text, sizeof text, think, sizeof think, args, 32);
    const FmlxApiEvent bad_seq[] = {EVT_TXT("vado"), EVT_TOOL(NULL, "f"), EVT_ARGS("{\"a\":1"),
                                    EVT_DONE(FMLX_API_FINISH_TOOL, 2, 3, 0)};
    for (size_t i = 0; i < 4; i++) CK(fmlx_api_acc_push(&a, &bad_seq[i]) == FMLX_API_OK, "push");
    CK(a.ntool == 1 && !a.tool[0].id[0], "id assente = quello lo genera l'adapter");
    CK(fmlx_api_acc_tool_ok(&a, 0) == 0, "JSON troncato: NON emettibile");

    /* `arguments` JSON valido ma NON un oggetto: comunque non emettibile */
    fmlx_api_acc_init(&a, text, sizeof text, think, sizeof think, args, 32);
    const FmlxApiEvent arr_seq[] = {EVT_TOOL("t", "f"), EVT_ARGS("42"),
                                    EVT_DONE(FMLX_API_FINISH_TOOL, 1, 1, 0)};
    for (size_t i = 0; i < 3; i++) fmlx_api_acc_push(&a, &arr_seq[i]);
    CK(fmlx_api_acc_tool_ok(&a, 0) == 0, "`42` è JSON ma `input` deve essere un oggetto");

    /* il motore che viola il confine */
    fmlx_api_acc_init(&a, text, sizeof text, think, sizeof think, args, 32);
    CK(fmlx_api_acc_push(&a, &bad_seq[2]) == FMLX_API_INTERNAL && a.overflow,
       "frame di argomenti SENZA tool aperto = errore, non 'lo scarto'");
    /* un token che non entra: la risposta non è più inviabile, e `overflow` deve dirlo */
    fmlx_api_acc_init(&a, text, 4, think, sizeof think, args, 32);
    const FmlxApiEvent long_tok = EVT_TXT("più di quattro byte");
    CK(fmlx_api_acc_push(&a, &long_tok) == FMLX_API_INTERNAL,
       "overflow su text");
    fmlx_api_acc_init(&a, text, sizeof text, think, sizeof think, args, 16);
    const FmlxApiEvent two[] = {EVT_TOOL("a", "f"), EVT_ARGS("{\"x\":123456789}"),
                                EVT_TOOL("b", "g"), EVT_ARGS("{}"),
                                EVT_DONE(FMLX_API_FINISH_TOOL, 1, 1, 0)};
    int all = 1;
    for (size_t i = 0; i < 5; i++)
        if (fmlx_api_acc_push(&a, &two[i]) != FMLX_API_OK) all = 0;
    /* Le misure si CONTANO: `{"x":123456789}` sono 15 byte, `{}` sono 2. E la prova che le
     * fette non si calpestano sono i BYTE del primo tool, letti DOPO che il secondo ha
     * scritto: con una `args` condivisa i `{}` stavano sopra l'inizio del JSON di `a`. */
    CK(all && a.tool[0].args_len == 15 && a.tool[1].args_len == 2 &&
           !memcmp(a.tool[0].args, "{\"x\":123456789}", 15),
       "ogni tool ha la SUA fetta di argomenti: condividerla avrebbe scritto fuori");

    /* le stringhe del finish (piano:501-504) */
    CK(!strcmp(fmlx_api_finish_openai(FMLX_API_FINISH_STOP), "stop") &&
           !strcmp(fmlx_api_finish_openai(FMLX_API_FINISH_LENGTH), "length") &&
           !strcmp(fmlx_api_finish_openai(FMLX_API_FINISH_TOOL), "tool_calls"),
       "vocabolario OpenAI");
    CK(!strcmp(fmlx_api_finish_anthropic(FMLX_API_FINISH_STOP), "end_turn") &&
           !strcmp(fmlx_api_finish_anthropic(FMLX_API_FINISH_LENGTH), "max_tokens") &&
           !strcmp(fmlx_api_finish_anthropic(FMLX_API_FINISH_TOOL), "tool_use") &&
           !strcmp(fmlx_api_finish_anthropic(FMLX_API_FINISH_STOPSEQ), "stop_sequence"),
       "vocabolario Anthropic");
    FmlxApiFinish f;
    CK(fmlx_api_finish_from_openai("length", &f) && f == FMLX_API_FINISH_LENGTH &&
           !fmlx_api_finish_from_openai("function_call", &f) &&
           !fmlx_api_finish_from_openai("sprobabile", &f),
       "il lessico non si inventa: un valore ignoto è un errore, non 'stop'");
}

/* ============================================================================ */

static FmlxApiChat chat_from(const char *src) {
    static FmlxJsonDoc d;
    static char keep[4096];
    FmlxApiChat c;
    FmlxApiErrCtx e;
    memcpy(keep, src, strlen(src) + 1);
    fmlx_api_chat_init(&c);
    const FmlxJsonValue *r = body(keep, &d);
    if (r && fmlx_api_openai_parse(r, &c, &e)) fmlx_api_chat_init(&c);
    return c;
}

static void t_openai_render(void) {
    printf("== 6. risposta OpenAI non-stream\n");
    FmlxApiChat c = chat_from("{\"model\":\"gpt-x\",\"messages\":[{\"role\":\"user\","
                              "\"content\":\"c\"}],\"logit_bias\":{}}");
    FmlxApiMeta m = {"chatcmpl-42", 1700000000, 0};
    char text[256], think[64], args[8 * 64], out[2048];
    FmlxApiAcc a;
    FmlxJsonW w;

    fmlx_api_acc_init(&a, text, sizeof text, think, sizeof think, args, 64);
    const FmlxApiEvent done = EVT_DONE(FMLX_API_FINISH_STOP, 11, 3, 0);
    const FmlxApiEvent t1 = EVT_TXT("ciao");
    fmlx_api_acc_push(&a, &t1);
    fmlx_api_acc_push(&a, &done);
    fmlx_jsonw_init(&w, out, sizeof out, sizeof out);
    CK(fmlx_api_openai_completion(&c, &m, &a, &w) == FMLX_API_OK, "completamento semplice");
    size_t n = 0;
    CK(fmlx_jsonw_finish(&w, &n) == FMLX_JSON_OK, "finish");
    out[n] = '\0';
    CK(HAS(out, "\"object\":\"chat.completion\""), "object");
    CK(HAS(out, "\"id\":\"chatcmpl-42\"") && HAS(out, "\"created\":1700000000"), "id e created dal meta");
    CK(HAS(out, "\"content\":\"ciao\"") && HAS(out, "\"finish_reason\":\"stop\""), "content e finish");
    CK(HAS(out, "\"usage\":{\"prompt_tokens\":11,\"completion_tokens\":3,\"total_tokens\":14}"),
       "total_tokens SOMMATO qui, non fidarsi del client: %s", out);
    CK(HAS(out, "\"logprobs\":null"), "logprobs null e non assente (il campo è nel contratto)");
    CK(HAS(out, "\"ignored_params\":[\"logit_bias\"]"), "il campo non onorato è DICHIARATO");
    CK(!HAS(out, "reasoning_content"), "niente thinking => nessun campo thinking");
    CK(!HAS(out, "tool_calls"), "niente tool => nessun campo tool_calls");
    fmlx_jsonw_free(&w);

    /* thinking presente */
    fmlx_api_acc_init(&a, text, sizeof text, think, sizeof think, args, 64);
    const FmlxApiEvent th = EVT_THK("rifletto");
    fmlx_api_acc_push(&a, &th);
    fmlx_api_acc_push(&a, &t1);
    fmlx_api_acc_push(&a, &done);
    fmlx_jsonw_init(&w, out, sizeof out, sizeof out);
    fmlx_api_openai_completion(&c, &m, &a, &w);
    fmlx_jsonw_finish(&w, &n);
    out[n] = '\0';
    CK(HAS(out, "\"reasoning_content\":\"rifletto\""),
       "il thinking e' in `message.reasoning_content` (estensione de-fatto, come nel delta; piano:466,469)");
    fmlx_jsonw_free(&w);

    /* tool con argomenti troncati */
    fmlx_api_acc_init(&a, text, sizeof text, think, sizeof think, args, 64);
    const FmlxApiEvent tl = EVT_TOOL(NULL, "f"), ar = EVT_ARGS("{\"a\":"),
                       dn2 = EVT_DONE(FMLX_API_FINISH_TOOL, 1, 2, 0);
    fmlx_api_acc_push(&a, &tl);
    fmlx_api_acc_push(&a, &ar);
    fmlx_api_acc_push(&a, &dn2);
    fmlx_jsonw_init(&w, out, sizeof out, sizeof out);
    CK(fmlx_api_openai_completion(&c, &m, &a, &w) == FMLX_API_OK, "risposta comunque emessa");
    fmlx_jsonw_finish(&w, &n);
    out[n] = '\0';
    CK(!HAS(out, "tool_calls"), "MAI un tool_calls parziale (piano:505): %s", out);
    CK(HAS(out, "\"finish_reason\":\"length\"") && !HAS(out, "\"tool_calls\""),
       "e il finish diventa length, non tool_calls: %s", out);
    CK(HAS(out, "\"content\":\"\""), "content vuoto e non null: nessun tool è stato emesso");
    fmlx_jsonw_free(&w);

    /* tool valido */
    fmlx_api_acc_init(&a, text, sizeof text, think, sizeof think, args, 64);
    const FmlxApiEvent tl2 = EVT_TOOL("call_7", "weather"), ar2 = EVT_ARGS("{\"city\":\"Roma\"}"),
                       dn3 = EVT_DONE(FMLX_API_FINISH_TOOL, 1, 2, 0);
    fmlx_api_acc_push(&a, &tl2);
    fmlx_api_acc_push(&a, &ar2);
    fmlx_api_acc_push(&a, &dn3);
    fmlx_jsonw_init(&w, out, sizeof out, sizeof out);
    fmlx_api_openai_completion(&c, &m, &a, &w);
    fmlx_jsonw_finish(&w, &n);
    out[n] = '\0';
    CK(HAS(out, "\"content\":null") && HAS(out, "\"finish_reason\":\"tool_calls\""),
       "tool chiamati => content null (il formato reale), non stringa vuota: %s", out);
    CK(HAS(out, "{\"id\":\"call_7\",\"type\":\"function\",\"function\":{\"name\":\"weather\","
                "\"arguments\":\"{\\\"city\\\":\\\"Roma\\\"}\"}}"),
       "arguments resta STRINGA in OpenAI: %s", out);
    fmlx_jsonw_free(&w);
}

static void t_anthropic_render(void) {
    printf("== 7. risposta Anthropic non-stream + ordine degli eventi\n");
    FmlxJsonDoc d;
    FmlxApiChat c;
    FmlxApiErrCtx e;
    const char *src = "{\"model\":\"claude-test\",\"messages\":[{\"role\":\"user\","
                      "\"content\":\"c\"}],\"max_tokens\":64}";
    const FmlxJsonValue *r = body(src, &d);
    CK(fmlx_api_anthropic_parse(r, &c, &e) == FMLX_API_OK, "parse per la risposta");
    FmlxApiMeta m = {"msg_1", 1700000000, 0};
    char text[256], think[128], args[8 * 128], out[2048];
    FmlxApiAcc a;
    FmlxJsonW w;

    fmlx_api_acc_init(&a, text, sizeof text, think, sizeof think, args, 128);
    const FmlxApiEvent seq[] = {EVT_THK("penso"), EVT_TXT("ciao"), EVT_TOOL("toolu_0", "get_weather"),
                                EVT_ARGS("{\"city\":\"Roma\"}"),
                                EVT_DONE(FMLX_API_FINISH_TOOL, 11, 5, 3)};
    for (size_t i = 0; i < 5; i++) fmlx_api_acc_push(&a, &seq[i]);
    fmlx_jsonw_init(&w, out, sizeof out, sizeof out);
    CK(fmlx_api_anthropic_message(&c, &m, &a, &w) == FMLX_API_OK, "message render");
    size_t n = 0;
    CK(fmlx_jsonw_finish(&w, &n) == FMLX_JSON_OK, "message finish");
    out[n] = '\0';
    /* IL PUNTO DEL PEZZO: la stessa stringa che OpenAI emette fra virgolette e qui è un
     * oggetto, e l'ordine dei blocchi è thinking(0), text(1), tool_use(2). */
    CK(!strcmp(out, "{\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\","
                    "\"model\":\"claude-test\",\"content\":[{"
                    "\"type\":\"thinking\",\"thinking\":\"penso\",\"signature\":\"\"},{"
                    "\"type\":\"text\",\"text\":\"ciao\"},{"
                    "\"type\":\"tool_use\",\"id\":\"toolu_0\",\"name\":\"get_weather\","
                    "\"input\":{\"city\":\"Roma\"}}],"
                    "\"stop_reason\":\"tool_use\",\"stop_sequence\":null,"
                    "\"usage\":{\"input_tokens\":11,\"output_tokens\":5,"
                    "\"cache_read_input_tokens\":3,\"cache_creation_input_tokens\":0}}")
           ? 1
           : (printf("  ---- atteso:\n  ---- avuto: %s\n", out), 0),
       "risposta Anthropic byte-esatta con il ponte arguments->input");
    fmlx_jsonw_free(&w);

    /* niente thinking => il campo è ASSENTE, non vuoto (piano:508) */
    fmlx_api_acc_init(&a, text, sizeof text, think, sizeof think, args, 128);
    const FmlxApiEvent s2[] = {EVT_TXT("solo testo"), EVT_DONE(FMLX_API_FINISH_STOP, 2, 1, 0)};
    for (size_t i = 0; i < 2; i++) fmlx_api_acc_push(&a, &s2[i]);
    fmlx_jsonw_init(&w, out, sizeof out, sizeof out);
    fmlx_api_anthropic_message(&c, &m, &a, &w);
    fmlx_jsonw_finish(&w, &n);
    out[n] = '\0';
    CK(!HAS(out, "thinking") && !HAS(out, "signature"),
       "blocco thinking assente, non una stringa vuota: %s", out);
    CK(HAS(out, "\"stop_reason\":\"end_turn\""), "end_turn per stop");
    CK(HAS(out, "\"content\":[{\"type\":\"text\",\"text\":\"solo testo\"}]"), "un solo blocco");
    fmlx_jsonw_free(&w);

    /* count_tokens: rifiutare è l'unica risposta onesta (piano:494) */
    memset(&e, 0, sizeof e);
    CK(fmlx_api_anthropic_count_tokens(&e) == FMLX_API_NOT_SUPPORTED &&
           fmlx_api_http_status(e.err) == 501 &&
           HAS(e.message, "does not estimate"),
       "count_tokens deve DIRE che non può, non stimare");
    fmlx_json_doc_free(&d);
}

/* ---- lo streaming ---------------------------------------------------------- */

static const char *EV_NAME(const char *buf, const char *what) {
    char key[64];
    /* `event: X` sul filo finisce `\r\n`, non `\n`: cercare il solo `\n` voleva dire non
     * trovare NULLA e leggere quel "nulla" come un ordine di eventi sbagliato. */
    snprintf(key, sizeof key, "event: %s\r\n", what);
    return strstr(buf, key);
}

static void t_stream(void) {
    printf("== 8. streaming SSE: ordine e [DONE]\n");
    static char buf[65536];
    FmlxHttpRes w;
    FmlxApiChat c = chat_from("{\"model\":\"gpt-x\",\"messages\":[{\"role\":\"user\","
                             "\"content\":\"c\"}],\"stream\":true,"
                             "\"stream_options\":{\"include_usage\":true}}");
    FmlxApiMeta m = {"chatcmpl-9", 1700000000, 0};
    const FmlxApiEvent seq[] = {EVT_TXT("ci"), EVT_TXT("ao"),
                                EVT_DONE(FMLX_API_FINISH_STOP, 7, 2, 0)};
    Script s = {seq, 3, 0};

    fmlx_http_res_init(&w, buf, sizeof buf);
    CK(fmlx_api_sse_head(&w) == FMLX_HTTP_OK, "head SSE");
    CK(HAS(buf, "Transfer-Encoding: chunked") && HAS(buf, "text/event-stream"),
       "chunked e content-type");
    CK(!HAS(buf, "Content-Length"), "uno stream non ha Content-Length");
    w.len = 0;
    FmlxApiErr se = fmlx_api_openai_stream(&c, &m, gen_next, &s, &w);
    CK(se == FMLX_API_OK, "stream openai (codice=%d, byte=%zu)", (int)se, w.len);
    buf[w.len] = '\0';
    CK(HAS(buf, "data: [DONE]"), "l'evento letterale [DONE] (piano:471)");
    CK(HAS(buf, "\"role\":\"assistant\""), "il primo frame porta il ruolo");
    CK(HAS(buf, "\"content\":\"ci\"") && HAS(buf, "\"content\":\"ao\""), "due delta separati");
    CK(HAS(buf, "\"object\":\"chat.completion.chunk\""), "ogni frame è un chunk");
    CK(HAS(buf, "\"finish_reason\":null"), "i delta hanno finish_reason NULL, non assente");
    CK(HAS(buf, "\"finish_reason\":\"stop\""), "e l'ultimo ce l'ha valorizzato");
    CK(HAS(buf, "\"total_tokens\":9"), "include_usage => usage nell'ultimo frame");
    int nulls = 0, total = 0;
    for (const char *p = buf; (p = strstr(p, "\"finish_reason\"")); p++) {
        total++;
        if (!strncmp(p + 15, ":null", 5)) nulls++;
    }
    CK(total == 4 && nulls == 3, "3 frame con null + 1 col valore (avuti %d/%d)", total, nulls);

    /* errore del motore a metà stream */
    fmlx_http_res_init(&w, buf, sizeof buf);
    fmlx_api_sse_head(&w);
    w.len = 0;
    Script sb = {seq, 0, 0};
    CK(fmlx_api_openai_stream(&c, &m, gen_err, &sb, &w) == FMLX_API_UPSTREAM_HUB,
       "l'errore del motore sale con IL SUO codice, non con 'internal'");
    buf[w.len] = '\0';
    CK(!HAS(buf, "[DONE]"), "un errore a metà stream non produce [DONE]");
    /* Ma la risposta chunked DEVE chiudersi lo stesso: senza il chunk terminale il client
     * resta attaccato al socket ad aspettare byte che non arriveranno mai. */
    CK(HAS(buf, "0\r\n\r\n"), "l'errore NON lascia la risposta chunked aperta");

    /* --- Anthropic: l'ordine è IL contratto (piano:489-492) --- */
    FmlxJsonDoc d;
    FmlxApiChat ac;
    FmlxApiErrCtx e;
    const char *src = "{\"model\":\"claude-test\",\"messages\":[{\"role\":\"user\","
                      "\"content\":\"c\"}],\"max_tokens\":64,\"stream\":true}";
    fmlx_api_anthropic_parse(body(src, &d), &ac, &e);
    const FmlxApiEvent aseq[] = {EVT_THK("pen"), EVT_THK("so"), EVT_TXT("ciao"),
                                 EVT_TOOL("toolu_0", "f"), EVT_ARGS("{\"a\":1}"),
                                 EVT_DONE(FMLX_API_FINISH_TOOL, 4, 6, 2)};
    Script as = {aseq, 6, 0};
    fmlx_http_res_init(&w, buf, sizeof buf);
    fmlx_api_sse_head(&w);
    w.len = 0;
    CK(fmlx_api_anthropic_stream(&ac, &m, gen_next, &as, &w) == FMLX_API_OK, "stream anthropic");
    buf[w.len] = '\0';
    const char *order[] = {"message_start", "content_block_start", "content_block_delta",
                           "content_block_stop", "message_delta", "message_stop"};
    const char *prev = NULL;
    int ord_ok = 1;
    for (size_t i = 0; i < 6; i++) {
        const char *p = EV_NAME(buf, order[i]);
        if (!p) { ord_ok = 0; break; }
        if (prev && p < prev) ord_ok = 0;
        prev = p;
    }
    CK(ord_ok, "l'ordine canonico dei sei eventi: %.120s", buf);
    CK(strstr(buf, "event: message_start") < strstr(buf, "event: content_block_start"),
       "message_start PRIMA di tutto");
    CK(strstr(buf, "event: message_delta") < strstr(buf, "event: message_stop"),
       "message_delta prima di message_stop");
    CK(HAS(buf, "\"type\":\"thinking_delta\"") && HAS(buf, "\"thinking\":\"pen\""),
       "thinking_delta con il campo `thinking` (piano:490)");
    CK(HAS(buf, "\"type\":\"signature_delta\""), "la chiusura del blocco thinking firma");
    CK(HAS(buf, "\"type\":\"text_delta\",\"text\":\"ciao\"") ||
           HAS(buf, "\"text\":\"ciao\""),
       "text_delta sul blocco di testo");
    CK(HAS(buf, "\"type\":\"input_json_delta\"") && HAS(buf, "\"partial_json\":\"{\\\"a\\\":1}\""),
       "input_json_delta con gli argomenti COMPLETI in un colpo solo (piano:506)");
    CK(HAS(buf, "\"index\":0") && HAS(buf, "\"index\":1") && HAS(buf, "\"index\":2"),
       "tre indici di blocco: thinking 0, text 1, tool 2");
    CK(HAS(buf, "\"stop_reason\":\"tool_use\""), "stop_reason finale tool_use");
    CK(HAS(buf, "\"usage\":{\"output_tokens\":6}"), "message_delta porta SOLO output_tokens");
    CK(HAS(buf, "\"input_tokens\":0"), "message_start porta input_tokens (0 = non contato)");
    int starts = 0, stops = 0;
    for (const char *p = buf; (p = strstr(p, "event: content_block_start")); p++) starts++;
    for (const char *p = buf; (p = strstr(p, "event: content_block_stop")); p++) stops++;
    CK(starts == 3 && stops == 3, "ogni blocco aperto è CHIUSO (avuti %d/%d)", starts, stops);
    fmlx_json_doc_free(&d);

    /* ARGOMENTI TRONCATI in streaming: nessun tool_use, stop_reason max_tokens */
    fmlx_api_anthropic_parse(body(src, &d), &ac, &e);
    const FmlxApiEvent tseq[] = {EVT_TXT("vado"), EVT_TOOL("t0", "f"), EVT_ARGS("{\"a\":"),
                                 EVT_DONE(FMLX_API_FINISH_TOOL, 1, 2, 0)};
    Script ts = {tseq, 4, 0};
    fmlx_http_res_init(&w, buf, sizeof buf);
    fmlx_api_sse_head(&w);
    w.len = 0;
    CK(fmlx_api_anthropic_stream(&ac, &m, gen_next, &ts, &w) == FMLX_API_OK,
       "lo stream si chiude comunque, non si appende");
    buf[w.len] = '\0';
    CK(!HAS(buf, "tool_use"), "MAI emesso un blocco tool_use con input parziale: %.200s", buf);
    CK(!HAS(buf, "partial_json"), "e nessun input_json_delta");
    CK(HAS(buf, "\"stop_reason\":\"max_tokens\""),
       "il motivo diventa max_tokens (piano:505): %s", strstr(buf, "message_delta") ?
           strstr(buf, "message_delta") : "(nessun message_delta)");
    CK(HAS(buf, "\"text\":\"vado\""), "il testo partito prima resta: un byte non si ritira");
    fmlx_json_doc_free(&d);

    /* ping e errore in-stream */
    fmlx_http_res_init(&w, buf, sizeof buf);
    fmlx_api_sse_head(&w);
    w.len = 0;
    CK(fmlx_api_anthropic_ping(&w) == FMLX_API_OK && HAS(buf, "event: ping") &&
           HAS(buf, "{\"type\":\"ping\"}"),
       "ping keepalive (piano:491)");
    FmlxApiErrCtx ec = {.err = FMLX_API_UPSTREAM_HUB, .message = "upstream said no"};
    CK(fmlx_api_anthropic_sse_error(&ec, &w) == FMLX_API_OK && HAS(buf, "event: error") &&
           HAS(buf, "{\"type\":\"error\",\"error\":{\"type\":\"api_error\","
                    "\"message\":\"upstream said no\"}}"),
       "l'evento error con il corpo di piano:518");
    buf[w.len] = '\0';

    /* e l'errore del motore IN MEZZO a uno stream Anthropic: message_start c'è, e dopo deve
     * arrivare `error`, non un `message_stop` che dice "finito bene". */
    fmlx_api_anthropic_parse(body(src, &d), &ac, &e);
    fmlx_http_res_init(&w, buf, sizeof buf);
    fmlx_api_sse_head(&w);
    w.len = 0;
    Script es = {seq, 0, 0};
    CK(fmlx_api_anthropic_stream(&ac, &m, gen_err, &es, &w) == FMLX_API_UPSTREAM_HUB,
       "il codice del motore sale anche qui");
    buf[w.len] = '\0';
    CK(HAS(buf, "event: message_start") && HAS(buf, "event: error") &&
           !HAS(buf, "event: message_stop"),
       "message_start → error, mai message_stop (piano:493)");
    CK(HAS(buf, "0\r\n\r\n"), "anche l'errore Anthropic chiude la risposta chunked");
    fmlx_json_doc_free(&d);
}

/* ============================================================================ */

/* La politica dei tool e `response_format`/`logprobs`: sono le tre decisioni del passo 4 che
 * NON richiedono il motore — chi decide cosa chiedere al modello, e cosa rifiutare. */
static void t_tool_policy_rf(void) {
    printf("== 9. tool policy, response_format, logprobs\n");
    FmlxJsonDoc d;
    FmlxApiChat c;
    FmlxApiErrCtx e;
    FmlxSlice named;

    /* tools senza tool_choice -> AUTO */
    const FmlxJsonValue *r = body("{\"model\":\"m\",\"messages\":[{\"role\":\"user\","
                                  "\"content\":\"c\"}],\"tools\":[{\"type\":\"function\","
                                  "\"function\":{\"name\":\"f\"}}]}",
                                  &d);
    CK(fmlx_api_openai_parse(r, &c, &e) == FMLX_API_OK, "tools parse");
    CK(fmlx_api_tool_mode(&c, &named) == FMLX_API_TOOL_AUTO, "senza tool_choice: AUTO");
    fmlx_json_doc_free(&d);

    r = body("{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"c\"}],"
             "\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"f\"}}],"
             "\"tool_choice\":\"none\"}",
             &d);
    fmlx_api_openai_parse(r, &c, &e);
    CK(fmlx_api_tool_mode(&c, &named) == FMLX_API_TOOL_NONE, "tool_choice:none spegne i tool");
    fmlx_json_doc_free(&d);

    r = body("{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"c\"}],"
             "\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"f\"}}],"
             "\"tool_choice\":\"required\"}",
             &d);
    fmlx_api_openai_parse(r, &c, &e);
    CK(fmlx_api_tool_mode(&c, &named) == FMLX_API_TOOL_REQUIRED, "tool_choice:required");
    fmlx_json_doc_free(&d);

    r = body("{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"c\"}],"
             "\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"f\"}}],"
             "\"tool_choice\":{\"type\":\"function\",\"function\":{\"name\":\"get_weather\"}}}",
             &d);
    fmlx_api_openai_parse(r, &c, &e);
    CK(fmlx_api_tool_mode(&c, &named) == FMLX_API_TOOL_NAMED &&
           named.n == 11 && !memcmp(named.p, "get_weather", 11),
       "tool_choice nominato: NAMED con get_weather");
    fmlx_json_doc_free(&d);

    /* Anthropic: {type:any} -> required; {type:tool,name} -> named */
    r = body("{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"c\"}],"
             "\"tools\":[{\"name\":\"f\"}],\"tool_choice\":{\"type\":\"any\"}}",
             &d);
    fmlx_api_anthropic_parse(r, &c, &e);
    CK(fmlx_api_tool_mode(&c, &named) == FMLX_API_TOOL_REQUIRED, "Anthropic type:any");
    fmlx_json_doc_free(&d);
    r = body("{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"c\"}],"
             "\"tools\":[{\"name\":\"f\"}],\"tool_choice\":{\"type\":\"tool\",\"name\":\"f\"}}",
             &d);
    fmlx_api_anthropic_parse(r, &c, &e);
    CK(fmlx_api_tool_mode(&c, &named) == FMLX_API_TOOL_NAMED && named.n == 1 &&
           named.p[0] == 'f',
       "Anthropic type:tool");
    fmlx_json_doc_free(&d);

    /* response_format: onorato per json_object/json_schema, NON finisce negli ignored */
    r = body("{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"c\"}],"
             "\"response_format\":{\"type\":\"json_object\"}}",
             &d);
    CK(fmlx_api_openai_parse(r, &c, &e) == FMLX_API_OK && c.rf_json_object &&
           c.response_format != NULL,
       "response_format json_object onorato");
    CK(c.n_ignored == 0, "response_format NON è fra gli ignorati, %u", c.n_ignored);
    fmlx_json_doc_free(&d);

    r = body("{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"c\"}],"
             "\"response_format\":{\"type\":\"json_schema\",\"json_schema\":{\"name\":\"x\","
             "\"schema\":{\"type\":\"object\",\"properties\":{\"a\":{\"type\":\"integer\"}}"
             "}}}}",
             &d);
    CK(fmlx_api_openai_parse(r, &c, &e) == FMLX_API_OK && c.rf_json_schema,
       "response_format json_schema onorato");
    fmlx_json_doc_free(&d);

    r = body("{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"c\"}],"
             "\"response_format\":{\"type\":\"nonesiste\"}}",
             &d);
    CK(fmlx_api_openai_parse(r, &c, &e) == FMLX_API_INVALID_REQUEST && e.param &&
           !strcmp(e.param, "response_format"),
       "tipo di response_format ignoto = errore, non prosa");
    fmlx_json_doc_free(&d);

    /* logprobs (ABI v2): onorato — il motore emette la logprob del token scelto. */
    r = body("{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"c\"}],"
             "\"logprobs\":true}",
             &d);
    CK(fmlx_api_openai_parse(r, &c, &e) == FMLX_API_OK && c.want_logprobs == 1,
       "logprobs:true è onorato (logprob del token scelto dall'ABI v2)");
    fmlx_json_doc_free(&d);
    /* top_logprobs (ABI v3): onorato con logprobs:true, esige logprobs e un K <= 20. */
    r = body("{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"c\"}],"
             "\"logprobs\":true,\"top_logprobs\":5}",
             &d);
    CK(fmlx_api_openai_parse(r, &c, &e) == FMLX_API_OK && c.want_logprobs == 1 &&
           c.want_top_logprobs == 5,
       "top_logprobs:5 onorato (ABI v3 token_topk)");
    fmlx_json_doc_free(&d);
    r = body("{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"c\"}],"
             "\"top_logprobs\":5}",
             &d);
    CK(fmlx_api_openai_parse(r, &c, &e) == FMLX_API_INVALID_REQUEST && e.param &&
           !strcmp(e.param, "top_logprobs"),
       "top_logprobs senza logprobs:true = errore (contratto OpenAI)");
    fmlx_json_doc_free(&d);
    r = body("{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"c\"}],"
             "\"logprobs\":true,\"top_logprobs\":21}",
             &d);
    CK(fmlx_api_openai_parse(r, &c, &e) == FMLX_API_INVALID_REQUEST && e.param &&
           !strcmp(e.param, "top_logprobs"),
       "top_logprobs oltre 20 = errore esplicito");
    fmlx_json_doc_free(&d);
    r = body("{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"c\"}],"
             "\"logprobs\":false}",
             &d);
    CK(fmlx_api_openai_parse(r, &c, &e) == FMLX_API_OK && c.want_logprobs == 0,
       "logprobs:false è onorato (non si chiede nulla)");
    fmlx_json_doc_free(&d);
    /* logprobs + tool: rifiutato, il testo passerebbe dal riconoscitore (token↔logprob non esatta) */
    r = body("{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"c\"}],"
             "\"logprobs\":true,\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"f\"}}]}",
             &d);
    CK(fmlx_api_openai_parse(r, &c, &e) == FMLX_API_INVALID_REQUEST && e.param &&
           !strcmp(e.param, "logprobs"),
       "logprobs + tool rifiutato (la corrispondenza token↔logprob non sarebbe esatta)");
    fmlx_json_doc_free(&d);
}

/* la logprob che viaggia su un evento TEXT (un token per evento). */
static FmlxApiEvent EVT_LP(const char *t, float lp) {
    FmlxApiEvent e = {.ev = FMLX_API_EV_TEXT, .text = t, .n = (uint32_t)strlen(t),
                      .has_logprob = 1, .logprob = lp};
    return e;
}

/* ============================================================================ */

/* Le logprobs in risposta: chat `logprobs.content[]` e completions legacy. La logprob del
 * token scelto è REALE (dall'evento); `top_logprobs` è vuoto/null perché l'ABI non dà il top-k. */
static void t_logprobs_render(void) {
    printf("== 10. logprobs in risposta (chat e completions)\n");
    FmlxApiChat c = chat_from("{\"model\":\"gpt-x\",\"messages\":[{\"role\":\"user\","
                              "\"content\":\"c\"}],\"logprobs\":true,\"top_logprobs\":0}");
    FmlxApiMeta m = {"chatcmpl-lp", 1700000000, 0};
    char text[256], think[64], args[8 * 64], out[4096];
    float lp[8];
    uint32_t lpo[8], lpl[8];
    FmlxApiAcc a;
    FmlxJsonW w;
    size_t n = 0;

    CK(c.want_logprobs == 1, "la chat chiede le logprobs");
    fmlx_api_acc_init(&a, text, sizeof text, think, sizeof think, args, 64);
    fmlx_api_acc_logbuf(&a, lp, lpo, lpl, 8);
    const FmlxApiEvent s[] = {EVT_LP("Hel", -0.5f), EVT_LP("lo", -1.25f),
                              EVT_DONE(FMLX_API_FINISH_STOP, 4, 2, 0)};
    for (int i = 0; i < 3; i++) fmlx_api_acc_push(&a, &s[i]);
    CK(a.n_logprobs == 2, "due token annotati (n=%d)", a.n_logprobs);
    fmlx_jsonw_init(&w, out, sizeof out, sizeof out);
    CK(fmlx_api_openai_completion(&c, &m, &a, &w) == FMLX_API_OK, "chat con logprobs");
    CK(fmlx_jsonw_finish(&w, &n) == FMLX_JSON_OK, "finish");
    out[n] = '\0';
    CK(HAS(out, "\"logprobs\":{\"content\":["), "logprobs.content presente: %s", out);
    CK(HAS(out, "\"token\":\"Hel\",\"logprob\":-0.5"), "logprob reale del primo token: %s", out);
    CK(HAS(out, "\"token\":\"lo\",\"logprob\":-1.25"), "logprob reale del secondo token");
    CK(HAS(out, "\"bytes\":[72,101,108]"), "i byte UTF-8 del token");
    CK(HAS(out, "\"top_logprobs\":[]"), "top_logprobs vuoto (l'ABI non dà le alternative)");
    fmlx_jsonw_free(&w);

    /* top_logprobs reali (ABI v3): le alternative dall'evento finiscono in `top_logprobs[]`. */
    {
        FmlxApiChat c2 = chat_from("{\"model\":\"gpt-x\",\"messages\":[{\"role\":\"user\","
                                   "\"content\":\"c\"}],\"logprobs\":true,\"top_logprobs\":2}");
        FmlxApiTopLogprob tt[8 * 2];
        FmlxApiEvent e = EVT_LP("lo", -1.25f);
        e.n_top = 2;
        memcpy(e.top[0].token, "lo", 3);
        e.top[0].n = 2;
        e.top[0].logprob = -1.25f;
        memcpy(e.top[1].token, "la", 3);
        e.top[1].n = 2;
        e.top[1].logprob = -2.0f;
        FmlxApiEvent d2 = EVT_DONE(FMLX_API_FINISH_STOP, 4, 1, 0);

        fmlx_api_acc_init(&a, text, sizeof text, think, sizeof think, args, 64);
        fmlx_api_acc_logbuf(&a, lp, lpo, lpl, 8);
        fmlx_api_acc_topbuf(&a, tt, 2);
        fmlx_api_acc_push(&a, &e);
        fmlx_api_acc_push(&a, &d2);
        fmlx_jsonw_init(&w, out, sizeof out, sizeof out);
        CK(fmlx_api_openai_completion(&c2, &m, &a, &w) == FMLX_API_OK, "chat con top_logprobs");
        CK(fmlx_jsonw_finish(&w, &n) == FMLX_JSON_OK, "finish top");
        out[n] = '\0';
        CK(HAS(out, "\"top_logprobs\":[{\"token\":\"lo\",\"logprob\":-1.25,"), "alternativa 0");
        CK(HAS(out, "{\"token\":\"la\",\"logprob\":-2.0,"), "alternativa 1: %s", out);
        fmlx_jsonw_free(&w);
    }

    /* completions legacy: le quattro liste parallele. */
    fmlx_api_acc_init(&a, text, sizeof text, think, sizeof think, args, 64);
    fmlx_api_acc_logbuf(&a, lp, lpo, lpl, 8);
    for (int i = 0; i < 3; i++) fmlx_api_acc_push(&a, &s[i]);
    fmlx_jsonw_init(&w, out, sizeof out, sizeof out);
    CK(fmlx_api_openai_completion_text(&c, &m, &a, &w) == FMLX_API_OK, "completions con logprobs");
    CK(fmlx_jsonw_finish(&w, &n) == FMLX_JSON_OK, "finish text");
    out[n] = '\0';
    CK(HAS(out, "\"tokens\":[\"Hel\",\"lo\"]"), "tokens paralleli: %s", out);
    CK(HAS(out, "\"token_logprobs\":[-0.5,-1.25]"), "token_logprobs reali");
    CK(HAS(out, "\"top_logprobs\":[null,null]"), "top_logprobs null per token");
    CK(HAS(out, "\"text_offset\":[0,3]"), "text_offset cumulativi");
    fmlx_jsonw_free(&w);
}

/* `/v1/embeddings`: parse (stringa/array, rifiuti espliciti) e forma della risposta. */
static void t_embeddings(void) {
    printf("== 11. embeddings (parse e render)\n");
    FmlxJsonDoc d;
    FmlxApiEmbeddings e;
    FmlxApiErrCtx ec;
    char out[1024];
    FmlxJsonW w;
    size_t n = 0;

    const FmlxJsonValue *r = body("{\"model\":\"m\",\"input\":\"hello\"}", &d);
    CK(fmlx_api_openai_embeddings_parse(r, &e, &ec) == FMLX_API_OK && e.n_input == 1,
       "input stringa: 1 voce");
    fmlx_json_doc_free(&d);

    r = body("{\"model\":\"m\",\"input\":[\"a\",\"b\",\"c\"]}", &d);
    CK(fmlx_api_openai_embeddings_parse(r, &e, &ec) == FMLX_API_OK && e.n_input == 3,
       "input array: 3 voci");
    fmlx_json_doc_free(&d);

    r = body("{\"model\":\"m\"}", &d);
    CK(fmlx_api_openai_embeddings_parse(r, &e, &ec) == FMLX_API_INVALID_REQUEST && ec.param &&
           !strcmp(ec.param, "input"),
       "input mancante = errore");
    fmlx_json_doc_free(&d);

    r = body("{\"model\":\"m\",\"input\":[1,2,3]}", &d);
    CK(fmlx_api_openai_embeddings_parse(r, &e, &ec) == FMLX_API_INVALID_REQUEST && ec.param &&
           !strcmp(ec.param, "input"),
       "input di token-id non supportato = errore");
    fmlx_json_doc_free(&d);

    r = body("{\"model\":\"m\",\"input\":\"x\",\"dimensions\":256}", &d);
    CK(fmlx_api_openai_embeddings_parse(r, &e, &ec) == FMLX_API_INVALID_REQUEST && ec.param &&
           !strcmp(ec.param, "dimensions"),
       "dimensions non supportato = errore");
    fmlx_json_doc_free(&d);

    r = body("{\"model\":\"m\",\"input\":\"x\",\"encoding_format\":\"base64\"}", &d);
    CK(fmlx_api_openai_embeddings_parse(r, &e, &ec) == FMLX_API_INVALID_REQUEST && ec.param &&
           !strcmp(ec.param, "encoding_format"),
       "encoding_format base64 = errore");
    fmlx_json_doc_free(&d);

    r = body("{\"model\":\"m\",\"input\":\"x\",\"user\":\"u\"}", &d);
    CK(fmlx_api_openai_embeddings_parse(r, &e, &ec) == FMLX_API_OK && e.n_ignored == 1,
       "campo ignoto (user) dichiarato in ignored");
    {
        float data[2] = {0.5f, -1.0f};
        fmlx_jsonw_init(&w, out, sizeof out, sizeof out);
        CK(fmlx_api_openai_embeddings_render(&e, data, 1, 2, 3, &w) == FMLX_API_OK &&
               fmlx_jsonw_finish(&w, &n) == FMLX_JSON_OK,
           "render embeddings");
        out[n] = '\0';
        CK(HAS(out, "\"object\":\"list\""), "object:list");
        CK(HAS(out, "\"embedding\":[0.5,-1.0]"), "vettore: %s", out);
        CK(HAS(out, "\"prompt_tokens\":3,\"total_tokens\":3"), "usage");
        CK(HAS(out, "\"ignored_params\":[\"user\"]"), "ignored_params");
        fmlx_jsonw_free(&w);
    }
    fmlx_json_doc_free(&d);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0); /* un SIGBUS deve dire QUALE sezione, non niente */
    t_router();
    t_errors();
    t_openai_parse();
    t_anthropic_parse();
    t_acc_bridge();
    t_openai_render();
    t_anthropic_render();
    t_stream();
    t_tool_policy_rf();
    t_logprobs_render();
    t_embeddings();
    printf("\napi: %d verifiche, %d fallite\n", checks, failures);
    return failures ? 1 : 0;
}
