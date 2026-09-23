/* fmlx_hf_curl.c — il trasporto HTTP: libcurl DI SISTEMA, nessuna copia di terze parti.
 *
 * Misurata prima di scriverla (non è una scelta di gusto, è ciò che il server accetta):
 *   `.../MacOSX.sdk/usr/include/curl/curl.h` → LIBCURL_VERSION 8.7.1
 *   `otool -L /usr/lib/libcurl.dylib` → SecureTransport, Security, CoreTLS: il TLS è quello di
 *   Apple, con i suoi trust store. Un bundle CA nostro sarebbe un SECONDO store da mantenere.
 *
 * Questo file NON decide nulla sul cache: nessun path, nessun hash, nessun lock. Prende un URL
 * e riporta ciò che il server ha detto (meta) o i suoi byte (get). Tutto il resto è in
 * fmlx_hf_fetch.c, dove si può provare senza rete.
 *
 * Regole prese da huggingface_hub 0.36.2, con la riga dove sta la misura. *(Le copie installate su
 * questa macchina sono DUE, entrambe "0.36.2" e NON lo stesso file: i numeri qui sotto sono della
 * copia `~/ai/env-mflux`, serie `httpx`. L'altra copia, `~/ai/env-vllmlx` (serie `requests`), ha
 * le STESSE regole a righe diverse — la tabella dei due set è in `LICENSES.md`. Una riga senza
 * quale copia si è aperta non è una prova.)*
 *  - HEAD per i metadati e `Accept-Encoding: identity` (file_download.py:1612): senza, il
 *    Content-Length racconterebbe il corpo compresso e l'hash non combacerebbe mai;
 *  - nel HEAD i redirect ASSOLUTI non si seguono: la 302 porta già gli X-Linked-* e il suo
 *    `Location` È l'URL del GET (`:1629`, e il commento `:1627-1628` dice perché non si può usare
 *    l'URL richiesto). Quelli RELATIVI sì, rifatti a mano da
 *    `_httpx_follow_relative_redirects_with_backoff` (`utils/_http.py:718-733`, con
 *    `follow_redirects=False` a `:723`): si aggiorna l'URL e si ripete, MAI lasciando che il client
 *    salti da sé a un CDN — è il caso del repo rinominato;
 *  - GET sul Location senza token se l'host è diverso (`file_download.py:1760-1766`:
 *    `urlparse(url).netloc != urlparse(metadata.location).netloc` → `headers.pop("authorization")`).
 *    Qui è doppia prudenza: `CURLOPT_UNRESTRICTED_AUTH` resta 0, quindi curl per conto suo non porta
 *    l'Authorization attraverso un redirect cross-host;
 *  - se un GET con `Range:` si sente rispondere 200 (e non 206) il server ha ignorato il Range:
 *    il file si azzera PRIMA di scrivere (`:384-396`: `seek(0)` + `truncate()` + `resume_size = 0`),
 *    altrimenti il temporaneo è un corpo doppio.
 *
 * Disciplina del mutex: `g_ctx.mu` protegge UN handle riusato (keepalive). Ogni via d'uscita
 * dalla sezione critica passa per l'etichetta `out:`, così non esiste un `return` che lasci il
 * lock preso — il genere di bug che non si vede mai finché non blocca un download.
 */
#include "fmlx/fmlx_fetch.h"
#include "fmlx_hf_internal.h"

#include <curl/curl.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CURL_HDR_LINE 8192 /* una riga di header più lunga di così non la si interpreta */
#define CURL_MAX_HOPS 8

/* `CURLOPT_XFERINFOFUNCTION` (progresso con contatori a 64 bit) esiste da curl 7.32; il
 * `curl_off_t` che usa arriva da `curl/curl.h`, che qui sopra è già incluso: niente check in
 * più. Se la libcurl di build è più vecchia, il watchdog fine non c'è e resta solo
 * `LOW_SPEED_TIME` (che di suo basta, ma misura la velocità MEDIA: vedi `xfer_cb`). */
#if CURL_AT_LEAST_VERSION(7, 32, 0)
#  define FMLX_HAVE_XFERINFO 1
#else
#  define FMLX_HAVE_XFERINFO 0
#endif

typedef struct {
    CURL *h;
    pthread_mutex_t mu;
    /* Stato della transazione in corso: valido SOLO con il mutex tenuto. */
    FmlxHfMeta *meta;
    char etag[CURL_HDR_LINE];  /* X-Linked-Etag, altrimenti ETag */
    char size[CURL_HDR_LINE];  /* X-Linked-Size, altrimenti Content-Length */
    char loc[CURL_HDR_LINE];   /* Location */
    char url[CURL_HDR_LINE];
    int fd;
    int64_t from, written, limit;
    int range_reset;
    /* Watchdog dello stream, in secondi SENZA un byte (0 => default 60). Viene da
     * `configure`/`FmlxHfFetchOpts.stall_timeout_s`, NON da una scelta di qui: la soglia è della
     * CLI (`--stall-timeout`), e un transport che se la sceglie da solo rende vuota l'opzione. */
    int stall_timeout_s;
    /* 1 = siamo stati NOI a troncare per stream fermo: distingue l'aborto della callback-da-noi
     * (ERR_STALL, ritentabile) da quello per corpo oltre la promessa (ERR_SIZE, non ritentabile),
     * che arrivano entrambi come CURLE_ABORTED_BY_CALLBACK. */
    int stalled;
    long long xfer_now;  /* dlnow+ulnow dell'ultimo giro di callback: -1 = mai visto */
    time_t xfer_at;      /* a che ora quel progresso è arrivato */
} Ctx;

static void meta_reset(FmlxHfMeta *m) {
    m->commit[0] = m->etag[0] = m->location[0] = '\0';
    m->size = -1;
    m->status = 0;
    m->not_found = 0;
}

static void hop_reset(Ctx *c) {
    c->etag[0] = c->size[0] = c->loc[0] = '\0';
    if (c->meta) meta_reset(c->meta);
}

/* Watchdog fine: riparte a OGNI perform. `xfer_now = -1` significa "nessun progresso visto
 * ancora", e il primo giro di callback fissa solo l'istante, senza giudicare. */
static void xfer_reset(Ctx *c) {
    c->stalled = 0;
    c->xfer_now = -1;
    c->xfer_at = time(NULL);
}

#if FMLX_HAVE_XFERINFO
/* "N secondi senza UN byte", non "N secondi a passo di lumaca". `CURLOPT_LOW_SPEED_TIME` misura
 * la velocita' MEDIA su una finestra, quindi con `--stall-timeout 5` cinque secondi a 1 byte/s
 * non lo fanno scattare mai: qui il clock si RESETTA a ogni byte ricevuto, e un progresso anche
 * minuscolo vale come progresso. Ritorna 1 = tronca la transazione (curl la chiama comunque il
 * numero di volte che le pare, quindi la misura deve essere dello STREAM, non del conteggio). */
static int xfer_cb(void *ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
                   curl_off_t ulnow) {
    Ctx *c = (Ctx *)ud;
    long long now_x = (long long)dlnow + (long long)ulnow;
    time_t t;
    (void)dltotal; (void)ultotal;
    t = time(NULL);
    if (now_x != c->xfer_now) { c->xfer_now = now_x; c->xfer_at = t; return 0; }
    if (c->stall_timeout_s > 0 && t - c->xfer_at >= (time_t)c->stall_timeout_s) {
        c->stalled = 1;
        return 1;
    }
    return 0;
}
#endif

static void copy_field(char *dst, size_t dst_len, const char *v, size_t vlen) {
    while (vlen && (v[vlen - 1] == '\r' || v[vlen - 1] == '\n' || v[vlen - 1] == ' ')) vlen--;
    if (vlen >= dst_len) vlen = dst_len - 1;
    memcpy(dst, v, vlen);
    dst[vlen] = '\0';
}

/* Caso-insensitivo: i nomi di header non distinguono maiuscole/minuscole (RFC 9110 §5.1). */
static int hdr_is(const char *name, size_t nlen, const char *want) {
    size_t wlen = strlen(want);
    if (nlen != wlen) return 0;
    for (size_t i = 0; i < nlen; i++) {
        char a = name[i], b = want[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

static size_t hdr_cb(char *buf, size_t size, size_t nitems, void *ud) {
    Ctx *c = (Ctx *)ud;
    size_t len = size * nitems;
    if (len == 0) return 0;

    if (len >= 5 && memcmp(buf, "HTTP/", 5) == 0) { /* riga di stato: inizia un'altra risposta */
        hop_reset(c);
        return len;
    }
    char *colon = (char *)memchr(buf, ':', len);
    if (!colon) return len;
    size_t nlen = (size_t)(colon - buf);
    const char *val = colon + 1;
    size_t vlen = len - (size_t)(val - buf);
    while (vlen && (*val == ' ' || *val == '\t')) { val++; vlen--; }

    if (hdr_is(buf, nlen, "X-Repo-Commit") && c->meta && c->meta->commit[0] == '\0')
        copy_field(c->meta->commit, sizeof c->meta->commit, val, vlen);
    else if (hdr_is(buf, nlen, "X-Linked-Etag"))
        copy_field(c->etag, sizeof c->etag, val, vlen);
    else if (hdr_is(buf, nlen, "ETag") && c->etag[0] == '\0') /* il linked-etag ha la precedenza */
        copy_field(c->etag, sizeof c->etag, val, vlen);
    else if (hdr_is(buf, nlen, "X-Linked-Size"))
        copy_field(c->size, sizeof c->size, val, vlen);
    else if (hdr_is(buf, nlen, "Content-Length") && c->size[0] == '\0')
        copy_field(c->size, sizeof c->size, val, vlen);
    else if (hdr_is(buf, nlen, "Location"))
        copy_field(c->loc, sizeof c->loc, val, vlen);
    else if (hdr_is(buf, nlen, "X-Xet-Hash") && c->meta)
        /* Solo un SEGNO: il protocollo Xet non è implementato e non si segue. Si dichiara sopra,
         * non qui, perché la HEAD e la GET passano di qui e il log deve essere uno. */
        c->meta->xet_announced = 1;
    return len;
}

static size_t body_cb(char *ptr, size_t size, size_t nitems, void *ud) {
    Ctx *c = (Ctx *)ud;
    size_t len = size * nitems;
    long code = 0;
    curl_easy_getinfo(c->h, CURLINFO_RESPONSE_CODE, &code);

    /* Range ignorato dal server (200 invece di 206): si ricomincia il file, come fa HF. */
    if (c->from > 0 && code == 200 && !c->range_reset) {
        c->range_reset = 1;
        if (ftruncate(c->fd, 0) != 0) return 0;
        if (lseek(c->fd, 0, SEEK_SET) < 0) return 0;
        c->written = 0;
    }
    /* Un server che mente sulla lunghezza non deve poter riempire il disco: oltre il limite la
     * callback RESTITUISCE 0, che per curl è CURLE_WRITE_ERROR e tronca la transazione. */
    if (c->limit >= 0 && c->written + (int64_t)len > c->limit) return 0;

    size_t off = 0;
    while (off < len) {
        ssize_t w = write(c->fd, ptr + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return 0; /* -> CURLE_WRITE_ERROR: il .incomplete resta, la ripresa no */
        }
        off += (size_t)w;
        c->written += (int64_t)w;
    }
    return len;
}

/* "https://host/path" → 1. Tutto il resto (comprese le forme senza schema) è RELATIVO, e i
 * redirect relativi sono esattamente il caso che HF gestisce a mano. */
static int url_is_absolute(const char *u) {
    for (const char *c = u; *c; c++) {
        if (*c == ':') return c != u;
        if (*c == '/' || *c == '?' || *c == '#') return 0;
    }
    return 0;
}

/* Sostituisce SOLO il path, come `urlparse(url)._replace(path=target.path)` (:297): schema, host
 * e PORT restano quelli della richiesta e la query viene lasciata — è ciò che fa HF, e spiega
 * perché un Location "/models/<org>/<repo>/resolve/..." funziona senza che il server ripeta
 * l'origine. */
static int join_relative(const char *base, const char *target, char *out, size_t out_len) {
    const char *sep = strstr(base, "://");
    char path[CURL_HDR_LINE];
    if (!sep) return -1;
    const char *slash = strchr(sep + 3, '/');
    size_t auth = slash ? (size_t)(slash - base) : strlen(base);
    if (target[0] == '/') {
        if (strlen(target) + 1 > sizeof path) return -1;
        snprintf(path, sizeof path, "%s", target);
    } else {
        const char *last = strrchr(base, '/');
        if (!last || last < sep + 3) return -1;
        if ((size_t)(last - base) + 1 + strlen(target) + 1 > sizeof path) return -1;
        snprintf(path, sizeof path, "%.*s/%s", (int)(last - base + 1), base, target);
    }
    if (auth + strlen(path) + 1 > out_len) return -1;
    memcpy(out, base, auth);
    memcpy(out + auth, path, strlen(path) + 1);
    return 0;
}

/* Configura l'handle per una chiamata. Ritorna !OK solo su setopt fallito (memoria), e in quel
 * caso ha già liberato la lista di header. */
static FmlxHfStatus setup(Ctx *c, const char *url, const char *token, int want_identity,
                          const char *extra_header, struct curl_slist **list_out) {
    struct curl_slist *list = NULL;
    char rbuf[CURL_HDR_LINE];
    CURLcode rc;

    curl_easy_reset(c->h);
    xfer_reset(c);
    if (want_identity) list = curl_slist_append(list, "Accept-Encoding: identity");
    if (token && *token) {
        char auth[2048];
        if (snprintf(auth, sizeof auth, "Authorization: Bearer %s", token) < (int)sizeof auth)
            list = curl_slist_append(list, auth);
        /* Un token più lungo di 2 KB non parte affatto, invece di partire monco: il messaggio
         * NON contiene il token, solo il fatto. */
        else {
            curl_slist_free_all(list);
            return fmlx_hf_fail(FMLX_HF_ERR_BUF_SMALL, "il token non sta in 2048 byte");
        }
    }
    if (extra_header) {
        if (snprintf(rbuf, sizeof rbuf, "%s", extra_header) >= (int)sizeof rbuf) {
            curl_slist_free_all(list);
            return fmlx_hf_fail(FMLX_HF_ERR_BUF_SMALL, "header aggiuntivo troppo lungo");
        }
        list = curl_slist_append(list, rbuf);
    }
    *list_out = list;

    rc = curl_easy_setopt(c->h, CURLOPT_URL, url);
    if (rc == CURLE_OK) rc = curl_easy_setopt(c->h, CURLOPT_HTTPHEADER, list);
    if (rc == CURLE_OK) rc = curl_easy_setopt(c->h, CURLOPT_NOSIGNAL, 1L);
    if (rc == CURLE_OK) rc = curl_easy_setopt(c->h, CURLOPT_CONNECTTIMEOUT, 30L);
    if (rc == CURLE_OK) rc = curl_easy_setopt(c->h, CURLOPT_LOW_SPEED_LIMIT, 1L); /* 1 byte/s ... */
    if (rc == CURLE_OK)
        rc = curl_easy_setopt(c->h, CURLOPT_LOW_SPEED_TIME, (long)(c->stall_timeout_s > 0 ? c->stall_timeout_s : 60));
#if FMLX_HAVE_XFERINFO
    /* Il watchdog vero (vedi xfer_cb). Resta anche LOW_SPEED_*: e' quello che parla quando la
     * connessione e' morta e curl non chiama nessuna callback, e qui sopra la finestra e' la
     * media, non "senza un byte da N secondi". I due insieme coprono casi diversi. */
    if (rc == CURLE_OK) rc = curl_easy_setopt(c->h, CURLOPT_XFERINFOFUNCTION, xfer_cb);
    if (rc == CURLE_OK) rc = curl_easy_setopt(c->h, CURLOPT_XFERINFODATA, c);
#endif
    if (rc == CURLE_OK) rc = curl_easy_setopt(c->h, CURLOPT_FOLLOWLOCATION, 0L);
    if (rc == CURLE_OK) rc = curl_easy_setopt(c->h, CURLOPT_UNRESTRICTED_AUTH, 0L);
    if (rc == CURLE_OK) rc = curl_easy_setopt(c->h, CURLOPT_SSL_VERIFYPEER, 1L);
    if (rc == CURLE_OK) rc = curl_easy_setopt(c->h, CURLOPT_SSL_VERIFYHOST, 2L);
    if (rc == CURLE_OK) rc = curl_easy_setopt(c->h, CURLOPT_HEADERFUNCTION, hdr_cb);
    if (rc == CURLE_OK) rc = curl_easy_setopt(c->h, CURLOPT_HEADERDATA, c);
#if LIBCURL_VERSION_NUM >= 0x075900 /* 7.85.0: la forma a maschera è deprecata */
    if (rc == CURLE_OK) rc = curl_easy_setopt(c->h, CURLOPT_PROTOCOLS_STR, "http,https");
    if (rc == CURLE_OK) rc = curl_easy_setopt(c->h, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    if (rc == CURLE_OK) rc = curl_easy_setopt(c->h, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    if (rc == CURLE_OK)
        rc = curl_easy_setopt(c->h, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
    if (rc != CURLE_OK) {
        curl_slist_free_all(list);
        *list_out = NULL;
        return fmlx_hf_fail(FMLX_HF_ERR_HTTP, "curl_easy_setopt: %s", curl_easy_strerror(rc));
    }
    return FMLX_HF_OK;
}

static int64_t parse_length(const char *v) {
    char *end = NULL;
    long long n;
    if (!v || !*v) return -1;
    errno = 0;
    n = strtoll(v, &end, 10);
    if (errno != 0 || end == v || n < 0) return -1;
    while (end && *end == ' ') end++;
    if (end && *end) return -1; /* "1024 bytes" non è una lunghezza: meglio non sapere che sapere male */
    return (int64_t)n;
}

static FmlxHfStatus curl_meta(void *ctx_, const char *url, const char *token, FmlxHfMeta *out) {
    Ctx *c = (Ctx *)ctx_;
    struct curl_slist *list = NULL;
    char next[CURL_HDR_LINE];
    FmlxHfStatus s;
    int hop, locked = 0;

    meta_reset(out);
    pthread_mutex_lock(&c->mu);
    locked = 1;
    c->meta = out;
    if (snprintf(c->url, sizeof c->url, "%s", url) >= (int)sizeof c->url) {
        s = fmlx_hf_fail(FMLX_HF_ERR_BUF_SMALL, "URL non sta in %d byte", CURL_HDR_LINE);
        goto out;
    }
    s = setup(c, c->url, token, 1, NULL, &list);
    if (s != FMLX_HF_OK) goto out;
    if (curl_easy_setopt(c->h, CURLOPT_NOBODY, 1L) != CURLE_OK) { /* HEAD, come huggingface_hub */
        s = fmlx_hf_fail(FMLX_HF_ERR_HTTP, "CURLOPT_NOBODY non accettato");
        goto out;
    }

    for (hop = 0; hop <= CURL_MAX_HOPS; hop++) {
        CURLcode rc;
        long code = 0;
        hop_reset(c);
        rc = curl_easy_perform(c->h);
        curl_easy_getinfo(c->h, CURLINFO_RESPONSE_CODE, &code);
        out->status = (int)code;
        if (rc != CURLE_OK) {
            if (rc == CURLE_ABORTED_BY_CALLBACK && c->stalled) {
                /* La risposta non arriva affatto: stesso ESITO del GET fermo, perche' il livello
                 * sopra ritenta BOTH e non deve imparare due lingue. */
                s = fmlx_hf_fail(FMLX_HF_ERR_STALL, "HEAD %s: nessun byte per %d secondi",
                                 c->url, c->stall_timeout_s);
            } else {
                s = fmlx_hf_fail(FMLX_HF_ERR_HTTP, "HEAD %s: %s", c->url, curl_easy_strerror(rc));
            }
            goto out;
        }
        if (code == 404 || code == 405) { out->not_found = 1; s = FMLX_HF_OK; goto out; }
        if (code >= 300 && code < 400) {
            if (!c->loc[0]) {
                s = fmlx_hf_fail(FMLX_HF_ERR_HTTP, "%s: HTTP %ld senza Location", url, code);
                goto out;
            }
            if (!url_is_absolute(c->loc)) {
                if (join_relative(c->url, c->loc, next, sizeof next) != 0) {
                    s = fmlx_hf_fail(FMLX_HF_ERR_HTTP, "redirect relativo incomprensibile: '%s'", c->loc);
                    goto out;
                }
                snprintf(c->url, sizeof c->url, "%s", next);
                if (curl_easy_setopt(c->h, CURLOPT_URL, c->url) != CURLE_OK) {
                    s = fmlx_hf_fail(FMLX_HF_ERR_HTTP, "URL di redirect non accettato");
                    goto out;
                }
                continue;
            }
            snprintf(out->location, sizeof out->location, "%s", c->loc);
            break; /* assoluto: gli header di QUESTA 302 sono la risposta che cerchiamo */
        }
        if (code >= 400) {
            s = fmlx_hf_fail(FMLX_HF_ERR_HTTP, "%s: HTTP %ld", url, code);
            goto out;
        }
        break; /* 2xx: nessun redirect da spiegare */
    }
    if (hop > CURL_MAX_HOPS) {
        s = fmlx_hf_fail(FMLX_HF_ERR_HTTP, "%s: troppi redirect (%d)", url, CURL_MAX_HOPS);
        goto out;
    }
    fmlx_hf_normalize_etag(c->etag, out->etag, sizeof out->etag);
    out->size = parse_length(c->size);
    s = FMLX_HF_OK;

out:
    c->meta = NULL;
    if (list) curl_slist_free_all(list);
    if (locked) pthread_mutex_unlock(&c->mu);
    return s;
}

static FmlxHfStatus curl_get(void *ctx_, const char *url, const char *token, int64_t from,
                             int64_t limit, int fd, int64_t *written) {
    Ctx *c = (Ctx *)ctx_;
    struct curl_slist *list = NULL;
    char range[64];
    FmlxHfStatus s;
    CURLcode rc;
    long code = 0;

    *written = 0;
    pthread_mutex_lock(&c->mu);
    c->meta = NULL; /* nessun header in uscita: questa chiamata scrive byte */
    c->fd = fd;
    c->from = from;
    c->limit = limit;
    c->written = 0;
    c->range_reset = 0;

    if (from > 0) snprintf(range, sizeof range, "Range: bytes=%lld-", (long long)from);
    else range[0] = '\0';

    s = setup(c, url, token, 1, from > 0 ? range : NULL, &list);
    if (s != FMLX_HF_OK) goto out;
    /* Il GET segue il CDN, dove il Location assoluto ci ha mandati (il HEAD no, di proposito). */
    curl_easy_setopt(c->h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c->h, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(c->h, CURLOPT_WRITEFUNCTION, body_cb);
    curl_easy_setopt(c->h, CURLOPT_WRITEDATA, c);

    rc = curl_easy_perform(c->h);
    curl_easy_getinfo(c->h, CURLINFO_RESPONSE_CODE, &code);
    *written = c->written;
    if (rc == CURLE_ABORTED_BY_CALLBACK && c->stalled) {
        /* L'ha troncato il NOSTRO watchdog, non la guardia sulla lunghezza: è la classe di errore
         * che si RITENTA dallo stesso byte, e i `written` fin qui sono lavoro valido nel
         * `.incomplete`. Il messaggio non contiene il token (regola della lane). */
        s = fmlx_hf_fail(FMLX_HF_ERR_STALL, "GET %s: nessun byte per %d secondi (%lld nuovi, %lld su %lld)",
                         url, c->stall_timeout_s, (long long)c->written,
                         (long long)(from + c->written), (long long)limit);
    } else if (rc == CURLE_WRITE_ERROR || rc == CURLE_ABORTED_BY_CALLBACK) {
        /* Il limite l'abbiamo imposto noi: è la lunghezza dichiarata dal server, quindi un corpo
         * più lungo è una promessa non mantenuta — stessa classe di ERR_SIZE. */
        s = fmlx_hf_fail(FMLX_HF_ERR_SIZE, "%s: il server ha dato piu' byte dei %lld dichiarati (HTTP %ld)",
                         url, (long long)limit, code);
    } else if (rc != CURLE_OK) {
        s = fmlx_hf_fail(FMLX_HF_ERR_HTTP, "GET %s: %s", url, curl_easy_strerror(rc));
    } else if (code == 404 || code == 405) {
        s = fmlx_hf_fail(FMLX_HF_ERR_NOT_ON_REMOTE, "GET %s: HTTP %ld", url, code);
    } else if (code < 200 || code >= 300) {
        s = fmlx_hf_fail(FMLX_HF_ERR_HTTP, "GET %s: HTTP %ld", url, code);
    } else {
        s = FMLX_HF_OK;
    }

out:
    c->fd = -1;
    c->meta = NULL;
    if (list) curl_slist_free_all(list);
    pthread_mutex_unlock(&c->mu);
    return s;
}

/* ---- il transport, come oggetto statico ---- */

/* Il watchdog deve sapere QUANTO aspettare, e non è il transport che lo decide: è
 * `--stall-timeout` della CLI, che arriva con le opzioni della chiamata. `opts == NULL` (o il
 * campo a zero) = 60 s, il comportamento storico di questo file. */
static void curl_configure(void *ctx_, const FmlxHfFetchOpts *opts) {
    Ctx *c = (Ctx *)ctx_;
    pthread_mutex_lock(&c->mu);
    c->stall_timeout_s = (opts && opts->stall_timeout_s > 0) ? opts->stall_timeout_s : 60;
    pthread_mutex_unlock(&c->mu);
}

static Ctx g_ctx;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static int g_ready;

static void ctx_init(void) {
    /* curl_global_init NON è thread-safe: qui dentro pthread_once, che lo è. Nessun
     * curl_global_cleanup: il processo che scarica modelli non è quello che si preoccupa di
     * restituire la memoria di curl a fine run. */
    curl_global_init(CURL_GLOBAL_DEFAULT);
    pthread_mutex_init(&g_ctx.mu, NULL);
    g_ctx.h = curl_easy_init();
    g_ctx.fd = -1;
    g_ctx.stall_timeout_s = 60;
    g_ctx.xfer_now = -1;
    g_ready = g_ctx.h != NULL;
}

/* Un solo oggetto, a livello di file: un compound literal in un blocco ha durata AUTOMATICA,
 * quindi `&(...)` dentro la funzione restituirebbe un puntatore a stack morto. */
static const FmlxHfTransport g_transport = {&g_ctx, curl_meta, curl_get, curl_configure};

const FmlxHfTransport *fmlx_hf_transport_curl(void) {
    pthread_once(&g_once, ctx_init);
    return g_ready ? &g_transport : NULL;
}
