/* fmlx_serve.c — la metà sporca del server: bind, accept, read, write, keep-alive.
 *
 * Qui non c'è nessuna decisione su COSA rispondere: quella è in `fmlx_serve_respond.c` ed è
 * provata in memoria. Questo file ha una sola ragione di essere corretto, ed è la stessa del codec
 * che sta sotto: **sapere dove finisce una richiesta e dove comincia la successiva**. Un loop che
 * sbaglia il conteggio dei byte del corpo lascia il residuo nella finestra del prossimo request, e
 * quello che ne viene fuori non è un bug visibile: è un server che risponde in modo diverso a
 * seconda di chi è arrivato prima.
 *
 * TRE COSE CHE SEMBRANO OTTIMIZZAZIONI E NON LO SONO:
 *  1. un solo buffer di lettura, allocato una volta per `serve` e non per connessione: la memoria
 *     del server non deve dipendere da quanti client si presentano;
 *  2. `SIGPIPE` ignorato: scrivere su un socket chiuso da un browser che si annoia È normale, e
 *     non è un motivo per far morire il processo senza una riga;
 *  3. `Expect: 100-continue` onorato PRIMA di leggere un corpo grande: se il client non sa che può
 *     mandarlo, si siede nei tempi morti e il ritardo sembra colpa nostra.
 *
 * La fine di una connessione NON È UN ERRORE. Il loop locale la tratta come una condizione, non
 * come un `FmlxHttpStatus`: i codici del codec descrivono byte che arrivano, non l'assenza di
 * byte, e inventarne uno per «il client ha chiuso» vorrebbe dire farne un codice da mappare su una
 * risposta che invece non si manda.
 */
#include "fmlx/fmlx_serve.h"

#include "engine/fmlx_engine.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h> /* SO_RCVTIMEO/SO_SNDTIMEO (D14-M0) */
#include <sys/types.h>
#include <unistd.h>

/* ---- memoria ---- */

int fmlx_serve_buf_init(FmlxServeBuf *b, size_t resp_cap, size_t repos_cap) {
    if (!b) return -1;
    b->resp = NULL;
    b->resp_cap = 0;
    b->repos = NULL;
    b->repos_cap = 0;
    if (!resp_cap) resp_cap = FMLX_SERVE_RESP_MAX;
    if (!repos_cap) repos_cap = FMLX_HF_CATALOG_REPOS_MAX;
    b->resp = malloc(resp_cap);
    if (!b->resp) return -1;
    b->repos = calloc(repos_cap, sizeof b->repos[0]);
    if (!b->repos) {
        free(b->resp);
        b->resp = NULL;
        return -1;
    }
    b->resp_cap = resp_cap;
    b->repos_cap = repos_cap;
    b->sink = NULL;
    b->sink_ctx = NULL;
    return 0;
}

void fmlx_serve_buf_free(FmlxServeBuf *b) {
    if (!b) return;
    free(b->resp);
    free(b->repos);
    b->resp = NULL;
    b->resp_cap = 0;
    b->repos = NULL;
    b->repos_cap = 0;
}

/* ---- il filo che esce ---- */

/* Scrive tutto, o dice perché no. `EINTR` si riprova (un segnale non è un fallimento). NON si
 * recupera `EAGAIN`: il socket è in blocking, e un `continue` su EAGAIN sarebbe una CPU al 100%
 * con il client che non legge. */
static int write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = send(fd, p, n, 0);
        if (w > 0) {
            p += w;
            n -= (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        /* EAGAIN/EWOULDBLOCK = SO_SNDTIMEO scaduto: il client non legge. Chiudere è l'unica
         * risposta onesta — una risposta a metà è peggio di una connessione chiusa. */
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return -1;
        return -1;
    }
    return 0;
}

/* Il sink dello streaming: i byte SSE escono sul socket appena il writer li consegna. `ctx` è
 * l'fd della connessione, che vive per tutta `serve_conn`. */
static int conn_sink(void *ctx, const char *p, size_t n) { return write_all(*(int *)ctx, p, n); }

static void logf(FILE *log, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void logf(FILE *log, const char *fmt, ...) {
    va_list ap;

    if (!log) return;
    va_start(ap, fmt);
    vfprintf(log, fmt, ap);
    va_end(ap);
    fputc('\n', log);
    fflush(log);
}

/* Il target nel log: la slice punta nel buffer di lettura e NON è terminato, quindi si copia.
 * 96 byte bastano a leggere una riga di log e il taglio è marcato con ` ~`, così che «il path era
 * più lungo» non diventi «il path era quello». */
static void log_target(const FmlxSlice *t, char *out, size_t n) {
    size_t i = 0;
    while (i < t->n && i + 4 < n) {
        out[i] = t->p[i] == '\n' || t->p[i] == '\r' ? '?' : t->p[i]; /* un log non si spezza */
        i++;
    }
    if (i < t->n) {
        snprintf(out + i, n - i, " ~");
        return;
    }
    out[i] = '\0';
}

/* ---- una risposta del loop, non del responder ---- */

/* Il loop deve rispondere a cose che il responder non vede nemmeno: una testata che il codec
 * rifiuta, o un corpo promesso più grande della finestra. Si passa DA `fmlx_api_error_response`
 * anche qui: una testata scritta a mano è il modo in cui un `Content-Length` smette di coincidere
 * coi byte inviati, e una connessione appesa è peggio di un 400. */
static int loop_refuse(FmlxServeBuf *sbuf, FmlxApiErr err, const char *code, const char *msg,
                       size_t *out_len) {
    FmlxApiErrCtx e;

    memset(&e, 0, sizeof e);
    e.err = err;
    e.code = code;
    e.message = msg;
    *out_len = 0;
    if (fmlx_api_error_response(&e, 0, sbuf->resp, sbuf->resp_cap, out_len) != FMLX_API_OK)
        return -1;
    /* Il `http10` è 0 di proposito: se il parser ha rifiutato, la request-line può essere
     * QUALSIASI cosa e la sua versione non è un fatto accertato. Il `close` invece lo è: questa
     * funzione esiste solo nei punti in cui il loop sta per chiudere. */
    if (fmlx_serve_agree_head(0, sbuf, out_len, 0) != FMLX_SERVE_OK) return -1;
    return 0;
}

/* Due cause distinte, due codici: `chunked` è «non lo implemento» (501); il resto è «quello che
 * hai mandato non è una richiesta che posso leggere in sicurezza» (400). Un `Transfer-Encoding`
 * insieme a `Content-Length` non è «non supportato»: è una divergenza fra noi e un proxy, e
 * rispondere 501 mentirebbe sulla causa. */
static int loop_refuse_parse(FmlxServeBuf *sbuf, FmlxHttpStatus s, size_t *out_len) {
    char msg[192];

    if (s == FMLX_HTTP_ERR_CHUNKED) {
        snprintf(msg, sizeof msg,
                 "Transfer-Encoding: chunked is not supported: send Content-Length");
        return loop_refuse(sbuf, FMLX_API_NOT_SUPPORTED, FMLX_SERVE_CODE_CHUNKED, msg, out_len);
    }
    snprintf(msg, sizeof msg, "malformed HTTP request: %s", fmlx_http_status_str(s));
    return loop_refuse(sbuf, FMLX_API_INVALID_REQUEST, FMLX_SERVE_CODE_FRAMING, msg, out_len);
}

/* ---- una connessione ---- */

typedef struct {
    char *buf;
    size_t cap; /* la finestra: testata + corpo */
    size_t len; /* byte letti e non ancora consumati */
} Conn;

/* Serve UNA connessione fino alla fine e la lascia chiusa al chiamante. Ritorna senza errori: un
 * socket che si rompe non è un fallimento del server, e l'unica cosa che il loop deve sapere è
 * QUANTE richieste sono state servite — contate in `*req_no`, che è anche il criterio di
 * `--once`/`max_conns`. */
/* D14: timeout di idle del keep-alive. Il server serve UNA connessione alla volta; senza un
 * timeout, un client che tiene aperta una connessione inattiva (es. una GET seguita da un POST su
 * altra connessione) bloccherebbe il server per sempre. Default 2000 ms, override FMLX_SERVE_KEEPALIVE_MS. */
static int fmlx_serve_keepalive_ms(const FmlxServeOpts *o) {
    (void)o;
    const char *v = getenv("FMLX_SERVE_KEEPALIVE_MS");
    if (v && *v) { char *end = NULL; long n = strtol(v, &end, 10); if (end && *end == '\0' && n >= 0 && n <= 3600000) return (int)n; }
    return 2000;
}

/* D14-M0: SO_RCVTIMEO/SO_SNDTIMEO sulla connessione. `ms<=0` = nessun timeout (comportamento di
 * prima). Il timeout di LETTURA trasforma uno slowloris — una testata o un corpo che avanza a
 * singhiozzo senza mai completarsi — in una connessione CHIUSA invece che in un server bloccato
 * (il server serve UNA connessione alla volta); quello di SCRITTURA fa lo stesso per un client che
 * smette di leggere lo stream. Riusa lo stesso valore dell'idle keep-alive: entrambi sono un
 * timeout di INATTIVITA', e averne due divergenti sarebbe due server diversi. */
static void fmlx_serve_set_conn_timeout(int fd, int ms) {
    if (ms <= 0) return;
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

static void serve_conn(int fd, const FmlxServeOpts *o, FmlxServeBuf *sbuf, Conn *cn, FILE *log,
                       unsigned *req_no, const volatile sig_atomic_t *stop) {
    fmlx_serve_set_conn_timeout(fd, fmlx_serve_keepalive_ms(o));
    for (;;) {
        FmlxHttpReq req;
        FmlxHttpStatus hs;
        FmlxServeSt st;
        size_t need, out_len = 0, used;
        int ka = 0, sent_100 = 0;
        const char *body;
        char tgt[100];

        if (*stop) return;
        fmlx_http_req_init(&req);

        /* D14: attesa con TIMEOUT dell'inizio di una NUOVA richiesta (keep-alive). Se non c'e' gia'
         * dato bufferizzato, poll con timeout: una connessione inattiva si chiude invece di bloccare
         * il server (mono-connessione). Il corpo/la testata gia' in corso non hanno timeout qui. */
        if (cn->len == 0) {
            struct pollfd pfd; pfd.fd = fd; pfd.events = POLLIN; pfd.revents = 0;
            int pr = poll(&pfd, 1, fmlx_serve_keepalive_ms(o));
            if (pr <= 0) return;   /* timeout di idle o errore: chiudi la connessione */
        }

        /* 1. La testata: si legge finché il codec dice NEED_MORE. */
        for (;;) {
            ssize_t r;
            hs = fmlx_http_parse_req(cn->buf, cn->len, &req);
            if (hs != FMLX_HTTP_NEED_MORE) break;
            if (cn->len >= cn->cap) {
                /* Il codec fermerebbe da sé una testata oltre FMLX_HTTP_MAX_HEAD; questo è il
                 * paraurti per una finestra più piccola della sua — senza di qui, NEED_MORE
                 * resterebbe vero per sempre e la connessione sembrerebbe lenta invece che rotta. */
                hs = FMLX_HTTP_ERR_HEAD_TOO_LARGE;
                break;
            }
            r = recv(fd, cn->buf + cn->len, cn->cap - cn->len, 0);
            if (r > 0) {
                cn->len += (size_t)r;
                continue;
            }
            if (r < 0 && errno == EINTR) continue;
            if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                logf(log, "timeout in lettura della testata: connessione chiusa");
                return;
            }
            return; /* 0 o errore: il client ha chiuso senza completare una testata */
        }
        if (hs != FMLX_HTTP_OK) {
            if (loop_refuse_parse(sbuf, hs, &out_len) == 0 && out_len)
                write_all(fd, sbuf->resp, out_len);
            log_target(&req.err_field, tgt, sizeof tgt);
            logf(log, "richiesta rifiutata dal codec [%s]%s%s", fmlx_http_status_str(hs),
                 tgt[0] ? " header=" : "", tgt);
            return; /* MAI keep-alive dopo un errore di incorniciatura: è la regola anti-smuggling */
        }

        /* 2. Il corpo, che il codec per contratto NON tocca. `need` è la PROMESSA di
         * `Content-Length`, confrontata col limite prima di leggere: non si aspetta un corpo da
         * 4 GB per dirlo dopo. */
        need = req.has_body ? (size_t)req.content_length : 0;
        if (need > fmlx_serve_body_limit(o)) {
            if (loop_refuse(sbuf, FMLX_API_INVALID_REQUEST, FMLX_SERVE_CODE_BODY_LARGE,
                            "request body is larger than this server accepts", &out_len) == 0 &&
                out_len)
                write_all(fd, sbuf->resp, out_len);
            logf(log, "corpo promesso di %llu byte oltre il limite: connessione chiusa",
                 (unsigned long long)req.content_length);
            return;
        }
        while (req.head_len + need > cn->len) {
            ssize_t r;
            if (!sent_100 && fmlx_http_expects_continue(&req)) {
                static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
                if (write_all(fd, cont, sizeof cont - 1) < 0) return;
                sent_100 = 1;
            }
            r = recv(fd, cn->buf + cn->len, cn->cap - cn->len, 0);
            if (r > 0) {
                cn->len += (size_t)r;
                continue;
            }
            if (r < 0 && errno == EINTR) continue;
            if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                logf(log, "timeout in lettura del corpo: connessione chiusa");
                return;
            }
            logf(log, "corpo promesso e mai arrivato: connessione chiusa");
            return;
        }

        /* 3. La risposta, che è DECISA altrove. Il sink è impostato SOLO per questa risposta:
         * `gen_stream` lo usa per consegnare i token appena generati, e `fmlx_serve_respond`
         * ritorna FMLX_SERVE_STREAMED dicendo "già fatto". */
        body = cn->buf + req.body_off;
        sbuf->sink = conn_sink;
        sbuf->sink_ctx = &fd;
        st = fmlx_serve_respond(&req, body, need, o, sbuf, &out_len, &ka);
        sbuf->sink = NULL;
        sbuf->sink_ctx = NULL;
        (*req_no)++;
        log_target(&req.target, tgt, sizeof tgt);
        if (st == FMLX_SERVE_STREAMED) {
            logf(log, "#%u %.*s %s -> stream consegnato al client", *req_no, (int)req.method.n,
                 req.method.p, tgt);
            used = req.head_len + need;
            if (used > cn->len) used = cn->len;
            memmove(cn->buf, cn->buf + used, cn->len - used);
            cn->len -= used;
            if (!ka) return;
            continue;
        }
        if (st != FMLX_SERVE_OK || !out_len) {
            logf(log, "%.*s %s -> nessuna risposta (%s): connessione chiusa", (int)req.method.n,
                 req.method.p, tgt, fmlx_serve_status_str(st));
            return;
        }
        if (write_all(fd, sbuf->resp, out_len) < 0) {
            logf(log, "%.*s %s -> il client ha chiuso prima di leggere", (int)req.method.n,
                 req.method.p, tgt);
            return;
        }
        logf(log, "#%u %.*s %s -> %u byte%s", *req_no, (int)req.method.n, req.method.p, tgt,
             (unsigned)out_len, ka ? "" : ", connessione chiusa");

        /* 4. Pipelining: si consumano la testata PIÙ il corpo promesso. Lasciarli lì è il buco per
         * cui il request successivo verrebbe letto dall'offset sbagliato. */
        used = req.head_len + need;
        if (used > cn->len) used = cn->len;
        memmove(cn->buf, cn->buf + used, cn->len - used);
        cn->len -= used;
        if (!ka) return;
    }
}

/* ============================================================================
 * D14 M1/M2 — pool I/O + worker engine single-owner (modello B del todo D14).
 *
 * Un POOL FISSO di thread I/O (uno leggere/scrivere UNA connessione per volta) + UN worker che
 * possiede il MOTORE: le chiamate al modello stanno su UN solo thread, mai su thread arbitrari
 * (§8: l'engine MLX non è concorrente). Il worker I/O legge la richiesta dal socket, la mette in
 * una coda come `SJob`, e poi DRENA verso il socket i byte che il worker engine produce nel buffer
 * del Job — quindi l'engine NON fa I/O sul socket, e un client lento non lo blocca.
 *
 * Memoria: `parallel` buffer di lettura (uno per worker, riusato), un responder-buffer riusato dal
 * worker engine, e un buffer di risposta per Job ATTIVO (cresce con la risposta, non col numero di
 * client in coda). Le due code sono LIMITATE: quanto lavoro può essere accodato è indipendente da
 * quanti client si presentano (backpressure: il worker I/O attende se la coda è piena).
 * ============================================================================ */

typedef struct SJob {
    struct SJob *next;
    int fd;
    char *raw;          /* testata+corpo (di proprietà del Job): le slice di `req` puntano qui */
    FmlxHttpReq req;
    size_t need;
    int ka;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    char *out;          /* risposta prodotta dal worker engine, consumata dal worker I/O */
    size_t out_len, out_cap, out_off;
    int engine_done, cancelled;
} SJob;

typedef struct {
    const FmlxServeOpts *o;
    FILE *log;
    volatile sig_atomic_t *stop;
    int ls;
    unsigned max_conns;
    pthread_mutex_t cnt_mu;
    unsigned conn_no; /* connessioni accettate (per --once/max_conns) */

    FmlxServeBuf ebuf; /* riusato dal worker engine (resp/repos) */

    SJob *qhead, *qtail;
    int qlen, qcap;
    pthread_mutex_t qmu;
    pthread_cond_t qcv_data, qcv_space;

    int *cfd;
    int chead, ctail, clen, ccap;
    pthread_mutex_t cmu;
    pthread_cond_t ccv_data, ccv_space;

    int running;
    int nio;
    pthread_t *io;
    pthread_t engine;
} SPool;

/* Il `sink` del worker engine: accoda i byte alla risposta del Job. Ritorna -1 se il client è
 * andato via (la generazione si ferma). Cresce il buffer del Job — con la RISPOSTA, non coi client. */
static int sjob_sink(void *ctx, const char *p, size_t n) {
    SJob *j = (SJob *)ctx;
    pthread_mutex_lock(&j->mu);
    if (j->cancelled) { pthread_mutex_unlock(&j->mu); return -1; }
    if (j->out_len + n > j->out_cap) {
        size_t nc = j->out_cap ? j->out_cap : 16384;
        while (nc < j->out_len + n) nc *= 2;
        char *np = (char *)realloc(j->out, nc);
        if (!np) { pthread_mutex_unlock(&j->mu); return -1; }
        j->out = np; j->out_cap = nc;
    }
    memcpy(j->out + j->out_len, p, n);
    j->out_len += n;
    pthread_cond_broadcast(&j->cv);
    pthread_mutex_unlock(&j->mu);
    return 0;
}

static void sjob_free(SJob *j) {
    if (!j) return;
    pthread_mutex_destroy(&j->mu);
    pthread_cond_destroy(&j->cv);
    free(j->out);
    free(j->raw);
    free(j);
}

static void spool_push(SPool *s, SJob *j) {
    pthread_mutex_lock(&s->qmu);
    while (s->qlen >= s->qcap && s->running) pthread_cond_wait(&s->qcv_space, &s->qmu);
    j->next = NULL;
    if (s->qtail) s->qtail->next = j; else s->qhead = j;
    s->qtail = j; s->qlen++;
    pthread_cond_signal(&s->qcv_data);
    pthread_mutex_unlock(&s->qmu);
}

static int spool_conn_pop(SPool *s) {
    int fd;
    pthread_mutex_lock(&s->cmu);
    while (s->clen == 0 && s->running) pthread_cond_wait(&s->ccv_data, &s->cmu);
    if (s->clen == 0) { pthread_mutex_unlock(&s->cmu); return -1; }
    fd = s->cfd[s->chead];
    s->chead = (s->chead + 1) % s->ccap;
    s->clen--;
    pthread_cond_signal(&s->ccv_space);
    pthread_mutex_unlock(&s->cmu);
    return fd;
}

static int spool_conn_push(SPool *s, int fd) {
    int ok;
    pthread_mutex_lock(&s->cmu);
    while (s->clen >= s->ccap && s->running) pthread_cond_wait(&s->ccv_space, &s->cmu);
    ok = s->running;
    if (ok) {
        s->cfd[s->ctail] = fd;
        s->ctail = (s->ctail + 1) % s->ccap;
        s->clen++;
        pthread_cond_signal(&s->ccv_data);
    }
    pthread_mutex_unlock(&s->cmu);
    return ok ? 0 : -1;
}

/* UN thread che possiede il motore: estrae un Job, esegue `fmlx_serve_respond` (che consegna i
 * token nel buffer del Job via `sink`), e segna `engine_done`. */
static void *spool_engine_worker(void *arg) {
    SPool *s = (SPool *)arg;
    for (;;) {
        SJob *j;
        size_t out_len = 0;
        int ka = 0;
        const char *body;

        pthread_mutex_lock(&s->qmu);
        while (s->qlen == 0 && s->running) pthread_cond_wait(&s->qcv_data, &s->qmu);
        j = s->qhead;
        if (!j) { pthread_mutex_unlock(&s->qmu); break; } /* shutdown con coda vuota */
        s->qhead = j->next;
        if (!s->qhead) s->qtail = NULL;
        s->qlen--;
        pthread_cond_signal(&s->qcv_space);
        pthread_mutex_unlock(&s->qmu);

        body = j->need ? j->raw + j->req.body_off : NULL;
        s->ebuf.sink = sjob_sink;
        s->ebuf.sink_ctx = j;
        (void)fmlx_serve_respond(&j->req, body, j->need, s->o, &s->ebuf, &out_len, &ka);
        s->ebuf.sink = NULL;
        s->ebuf.sink_ctx = NULL;
        if (out_len) sjob_sink(j, s->ebuf.resp, out_len); /* non-streaming: consegna il corpo */
        pthread_mutex_lock(&j->mu);
        j->ka = ka;
        j->engine_done = 1;
        pthread_cond_broadcast(&j->cv);
        pthread_mutex_unlock(&j->mu);
    }
    return NULL;
}

/* Il worker I/O: una connessione per volta. Legge la richiesta, la accoda, drena la risposta del
 * Job verso il socket (scrivendo dal worker I/O, MAI dall'engine), gestisce il keep-alive. */
static void *spool_io_worker(void *arg) {
    SPool *s = (SPool *)arg;
    Conn cn;
    FmlxServeBuf rb; /* per i rifiuti del loop (framing, corpo troppo grande) */
    char chunk[16384];

    cn.cap = FMLX_HTTP_MAX_HEAD + fmlx_serve_body_limit(s->o) + 1;
    cn.len = 0;
    cn.buf = (char *)malloc(cn.cap);
    if (!cn.buf) return NULL;
    if (fmlx_serve_buf_init(&rb, 0, 0) != 0) { free(cn.buf); return NULL; }

    for (;;) {
        int fd = spool_conn_pop(s);
        int keep = 1;
        if (fd < 0) break;
        fmlx_serve_set_conn_timeout(fd, fmlx_serve_keepalive_ms(s->o));
        while (keep && s->running) {
            FmlxHttpReq req;
            FmlxHttpStatus hs;
            size_t need = 0, out_len = 0;
            int ka = 0, sent_100 = 0;
            SJob *j;
            char tgt[100];

            fmlx_http_req_init(&req);
            if (cn.len == 0) {
                struct pollfd pfd;
                pfd.fd = fd; pfd.events = POLLIN; pfd.revents = 0;
                if (poll(&pfd, 1, fmlx_serve_keepalive_ms(s->o)) <= 0) break;
            }
            /* testata */
            for (;;) {
                ssize_t r;
                hs = fmlx_http_parse_req(cn.buf, cn.len, &req);
                if (hs != FMLX_HTTP_NEED_MORE) break;
                if (cn.len >= cn.cap) { hs = FMLX_HTTP_ERR_HEAD_TOO_LARGE; break; }
                r = recv(fd, cn.buf + cn.len, cn.cap - cn.len, 0);
                if (r > 0) { cn.len += (size_t)r; continue; }
                if (r < 0 && errno == EINTR) continue;
                if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    logf(s->log, "timeout in lettura della testata: connessione chiusa");
                }
                keep = 0;
                break;
            }
            if (!keep) break;
            if (hs != FMLX_HTTP_OK) {
                out_len = 0;
                if (loop_refuse_parse(&rb, hs, &out_len) == 0 && out_len) write_all(fd, rb.resp, out_len);
                log_target(&req.err_field, tgt, sizeof tgt);
                logf(s->log, "richiesta rifiutata dal codec [%s]%s%s",
                     fmlx_http_status_str(hs), tgt[0] ? " header=" : "", tgt);
                break;
            }
            /* corpo */
            need = req.has_body ? (size_t)req.content_length : 0;
            if (need > fmlx_serve_body_limit(s->o)) {
                out_len = 0;
                if (loop_refuse(&rb, FMLX_API_INVALID_REQUEST, FMLX_SERVE_CODE_BODY_LARGE,
                                "request body is larger than this server accepts", &out_len) == 0 &&
                    out_len)
                    write_all(fd, rb.resp, out_len);
                logf(s->log, "corpo promesso di %llu byte oltre il limite: connessione chiusa",
                     (unsigned long long)req.content_length);
                break;
            }
            while (req.head_len + need > cn.len) {
                ssize_t r;
                if (!sent_100 && fmlx_http_expects_continue(&req)) {
                    static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
                    if (write_all(fd, cont, sizeof cont - 1) < 0) { keep = 0; break; }
                    sent_100 = 1;
                }
                if (!keep) break;
                r = recv(fd, cn.buf + cn.len, cn.cap - cn.len, 0);
                if (r > 0) { cn.len += (size_t)r; continue; }
                if (r < 0 && errno == EINTR) continue;
                if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                    logf(s->log, "timeout in lettura del corpo: connessione chiusa");
                else
                    logf(s->log, "corpo promesso e mai arrivato: connessione chiusa");
                keep = 0;
                break;
            }
            if (!keep) break;

            /* Job: il buffer di lettura PASSA al Job (le slice di `req` restano valide); un eventuale
             * request PIPELINED resta nel buffer NUOVO del worker, non in quello del Job. */
            {
                size_t used = req.head_len + need;
                size_t leftover = cn.len > used ? cn.len - used : 0;
                char *fresh = (char *)malloc(cn.cap);
                if (!fresh) { keep = 0; break; }
                if (leftover) memmove(fresh, cn.buf + used, leftover);
                j = (SJob *)calloc(1, sizeof *j);
                if (!j) { free(fresh); keep = 0; break; }
                pthread_mutex_init(&j->mu, NULL);
                pthread_cond_init(&j->cv, NULL);
                j->fd = fd;
                j->raw = cn.buf;
                j->req = req;
                j->need = need;
                cn.buf = fresh;
                cn.len = leftover;
                spool_push(s, j);
            }

            /* drena la risposta del Job: scrive il worker I/O, non l'engine. La copia sotto lock
             * evita che il `realloc` dell'engine invalidi il puntatore durante la `write`. */
            for (;;) {
                size_t avail, take;
                int edone;
                pthread_mutex_lock(&j->mu);
                while (j->out_off == j->out_len && !j->engine_done) pthread_cond_wait(&j->cv, &j->mu);
                avail = j->out_len - j->out_off;
                take = avail > sizeof chunk ? sizeof chunk : avail;
                if (take) memcpy(chunk, j->out + j->out_off, take);
                edone = j->engine_done;
                j->out_off += take;
                pthread_mutex_unlock(&j->mu);
                if (take && write_all(fd, chunk, take) < 0) {
                    pthread_mutex_lock(&j->mu);
                    j->cancelled = 1; /* la generazione si ferma al prossimo token */
                    pthread_mutex_unlock(&j->mu);
                    keep = 0;
                    break;
                }
                if (edone && take == 0) { ka = j->ka; break; }
            }
            sjob_free(j);
            if (!keep) break;
            if (!ka) break;
        }
        close(fd);
        pthread_mutex_lock(&s->cnt_mu);
        s->conn_no++;
        pthread_mutex_unlock(&s->cnt_mu);
    }
    fmlx_serve_buf_free(&rb);
    free(cn.buf);
    return NULL;
}

/* Il loop del modello B: crea il worker engine e il pool I/O, accetta e distribuisce. */
static FmlxServeSt fmlx_serve_pooled(const FmlxServeOpts *o, int ls, FILE *log,
                                     volatile sig_atomic_t *stop, unsigned *served, int nio) {
    SPool s;
    int i, one = 1;

    memset(&s, 0, sizeof s);
    s.o = o;
    s.log = log;
    s.stop = stop;
    s.ls = ls;
    s.max_conns = o->max_conns;
    s.nio = nio;
    s.running = 1;
    s.qcap = nio * 4 > 0 ? nio * 4 : 4;
    s.ccap = nio * 4 > 0 ? nio * 4 : 4;
    pthread_mutex_init(&s.cnt_mu, NULL);
    pthread_mutex_init(&s.qmu, NULL);
    pthread_cond_init(&s.qcv_data, NULL);
    pthread_cond_init(&s.qcv_space, NULL);
    pthread_mutex_init(&s.cmu, NULL);
    pthread_cond_init(&s.ccv_data, NULL);
    pthread_cond_init(&s.ccv_space, NULL);
    s.cfd = (int *)malloc((size_t)s.ccap * sizeof(int));
    s.io = (pthread_t *)malloc((size_t)nio * sizeof(pthread_t));
    if (fmlx_serve_buf_init(&s.ebuf, 0, 0) != 0) {
        logf(log, "memoria negata per il buffer del worker engine");
        free(s.cfd); free(s.io);
        return FMLX_SERVE_BUF_SMALL;
    }
    if (!s.cfd || !s.io) { free(s.cfd); free(s.io); fmlx_serve_buf_free(&s.ebuf); return FMLX_SERVE_BUF_SMALL; }

    if (pthread_create(&s.engine, NULL, spool_engine_worker, &s) != 0) {
        logf(log, "pthread_create (worker engine): %s", strerror(errno));
        free(s.cfd); free(s.io); fmlx_serve_buf_free(&s.ebuf);
        return FMLX_SERVE_BIND_FAILED;
    }
    for (i = 0; i < nio; i++) {
        if (pthread_create(&s.io[i], NULL, spool_io_worker, &s) != 0) {
            logf(log, "pthread_create (worker I/O %d): %s", i, strerror(errno));
            break;
        }
    }
    s.nio = i; /* quanti ne sono nati davvero */
    if (s.nio == 0) {
        pthread_mutex_lock(&s.qmu); s.running = 0;
        pthread_cond_broadcast(&s.qcv_data); pthread_mutex_unlock(&s.qmu);
        pthread_join(s.engine, NULL);
        free(s.cfd); free(s.io); fmlx_serve_buf_free(&s.ebuf);
        return FMLX_SERVE_BIND_FAILED;
    }

    /* accept: una connessione per volta dal thread principale, distribuita ai worker I/O. */
    for (;;) {
        int fd;
        unsigned n;
        if (*stop) break;
        pthread_mutex_lock(&s.cnt_mu);
        n = s.conn_no;
        pthread_mutex_unlock(&s.cnt_mu);
        if (s.max_conns && n >= s.max_conns) break;
        fd = accept(ls, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            logf(log, "accept: %s", strerror(errno));
            if (errno == EMFILE || errno == ENFILE) break;
            continue;
        }
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        /* backpressure sull'accept: se il pool è saturo si attende qui, non si accumula memoria. */
        if (spool_conn_push(&s, fd) != 0) { close(fd); break; }
    }

    /* shutdown: ferma i worker, sveglia tutte le attese, join. */
    pthread_mutex_lock(&s.cmu); s.running = 0;
    pthread_cond_broadcast(&s.ccv_data); pthread_cond_broadcast(&s.ccv_space);
    pthread_mutex_unlock(&s.cmu);
    pthread_mutex_lock(&s.qmu);
    pthread_cond_broadcast(&s.qcv_data); pthread_cond_broadcast(&s.qcv_space);
    pthread_mutex_unlock(&s.qmu);
    for (i = 0; i < s.nio; i++) pthread_join(s.io[i], NULL);
    pthread_join(s.engine, NULL);
    /* job rimasti in coda (shutdown) */
    pthread_mutex_lock(&s.qmu);
    while (s.qhead) {
        SJob *j = s.qhead;
        s.qhead = j->next;
        sjob_free(j);
    }
    pthread_mutex_unlock(&s.qmu);

    free(s.cfd);
    free(s.io);
    fmlx_serve_buf_free(&s.ebuf);
    if (served) *served = s.conn_no;
    return FMLX_SERVE_OK;
}

/* ---- il loop ---- */

static volatile sig_atomic_t g_stop;
static void (*saved_int)(int);
static void (*saved_term)(int);

static void on_stop_signal(int sig) {
    (void)sig;
    g_stop = 1; /* un flag, non una printf: è l'unica cosa lecita in un handler */
}

/* Solo quando il chiamante NON ha dato un suo `stop`: chi incorpora il server in un processo più
 * grande ha già i suoi segnali, e rubarglieli sarebbe un effetto collaterale invisibile. */
static void install_handlers(void) {
    struct sigaction sa, old;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_stop_signal;
    sigemptyset(&sa.sa_mask);
    /* NESSUN SA_RESTART: `accept` deve poter tornare EINTR. Con il restart il processo riceverebbe
     * il segnale DOPO aver aspettato il prossimo client, e Ctrl-C sembrerebbe non funzionare. */
    sigaction(SIGINT, &sa, &old);
    saved_int = old.sa_handler;
    sigaction(SIGTERM, &sa, &old);
    saved_term = old.sa_handler;
    signal(SIGPIPE, SIG_IGN); /* il pipe rotto non è un errore del server: vedi l'intestazione */
}

static void restore_handlers(void) {
    signal(SIGINT, saved_int ? saved_int : SIG_DFL);
    signal(SIGTERM, saved_term ? saved_term : SIG_DFL);
}

FmlxServeSt fmlx_serve(const FmlxServeOpts *o, FmlxServeBuf *sbuf, FILE *log,
                       volatile sig_atomic_t *stop, unsigned *served) {
    FmlxServeBuf owned;
    int use_owned = 0, one = 1, ls = -1, fd;
    struct addrinfo hints, *ai = NULL, *p;
    char port[16];
    const char *host = (o && o->host) ? o->host : FMLX_SERVE_HOST_DEFAULT;
    int lport = (o && o->port) ? o->port : FMLX_SERVE_PORT_DEFAULT;
    Conn cn;
    volatile sig_atomic_t *want_stop;
    unsigned req_no = 0;
    int bind_errno = 0; /* l'ultimo diniego di socket/bind/listen, copiato PRIMA della close */

    if (lport < 1 || lport > 65535) return FMLX_SERVE_BAD_ARG;
    if (served) *served = 0;
    memset(&cn, 0, sizeof cn);
    if (!sbuf) {
        memset(&owned, 0, sizeof owned);
        /* Memoria negata, NON un argomento insensato: i due codici devono dire cose diverse,
         * perché "hai passato una porta sbagliata" e "il processo non ha più RAM" sono due
         * diagnostiche opposte per chi legge il log. */
        if (fmlx_serve_buf_init(&owned, 0, 0) != 0) {
            logf(log, "memoria negata per i buffer del server");
            return FMLX_SERVE_BUF_SMALL;
        }
        sbuf = &owned;
        use_owned = 1;
    }
    cn.cap = FMLX_HTTP_MAX_HEAD + fmlx_serve_body_limit(o) + 1;
    cn.buf = malloc(cn.cap);
    if (!cn.buf) {
        logf(log, "memoria negata per la finestra di lettura (%llu byte)",
             (unsigned long long)cn.cap);
        if (use_owned) fmlx_serve_buf_free(sbuf);
        return FMLX_SERVE_BUF_SMALL;
    }

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC; /* IPv4 e IPv6: escludere uno dei due di default è una sorpresa */
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
    snprintf(port, sizeof port, "%d", lport);
    if (getaddrinfo(host, port, &hints, &ai) != 0) {
        logf(log, "l'indirizzo '%s' non si risolve", host);
        free(cn.buf);
        if (use_owned) fmlx_serve_buf_free(sbuf);
        return FMLX_SERVE_BAD_ARG;
    }
    for (p = ai; p; p = p->ai_next) {
        int e;
        ls = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (ls < 0) {
            if (bind_errno != EADDRINUSE) bind_errno = errno;
            continue;
        }
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (bind(ls, p->ai_addr, p->ai_addrlen) == 0 && listen(ls, FMLX_SERVE_LISTEN_BACKLOG) == 0)
            break;
        /* `errno` SI COPIA QUI, prima della `close`: una `close` che fallisce lo sovrascrive, e il
         * messaggio che ne viene fuori accusa il bind per l'errore di un'altra chiamata. */
        e = errno;
        if (bind_errno != EADDRINUSE) bind_errno = e;
        close(ls);
        ls = -1;
    }
    freeaddrinfo(ai);
    if (ls < 0) {
        FmlxServeSt why = FMLX_SERVE_BIND_FAILED;
        /* Tre dinieghi, tre da-fare diversi. «Argomento non valido» mandava l'operatore a
         * rileggere i flag quando i flag erano giusti e la porta era di un altro processo. */
        if (bind_errno == EADDRINUSE) why = FMLX_SERVE_ADDR_IN_USE;
        else if (bind_errno == EACCES) why = FMLX_SERVE_NO_PERM;
        logf(log, "non riesco ad ascoltare su %s:%d: %s%s", host, lport,
             bind_errno ? strerror(bind_errno) : "nessun indirizzo tentato",
             why == FMLX_SERVE_ADDR_IN_USE
                 ? " — la porta è già presa: `--port` diversa, o ferma chi ci ascolta"
                 : why == FMLX_SERVE_NO_PERM
                       ? " — le porte sotto 1024 vogliono i privilegi (root)"
                         : "");
        free(cn.buf);
        if (use_owned) fmlx_serve_buf_free(sbuf);
        return why;
    }

    want_stop = stop ? stop : &g_stop;
    if (!stop) install_handlers();

    if (o) {
        int nio = (int)o->parallel;
        if (o->batch && nio <= 1) nio = 8; /* il lotto ha senso con più richieste in volo */
        if (o->batch) fmlx_engine_batch_request(1);
        if (nio > 1) {
            FmlxServeSt ps = fmlx_serve_pooled(o, ls, log, want_stop, served, nio);
            shutdown(ls, SHUT_RDWR);
            close(ls);
            if (!stop) restore_handlers();
            free(cn.buf);
            if (use_owned) fmlx_serve_buf_free(sbuf);
            return ps;
        }
    }

    logf(log, "in ascolto su http://%s:%d: rispondo a /v1/models e /health, il motore non è "
              "collegato",
         host, lport);
    for (;;) {
        if (*want_stop) break;
        fd = accept(ls, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue; /* il segnale si vede dal giro dopo, su *want_stop */
            logf(log, "accept: %s", strerror(errno));
            if (errno == EMFILE || errno == ENFILE) break;
            continue;
        }
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        serve_conn(fd, o, sbuf, &cn, log, &req_no, want_stop);
        close(fd);
        if (o && o->max_conns && req_no >= o->max_conns) break;
    }
    shutdown(ls, SHUT_RDWR);
    close(ls);
    if (!stop) restore_handlers();
    free(cn.buf);
    if (use_owned) fmlx_serve_buf_free(sbuf);
    if (served) *served = req_no;
    return FMLX_SERVE_OK;
}
