/* fmlx_ps.c — `fastmlx ps`: cosa sta servendo questa macchina, e con quali modelli in RAM.
 *
 * IL CRITERIO, che va detto perché è una scelta e non una scorciatoia. FastLLM non ha un registro
 * di processi: la cache residente (`fmlx_engine_model`) vive nella memoria del PROCESSO che genera,
 * e un `fastmlx ps` è un altro processo — non può vedere `g_slot[]` di un `serve` in corso. Le due
 * risposte oneste sono (a) fingersi un registro e mentire, o (b) OSSERVARE l'unica cosa
 * introspezionabile da fuori: la porta. Qui si fa (b):
 *
 *   1. si risolve host/porta con la STESSA precedenza del server (`--host`/`--port` >
 *      `FASTMLX_HOST`/`FASTMLX_PORT` > default 127.0.0.1:8000), perché `ps` senza bandiere deve
 *      guardare dove `serve` senza bandiere ascolta;
 *   2. si apre una connessione TCP e si chiede `GET /v1/models/status`, la rotta che il server ha
 *      già per dire i modelli RESIDENTI. Nessuna rotta nuova: se un giorno la cache cambia forma, la
 *      vista di `ps` cambia con lei, non in un formato parallelo.
 *
 * Conseguenze dette per intero:
 *  - un `serve` che NON ascolta dà «nessun server in ascolto», che NON è un fallimento (`ps` è una
 *    domanda, non un'operazione: uscita 0, come `ps` di sistema senza processi);
 *  - i modelli CARICATI DA QUESTO PROCESSO non appaiono, perché questo processo non ne carica: `ps`
 *    non è un `run` e non tocca il motore;
 *  - con `FASTMLX_API_KEY` impostata, la rotta esige la chiave e `ps` la manda (Bearer), altrimenti
 *    vedrebbe un 401 che non è un guasto del server.
 */
#include "fmlx_cli_internal.h"

#include "fmlx/fmlx_json.h"
#include "fmlx/fmlx_serve.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* Un GET HTTP/1.0 con `Connection: close`: si legge fino alla chiusura, quindi non serve un parser
 * di framing. Il tetto difende da una risposta che non finisce mai; la risposta vera (una lista di
 * modelli) è di ordini di grandezza più piccola. */
#define PS_RESP_MAX (256u * 1024)

/* Apre la connessione e scrive la richiesta. Ritorna il fd, o -1 con la causa in `err`. */
static int ps_connect(const char *host, int port, char *err, size_t errcap) {
    struct addrinfo hints, *res = NULL, *rp;
    char portstr[16];
    int fd = -1, rc;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof portstr, "%d", port);
    rc = getaddrinfo(host, portstr, &hints, &res);
    if (rc != 0) {
        snprintf(err, errcap, "%s: %s", host, gai_strerror(rc));
        return -1;
    }
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) snprintf(err, errcap, "%s", strerror(errno));
    return fd;
}

/* GET + lettura completa. `status` è il codice HTTP (0 se la risposta è illeggibile). Ritorna 0 se
 * si è ricevuta una risposta (anche 4xx/5xx: è una risposta), -1 se non si è potuto parlare. */
static int ps_get(const char *host, int port, const char *path, const char *key, char **body,
                  int *status, char *err, size_t errcap) {
    int fd;
    char req[1024];
    char *buf;
    size_t len = 0;
    int n;

    *body = NULL;
    *status = 0;
    fd = ps_connect(host, port, err, errcap);
    if (fd < 0) return -1;

    n = snprintf(req, sizeof req,
                 "GET %s HTTP/1.0\r\nHost: %s:%d\r\nConnection: close\r\n%s%s%s\r\n",
                 path, host, port, key ? "Authorization: Bearer " : "", key ? key : "",
                 key ? "\r\n" : "");
    if (n < 0 || (size_t)n >= sizeof req) {
        snprintf(err, errcap, "richiesta troppo lunga");
        close(fd);
        return -1;
    }
    {
        size_t off = 0;
        while (off < (size_t)n) {
            ssize_t w = write(fd, req + off, (size_t)n - off);
            if (w < 0) {
                if (errno == EINTR) continue;
                snprintf(err, errcap, "scrittura della richiesta fallita: %s", strerror(errno));
                close(fd);
                return -1;
            }
            off += (size_t)w;
        }
    }

    buf = malloc(PS_RESP_MAX + 1);
    if (!buf) {
        snprintf(err, errcap, "memoria negata");
        close(fd);
        return -1;
    }
    while (len < PS_RESP_MAX && (n = (int)read(fd, buf + len, PS_RESP_MAX - len)) > 0)
        len += (size_t)n;
    close(fd);
    buf[len] = '\0';

    {
        char *sep = strstr(buf, "\r\n\r\n");
        if (len >= 12 && !memcmp(buf, "HTTP/", 5)) *status = atoi(buf + 9);
        if (sep) {
            size_t bodylen = len - (size_t)(sep + 4 - buf);
            char *b = malloc(bodylen + 1);
            if (!b) {
                free(buf);
                snprintf(err, errcap, "memoria negata");
                return -1;
            }
            memcpy(b, sep + 4, bodylen);
            b[bodylen] = '\0';
            *body = b;
        }
    }
    free(buf);
    return 0;
}

/* Stampa i modelli residenti da `{"object":"list","data":[{"id":...,"state":...}]}`. Se il corpo non
 * è il JSON atteso lo DICE, invece di mostrare una lista vuota che sembra «nessun modello». */
static void ps_print_resident(const char *body, FILE *out) {
    FmlxJsonDoc doc;
    const FmlxJsonValue *root, *data;
    size_t i, n;

    if (!body) return;
    fmlx_json_doc_init(&doc);
    if (fmlx_json_parse(body, strlen(body), 0, &doc) != FMLX_JSON_OK) {
        fprintf(out, "modelli residenti: (risposta non leggibile come JSON)\n");
        fmlx_json_doc_free(&doc);
        return;
    }
    root = fmlx_json_root(&doc);
    data = fmlx_json_obj_get(root, "data");
    n = fmlx_json_arr_n(data);
    if (!data || data->type != FMLX_JSON_ARR) {
        fprintf(out, "modelli residenti: (risposta senza lista `data`)\n");
        fmlx_json_doc_free(&doc);
        return;
    }
    fprintf(out, "modelli residenti (%zu):\n", n);
    for (i = 0; i < n; i++) {
        const FmlxJsonValue *m = fmlx_json_arr_at(data, i);
        const FmlxJsonValue *id = fmlx_json_obj_get(m, "id");
        const FmlxJsonValue *state = fmlx_json_obj_get(m, "state");
        const char *ip = NULL, *sp = NULL;
        size_t il = 0, sl = 0;
        fmlx_json_as_str(id, &ip, &il);
        fmlx_json_as_str(state, &sp, &sl);
        fprintf(out, "  %.*s  %.*s\n", (int)il, ip ? ip : "?", (int)sl, sp ? sp : "");
    }
    fmlx_json_doc_free(&doc);
}

FmlxCliStatus fmlx_ps_exec(const FmlxCliArgs *a, FILE *out, FILE *err) {
    FmlxServeOpts o;
    const char *host, *key = NULL;
    char why[256] = "";
    char e[256] = "";
    char *body = NULL;
    int status = 0;

    if (!a) return FMLX_CLI_USAGE;
    if (!out) out = stdout;
    if (!err) err = stderr;

    o = a->serve;
    if (fmlx_serve_apply_env(&o, why, sizeof why) != FMLX_SERVE_OK) {
        fprintf(err, "fastmlx ps: %s\n", why);
        return FMLX_CLI_USAGE;
    }
    host = o.host ? o.host : FMLX_SERVE_HOST_DEFAULT;
    if (o.port == 0) o.port = FMLX_SERVE_PORT_DEFAULT;
    (void)fmlx_serve_key_from_env(&key);

    if (ps_get(host, o.port, "/v1/models/status", key, &body, &status, e, sizeof e) != 0) {
        /* Non poter parlare NON è un errore di sintassi: è la risposta alla domanda «c'è un
         * server?». Uscita 0, e il perché in una riga leggibile. */
        fprintf(out, "nessun server in ascolto su %s:%d (%s)\n", host, o.port, e);
        return FMLX_CLI_OK;
    }
    if (status == 401) {
        fprintf(out, "server in ascolto su %s:%d, ma la rotta richiede una chiave (401)\n", host,
                o.port);
    } else if (status == 200) {
        fprintf(out, "server in ascolto su %s:%d\n", host, o.port);
        ps_print_resident(body, out);
    } else {
        fprintf(out, "server in ascolto su %s:%d, risposta HTTP %d\n", host, o.port, status);
    }
    free(body);
    return FMLX_CLI_OK;
}
