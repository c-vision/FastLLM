/* fmlx_http.c — vedi fmlx_http.h per il contratto. Qui sta solo la meccanica.
 *
 * La scelta di fondo di questo file: NON esistere un modo di "leggere lo stesso" una
 * richiesta ambigua. Ogni ambiguità di framing è un RIFIUTO con un codice che dice quale,
 * perché l'alternativa — indovinare — è esattamente la classe di bug in cui il nostro
 * parser e un proxy davanti a noi non sono lo stesso server (request smuggling).
 */
#include "fmlx/fmlx_http.h"

#include <stdio.h>
#include <string.h>

/* ---- utilità -------------------------------------------------------------- */

/* RFC 9110 §5.6.2 `tchar`. Scritta come tabella di verità, non come `isalnum` + eccezioni:
 * `isalnum` dipende dal locale, e un header con `é` nel nome deve essere rifiutato allo
 * stesso modo qui e sulla macchina di chi fa il build. */
static int is_tchar(unsigned char c) {
    static const char ok[] = "!#$%&'*+-.^_`|~";
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return 1;
    return memchr(ok, c, sizeof ok - 1) != NULL;
}

/* CTRL crudi: qualunque byte < 0x20 (tranne HTAB) o 0x7F. Sono ciò che trasforma un header
 * in una seconda richiesta iniettata. */
static int is_ctl(unsigned char c) { return c < 0x20 || c == 0x7f; }

/* Case-insensitive SU ENTRAMBI i lati. Il parametro si chiama `lower` per dire "scrivi
 * minuscolo, è più veloce", ma un chiamante che passa `CONTENT-LENGTH` — che è la grafia
 * con cui un header si nomina in un messaggio — deve funzionare lo stesso: un helper di
 * lookup che funziona solo se il chiamante indovina il registro è il difetto, non il costo.
 * (Misurato: `has_header(r, "CONTENT-LENGTH")` rispondeva "assente" su una richiesta che
 * conteneva `content-length`.) */
static int slice_ieq_n(FmlxSlice s, const char *lit, size_t n) {
    if (s.n != n) return 0;
    for (uint32_t i = 0; i < s.n; i++) {
        char a = s.p[i], b = lit[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

int fmlx_http_slice_ieq(FmlxSlice s, const char *lit) {
    if (!lit) return 0;
    return slice_ieq_n(s, lit, strlen(lit));
}

size_t fmlx_http_slice_copy(FmlxSlice s, char *dst, size_t dn) {
    if (!dst && s.n) return (size_t)-1;
    if (s.n > dn) return (size_t)-1;
    if (s.n) memcpy(dst, s.p, s.n);
    return s.n;
}

const char *fmlx_http_status_str(FmlxHttpStatus s) {
    switch (s) {
        case FMLX_HTTP_OK: return "ok";
        case FMLX_HTTP_NEED_MORE: return "incompleto";
        case FMLX_HTTP_ERR_BAD_METHOD: return "METODO";
        case FMLX_HTTP_ERR_BAD_TARGET: return "TARGET";
        case FMLX_HTTP_ERR_BAD_VERSION: return "VERSIONE";
        case FMLX_HTTP_ERR_BAD_REQ_LINE: return "RIGA";
        case FMLX_HTTP_ERR_BAD_HEADER: return "HEADER";
        case FMLX_HTTP_ERR_BAD_HDR_VALUE: return "VALORE";
        case FMLX_HTTP_ERR_FOLDING: return "FOLDING";
        case FMLX_HTTP_ERR_TOO_MANY_HDRS: return "TROPPI_HEADER";
        case FMLX_HTTP_ERR_HEAD_TOO_LARGE: return "TESTATA_GRANDE";
        case FMLX_HTTP_ERR_LONE_CR: return "CR_ISOLATO";
        case FMLX_HTTP_ERR_NO_CONTENT_LENGTH: return "SENZA_CONTENT_LENGTH";
        case FMLX_HTTP_ERR_BAD_CONTENT_LENGTH: return "CONTENT_LENGTH";
        case FMLX_HTTP_ERR_CL_AMBIGUOUS: return "CONTENT_LENGTH_AMBIGUO";
        case FMLX_HTTP_ERR_CHUNKED: return "CHUNKED";
        case FMLX_HTTP_ERR_TE_WITH_CL: return "TE_E_CL";
        case FMLX_HTTP_ERR_BAD_TE: return "TRANSFER_ENCODING";
        case FMLX_HTTP_ERR_BUF_SMALL: return "BUFFER";
        case FMLX_HTTP_ERR_BAD_ARG: return "argomento";
        case FMLX_HTTP_ERR_BAD_STATUS: return "STATUS";
    }
    return "?";
}

void fmlx_http_req_init(FmlxHttpReq *r) {
    if (!r) return;
    memset(r, 0, sizeof *r);
    r->err_off = FMLX_HTTP_NO_OFF;
}

/* ---- scansione della testata --------------------------------------------- */

typedef struct {
    const char *buf;
    size_t len;
    size_t pos;
    FmlxHttpReq *r;
    const char *err_msg;
} S;

static FmlxHttpStatus fail(S *s, FmlxHttpStatus code, const char *msg, size_t off) {
    s->r->err_msg = msg;
    s->r->err_off = off;
    return code;
}

/* 1 = riga completa disponibile, 0 = serve altro, -1 = `\r` non seguito da `\n`.
 *
 * Un `\n` sciolto è ACCETTATO come fine riga (un client scritto a mano lo manda, e qui non
 * compra nulla essere severi: la SEVERITÀ che serve è sul framing del body, non sugli
 * byte di fine riga). Un `\r` sciolto NO: lascia il dubbio su dove finisse la riga, e su
 * quel dubbio si costruiscono gli smuggling. */
static int next_line(S *s, FmlxSlice *out, size_t *consumed) {
    for (size_t i = s->pos; i < s->len; i++) {
        unsigned char c = (unsigned char)s->buf[i];
        if (c == '\n') {
            out->p = s->buf + s->pos;
            out->n = (uint32_t)(i - s->pos);
            *consumed = i + 1;
            return 1;
        }
        if (c == '\r') {
            if (i + 1 >= s->len) return 0;
            if (s->buf[i + 1] != '\n') return -1;
            out->p = s->buf + s->pos;
            out->n = (uint32_t)(i - s->pos);
            *consumed = i + 2;
            return 1;
        }
    }
    return 0;
}

/* Valore di un header senza gli OWS attorno (RFC 9112 §5.5). */
static FmlxSlice trim_ows(FmlxSlice v) {
    while (v.n && (v.p[0] == ' ' || v.p[0] == '\t')) { v.p++; v.n--; }
    while (v.n && (v.p[v.n - 1] == ' ' || v.p[v.n - 1] == '\t')) v.n--;
    return v;
}

/* True se `s` contiene il token `tok` in una lista separata da virgole, case-insensitive.
 * Serve così perché `Connection: close, keep-alive` CONTIENE keep-alive: un `strstr`
 * sul valore intero darebbe la risposta sbagliata con aria di precisione. */
static int list_has_token(FmlxSlice s, const char *tok) {
    size_t tl = strlen(tok);
    size_t i = 0;
    while (i <= s.n) {
        size_t j = i;
        while (j < s.n && s.p[j] != ',') j++;
        FmlxSlice part = {s.p + i, (uint32_t)(j - i)};
        if (slice_ieq_n(trim_ows(part), tok, tl)) return 1;
        if (j == s.n) break;
        i = j + 1;
    }
    return 0;
}

static int parse_content_length(FmlxSlice v, uint64_t *out) {
    v = trim_ows(v);
    if (!v.n) return 0;
    uint64_t acc = 0;
    for (uint32_t i = 0; i < v.n; i++) {
        unsigned char c = (unsigned char)v.p[i];
        if (c < '0' || c > '9') return 0; /* `+5`, ` 5`, `5 `, `0x10`, `-1`: tutti rifiutati */
        if (acc > (UINT64_MAX - (uint64_t)(c - '0')) / 10u) return 0;
        acc = acc * 10u + (uint64_t)(c - '0');
    }
    *out = acc;
    return 1;
}

/* ---- request-line -------------------------------------------------------- */

static FmlxHttpStatus parse_req_line(S *s, FmlxSlice line) {
    FmlxHttpReq *r = s->r;
    const char *sp1 = memchr(line.p, ' ', line.n);
    if (!sp1) return fail(s, FMLX_HTTP_ERR_BAD_REQ_LINE, "request-line senza metodo e target", s->pos);
    r->method.p = line.p;
    r->method.n = (uint32_t)(sp1 - line.p);
    if (!r->method.n) return fail(s, FMLX_HTTP_ERR_BAD_METHOD, "metodo vuoto", s->pos);
    for (uint32_t i = 0; i < r->method.n; i++)
        if (!is_tchar((unsigned char)r->method.p[i]))
            return fail(s, FMLX_HTTP_ERR_BAD_METHOD, "metodo con caratteri non tchar", s->pos);

    const char *rest = sp1 + 1;
    size_t left = line.n - (size_t)(rest - line.p);
    const char *sp2 = memchr(rest, ' ', left);
    if (!sp2) return fail(s, FMLX_HTTP_ERR_BAD_REQ_LINE, "request-line senza versione", s->pos + (size_t)(sp1 - line.p));
    r->target.p = rest;
    r->target.n = (uint32_t)(sp2 - rest);
    FmlxSlice ver = {sp2 + 1, (uint32_t)(left - (size_t)(sp2 - rest) - 1)};

    /* Un TERZO spazio significa che c'è un campo di troppo: le tre parti della request-line
     * sono un accordo, non un'approssimazione, e "l'ultimo pezzo è la versione" lascerebbe
     * un target con uno spazio dentro — che è un path diverso da quello che vediamo noi. */
    if (memchr(ver.p, ' ', ver.n))
        return fail(s, FMLX_HTTP_ERR_BAD_REQ_LINE, "request-line con piu' di tre campi", s->pos);
    if (ver.n == 8 && !memcmp(ver.p, "HTTP/1.0", 8)) r->http10 = 1;
    else if (ver.n == 8 && !memcmp(ver.p, "HTTP/1.1", 8)) r->http10 = 0;
    else return fail(s, FMLX_HTTP_ERR_BAD_VERSION, "versione non HTTP/1.0 ne' HTTP/1.1",
                     s->pos + (size_t)(sp2 + 1 - line.p));

    if (!r->target.n || r->target.n > FMLX_HTTP_MAX_TARGET)
        return fail(s, FMLX_HTTP_ERR_BAD_TARGET, "target vuoto o troppo lungo", s->pos);
    for (uint32_t i = 0; i < r->target.n; i++)
        if (is_ctl((unsigned char)r->target.p[i]))
            return fail(s, FMLX_HTTP_ERR_BAD_TARGET, "target con CTRL crudo", s->pos + i);
    if (r->target.p[0] == '/') r->target_form = FMLX_HTTP_TARGET_ORIGIN;
    else if (r->target.n == 1 && r->target.p[0] == '*') r->target_form = FMLX_HTTP_TARGET_STAR;
    else if ((r->target.n > 7 && !memcmp(r->target.p, "http://", 7)) ||
             (r->target.n > 8 && !memcmp(r->target.p, "https://", 8)))
        r->target_form = FMLX_HTTP_TARGET_ABSOLUTE;
    else return fail(s, FMLX_HTTP_ERR_BAD_TARGET,
                     "target in una forma non ammessa (atteso '/', '*' o forma assoluta)", s->pos);
    return FMLX_HTTP_OK;
}

/* ---- parse --------------------------------------------------------------- */

FmlxHttpStatus fmlx_http_parse_req(const char *buf, size_t len, FmlxHttpReq *r) {
    if (!buf || !r) return FMLX_HTTP_ERR_BAD_ARG;
    S s = {buf, len, 0, r, NULL};
    r->n_hdr = 0;
    r->err_msg = NULL;
    r->err_off = FMLX_HTTP_NO_OFF;
    memset(&r->method, 0, sizeof r->method);

    /* `Transfer-Encoding` e `Content-Length` sono valutati DOPO la scansione, in un ordine
     * fisso e documentato: `TE_WITH_CL` vince su `CHUNKED` perché la presenza di entrambi è
     * il segnale in sé, e dire "chunked non supportato" nasconderebbe il caso interessante. */
    int te_seen = 0;
    FmlxSlice te_val = {NULL, 0};
    int cl_seen = 0;
    uint64_t cl_val = 0;

    for (;;) {
        if (s.pos > FMLX_HTTP_MAX_HEAD)
            return fail(&s, FMLX_HTTP_ERR_HEAD_TOO_LARGE, "testata oltre il limite", s.pos);
        FmlxSlice line;
        size_t consumed = 0;
        int k = next_line(&s, &line, &consumed);
        if (k < 0) return fail(&s, FMLX_HTTP_ERR_LONE_CR, "\\r non seguito da \\n", s.pos);
        if (k == 0) return FMLX_HTTP_NEED_MORE; /* testata non ancora completa */

        FmlxHttpStatus e = FMLX_HTTP_OK;
        if (s.pos == 0) {
            if (!line.n) return fail(&s, FMLX_HTTP_ERR_BAD_REQ_LINE, "request-line vuota", 0);
            e = parse_req_line(&s, line);
        } else if (!line.n) {
            s.pos = consumed;
            break; /* fine della testata */
        } else {
            unsigned char c0 = (unsigned char)line.p[0];
            if (c0 == ' ' || c0 == '\t')
                return fail(&s, FMLX_HTTP_ERR_FOLDING,
                            "obs-fold: RFC 9112 vieta gli header spezzati, e il valore "
                            "ripiegato cambia a chi lo legge", s.pos);
            const char *colon = memchr(line.p, ':', line.n);
            if (!colon) return fail(&s, FMLX_HTTP_ERR_BAD_HEADER, "header senza ':'", s.pos);
            FmlxSlice name = {line.p, (uint32_t)(colon - line.p)};
            if (!name.n) return fail(&s, FMLX_HTTP_ERR_BAD_HEADER, "nome header vuoto", s.pos);
            for (uint32_t i = 0; i < name.n; i++)
                if (!is_tchar((unsigned char)name.p[i]))
                    return fail(&s, FMLX_HTTP_ERR_BAD_HEADER,
                                "nome header con carattere non tchar", s.pos + i);
            FmlxSlice raw = {colon + 1, (uint32_t)(line.n - (size_t)(colon - line.p) - 1)};
            FmlxSlice val = trim_ows(raw);
            for (uint32_t i = 0; i < val.n; i++) {
                unsigned char c = (unsigned char)val.p[i];
                if (is_ctl(c) && c != '\t') {
                    r->err_field = name;
                    return fail(&s, FMLX_HTTP_ERR_BAD_HDR_VALUE,
                                "valore header con CTRL crudo", s.pos + (uint32_t)(colon - line.p) + 1 + i);
                }
            }
            if (r->n_hdr >= FMLX_HTTP_MAX_HEADERS)
                return fail(&s, FMLX_HTTP_ERR_TOO_MANY_HDRS, "troppi header", s.pos);
            r->hdr[r->n_hdr].name = name;
            r->hdr[r->n_hdr].value = val;
            r->n_hdr++;

            if (fmlx_http_slice_ieq(name, "content-length")) {
                uint64_t v;
                if (!parse_content_length(val, &v)) {
                    r->err_field = name;
                    return fail(&s, FMLX_HTTP_ERR_BAD_CONTENT_LENGTH,
                                "Content-Length non decifrabile come intero", s.pos);
                }
                if (cl_seen && v != cl_val) {
                    r->err_field = name;
                    return fail(&s, FMLX_HTTP_ERR_CL_AMBIGUOUS,
                                "due Content-Length con valori diversi", s.pos);
                }
                cl_seen = 1;
                cl_val = v;
            } else if (fmlx_http_slice_ieq(name, "transfer-encoding")) {
                te_seen = 1;
                te_val = val;
            }
        }
        if (e != FMLX_HTTP_OK) return e;
        s.pos = consumed;
    }

    /* Ordine delle decisioni di framing. */
    if (te_seen) {
        r->err_field = (FmlxSlice){"transfer-encoding", 17};
        if (cl_seen) return fail(&s, FMLX_HTTP_ERR_TE_WITH_CL,
                                 "Content-Length e Transfer-Encoding insieme", s.pos);
        if (!list_has_token(te_val, "chunked"))
            return fail(&s, FMLX_HTTP_ERR_BAD_TE,
                        "Transfer-Encoding senza chunked: non sappiamo incorniciare il corpo",
                        s.pos);
        return fail(&s, FMLX_HTTP_ERR_CHUNKED,
                    "Transfer-Encoding: chunked non supportato in richiesta", s.pos);
    }

    r->has_content_length = cl_seen;
    r->content_length = cl_seen ? cl_val : 0;
    r->head_len = s.pos;
    r->body_off = s.pos;
    if (cl_seen) r->has_body = cl_val > 0;

    /* POST/PUT/PATCH/PAYLOAD senza `Content-Length`: NON indoviniamo "corpo vuoto". Un
     * client che manda un corpo senza dichiararlo e un server che legge 0 byte sono due
     * idee della stessa richiesta: la risposta è un rifiuto, non un'ipotesi. */
    if (!cl_seen && !te_seen) {
        static const char *bodyish[] = {"post", "put", "patch", NULL};
        for (int i = 0; bodyish[i]; i++) {
            if (slice_ieq_n(r->method, bodyish[i], strlen(bodyish[i]))) {
                r->err_field = r->method;
                return fail(&s, FMLX_HTTP_ERR_NO_CONTENT_LENGTH,
                            "metodo con corpo atteso ma nessun Content-Length", s.pos);
            }
        }
    }
    return FMLX_HTTP_OK;
}

FmlxSlice fmlx_http_get_header(const FmlxHttpReq *r, const char *name) {
    FmlxSlice none = {NULL, 0};
    if (!r || !name) return none;
    for (uint32_t i = 0; i < r->n_hdr; i++)
        if (fmlx_http_slice_ieq(r->hdr[i].name, name)) return r->hdr[i].value;
    return none;
}

int fmlx_http_has_header(const FmlxHttpReq *r, const char *name) {
    if (!r || !name) return 0;
    for (uint32_t i = 0; i < r->n_hdr; i++)
        if (fmlx_http_slice_ieq(r->hdr[i].name, name)) return 1;
    return 0;
}

int fmlx_http_keep_alive(const FmlxHttpReq *r) {
    if (!r) return 0;
    int ka = !r->http10; /* 1.0 chiude di default, 1.1 no */
    /* `close` VINCERE sempre, in qualunque posizione della lista. Non è il "last token
     * wins" che si leggerebbe in un RFC: per un server chiudere è sempre sicuro, restare
     * aperti su una connessione che il client voleva chiudere no (il corpo della richiesta
     * successiva resterebbe in coda sul socket). `Connection` con entrambi i token è una
     * richiesta contraddittoria, e la regola è quella che non può fare danno. */
    for (uint32_t i = 0; i < r->n_hdr; i++) {
        if (!fmlx_http_slice_ieq(r->hdr[i].name, "connection")) continue;
        FmlxSlice c = r->hdr[i].value;
        if (list_has_token(c, "close")) return 0;
        if (list_has_token(c, "keep-alive")) ka = 1;
    }
    return ka;
}

int fmlx_http_expects_continue(const FmlxHttpReq *r) {
    if (!r) return 0;
    for (uint32_t i = 0; i < r->n_hdr; i++)
        if (fmlx_http_slice_ieq(r->hdr[i].name, "expect") &&
            list_has_token(r->hdr[i].value, "100-continue"))
            return 1;
    return 0;
}

/* ---- risposta ------------------------------------------------------------ */

static const struct { int code; const char *phrase; } REASON[] = {
    {100, "Continue"}, {200, "OK"}, {201, "Created"}, {202, "Accepted"}, {204, "No Content"},
    {400, "Bad Request"}, {401, "Unauthorized"}, {403, "Forbidden"}, {404, "Not Found"},
    {405, "Method Not Allowed"}, {406, "Not Acceptable"}, {408, "Request Timeout"},
    {409, "Conflict"}, {410, "Gone"}, {411, "Length Required"}, {413, "Content Too Large"},
    {414, "URI Too Long"}, {415, "Unsupported Media Type"}, {422, "Unprocessable Content"},
    {429, "Too Many Requests"}, {500, "Internal Server Error"}, {501, "Not Implemented"},
    {502, "Bad Gateway"}, {503, "Service Unavailable"}, {504, "Gateway Timeout"},
};

static const char *reason_of(int status) {
    for (size_t i = 0; i < sizeof REASON / sizeof *REASON; i++)
        if (REASON[i].code == status) return REASON[i].phrase;
    return NULL;
}

void fmlx_http_res_init(FmlxHttpRes *w, char *buf, size_t cap) {
    if (!w) return;
    w->buf = buf;
    w->cap = buf ? cap : 0;
    w->len = 0;
    w->chunked = 0;
    w->err = FMLX_HTTP_OK;
    w->sink = NULL;
    w->sink_ctx = NULL;
}

/* Svuota il buffer verso il sink (se c'è) e riparte da zero. Usata dallo streaming: il
 * contenuto già consegnato non resta in memoria, quindi il tetto `cap` non è un tetto sulla
 * lunghezza della risposta. */
FmlxHttpStatus fmlx_http_flush(FmlxHttpRes *w) {
    if (!w) return FMLX_HTTP_ERR_BAD_ARG;
    if (w->err) return w->err;
    if (!w->sink || w->len == 0) return w->err;
    if (w->sink(w->sink_ctx, w->buf, w->len) != 0) {
        w->err = FMLX_HTTP_ERR_BUF_SMALL;
        return w->err;
    }
    w->len = 0;
    return w->err;
}

/* Scrive SOLO se entra intera: mai un pezzo di header e poi l'errore. */
static int put(FmlxHttpRes *w, const char *s, size_t n) {
    if (w->len > w->cap || n > w->cap - w->len) {
        w->err = FMLX_HTTP_ERR_BUF_SMALL;
        return 0;
    }
    if (n) memcpy(w->buf + w->len, s, n);
    w->len += n;
    return 1;
}

static int puts1(FmlxHttpRes *w, const char *s) { return put(w, s, strlen(s)); }

/* Un nome/valore che finisce in una riga di header non deve poter contenere né CTRL né `:`:
 * il secondo è il segno che il chiamante ha provato a passare due header in un campo solo.
 * È il motivo per cui questa funzione esiste invece di un `sprintf` del chiamante. */
static int check_field(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (is_ctl(c) && c != '\t') return 0;
        if (c == ':') return 0;
    }
    return 1;
}

/* `extra_headers` è una lista di righe `Nome: valore` separate da CRLF e SENZA CRLF finale
 * (lo aggiunge il writer). Le righe devono essere complete: un CRLF a metà è l'unico modo
 * di passare più header, ma un CRLF finale creerebbe una riga vuota DENTRO la testata —
 * cioè la fine della testata, cioè un corpo che diventa una seconda richiesta.
 *
 * Più header in una stringa sola è la scelta che tiene fuori la variadic: `...` non può
 * essere controllato allo stesso modo, e un `printf` del chiamante è esattamente dove
 * l'iniezione di header entra. */
static int extra_ok(const char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        size_t st = i;
        long colon = -1;
        while (!(s[i] == '\r' && i + 1 < n && s[i + 1] == '\n')) {
            unsigned char c = (unsigned char)s[i];
            /* Ogni CTRL eccetto HTAB è vietato; un `\r` o `\n` sciolti qui cadono in questo
             * controllo, perché il separatore legale è solo la coppia `\r\n`. */
            if (is_ctl(c) && c != '\t') return 0;
            if (c == ':' && colon < 0) colon = (long)(i - st);
            i++;
            if (i >= n) break;
        }
        if (colon <= 0) return 0; /* nessun `:`, o nome vuoto */
        if (i >= n) return 1;     /* ultima riga, senza CRLF finale: corretto */
        i += 2;                   /* salta il CRLF separatore */
        if (i >= n) return 0;     /* CRLF FINALE: lo aggiungiamo noi, non si accetta */
    }
    return 1;
}

FmlxHttpStatus fmlx_http_write_head(FmlxHttpRes *w, int status, const char *content_type,
                                    uint64_t body_len, const char *extra_headers) {
    if (!w || !w->buf) return FMLX_HTTP_ERR_BAD_ARG;
    if (w->err) return w->err;

    /* TUTTO sotto validazione prima di scrivere un byte: un `Content-Type` con un CRLF
     * dentro, o un buffer che non basta, devono lasciare il writer COM'ERA. Un `HTTP/1.1 2`
     * scritto e poi abortito è una connessione rotta, non una risposta rifiutata. */
    if (status < 100 || status > 999) {
        w->err = FMLX_HTTP_ERR_BAD_STATUS;
        return w->err;
    }
    if (content_type && !check_field(content_type, strlen(content_type))) {
        w->err = FMLX_HTTP_ERR_BAD_HDR_VALUE;
        return w->err;
    }
    size_t xl = extra_headers ? strlen(extra_headers) : 0;
    if (xl && !extra_ok(extra_headers, xl)) {
        w->err = FMLX_HTTP_ERR_BAD_HDR_VALUE;
        return w->err;
    }
    const char *ph = reason_of(status);
    if (!ph)
        ph = status < 200 ? "Continue" : status < 300 ? "OK" : status < 400 ? "Redirect"
           : status < 500 ? "Client Error" : "Server Error";

    size_t mark = w->len;
    int ok = 1;
    char line[128];
    int n = snprintf(line, sizeof line, "HTTP/1.1 %d %s\r\n", status, ph);
    if (n > 0 && (size_t)n < sizeof line) ok = ok && put(w, line, (size_t)n);
    else ok = 0;
    if (ok && content_type)
        ok = ok && puts1(w, "Content-Type: ") && put(w, content_type, strlen(content_type)) &&
             puts1(w, "\r\n");
    if (ok) {
        if (w->chunked) ok = ok && puts1(w, "Transfer-Encoding: chunked\r\n");
        else {
            n = snprintf(line, sizeof line, "Content-Length: %llu\r\n",
                         (unsigned long long)body_len);
            ok = ok && n > 0 && (size_t)n < sizeof line && put(w, line, (size_t)n);
        }
    }
    if (ok && xl) ok = ok && put(w, extra_headers, xl) && puts1(w, "\r\n");
    if (ok) ok = ok && puts1(w, "\r\n");
    if (!ok) {
        w->len = mark; /* nessun pezzo di testata indietro: il buffer è nostro */
        if (!w->err) w->err = FMLX_HTTP_ERR_BUF_SMALL;
    }
    return w->err;
}

/* Numero di righe `data:` e byte totali del loro contenuto.
 *
 * SSE NON distingue un payload che finisce con `\n` da uno che non finisce: il lettore
 * aggiunge sempre un newline per ogni campo `data`. Quindi l'ultimo segmento vuoto viene
 * **droppato** dall'encoding, che è l'unico modo di non far crescere il payload a ogni
 * passaggio. Questo è un limite del formato, non una scelta nostra: è dichiarato qui così
 * che nessuno lo scopra riscrivendo la funzione. */
static void sse_shape(FmlxSlice data, size_t *segs, size_t *content) {
    size_t nl = 0;
    for (uint32_t i = 0; i < data.n; i++) {
        if (data.p[i] == '\n') nl++;
    }
    *segs = nl + 1u - ((data.n && data.p[data.n - 1] == '\n') ? 1u : 0u);
    *content = data.n - nl;
}

FmlxHttpStatus fmlx_http_sse_write(FmlxHttpRes *w, const char *event, FmlxSlice data) {
    if (!w) return FMLX_HTTP_ERR_BAD_ARG;
    if (w->err) return w->err; /* già in errore: nessun pezzo di evento nel flusso */
    if (!data.p) return FMLX_HTTP_ERR_BAD_ARG;
    for (uint32_t i = 0; i < data.n; i++) {
        unsigned char c = (unsigned char)data.p[i];
        if (is_ctl(c) && c != '\n' && c != '\t') {
            w->err = FMLX_HTTP_ERR_BAD_HDR_VALUE;
            return w->err;
        }
    }
    size_t el = event ? strlen(event) : 0;
    if (event && !check_field(event, el)) {
        w->err = FMLX_HTTP_ERR_BAD_HDR_VALUE;
        return w->err;
    }
    size_t segs = 0, content = 0;
    sse_shape(data, &segs, &content);
    size_t mark = w->len;
    int ok = 1;
    if (w->chunked) {
        /* Il chunk copre l'EVENTO intero, non le sue righe: un chunk per riga sarebbe
         * comunque valido, ma rende illeggibile il flusso su wire. */
        size_t ev = (event ? 9u + el : 0u) + segs * 8u + content + 2u;
        char len[32];
        int ln = snprintf(len, sizeof len, "%zx\r\n", ev);
        ok = ok && ln > 0 && put(w, len, (size_t)ln);
    }
    if (ok && event)
        ok = ok && puts1(w, "event: ") && put(w, event, el) && puts1(w, "\r\n");
    /* UN `\n` dentro il payload = UNA `data:` riga in più. È l'errore SSE classico: i
     * JSON compatti non lo contengono, ma un `tool_calls` con dentro testo sì. */
    size_t seg = 0, emitted = 0;
    for (uint32_t i = 0; ok && i <= data.n && emitted < segs; i++) {
        if (i != data.n && data.p[i] != '\n') continue;
        ok = ok && puts1(w, "data: ") && put(w, data.p + seg, i - seg) && puts1(w, "\r\n");
        seg = (size_t)i + 1;
        emitted++;
    }
    if (ok) ok = ok && puts1(w, "\r\n");
    if (ok && w->chunked) ok = ok && puts1(w, "\r\n"); /* chiude il chunk */
    if (!ok) {
        w->len = mark; /* addietro anche l'intestazione del chunk: non esiste orfana */
        if (!w->err) w->err = FMLX_HTTP_ERR_BUF_SMALL;
    }
    /* Streaming reale: ogni evento COMPLETO esce subito (TTFT = primo token) e il tetto `cap`
     * non è un tetto sulla lunghezza della risposta. Senza sink la risposta resta in memoria. */
    if (w->err == FMLX_HTTP_OK && w->sink) (void)fmlx_http_flush(w);
    return w->err;
}

FmlxHttpStatus fmlx_http_sse_end(FmlxHttpRes *w) {
    if (!w) return FMLX_HTTP_ERR_BAD_ARG;
    if (w->err) return w->err;
    if (w->chunked) {
        if (!puts1(w, "0\r\n\r\n")) return w->err;
    }
    return w->err;
}
