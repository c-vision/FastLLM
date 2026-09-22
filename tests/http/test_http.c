/* test_http.c — il codec HTTP si prova sul FRAMING, non sulla cortesia.
 *
 * Le asserzioni che contano qui sono quelle che impediscono a due lettori dello stesso byte
 * di vedere due richieste diverse: `Content-Length` ambiguo, `Transfer-Encoding` insieme a
 * `Content-Length`, `\r` sciolto, obs-fold, un metodo con corpo e nessuna dichiarazione.
 * Ognuna di queste è un RIFIUTO con un codice proprio — e un codice per due cause diverse
 * sarebbe il difetto, non il fix.
 *
 * Sull'output: confronto BYTE-ESATTO, perché un `Content-Length` scritto male non è uno
 * stile, è una connessione appesa. Sul chunking SSE in più c'è la PROPRIETÀ (il conteggio
 * dichiarato deve eguagliare i byte realmente emessi), che è l'asserzione che scopre da
 * sola se l'aritmetica di `sse_shape` è sbagliata.
 */
#include "fmlx/fmlx_http.h"

#include <stdio.h>
#include <string.h>

static int checks, failures;

#define CK(cond, ...)                                                                            \
    do {                                                                                         \
        checks++;                                                                                \
        if (!(cond)) {                                                                           \
            failures++;                                                                          \
            printf("  FALLIMENTO %s:%d: ", __FILE__, __LINE__);                                   \
            printf(__VA_ARGS__);                                                                  \
            printf("\n");                                                                        \
        }                                                                                        \
    } while (0)

/* ---- 1. la richiesta che arriva davvero da un client OpenAI ---- */

static const char REQ[] =
    "POST /v1/chat/completions HTTP/1.1\r\n"
    "Host: api.example.com\r\n"
    "Content-Type: application/json\r\n"
    "content-length: 42\r\n"
    "Authorization: Bearer xyz\r\n"
    "\r\n";

static FmlxHttpReq R;

static void test_ok(void) {
    puts("-- richiesta ben formata");
    fmlx_http_req_init(&R);
    FmlxHttpStatus e = fmlx_http_parse_req(REQ, sizeof REQ - 1, &R);
    CK(e == FMLX_HTTP_OK, "parse -> %s (%s)", fmlx_http_status_str(e),
       R.err_msg ? R.err_msg : "-");
    CK(R.method.n == 4 && !memcmp(R.method.p, "POST", 4), "metodo %.*s", (int)R.method.n,
       R.method.p);
    static const char WANT_TGT[] = "/v1/chat/completions";
    CK(R.target.n == sizeof WANT_TGT - 1 && !memcmp(R.target.p, WANT_TGT, sizeof WANT_TGT - 1),
       "target %.*s", (int)R.target.n, R.target.p);
    CK(R.target_form == FMLX_HTTP_TARGET_ORIGIN, "forma target %d", (int)R.target_form);
    CK(!R.http10, "versione 1.1");
    CK(R.n_hdr == 4, "header conteggiati %u (uno per riga, non uno per valore)", R.n_hdr);
    CK(R.has_content_length && R.content_length == 42, "Content-Length %llu",
       (unsigned long long)R.content_length);
    CK(R.has_body, "con CL 42 il corpo c'e'");
    /* `head_len` è ciò che il trasporto deve scartare e da dove comincia il corpo: se è
     * sbagliato di uno, il primo byte del JSON viene mangiato o duplicato. */
    CK(R.head_len == sizeof REQ - 1 && R.body_off == R.head_len, "head_len %zu vs %zu",
       R.head_len, sizeof REQ - 1);
    CK(fmlx_http_keep_alive(&R), "1.1 senza Connection resta aperta");

    /* Il nome è confrontato case-insensitive: `content-length` minuscolo è LO STESSO header
     * (misurato: `R.n_hdr` lo conta una volta sola e `has_header` lo trova). */
    CK(fmlx_http_has_header(&R, "CONTENT-LENGTH"), "Content-Length maiuscolo trovato");
    FmlxSlice ct = fmlx_http_get_header(&R, "content-type");
    CK(ct.n == 16 && !memcmp(ct.p, "application/json", 16), "content-type %.*s", (int)ct.n, ct.p);
    FmlxSlice nope = fmlx_http_get_header(&R, "x-assente");
    CK(nope.n == 0 && !nope.p, "header assente -> slice vuota, non valore vuoto");
}

/* ---- 2. NEED_MORE: "non basta" NON è un errore ---- */

static void test_need_more(void) {
    puts("-- testata incompleta");
    static const char PART[] = "GET /v1/models HTTP/1.1\r\nHost: x\r\n";
    fmlx_http_req_init(&R);
    CK(fmlx_http_parse_req(PART, sizeof PART - 1, &R) == FMLX_HTTP_NEED_MORE,
       "senza la riga vuota finale");

    /* `\r` come ULTIMO byte: non sappiamo ancora se era CRLF o un CR sciolto. Dire
     * `LONE_CR` qui butterebbe via una richiesta perfetta fermata a metà dal TCP. */
    static const char CR[] = "GET / HTTP/1.1\r";
    fmlx_http_req_init(&R);
    CK(fmlx_http_parse_req(CR, sizeof CR - 1, &R) == FMLX_HTTP_NEED_MORE, "CR in coda");

    /* Neanche la request-line è finita. */
    fmlx_http_req_init(&R);
    CK(fmlx_http_parse_req("GET / H", 7, &R) == FMLX_HTTP_NEED_MORE, "riga a meta'");
}

/* ---- 3. i rifiuti della request-line ---- */
static void want(const char *req, size_t n, FmlxHttpStatus want_code, const char *cosa) {
    fmlx_http_req_init(&R);
    FmlxHttpStatus e = fmlx_http_parse_req(req, n, &R);
    checks++;
    if (e != want_code) {
        failures++;
        printf("  FALLIMENTO %s: -> %s, atteso %s (%s)\n", cosa, fmlx_http_status_str(e),
               fmlx_http_status_str(want_code), R.err_msg ? R.err_msg : "-");
    }
}
#define W(s, code, cosa) want(s, sizeof s - 1, code, cosa)

static void test_req_line(void) {
    puts("-- request-line");
    W("GET / HTTP/1.1\r\n\r\n", FMLX_HTTP_OK, "GET minimo con riga vuota");
    W("GET / HTTP/2.0\r\n\r\n", FMLX_HTTP_ERR_BAD_VERSION, "HTTP/2.0 in una riga di testo");
    W("GET / http/1.1\r\n\r\n", FMLX_HTTP_ERR_BAD_VERSION, "versione minuscola");
    W("GET / HTTP/1.1 x\r\n\r\n", FMLX_HTTP_ERR_BAD_REQ_LINE, "quarto campo");
    W("GET\r\n\r\n", FMLX_HTTP_ERR_BAD_REQ_LINE, "solo metodo");
    /* `GET / ` NON ha un target vuoto: ha una versione ASSENTE (lo spazio finale è il
     * separatore). Il target vuoto è DUE spazi, ed è un caso diverso — misurato qui sotto. */
    W("GET / \r\n\r\n", FMLX_HTTP_ERR_BAD_VERSION, "nessuna versione dopo lo spazio");
    W("GET  HTTP/1.1\r\n\r\n", FMLX_HTTP_ERR_BAD_TARGET, "target vuoto (doppio spazio)");
    /* Uno spazio NON può stare dentro un metodo senza creare un campo in più, quindi
     "metodo con spazio" non è un caso esprimibile: ciò che un metodo può avere è un
     * carattere che `tchar` non è. */
    W("G ET / HTTP/1.1\r\n\r\n", FMLX_HTTP_ERR_BAD_REQ_LINE, "spazio = campo in piu'");
    W("G(E)T / HTTP/1.1\r\n\r\n", FMLX_HTTP_ERR_BAD_METHOD, "metodo con carattere non tchar");
    W("G\xC3\x89T / HTTP/1.1\r\n\r\n", FMLX_HTTP_ERR_BAD_METHOD, "metodo non ASCII");
    W("GET example.com:80 HTTP/1.1\r\n\r\n", FMLX_HTTP_ERR_BAD_TARGET, "authority-form (CONNECT)");
    W("GET /\x01 HTTP/1.1\r\n\r\n", FMLX_HTTP_ERR_BAD_TARGET, "CTRL crudo nel target");
    W("GET * HTTP/1.1\r\n\r\n", FMLX_HTTP_OK, "OPTIONS con target '*'");
    W("GET http://h/p HTTP/1.1\r\n\r\n", FMLX_HTTP_OK, "forma assoluta accettata");
    fmlx_http_req_init(&R);
    W("GET http://h/p HTTP/1.1\r\n\r\n", FMLX_HTTP_OK, "forma assoluta");
    CK(R.target_form == FMLX_HTTP_TARGET_ABSOLUTE, "forma assoluta segnalata (%d)",
       (int)R.target_form);

    /* Un target oltre il cap: il frammento lungo è un dato, non una richiesta API. */
    static const char PFX[] = "GET /", SUF[] = " HTTP/1.1\r\n\r\n";
    static char big[FMLX_HTTP_MAX_TARGET + 64];
    /* Le lunghezze vengono DAI LETTERALI: contarle a mano è il tipo di numero che ho già
     * scritto falso tre volte in questi giorni. */
    size_t k = sizeof PFX - 1;
    memcpy(big, PFX, k);
    memset(big + k, 'a', FMLX_HTTP_MAX_TARGET + 8);
    k += FMLX_HTTP_MAX_TARGET + 8;
    memcpy(big + k, SUF, sizeof SUF - 1);
    want(big, k + sizeof SUF - 1, FMLX_HTTP_ERR_BAD_TARGET, "target troppo lungo");
}

/* ---- 4. header: forma e folding ---- */
static void test_headers(void) {
    puts("-- header");
    W("GET / HTTP/1.1\r\nnocolon\r\n\r\n", FMLX_HTTP_ERR_BAD_HEADER, "header senza ':'");
    W("GET / HTTP/1.1\r\n: v\r\n\r\n", FMLX_HTTP_ERR_BAD_HEADER, "nome vuoto");
    W("GET / HTTP/1.1\r\na b: v\r\n\r\n", FMLX_HTTP_ERR_BAD_HEADER, "spazio nel nome");
    W("GET / HTTP/1.1\r\na\x01: v\r\n\r\n", FMLX_HTTP_ERR_BAD_HEADER, "CTRL nel nome");
    W("GET / HTTP/1.1\r\nA: b\r\n  folded\r\n\r\n", FMLX_HTTP_ERR_FOLDING, "obs-fold");
    W("GET / HTTP/1.1\r\nA: b\x01c\r\n\r\n", FMLX_HTTP_ERR_BAD_HDR_VALUE, "CTRL nel valore");
    W("GET / HTTP/1.1\r\nA: b\rc\r\n\r\n", FMLX_HTTP_ERR_LONE_CR, "CR sciolto nel valore");

    /* HTAB dentro un valore è OWS legale: `A:\tb\t` → valore `b`. Confonderlo con un CTRL
     * rifiuterebbe richieste legali. */
    static const char TAB[] = "GET / HTTP/1.1\r\nA:\t b \t\r\n\r\n";
    fmlx_http_req_init(&R);
    CK(fmlx_http_parse_req(TAB, sizeof TAB - 1, &R) == FMLX_HTTP_OK, "tab nel valore");
    FmlxSlice v = fmlx_http_get_header(&R, "a");
    CK(v.n == 1 && v.p[0] == 'b', "OWS tolti, resta '%.*s' (%u)", (int)v.n, v.p, v.n);

    /* Il valore vuoto È un header presente: `A:` non è la stessa cosa di `A` assente. */
    static const char EMPTYV[] = "GET / HTTP/1.1\r\nA:\r\n\r\n";
    fmlx_http_req_init(&R);
    CK(fmlx_http_parse_req(EMPTYV, sizeof EMPTYV - 1, &R) == FMLX_HTTP_OK, "valore vuoto");
    CK(fmlx_http_has_header(&R, "a") && fmlx_http_get_header(&R, "a").n == 0,
       "presente ma vuoto distinguibile da assente");

    char many[8192];
    /* `(size_t)memcpy(...)` NON è la lunghezza: è il PUNTATORE restituito convertito. Scrivendo
     * `many + k` con quel valore si finisce in una pagina non mappata — era il SIGBUS di
     * questo file, e il motivo per cui `puts` senza flush non raccontava niente. */
    memcpy(many, "GET / HTTP/1.1\r\n", 16);
    size_t k = 16;
    for (uint32_t i = 0; i <= FMLX_HTTP_MAX_HEADERS; i++)
        k += (size_t)snprintf(many + k, 16, "X-%u: 1\r\n", i);
    memcpy(many + k, "\r\n", 2);
    want(many, k + 2, FMLX_HTTP_ERR_TOO_MANY_HDRS, "un header oltre il cap");
}

/* ---- 5. il framing del body: la parte che non ammette "procediamo" ---- */
static void test_framing(void) {
    puts("-- framing del body");
    W("POST /v1 HTTP/1.1\r\nContent-Length: 42\r\n\r\n", FMLX_HTTP_OK, "POST con CL");
    W("GET /v1 HTTP/1.1\r\nContent-Length: 0\r\n\r\n", FMLX_HTTP_OK, "CL 0");
    W("GET /v1 HTTP/1.1\r\nContent-Length: +5\r\n\r\n", FMLX_HTTP_ERR_BAD_CONTENT_LENGTH, "CL con +");
    W("GET /v1 HTTP/1.1\r\nContent-Length: 5x\r\n\r\n", FMLX_HTTP_ERR_BAD_CONTENT_LENGTH, "CL con x");
    W("GET /v1 HTTP/1.1\r\nContent-Length: -1\r\n\r\n", FMLX_HTTP_ERR_BAD_CONTENT_LENGTH, "CL negativo");
    W("GET /v1 HTTP/1.1\r\nContent-Length:\r\n\r\n", FMLX_HTTP_ERR_BAD_CONTENT_LENGTH, "CL vuoto");
    W("GET /v1 HTTP/1.1\r\nContent-Length: 0x10\r\n\r\n", FMLX_HTTP_ERR_BAD_CONTENT_LENGTH, "CL esadecimale");
    W("GET /v1 HTTP/1.1\r\nContent-Length: 99999999999999999999999\r\n\r\n",
      FMLX_HTTP_ERR_BAD_CONTENT_LENGTH, "CL oltre uint64");
    W("GET /v1 HTTP/1.1\r\nContent-Length: 12\r\nContent-Length:  12 \r\n\r\n", FMLX_HTTP_OK,
      "due CL IDENTICI (con OWS) collassano");
    CK(R.content_length == 12, "CL collassato a %llu", (unsigned long long)R.content_length);
    W("POST /v1 HTTP/1.1\r\nContent-Length: 12\r\nContent-Length: 13\r\n\r\n",
      FMLX_HTTP_ERR_CL_AMBIGUOUS, "due CL DIVERSI");

    /* POST senza dichiarare il corpo: NON indoviniamo "0 byte". */
    W("POST /v1 HTTP/1.1\r\nHost: x\r\n\r\n", FMLX_HTTP_ERR_NO_CONTENT_LENGTH, "POST senza CL");
    W("PUT /v1 HTTP/1.1\r\n\r\n", FMLX_HTTP_ERR_NO_CONTENT_LENGTH, "PUT senza CL");
    W("PATCH /v1 HTTP/1.1\r\n\r\n", FMLX_HTTP_ERR_NO_CONTENT_LENGTH, "PATCH senza CL");
    fmlx_http_req_init(&R);
    W("GET /v1 HTTP/1.1\r\nHost: x\r\n\r\n", FMLX_HTTP_OK, "GET senza CL e' legale");
    CK(!R.has_content_length && !R.has_body, "GET: nessun corpo");
    /* `post` minuscolo è un metodo diverso, non la stessa parola: la confronto case-
     * insensitive come ogni header, MA il metodo lo è solo per i metodi conosciuti. */
    W("post /v1 HTTP/1.1\r\n\r\n", FMLX_HTTP_ERR_NO_CONTENT_LENGTH, "post minuscolo");

    /* TE: chunked non lo implementiamo, e TE+CL è il segnale di smuggling in sé. */
    W("POST /v1 HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", FMLX_HTTP_ERR_CHUNKED,
      "chunked in richiesta");
    W("POST /v1 HTTP/1.1\r\nTransfer-Encoding: gzip\r\n\r\n", FMLX_HTTP_ERR_BAD_TE,
      "TE non chunked");
    W("POST /v1 HTTP/1.1\r\nTransfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\n",
      FMLX_HTTP_ERR_TE_WITH_CL, "TE+CL vince su CHUNKED");
    W("POST /v1 HTTP/1.1\r\nTRANSFER-ENCODING:Chunked\r\nContent-Length: 5\r\n\r\n",
      FMLX_HTTP_ERR_TE_WITH_CL, "TE+CL con nomi/casi diversi");
    /* `Content-Encoding: gzip` NON è Transfer-Encoding: confonderli è un errore che si
     * vede solo su un client che compressa il corpo. */
    W("POST /v1 HTTP/1.1\r\nContent-Encoding: gzip\r\nContent-Length: 5\r\n\r\n",
      FMLX_HTTP_OK, "Content-Encoding non e' Transfer-Encoding");
}

/* ---- 6. Connection ed Expect ---- */
static void test_conn(void) {
    puts("-- Connection ed Expect");
    static const char H10[] = "GET / HTTP/1.0\r\n\r\n";
    fmlx_http_req_init(&R);
    CK(fmlx_http_parse_req(H10, sizeof H10 - 1, &R) == FMLX_HTTP_OK, "HTTP/1.0");
    CK(!fmlx_http_keep_alive(&R), "1.0 chiude di default");
    static const char H10K[] = "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n";
    fmlx_http_req_init(&R);
    fmlx_http_parse_req(H10K, sizeof H10K - 1, &R);
    CK(fmlx_http_keep_alive(&R), "1.0 + keep-alive");

    /* Il token `close` VINCERE sempre, in qualunque posizione: per un server chiudere è
     * sempre sicuro, restare aperti su una connessione che il client voleva chiudere no
     * (il corpo della richiesta successiva resta in coda sul socket). */
    static const char BOTH1[] = "GET / HTTP/1.1\r\nConnection: close, keep-alive\r\n\r\n";
    fmlx_http_req_init(&R);
    fmlx_http_parse_req(BOTH1, sizeof BOTH1 - 1, &R);
    CK(!fmlx_http_keep_alive(&R), "'close, keep-alive' -> close vince");
    static const char BOTH2[] = "GET / HTTP/1.1\r\nConnection: keep-alive, close\r\n\r\n";
    fmlx_http_req_init(&R);
    fmlx_http_parse_req(BOTH2, sizeof BOTH2 - 1, &R);
    CK(!fmlx_http_keep_alive(&R), "'keep-alive, close' -> close vince");
    static const char NOKA[] = "GET / HTTP/1.1\r\nConnection: xkeep-alive\r\n\r\n";
    fmlx_http_req_init(&R);
    fmlx_http_parse_req(NOKA, sizeof NOKA - 1, &R);
    CK(fmlx_http_keep_alive(&R), "'xkeep-alive' NON e' keep-alive (confronto per token)");

    static const char EXP[] = "POST /v1 HTTP/1.1\r\nContent-Length: 2\r\nExpect: 100-Continue\r\n\r\n";
    fmlx_http_req_init(&R);
    CK(fmlx_http_parse_req(EXP, sizeof EXP - 1, &R) == FMLX_HTTP_OK, "Expect");
    CK(fmlx_http_expects_continue(&R), "100-Continue case-insensitive");
}

/* ---- 7. risposta ---- */
static char B[4096];
static FmlxHttpRes W1;

static void expects(const char *want, const char *cosa) {
    checks++;
    size_t n = strlen(want);
    if (W1.len != n || memcmp(W1.buf, want, n)) {
        failures++;
        printf("  FALLIMENTO %s:\n    ottenuto [%.*s]\n    atteso  [%s]\n", cosa, (int)W1.len,
               W1.buf, want);
    }
}

static void test_res(void) {
    puts("-- testata di risposta");
    fmlx_http_res_init(&W1, B, sizeof B);
    CK(fmlx_http_write_head(&W1, 200, "application/json", 42, NULL) == FMLX_HTTP_OK, "200");
    expects("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 42\r\n\r\n",
            "200 byte-esatta");

    fmlx_http_res_init(&W1, B, sizeof B);
    fmlx_http_write_head(&W1, 404, "application/json", 0, "X-Request-Id: abc\r\nRetry-After: 2");
    expects("HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: 0\r\n"
            "X-Request-Id: abc\r\nRetry-After: 2\r\n\r\n",
            "extra header con CRLF interno gia' normalizzato");

    /* Nessun Content-Type: un 204 senza corpo non ne ha bisogno. */
    fmlx_http_res_init(&W1, B, sizeof B);
    fmlx_http_write_head(&W1, 204, NULL, 0, NULL);
    expects("HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n", "senza content-type");

    /* Un CRLF dentro un extra header è INIEZIONE di risposta: deve essere rifiutato, non
     * scritto. È l'unico punto del writer dove un chiamante in buona fede può rompere la
     * frammentazione. */
    fmlx_http_res_init(&W1, B, sizeof B);
    CK(fmlx_http_write_head(&W1, 200, NULL, 0, "X: a\r\n\r\nGET / HTTP/1.1") ==
           FMLX_HTTP_ERR_BAD_HDR_VALUE,
       "CRLF negli extra header rifiutato");
    CK(W1.len == 0, "in errore non e' uscito nulla (%zu byte)", W1.len);

    /* Idem nel Content-Type. */
    fmlx_http_res_init(&W1, B, sizeof B);
    CK(fmlx_http_write_head(&W1, 200, "a\r\nX: 1", 0, NULL) == FMLX_HTTP_ERR_BAD_HDR_VALUE,
       "CRLF nel content-type");

    CK(fmlx_http_write_head(&W1, 200, NULL, 0, NULL) == FMLX_HTTP_ERR_BAD_HDR_VALUE,
       "dopo l'errore il writer ritorna QUELLO errore, e non scrive");
    CK(W1.len == 0, "e il buffer resta a 0 (%zu)", W1.len);

    fmlx_http_res_init(&W1, B, sizeof B);
    CK(fmlx_http_write_head(&W1, 99, NULL, 0, NULL) == FMLX_HTTP_ERR_BAD_STATUS, "status 99");
    fmlx_http_res_init(&W1, B, sizeof B);
    CK(fmlx_http_write_head(&W1, 1000, NULL, 0, NULL) == FMLX_HTTP_ERR_BAD_STATUS, "status 1000");
    fmlx_http_res_init(&W1, B, sizeof B);
    fmlx_http_write_head(&W1, 599, NULL, 0, NULL);
    CK(strstr(W1.buf, "599 Server Error") != NULL, "status ignoto con una reason comunque");

    /* Capienza: o entra tutto, o niente. Un `HTTP/1.1 200 O` a metà è una connessione rotta. */
    char tiny[20];
    fmlx_http_res_init(&W1, tiny, sizeof tiny);
    CK(fmlx_http_write_head(&W1, 200, "application/json", 42, NULL) == FMLX_HTTP_ERR_BUF_SMALL,
       "buffer piccolo -> BUFFER");
    fmlx_http_res_init(&W1, B, 0);
    CK(fmlx_http_write_head(&W1, 200, NULL, 0, NULL) == FMLX_HTTP_ERR_BUF_SMALL, "cap 0");
}

/* ---- 8. SSE ---- */
static char PL[2048];  /* senza chunking */
static char CH[2048];  /* con chunking */

static void test_sse(void) {
    puts("-- SSE framing");
    FmlxHttpRes w;
    fmlx_http_res_init(&w, PL, sizeof PL);
    FmlxSlice d = {"{\"n\":1}", 7};
    CK(fmlx_http_sse_write(&w, NULL, d) == FMLX_HTTP_OK, "evento semplice");
    CK(fmlx_http_sse_end(&w) == FMLX_HTTP_OK, "end senza chunking non scrive nulla");
    static const char WANT_EVT[] = "data: {\"n\":1}\r\n\r\n";
    checks++;
    if (w.len != sizeof WANT_EVT - 1 || memcmp(w.buf, WANT_EVT, w.len)) {
        failures++;
        printf("  FALLIMENTO evento semplice: [%.*s] (%zu, atteso %zu)\n", (int)w.len, w.buf,
               w.len, sizeof WANT_EVT - 1);
    }

    /* `\n` DENTRO il payload: due righe `data:`, non un payload spezzato che il lettore
     * ricompone male. Questo è l'errore SSE che passa i test con payload JSON compatti e
     * esplode con un `tool_calls` che contiene testo. */
    fmlx_http_res_init(&w, PL, sizeof PL);
    FmlxSlice multi = {"a\nb\nc", 5};
    fmlx_http_sse_write(&w, "content_block_delta", multi);
    static const char WANT_MULTI[] =
        "event: content_block_delta\r\ndata: a\r\ndata: b\r\ndata: c\r\n\r\n";
    checks++;
    if (w.len != sizeof WANT_MULTI - 1 || memcmp(w.buf, WANT_MULTI, w.len)) {
        failures++;
        printf("  FALLIMENTO payload multilinea: [%.*s] (%zu, atteso %zu)\n", (int)w.len, w.buf,
               w.len, sizeof WANT_MULTI - 1);
    }

    /* Il payload che FINISCE con `\n` non deve crescere: SSE aggiunge già il newline alla
     * fine del data, quindi l'ultimo segmento vuoto va droppato (è un limite del formato,
     * documentato in fmlx_http.c). */
    fmlx_http_res_init(&w, PL, sizeof PL);
    FmlxSlice trail = {"a\n", 2};
    fmlx_http_sse_write(&w, NULL, trail);
    CK(w.len == 11 && !memcmp(w.buf, "data: a\r\n\r\n", 11), "trailing newline non duplicato: [%.*s] (%zu)",
       (int)w.len, w.buf, w.len);

    fmlx_http_res_init(&w, PL, sizeof PL);
    FmlxSlice empty = {"", 0};
    fmlx_http_sse_write(&w, NULL, empty);
    CK(w.len == 10 && !memcmp(w.buf, "data: \r\n\r\n", 10), "payload vuoto -> una riga vuota");

    /* CTRL dentro il payload: `data:` non ha scuse, un `\r` crudo spezzerebbe l'evento. */
    fmlx_http_res_init(&w, PL, sizeof PL);
    FmlxSlice ctl = {"a\rb", 3};
    CK(fmlx_http_sse_write(&w, NULL, ctl) == FMLX_HTTP_ERR_BAD_HDR_VALUE, "CR nel payload");
    FmlxSlice nul = {"a\0b", 3};
    fmlx_http_res_init(&w, PL, sizeof PL);
    CK(fmlx_http_sse_write(&w, NULL, nul) == FMLX_HTTP_ERR_BAD_HDR_VALUE, "NUL nel payload");

    /* LA PROPRIETÀ sul chunking: la lunghezza esadecimale dichiarata deve eguagliare i byte
     * realmente emessi, altrimenti il lettore si ferma prima o legge la prossima riga come
     * corpo. Si confronta con la stessa scrittura SENZA chunking, che è il testimone
     * indipendente: se l'aritmetica di `sse_shape` sbaglia, questo rosso arriva da solo. */
    static const struct { const char *ev; const char *data; } CASES[] = {
        {NULL, "{\"x\":1}"},
        {"message_start", "{\"type\":\"message_start\"}"},
        {"content_block_delta", "a\nb\nc\nd"},
        {NULL, ""},
        {NULL, "\n"},
        {"ping", "x\n"},
        {"done", "[1,2,3]\n\n"},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof *CASES; i++) {
        FmlxSlice dd = {CASES[i].data, strlen(CASES[i].data)};
        fmlx_http_res_init(&w, PL, sizeof PL);
        fmlx_http_sse_write(&w, CASES[i].ev, dd);
        size_t plain = w.len;

        FmlxHttpRes c;
        fmlx_http_res_init(&c, CH, sizeof CH);
        c.chunked = 1;
        fmlx_http_sse_write(&c, CASES[i].ev, dd);
        fmlx_http_sse_end(&c);

        unsigned long decl = 0;
        int used = 0;
        checks += 3;
        if (sscanf(c.buf, "%lx\r\n%n", &decl, &used) != 1 || used == 0) {
            failures += 3;
            printf("  FALLIMENTO chunk %zu: intestazione non letubile [%.*s]\n", i, (int)c.len,
                   c.buf);
            continue;
        }
        if (decl != plain) {
            failures++;
            printf("  FALLIMENTO chunk %zu: dichiarati %lu byte, emessi %zu\n", i, decl, plain);
        }
        if (c.len != (size_t)used + plain + 2 + 5) {
            failures++;
            printf("  FALLIMENTO chunk %zu: totale %zu, atteso %zu (intest %d + corpo %zu + "
                   "CRLF + '0\\r\\n\\r\\n')\n",
                   i, c.len, (size_t)used + plain + 7, used, plain);
        } else {
            checks++;
            if (memcmp(c.buf + used, PL, plain)) {
                failures++;
                printf("  FALLIMENTO chunk %zu: il corpo del chunk non e' l'evento in chiaro\n", i);
            }
        }
    }

    /* Chunking con buffer pieno: l'intestazione NON deve restare scritta da sola. */
    char small[24];
    FmlxHttpRes s2;
    fmlx_http_res_init(&s2, small, sizeof small);
    s2.chunked = 1;
    FmlxSlice big = {"abcdefghijklmnopqrstuvwxyz", 26};
    CK(fmlx_http_sse_write(&s2, NULL, big) == FMLX_HTTP_ERR_BUF_SMALL, "chunk senza spazio");
    CK(s2.len == 0, "niente intestazione orfana (%zu byte)", s2.len);
}

int main(void) {
    /* Non buffered: un crash dentro una sezione deve dire QUALE, non cancellare tutto ciò
     * che aveva stampato. È il modo in cui questo file ha trovato il proprio SIGBUS. */
    setvbuf(stdout, NULL, _IONBF, 0);
    test_ok();
    test_need_more();
    test_req_line();
    test_headers();
    test_framing();
    test_conn();
    test_res();
    test_sse();
    printf("\n%d verifiche, %d fallimenti\n", checks, failures);
    return failures ? 1 : 0;
}
