/* hf_stall_probe.c — il watchdog di stall misurato su una connessione VERA, senza rete.
 *
 * NON è un caso di `ctest` e non lo diventerà: sta nel tempo. Un sonno di alcuni secondi dentro
 * una suite che deve girare in due è il modo in cui i test vengono spenti, non verificati. Ma
 * questa è la guardia che rende `pull` riprendibile su un blob da 30 GB, e fin qui NON era mai
 * stata eseguita nemmeno una volta:
 *  - `test_hf_fetch` prova la POLITICA dei ritentativi con un transport finto che risponde
 *    `ERR_STALL` su comando — giusto, ma è il livello sopra;
 *  - `hf_fetch_probe` parla con huggingface.co, dove il server NON lo puoi far fermare.
 * Qui il server che si ferma è nostro, locale, e la misura è su libcurl vera.
 *
 * Il copione, fase 1 (`stall_retries >= 1`):
 *  1. HEAD → 200 con `Content-Length`, `X-Repo-Commit` (senza il quale il cache non sa in quale
 *     snapshot scrivere: `fmlx_hf_fetch.c:305`) e un `ETag` che È il git blob id del corpo —
 *     calcolato con `fmlx_hf_hash_fd`, la STESSA funzione della lane, non una copia;
 *  2. GET → 200, i primi `CUT` byte, poi SILENZIO tenendo il socket APERTO. Se lo chiudessi, curl
 *     vedrebbe una fine prematura della risposta: un errore DI TRANSPORT, non uno stall. La
 *     distinzione fra «il tubo è rotto» e «dal tubo non arriva più niente» è esattamente ciò che
 *     questo probe esiste per non perdere, perché le due cose escono con due codici diversi e si
 *     ritentano in due modi diversi;
 *  3. il watchdog deve tagliare entro ~`stall_timeout_s`, e il ritentativo deve chiedere
 *     `Range: bytes=<CUT>-`.
 *
 * La prova che il numero ripartito sia quello GIUSTO non viene da una parola del server: viene dai
 * byte. Se il resume ripartisse dal posto sbagliato, il file finale sarebbe diverso da `BODY` e
 * l'hash non combacierebbe — che è il modo in cui un resume sbagliato si trasformerebbe in un blob
 * corrotto messo nel cache come se fosse a posto.
 *
 * Fase 2 (`stall_retries = 0`): lo stall deve RISALIRE come `ERR_STALL` e i `CUT` byte ricevuti
 * devono RESTARE nel `.incomplete`.
 *
 * COSA HA MISURATO QUESTO PROBE LA PRIMA VOLTA (2026-09-23, libcurl 8.7.1 SecureTransport, la
 * libcurl DI SISTEMA che questo build linka): la callback `CURLOPT_XFERINFOFUNCTION` non viene
 * chiamata NEPPURE UNA VOLTA su uno stream muto — zero, con qualsiasi soglia — quindi il watchdog
 * fine descritto qui sopra non taglia mai per primo; l'unico a parlare è `CURLOPT_LOW_SPEED_TIME`,
 * e lo fa a `soglia + 6` secondi (misurato con 1, 2, 3, 5, 6: 7,03 / 8,02 / 9,03 / 11,04 / 12,04).
 * Il suo esito è `CURLE_OPERATION_TIMEDOUT`, che prima di questa misura finiva in `ERR_HTTP`: la
 * classe NON ritentabile. Risultato: su un download reale fermo a metà il resume non sarebbe mai
 * partito — esattamente la cosa che `--stall-retries` promette. Da qui il ramo
 * `curl_stalled_by_lowspeed` in `fmlx_hf_curl.c`, e da qui il paletti di `soglia + 9` qui sotto.
 *
 * Uso: hf_stall_probe [dir-lavoro] [secondi-timeout] [ritentativi]
 *   dir-lavoro: dove il probe scrive il suo cache FINTO. Sotto quella radice il probe crea
 *   `<dir>-ripresa` e `<dir>-riesce` e li AZZERA a ogni fase (partire da un cache già pieno
 *   significherebbe non toccare la rete e misurare un deserto).
 */
#include "fmlx/fmlx_fetch.h"
#include "fmlx/fmlx_hf.h"
#include "fmlx_hf_internal.h" /* il git blob id: la stessa funzione che verifica i blob */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ftw.h>
#include <time.h>
#include <unistd.h>

#define BODY_LEN 4096 /* il corpo che facciamo scaricare: piccolo abbastanza, intero abbastanza */
#define CUT 1500      /* byte serviti PRIMA dello stallo: il punto da cui va ripartito il resume */

static char BODY[BODY_LEN];
static int checks, failures;

#define CK(cond, ...)                                                                              \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(cond)) {                                                                             \
            failures++;                                                                            \
            printf("  FALLIMENTO %s:%d: ", __FILE__, __LINE__);                                    \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
        }                                                                                          \
    } while (0)

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int put_all(int fd, const void *p, size_t n) {
    const char *c = p;
    while (n) {
        ssize_t w = write(fd, c, n);
        if (w > 0) {
            c += w;
            n -= (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        return -1;
    }
    return 0;
}

/* Ascoltatore sull'ephemeral: la PORTA la chiede il padre PRIMA del fork, così sa l'endpoint da
 * dare a libcurl. `bind(0)` + `getsockname` è l'unico modo di non indovinare una porta libera. */
static int listen_local(int *port_out) {
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    int one = 1, ls;

    ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) return -1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    if (bind(ls, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(ls, 8) != 0) {
        close(ls);
        return -1;
    }
    if (getsockname(ls, (struct sockaddr *)&sa, &sl) != 0) {
        close(ls);
        return -1;
    }
    *port_out = (int)ntohs(sa.sin_port);
    return ls;
}

/* Il momento in cui il finto server molla: `soglia + 9` è un PALETTI, non la misura. La misura è il
 * tempo del padre. Il numero non è tondo di proposito: MISURATO su questa macchina (libcurl 8.7.1
 * SecureTransport) il taglio di `LOW_SPEED_TIME` arriva a `soglia + 6` secondi — i 3 di margine sono
 * il tempo proprio del resume. Se il paletti parlasse per primo, il GET non concluderebbe (ERR_SIZE
 * o ERR_HTTP) e «atteso OK, arrivato ...» lo direbbe. Non si chiude il socket appena il watchdog
 * taglia: la distinzione fra «tubo rotto» e «tubo muto» è ciò che si sta provando. */
#define STALL_PALETTI_ADD 9

static void serve_child(int ls, int n_wanted, int stall_s, const char *etag, const char *commit) {
    int i;

    for (i = 0; i < n_wanted; i++) {
        char rq[4096], head[512];
        size_t have = 0;
        long long from = -1;
        int fd, hn, is_head;
        ssize_t k;

        fd = accept(ls, NULL, NULL);
        if (fd < 0) return;
        while (have + 1 < sizeof rq) {
            k = read(fd, rq + have, sizeof rq - 1 - have);
            if (k <= 0) break;
            have += (size_t)k;
            rq[have] = '\0';
            if (strstr(rq, "\r\n\r\n")) break;
        }
        if (!have) {
            close(fd);
            return;
        }
        is_head = strncmp(rq, "HEAD ", 5) == 0;
        {
            const char *r = strstr(rq, "\r\nRange: bytes=");
            if (r) from = strtoll(r + 15, NULL, 10);
        }
        if (is_head) {
            hn = snprintf(head, sizeof head,
                          "HTTP/1.1 200 OK\r\nContent-Length: %d\r\nETag: \"%s\"\r\n"
                          "X-Repo-Commit: %s\r\nAccept-Ranges: bytes\r\nConnection: close\r\n\r\n",
                          BODY_LEN, etag, commit);
            put_all(fd, head, (size_t)hn);
            close(fd);
            continue;
        }
        if (from < 0) {
            /* La richiesta intera: taglio il corpo a `CUT` e poi STO ZITTO. Il tetto è il quadruplo
             * del budget del watchdog: abbastanza perché sia LUI a parlare, non la fine della
             * connessione. */
            hn = snprintf(head, sizeof head,
                          "HTTP/1.1 200 OK\r\nContent-Length: %d\r\nAccept-Ranges: bytes\r\n"
                          "Connection: close\r\n\r\n",
                          BODY_LEN);
            if (put_all(fd, head, (size_t)hn) != 0 || put_all(fd, BODY, CUT) != 0) {
                close(fd);
                return;
            }
            {
                fd_set rs;
                struct timeval tv;
                FD_ZERO(&rs);
                FD_SET(fd, &rs);
                tv.tv_sec = (long)(stall_s + STALL_PALETTI_ADD);
                tv.tv_usec = 0;
                select(fd + 1, &rs, NULL, NULL, &tv); /* il risultato non serve: si aspettava basta */
            }
            close(fd);
            continue;
        }
        {
            long long left = (long long)BODY_LEN - from;
            if (left <= 0 || from > BODY_LEN) {
                close(fd);
                return;
            }
            hn = snprintf(head, sizeof head,
                          "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes %lld-%d/%d\r\n"
                          "Content-Length: %lld\r\nConnection: close\r\n\r\n",
                          from, BODY_LEN - 1, BODY_LEN, left);
            put_all(fd, head, (size_t)hn);
            put_all(fd, BODY + from, (size_t)left);
            close(fd);
        }
    }
    _exit(0);
}

/* ---- il cache di lavoro, azzerato a ogni fase ---- */

static int rm_one(const char *path, const struct stat *st, int flags, struct FTW *ftw) {
    (void)st;
    (void)flags;
    (void)ftw;
    return remove(path);
}
/* `FTW_PHYS`: il cache è FATTO di symlink (`snapshots/.../file -> ../../blobs/<etag>`), e seguirli
 * significherebbe cancellare i byte attraverso il link invece che il link. */
static int wipe(const char *path, const char *base) {
    if (strstr(path, base) != path) {
        printf("  FALLIMENTO: %s non è sotto %s: non cancello nulla\n", path, base);
        failures++;
        return -1;
    }
    if (nftw(path, rm_one, 16, FTW_DEPTH | FTW_PHYS) != 0 && errno != ENOENT) {
        printf("  pulizia %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (fmlx_hf_mkdir_p(path) != 0) {
        printf("  mkdir %s: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

/* Il `.incomplete` del cache: lo CERCHIO (un nome costruito a mano sarebbe una seconda autorità
 * sul path dei temporanei). Ritorna i byte che contiene, o -1 se non c'è. */
static long long incomplete_bytes(const char *cache_root, const char *repo) {
    char dir[FMLX_HF_PATH_MAX], p[FMLX_HF_PATH_MAX * 2];
    DIR *d;
    struct dirent *de;
    long long n = -1;

    if (fmlx_hf_repo_dir(cache_root, repo, dir, sizeof dir) != FMLX_HF_OK) return -1;
    if (snprintf(p, sizeof p, "%s/blobs", dir) >= (int)sizeof p) return -1;
    d = opendir(p);
    if (!d) return -1;
    while ((de = readdir(d)) != NULL) {
        size_t l = strlen(de->d_name);
        struct stat st;
        if (l < 11 || strcmp(de->d_name + l - 11, ".incomplete") != 0) continue;
        {
            char full[FMLX_HF_PATH_MAX * 3];
            if (snprintf(full, sizeof full, "%s/%s", p, de->d_name) >= (int)sizeof full) continue;
            if (stat(full, &st) != 0) continue;
            n = (long long)st.st_size;
        }
    }
    closedir(d);
    return n;
}

/* Il git blob id di `buf`: scritto su un file provvisorio e hashato con la funzione della lane.
 * Una copia di SHA1 qui sarebbe il secondo parere che non volevamo. */
static int hash_of(const char *tmp_path, const void *buf, size_t n, char *hex) {
    int fd = open(tmp_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    int rc = -1;
    if (fd < 0) {
        printf("  %s: %s\n", tmp_path, strerror(errno));
        return -1;
    }
    if (put_all(fd, buf, n) == 0 && lseek(fd, 0, SEEK_SET) == 0 &&
        fmlx_hf_hash_fd(fd, FMLX_HF_HASH_GIT_BLOB, (int64_t)n, hex, NULL) == 0)
        rc = 0;
    close(fd);
    unlink(tmp_path);
    if (rc != 0) printf("  hash di %zu byte in %s: %s\n", n, tmp_path, strerror(errno));
    return rc;
}

/* ---- una fase ---- */

static void run_phase(const char *label, const char *cache_root, const char *base,
                      const char *etag, const char *commit, int stall_s, int retries,
                      int expect_ok) {
    const char *repo = "org/stall", *rev = "main", *rel = "peso.bin";
    char endpoint[64];
    FmlxHfFetchOpts o;
    FmlxHfTransport const *t = fmlx_hf_transport_curl();
    FmlxHfRef ref;
    FmlxHfStatus s;
    double t0, t1;
    int ls, port, pid, w = 0, want;
    unsigned char *got;
    FILE *f;

    printf("\n== %s: timeout %d s, ritentativi %d, esito atteso %s\n", label, stall_s, retries,
           expect_ok ? "OK (ripreso)" : "ERR_STALL (risale)");
    if (wipe(cache_root, base) != 0) return;

    ls = listen_local(&port);
    if (ls < 0) {
        printf("  bind locale: %s\n", strerror(errno));
        failures++;
        return;
    }
    snprintf(endpoint, sizeof endpoint, "http://127.0.0.1:%d", port);
    /* Quante richieste servono la scena: HEAD + GET-fermo, più la GET di ripresa se si ritenta.
     * Una sola scena da `retries >= 1`: la GET col Range NON si ferma, quindi il ciclo della lane
     * si chiude al primo ritentativo qualunque sia il numero passato. */
    want = expect_ok ? 3 : 2;

    fflush(stdout);
    pid = fork();
    if (pid < 0) {
        printf("  fork: %s\n", strerror(errno));
        failures++;
        close(ls);
        return;
    }
    if (pid == 0) {
        serve_child(ls, want, stall_s, etag, commit);
        _exit(0);
    }
    close(ls); /* la porta è del figlio: due ascoltatori sulla stessa scena farebbero arrivare le
                * richieste al fratello sbagliato */

    memset(&o, 0, sizeof o);
    o.cache_root = cache_root;
    o.endpoint = endpoint;
    o.token = ""; /* + `unsetenv` in `main`: la lane cade su `getenv("HF_TOKEN")` quando il token è
                   * VUOTO (fmlx_hf_fetch.c:255), quindi il campo da solo NON basta — e un token
                   * vero non deve poter raggiungere il server di un probe, nemmeno locale. */
    o.stall_timeout_s = stall_s;
    o.stall_retries = retries;
    o.wait_lock_ms = 2000;

    t0 = now_s();
    s = fmlx_hf_download_with(repo, rev, rel, &o, t, &ref);
    t1 = now_s();

    printf("   esito = %s", fmlx_hf_status_str(s));
    if (s != FMLX_HF_OK && fmlx_hf_last_error()) printf("  (%s)", fmlx_hf_last_error());
    printf("\n   tempo = %.2f s (budget del watchdog: %d s)\n", t1 - t0, stall_s);

    if (expect_ok) {
        CK(s == FMLX_HF_OK, "%s: atteso OK, arrivato %s", label, fmlx_hf_status_str(s));
        CK(ref.size == BODY_LEN, "%s: dimensione %lld, attesi %d", label, (long long)ref.size,
           BODY_LEN);
        CK(t1 - t0 >= (double)stall_s - 0.5,
           "%s: %.2f s < del timeout %d s: lo stall NON c'è mai stato, il test è vuoto", label,
           t1 - t0, stall_s);
        /* Il limite DI SOPRA è la misura del watchdog: se la ripresa fosse avvenuta dopo il
         * default di 60 s, il tempo lo direbbe. I 12 sono i 6 secondi di ritardo misurati su
         * libcurl 8.7.1 (il taglio arriva a soglia+6) più il margine della seconda gamba. */
        CK(t1 - t0 < (double)stall_s * 2 + 12, "%s: %.2f s, il watchdog non ha tagliato a %d s",
           label, t1 - t0, stall_s);
        got = malloc(BODY_LEN);
        CK(got != NULL, "%s: malloc", label);
        f = got ? fopen(ref.path, "rb") : NULL;
        CK(f != NULL, "%s: aprire %s: %s", label, ref.path, strerror(errno));
        if (f && got) {
            size_t rd = fread(got, 1, BODY_LEN, f);
            fclose(f);
            CK(rd == BODY_LEN, "%s: letti %zu byte su %d", label, rd, BODY_LEN);
            CK(rd == BODY_LEN && memcmp(got, BODY, BODY_LEN) == 0,
               "%s: i byte NON sono quelli: la ripresa è partita dal posto sbagliato", label);
        }
        free(got);
        {
            int64_t hashed = 0;
            CK(fmlx_hf_verify(&ref, &hashed) == FMLX_HF_OK, "%s: il blob non verifica", label);
            CK(hashed == BODY_LEN, "%s: hash di %lld byte", label, (long long)hashed);
        }
    } else {
        long long inc;
        CK(s == FMLX_HF_ERR_STALL,
           "%s: atteso ERR_STALL, arrivato %s — se è ERR_HTTP il codice 28 di libcurl non è stato "
           "tradotto (curl_stalled_by_lowspeed): lo stall esiste, ma il livello sopra lo legge come "
           "«non ritentare» e la ripresa non parte",
           label, fmlx_hf_status_str(s));
        CK(t1 - t0 >= (double)stall_s - 0.5, "%s: %.2f s: lo stall non è arrivato", label, t1 - t0);
        /* `soglia + 9`: i 6 secondi di ritardo MISURATI dell'aborto di libcurl, più il margine
         * della macchina. Il numero che conta qui è che NON sia il default di 60 s. */
        CK(t1 - t0 < (double)stall_s + 9, "%s: %.2f s oltre ogni budget", label, t1 - t0);
        /* I byte già arrivati SONO lavoro valido: buttarli vorrebbe dire ricominciare da zero un
         * 30 GB al 90%. */
        inc = incomplete_bytes(cache_root, repo);
        CK(inc == CUT, "%s: il .incomplete ha %lld byte, attesi %d", label, inc, CUT);
    }

    kill(pid, SIGKILL); /* se il copione è finito prima, il figlio è in `accept`: niente di cui
                         * discutere */
    waitpid(pid, &w, 0);
}

int main(int argc, char **argv) {
    char base[FMLX_HF_PATH_MAX], ea[FMLX_HF_PATH_MAX], eb[FMLX_HF_PATH_MAX];
    char tmp[FMLX_HF_PATH_MAX], etag[FMLX_HF_HEX_MAX], commit[FMLX_HF_HEX_MAX];
    char sentinel[32];
    int stall_s = argc > 2 ? atoi(argv[2]) : 2;
    int retries = argc > 3 ? atoi(argv[3]) : 1;
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);
    snprintf(base, sizeof base, "%s", argc > 1 ? argv[1] : "work-hf-stall");
    /* Il figlio scrive su un socket la cui estremità opposta il watchdog STA chiudendo: senza
     * questo, SIGPIPE lo uccide e il padre vede ERR_HTTP al posto dello stall — un fallimento che
     * accuserebbe il codice giusto con la ragione sbagliata. */
    signal(SIGPIPE, SIG_IGN);
    /* La lane cerca `HF_TOKEN` nell'ambiente quando il token passato è vuoto: qui non deve trovarlo.
     * Questa riga è la garanzia di quella che altrimenti sarebbe una promessa. */
    unsetenv("HF_TOKEN");
    CK(getenv("HF_TOKEN") == NULL, "HF_TOKEN ancora presente: il probe potrebbe esporre un token");

    for (i = 0; i < BODY_LEN; i++)
        BODY[i] = (char)('A' + (i * 7 + (i / 91)) % 26); /* deterministico, non tutto uguale: un
                                                          * resume dal posto sbagliato deve potersi
                                                          * VEDERE nel confronto byte a byte */

    snprintf(tmp, sizeof tmp, "%s-corpo.tmp", base);
    if (hash_of(tmp, BODY, BODY_LEN, etag) != 0) return 2;
    /* Il commit finto DEVE essere 40 hex (`fmlx_hf_blob_hash_kind`) ma NON può essere lo stesso
     * valore dell'ETag: se la lane scambiasse i due, un probe che usa lo stesso numero per entrambi
     * non se ne accorgerebbe mai. */
    snprintf(sentinel, sizeof sentinel, "commit-finto-per-stall-probe");
    snprintf(tmp, sizeof tmp, "%s-commit.tmp", base);
    if (hash_of(tmp, sentinel, strlen(sentinel), commit) != 0) return 2;
    CK(strcmp(commit, etag) != 0, "commit e etag coincidono: la prova non distinguerebbe i due");
    printf("corpo = %d byte, git blob id = %s\n", BODY_LEN, etag);
    printf("commit finto = %s\n", commit);

    snprintf(ea, sizeof ea, "%s-ripresa", base);
    snprintf(eb, sizeof eb, "%s-riesce", base);
    if (retries > 0) run_phase("fase 1", ea, base, etag, commit, stall_s, retries, 1);
    run_phase("fase 2", eb, base, etag, commit, stall_s, 0, 0);

    printf("\nhf_stall_probe: %d verifiche, %d fallite\n", checks, failures);
    return failures ? 1 : 0;
}
