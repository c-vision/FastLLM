/* test_serve.c — cosa vede un client che parla a questo server OGGI, senza il motore.

 * Le asserzioni sono di tre specie, e l'ordine conta:
 *  1. ciò che è VERO (`/v1/models`, `/health`) deve avere il contenuto giusto E i numeri giusti —
 *     un elenco tagliato a metà passerebbe comunque il test "contiene org/uno", quindi qui si
 *     conta QUANTE volte esce;
 *  2. ciò che chiede il motore deve essere RIFIUTATO (501 + `code`), mai un 200 con il corpo
 *     vuoto: si asserisce il CODICE, non la frase, perché le frasi si possono riscrivere e i codici
 *     no;
 *  3. la validazione che NON chiede il motore (auth, framing, JSON, `anthropic-version`) deve
 *     comportarsi ESATTAMENTE come si comporterà dopo: un 400 oggi non può diventare un 200 con la
 *     stessa richiesta quando arriva la lane ①.
 *
 * Il cache è COSTRUITO su disco: `/v1/models` legge il disco, quindi o il test scrive file veri, o
 * sta provando un cammino che non ha mai corso.
 *
 * Le sezioni finali sono le uniche che toccano un socket vero: il framing del loop (byte consumati,
 * pipelining, `Connection: close`, il via libera di `Expect: 100-continue`) non si prova in memoria,
 * e un giro su 127.0.0.1 con `max_conns` è il modo di dire che ciò che arriva è lungo ESATTAMENTE
 * quanto la testata promette.
 *
 * L'ultimo gruppo è ciò che decide il SO e non la nostra logica: un `bind` rifiutato (porta occupata,
 * porta sotto i 1024) e il segnale che ferma il loop. Sono gli unici cammini in cui un esito sbagliato
 * non si vede nelle risposte — si vede in un server che non parte accusando l'operatore, o in uno che
 * muore di `SIGTERM` senza pulirsi.
 */
#include "fmlx/fmlx_serve.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <netinet/in.h>
#include <signal.h> /* `kill`, `SIGKILL`, `sig_atomic_t`: il figlio li usa quanto il padre */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h> /* `struct timeval` per `SO_RCVTIMEO` */
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int checks, failures;
static char ROOT[FMLX_HF_PATH_MAX];

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

#define HAS(hay, needle) ((hay) && strstr((hay), (needle)) != NULL)

static int count_occ(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    int n = 0;
    const char *p = hay;
    while (p && (p = strstr(p, needle)) != NULL) {
        n++;
        p += nl;
    }
    return n;
}

/* ---- fixture su disco ---- */

static int remove_one(const char *p, const struct stat *st, int flags, struct FTW *ftwbuf) {
    (void)st;
    (void)flags;
    (void)ftwbuf;
    return remove(p);
}

static void wipe(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return;
    /* FTW_PHYS: senza, i symlink dello snapshot verrebbero SEGUITI e cancellati nel bersaglio. */
    if (nftw(path, remove_one, 6, FTW_DEPTH | FTW_PHYS) != 0)
        printf("  ATTENZIONE: wipe di %s incompleto: %s\n", path, strerror(errno));
}

/* Il path assoluto del fixture: `put` se lo compone da sé, chi deve `chmod` no — e due modi diversi
 * di dire lo STESSO path sono il motivo per cui un lock si posa sulla directory sbagliata e il caso
 * resta verde per la ragione sbagliata. UN buffer statico, come negli altri file di test. */
static const char *at(const char *rel) {
    static char p[FMLX_HF_PATH_MAX * 2];
    snprintf(p, sizeof p, "%s/%s", ROOT, rel);
    return p;
}

/* Si riapre PRIMA di ogni giro (all'avvio e alla fine): un processo morto con la directory murata
 * lascia un fixture che nessun `wipe` può rimuovere, e il giro dopo sarebbe tre repo invece di
 * quattro con tutti i conteggi asseriti che diventano rossi per una ragione fantasima. */
static void unlock_fixture(void) { (void)chmod(at("hub/models--org--murato"), 0755); }

static void put(const char *rel, const char *body) {
    char p[FMLX_HF_PATH_MAX * 2];
    FILE *f;

    snprintf(p, sizeof p, "%s/%s", ROOT, rel);
    if (fmlx_hf_ensure_parent(p) != 0) {
        printf("  FIXTURE: genitore di %s: %s\n", rel, strerror(errno));
        failures++;
        return;
    }
    f = fopen(p, "wb");
    if (!f) {
        printf("  FIXTURE: open %s: %s\n", rel, strerror(errno));
        failures++;
        return;
    }
    fputs(body, f);
    fclose(f);
}

/* La radice del cache per le fixture: i file stanno in `<ROOT>/hub/models--…`, e
 * `fmlx_hf_catalog(root, …)` cerca `<root>/models--*`. Comporlo in UNO posto solo, perché due
 * modi diversi di dire la stessa radice fanno fallire i test nel punto sbagliato. */
static const char *hub(void) {
    static char b[FMLX_HF_PATH_MAX];
    snprintf(b, sizeof b, "%s/hub", ROOT);
    return b;
}

/* Un cache con le QUATTRO cose che il filtro deve distinguere: un modello vero (con byte suoi), un
 * dataset (non è un modello), un repo di soli ref (non è un modello, per un altro motivo), e un repo
 * che il filesystem NON apre — che non è un modello per un terzo motivo, e diverso da tutti: «non lo
 * so». Il quarto è qui e non solo nella lane perché `/v1/models` è la superficie dove un repo
 * illeggibile DIVENTA un modello in vendita: un `data: []` con dentro un id che nessun client riesce
 * a caricare è peggio di un'omissione, ed è per questo che `/health` deve continuare a CONTARLO. */
static int murated; /* 1 = il quarto repo C'È ed è davvero murato (root non può: vedi sotto) */

static void build_cache(void) {
    put("hub/models--org--uno/refs/main", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    put("hub/models--org--uno/blobs/peso", "0123456789");
    put("hub/models--org--uno/snapshots/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/config.json",
        "{\"model_type\":\"llama\"}");
    put("hub/datasets--org--ds/blobs/tab", "a,b,c");
    put("hub/models--org--rifonly/refs/main", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    unlock_fixture();
    /* Root NON può provare questa cosa: con l'euid 0 i controlli di permesso sono saltati, la
     * directory si aprirebbe lo stesso e il quarto repo diventerebbe un modello in più nell'elenco
     * — verde per la ragione sbagliata, e `data` con un id in più. Meglio tre casi veri che quattro
     * di cui uno raccontato. */
    murated = (geteuid() != 0);
    if (!murated) {
        printf("  (root: nessun chmod frena nessuno: il quarto repo non si costruisce)\n");
        return;
    }
    put("hub/models--org--murato/refs/main", "cccccccccccccccccccccccccccccccccccccccc");
    put("hub/models--org--murato/blobs/peso", "0123456789");
    if (chmod(at("hub/models--org--murato"), 0) != 0) {
        printf("  ATTENZIONE: chmod sul repo murato: %s: quarto caso perduto\n", strerror(errno));
        murated = 0;
    }
}

/* ---- il responder visto da un client ---- */

/* Richiesta grezza costruita: metodo+percorso, header opzionali (uno per riga, NULL-terminati),
 * corpo opzionale. Il `Content-Length` è il numero VERO del corpo: scriverlo a mano è il modo in
 * cui un test passa con un JSON tagliato — il responder vede meno byte, risponde 400 per una
 * ragione che non è quella che si voleva provare, e il numero di verifiche verdi resta lo stesso. */
static char *rq(const char *line, const char *const *headers, const char *body) {
    static char buf[2048];
    size_t n = 0;
    n += (size_t)snprintf(buf + n, sizeof buf - n, "%s HTTP/1.1\r\nHost: x\r\n", line);
    if (headers) {
        size_t i;
        for (i = 0; headers[i]; i++) n += (size_t)snprintf(buf + n, sizeof buf - n, "%s\r\n", headers[i]);
    }
    if (body) n += (size_t)snprintf(buf + n, sizeof buf - n, "Content-Length: %zu\r\n", strlen(body));
    n += (size_t)snprintf(buf + n, sizeof buf - n, "\r\n");
    if (body) n += (size_t)snprintf(buf + n, sizeof buf - n, "%s", body);
    if (n >= sizeof buf) {
        CK(0, "la richiesta di test non sta in %zu byte", sizeof buf);
        buf[0] = '\0';
    }
    return buf;
}

typedef struct {
    const char *s; /* buf->resp, sempre terminato: se non ci sta, viene AZZERATO */
    size_t n;      /* byte di risposta */
    int ka;
    int fits; /* 0 = più lunga del buffer: si asserisce su `st`, non sul testo */
    FmlxServeSt st;
} Resp;

static FmlxServeSt ask(const char *raw, const FmlxServeOpts *o, FmlxServeBuf *b, Resp *r) {
    FmlxHttpReq req;
    size_t blen;

    fmlx_http_req_init(&req);
    r->s = b->resp;
    r->n = 0;
    r->ka = -1;
    r->fits = 0;
    b->resp[0] = '\0';
    /* Se il codec NON accetta la richiesta la colpa è del test: oltre qui il responder non ci
     * arriva, e asserire sul suo comportamento con una richiesta mai arrivata sarebbe peggio di
     * un test mancante. */
    if (fmlx_http_parse_req(raw, strlen(raw), &req) != FMLX_HTTP_OK) {
        r->st = FMLX_SERVE_BAD_ARG;
        CK(0, "la richiesta di test non è incorniciata: [%s]", raw);
        return FMLX_SERVE_BAD_ARG;
    }
    blen = req.has_body ? (size_t)req.content_length : 0;
    r->st = fmlx_serve_respond(&req, raw + req.body_off, blen, o, b, &r->n, &r->ka);
    if (r->n < b->resp_cap) {
        b->resp[r->n] = '\0';
        r->fits = 1;
    } else {
        r->n = 0;
        r->s = b->resp; /* resta vuota: `%s` e `strstr` sono leciti, e non mentono */
    }
    return r->st;
}

static int r_status(const Resp *r) {
    if (!r->fits) return -1;
    if (strncmp(r->s, "HTTP/1.1 ", 9) && strncmp(r->s, "HTTP/1.0 ", 9)) return -1;
    return atoi(r->s + 9);
}

static const char *r_body(const Resp *r) {
    const char *p;
    if (!r->fits) return NULL;
    p = strstr(r->s, "\r\n\r\n");
    return p ? p + 4 : NULL;
}

/* La promessa della testata confrontata coi byte veri: è LA verifica di questo file, perché il
 * difetto più frequente di un server nuovo è un `Content-Length` che non coincide con ciò che
 * segue — e il sintomo arriva dopo, sulla richiesta successiva, con un client appeso. */
static void ck_length(const Resp *r) {
    const char *cl = r->fits ? strstr(r->s, "Content-Length: ") : NULL;
    const char *b = r_body(r);

    CK(cl != NULL, "manca Content-Length\n%s", r->s);
    if (!cl || !b) return;
    CK((size_t)atol(cl + 16) == r->n - (size_t)(b - r->s), "Content-Length=%ld, corpo=%zu",
       (long)atol(cl + 16), r->n - (size_t)(b - r->s));
}

static FmlxServeOpts opts_default(void) {
    FmlxServeOpts o;
    memset(&o, 0, sizeof o);
    o.cache_root = hub();
    return o;
}

/* ============================================================================ */

/* GET /v1/models: l'elenco VERO di ciò che c'è sul disco, meno ciò che modello non è. */
static void case_models(void) {
    FmlxServeBuf b;
    FmlxServeOpts o = opts_default();
    Resp r;

    CK(fmlx_serve_buf_init(&b, 0, 0) == 0, "buf_init");
    CK(ask(rq("GET /v1/models", NULL, NULL), &o, &b, &r) == FMLX_SERVE_OK, "models -> %s",
       fmlx_serve_status_str(r.st));
    CK(r_status(&r) == 200, "models status=%d\n%s", r_status(&r), r.s);
    CK(r.ka == 1, "HTTP/1.1 senza `Connection: close` tiene aperto (ka=%d)", r.ka);
    CK(HAS(r_body(&r), "\"object\":\"list\""), "la forma OpenAI: %s", r_body(&r));
    CK(HAS(r_body(&r), "\"id\":\"org/uno\""), "manca il modello: %s", r_body(&r));
    /* I TRE FILTRI, come negazioni: un dataset, un repo di soli ref e un repo che il filesystem non
     * apre non sono modelli servibili. Il terzo è la negazione che costa di più lasciare andare:
     * `/v1/models` è una VETRINA, e un id che nessun client riesce a caricare è peggio di un'assenza
     * — promette un modello che non c'è (o che c'è, ma non apre: per chi chiama, lo stesso). */
    CK(!HAS(r_body(&r), "org/ds"), "il dataset è nell'elenco: %s", r_body(&r));
    CK(!HAS(r_body(&r), "org/rifonly"), "il repo senza byte è nell'elenco: %s", r_body(&r));
    CK(!HAS(r_body(&r), "org/murato"), "il repo illeggibile è in vendita: %s", r_body(&r));
    /* UNO, non «uno dei tre»: la negazione di sopra dice che quel nome non c'è, questo dice che
     * l'elenco non si è allargato. Sotto root (`murated == 0`) il quarto repo non esiste nemmeno e
     * il conteggio resta uno — è la riga che rende il caso falsificabile in entrambi i giri. */
    CK(count_occ(r_body(&r), "\"id\":") == 1, "il modello di servizio non è l'unico: %s", r_body(&r));
    CK(HAS(r_body(&r), "\"owned_by\":\"local\""), "owned_by mancante");
    /* `created` è l'mtime del blob — un fatto del disco, non un orologio e non zero. */
    CK(HAS(r_body(&r), "\"created\":1"), "created deve essere l'mtime (1.7e9), non 0: %s",
       r_body(&r));
    ck_length(&r);

    /* La query NON devia la rotta: `/v1/models?x=1` è l'elenco. */
    CK(ask(rq("GET /v1/models?x=1", NULL, NULL), &o, &b, &r) == FMLX_SERVE_OK &&
               r_status(&r) == 200,
       "la query devia la rotta");
    fmlx_serve_buf_free(&b);
}

static void case_health(void) {
    FmlxServeBuf b;
    FmlxServeOpts o = opts_default();
    Resp r;

    CK(fmlx_serve_buf_init(&b, 0, 0) == 0, "buf_init");
    CK(ask(rq("GET /health", NULL, NULL), &o, &b, &r) == FMLX_SERVE_OK, "health st");
    /* 200 + `degraded`: il server fa le due cose che dichiara e non genera. Non `ok` (bugiardo
     * per omissione), non 503 (un supervisor ucciderebbe un processo sano). */
    CK(r_status(&r) == 200, "health status=%d", r_status(&r));
    CK(HAS(r_body(&r), "\"status\":\"degraded\""), "%s", r_body(&r));
    CK(HAS(r_body(&r), "\"engine\":\"ready\""), "la lane del motore deve dirsi pronta: %s",
       r_body(&r));
    CK(HAS(r_body(&r), "\"catalog\":\"ready\""), "%s", r_body(&r));
    CK(HAS(r_body(&r), "\"auth\":\"disabled\""), "senza chiave deve dire disabled");
    /* I CONTI dei filtrati: filtrare non vuol dire tacere. La partizione deve TORNARE — 1 elencato,
     * 1 dataset, 1 solo-ref, 1 illeggibile fanno 4 — perché `repos_unreadable` è la quarta classe e
     * se una di esse perde il suo conteggio è il TOTALE a ricominciare a dire un'altra cosa. Sotto
     * root la quarta classe non si costruisce (`murated == 0`) e i numeri sono gli stessi meno uno:
     * l'aspettativa si COMPONE, non si scrive a mano due volte. */
    {
        char want[64];
        snprintf(want, sizeof want, "\"repos_total\":%d", murated ? 4 : 3);
        CK(HAS(r_body(&r), want), "totale (%s): %s", want, r_body(&r));
        snprintf(want, sizeof want, "\"repos_unreadable\":%d", murated ? 1 : 0);
        CK(HAS(r_body(&r), want), "la quarta classe non è contata (%s): %s", want, r_body(&r));
    }
    CK(HAS(r_body(&r), "\"repos_listed\":1"), "elencati: %s", r_body(&r));
    CK(HAS(r_body(&r), "\"repos_refs_only\":1"), "refs-only: %s", r_body(&r));
    CK(HAS(r_body(&r), "\"repos_non_model\":1"), "non-model: %s", r_body(&r));
    /* `listed` NON conta l'illeggibile: è il numero dei modelli che il server si propone di servire,
     * e un +1 lì dentro sarebbe la prova che il filtro di `/v1/models` è un `if` in meno. */
    CK(HAS(r_body(&r), hub()), "la radice del cache deve leggersi in /health");
    ck_length(&r);
    fmlx_serve_buf_free(&b);
}

/* Il cache che NON si legge: è l'unico caso in cui `/health` dice «non sono in grado». */
static void case_health_unavailable(void) {
    FmlxServeBuf b;
    FmlxServeOpts o = opts_default();
    Resp r;

    o.cache_root = "/non-esiste-di-sicuro-fastllm-test";
    CK(fmlx_serve_buf_init(&b, 0, 0) == 0, "buf_init");
    CK(ask(rq("GET /health", NULL, NULL), &o, &b, &r) == FMLX_SERVE_OK, "health st");
    CK(r_status(&r) == 503, "cache illeggibile -> 503, non 200 (%d)", r_status(&r));
    CK(HAS(r_body(&r), "\"status\":\"unavailable\""), "%s", r_body(&r));
    CK(HAS(r_body(&r), "\"catalog\":\"error\""), "%s", r_body(&r));
    /* Il codice della lane, NUMERICO: la sua `message` è in italiano e su filo non ci va. */
    CK(HAS(r_body(&r), "\"status_code\":") && !HAS(r_body(&r), "\"status_code\":0"),
       "il codice della lane deve essere nel corpo: %s", r_body(&r));
    ck_length(&r);

    /* `/v1/models` sullo stesso cache NON risponde `data: []`: sarebbe l'indistinguibilità fra
     * «non c'è niente» e «non riesco a guardare». 500 con la causa. */
    CK(ask(rq("GET /v1/models", NULL, NULL), &o, &b, &r) == FMLX_SERVE_OK, "models st");
    CK(r_status(&r) == 500, "models su cache illeggibile -> 500 (%d)", r_status(&r));
    CK(HAS(r_body(&r), FMLX_SERVE_CODE_CATALOG), "manca il codice della causa");
    ck_length(&r);
    fmlx_serve_buf_free(&b);
}

/* `repos` più piccolo dei repo: il catalogo risponde TOO_MANY con il numero VERO e il server dice
 * 500. L'alternativa — i primi N — è un elenco troncato che ha l'aspetto esatto di una risposta. */
static void case_catalog_too_many(void) {
    FmlxServeBuf b;
    FmlxServeOpts o = opts_default();
    Resp r;

    CK(fmlx_serve_buf_init(&b, 0, 1) == 0, "buf_init con repos_cap=1");
    CK(ask(rq("GET /v1/models", NULL, NULL), &o, &b, &r) == FMLX_SERVE_OK, "st");
    CK(r_status(&r) == 500, "repos_cap troppo piccolo -> 500, non elenco monco (%d)",
       r_status(&r));
    CK(!HAS(r_body(&r), "\"id\":\"org/uno\""), "nessun modello esce da un elenco troncato");
    fmlx_serve_buf_free(&b);
}

/* ---- autenticazione ---- */

static void case_auth(void) {
    FmlxServeBuf b;
    FmlxServeOpts o = opts_default();
    const char *hd1[] = {"Authorization: Bearer sekret", NULL};
    const char *hd2[] = {"Authorization: Bearer segret", NULL};
    const char *hd3[] = {"Authorization: sekret", NULL};
    const char *hd4[] = {"x-api-key: sekret", NULL};
    Resp r;

    o.api_key = "sekret";
    CK(fmlx_serve_buf_init(&b, 0, 0) == 0, "buf_init");

    CK(ask(rq("GET /health", NULL, NULL), &o, &b, &r) == FMLX_SERVE_OK, "st");
    CK(r_status(&r) == 401, "senza chiave: 401, non 200 (%d)", r_status(&r));
    CK(HAS(r_body(&r), FMLX_SERVE_CODE_AUTH), "%s", r_body(&r));
    CK(r.ka == 0, "un 401 NON tiene aperta la connessione");
    CK(HAS(r.s, "Connection: close"), "la testata deve dirlo quanto il ritorno");

    CK(ask(rq("GET /health", hd1, NULL), &o, &b, &r) == FMLX_SERVE_OK && r_status(&r) == 200,
       "la chiave giusta passa (%d)\n%s", r_status(&r), r.s);
    CK(ask(rq("GET /health", hd2, NULL), &o, &b, &r) == FMLX_SERVE_OK && r_status(&r) == 401,
       "un byte diverso è 401 (%d)", r_status(&r));
    CK(ask(rq("GET /health", hd3, NULL), &o, &b, &r) == FMLX_SERVE_OK && r_status(&r) == 401,
       "senza `Bearer ` non è una chiave (%d)", r_status(&r));
    CK(ask(rq("GET /health", hd4, NULL), &o, &b, &r) == FMLX_SERVE_OK && r_status(&r) == 200,
       "il dialetto Anthropic passa da x-api-key (%d)", r_status(&r));
    /* Il 404 non è un premio per chi indovina una rotta: senza chiave si vede 401, con la chiave
     * la stessa 404 di sempre. «Sei autorizzato» non dipende da cosa chiedi. */
    CK(ask(rq("GET /v1/da-sondare", NULL, NULL), &o, &b, &r) == FMLX_SERVE_OK &&
               r_status(&r) == 401,
       "rotta ignota senza chiave è 401, non 404 (%d)", r_status(&r));
    {
        const char *hd[] = {"Authorization: Bearer sekret", NULL};
        CK(ask(rq("GET /v1/da-sondare", hd, NULL), &o, &b, &r) == FMLX_SERVE_OK &&
                   r_status(&r) == 404,
           "con la chiave la rotta ignota è 404 come sempre (%d)", r_status(&r));
    }
    fmlx_serve_buf_free(&b);
}

/* ---- il motore: le rotte GENERANO; ciò che non è in questa tappa resta 501 ----
 *
 * Il motore è cablato (lane LLMEngine). chat/completions/messages generano — con un modello
 * che non esiste sono un 404 del dialetto, NON il vecchio 501 «manca il motore»; count_tokens
 * conta con lo stesso render. Le rotte non ancora cablate (responses, embeddings, load, unload,
 * status, metrics) restano 501 col loro codice stabile: mai un 200 vuoto. */

static void case_engine_refusals(void) {
    FmlxServeBuf b;
    FmlxServeOpts o = opts_default();
    Resp r;

    CK(fmlx_serve_buf_init(&b, 0, 0) == 0, "buf_init");

    /* Un modello che NON esiste: 404 con il tipo del dialetto. Il motore c'è, è il nome a
     * essere sbagliato — e i due casi devono dire cose diverse. */
    CK(ask(rq("POST /v1/chat/completions", NULL,
              "{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"x\"}]}"),
           &o, &b, &r) == FMLX_SERVE_OK,
       "st chat");
    CK(r_status(&r) == 404 && HAS(r_body(&r), "\"type\":\"model_not_found\""),
       "modello assente -> 404 model_not_found (%d)\n%s", r_status(&r), r.s);
    ck_length(&r);

    /* Le rotte NON ancora cablate restano 501: l'elenco è la tabella del router, non memoria.
     * `load`/`unload`/`status` NON sono più qui: il motore li espone via l'ABI e sono cablati
     * (provati sotto). `responses` resta 501 col suo codice stabile. `metrics` è cablata
     * (testo Prometheus) ed è provata qui sotto; `embeddings` è cablata (ABI v4 `embed_ids`) ed è
     * provata subito dopo. */
    {
        static const struct { const char *path; int post; } R[] = {
            {"/v1/responses", 1},
        };
        size_t i;
        for (i = 0; i < sizeof R / sizeof R[0]; i++) {
            char m[64];
            snprintf(m, sizeof m, "%s %s", R[i].post ? "POST" : "GET", R[i].path);
            CK(ask(rq(m, NULL, R[i].post ? "{\"model\":\"m\"}" : NULL), &o, &b, &r) ==
                       FMLX_SERVE_OK,
               "st %s", R[i].path);
            CK(r_status(&r) == 501 && HAS(r_body(&r), FMLX_SERVE_CODE_ENGINE),
               "%s -> %d (atteso 501 + %s)\n%s", R[i].path, r_status(&r), FMLX_SERVE_CODE_ENGINE,
               r_body(&r));
            ck_length(&r);
        }
    }

    /* `/v1/embeddings` è cablata (ABI v4 `embed_ids`): senza `input` è un 400 del corpo; con
     * `input` ma modello assente è il 404 del modello (stessa disciplina della chat). */
    CK(ask(rq("POST /v1/embeddings", NULL, "{\"model\":\"m\"}"), &o, &b, &r) == FMLX_SERVE_OK,
       "st embeddings");
    CK(r_status(&r) == 400 && HAS(r_body(&r), "\"param\":\"input\""),
       "/v1/embeddings senza input -> 400 (%d)\n%s", r_status(&r), r_body(&r));
    ck_length(&r);
    CK(ask(rq("POST /v1/embeddings", NULL, "{\"model\":\"m\",\"input\":\"x\"}"), &o, &b, &r) ==
           FMLX_SERVE_OK,
       "st embeddings 2");
    CK(r_status(&r) == 404 && HAS(r_body(&r), "\"type\":\"model_not_found\""),
       "/v1/embeddings con modello assente -> 404 (%d)\n%s", r_status(&r), r_body(&r));
    ck_length(&r);

    /* `/metrics`: NON è più un 501 — è testo Prometheus coi contatori del server. Il
     * Content-Type è quello che un Prometheus accetta, e i nomi delle metriche sono il
     * contratto: cambiarli rompe la raccolta, quindi si asseriscono. */
    CK(ask(rq("GET /metrics", NULL, NULL), &o, &b, &r) == FMLX_SERVE_OK, "st metrics");
    CK(r_status(&r) == 200 && HAS(r.s, "Content-Type: text/plain"),
       "/metrics -> 200 text/plain (%d)\n%s", r_status(&r), r.s);
    CK(HAS(r_body(&r), "fastllm_requests_total") &&
           HAS(r_body(&r), "fastllm_requests_by_endpoint_total") &&
           HAS(r_body(&r), "fastllm_prefix_cache_hits_total") &&
           HAS(r_body(&r), "fastllm_prefix_cache_misses_total") &&
           HAS(r_body(&r), "fastllm_tokens_total") && HAS(r_body(&r), "fastllm_models_loaded") &&
           HAS(r_body(&r), "fastllm_kv_tokens_used") &&
           HAS(r_body(&r), "fastllm_kv_tokens_capacity"),
       "/metrics non elenca i contatori attesi:\n%s", r_body(&r));
    ck_length(&r);

    /* Amministrazione modelli: `status` elenca i residenti (vuoto = nessuno), `unload` di un
     * non-residente è un 200 `not_loaded`, `load` di un nome inesistente è il 404 del modello. */
    CK(ask(rq("GET /v1/models/status", NULL, NULL), &o, &b, &r) == FMLX_SERVE_OK,
       "st status");
    CK(r_status(&r) == 200 && HAS(r_body(&r), "\"object\":\"list\""),
       "/v1/models/status -> 200 list (%d)\n%s", r_status(&r), r_body(&r));
    ck_length(&r);
    CK(ask(rq("POST /v1/models/unload", NULL, "{\"model\":\"m\"}"), &o, &b, &r) == FMLX_SERVE_OK,
       "st unload");
    CK(r_status(&r) == 200 && HAS(r_body(&r), "\"state\":\"not_loaded\""),
       "/v1/models/unload -> 200 not_loaded (%d)\n%s", r_status(&r), r_body(&r));
    ck_length(&r);
    CK(ask(rq("POST /v1/models/load", NULL, "{\"model\":\"m\"}"), &o, &b, &r) == FMLX_SERVE_OK,
       "st load");
    CK(r_status(&r) == 404 && HAS(r_body(&r), "model_not_found"),
       "/v1/models/load di un nome inesistente -> 404 (%d)\n%s", r_status(&r), r_body(&r));
    ck_length(&r);

    /* `/v1/completions` genera anche lei: stesso 404 se il modello non c'è. */
    CK(ask(rq("POST /v1/completions", NULL, "{\"model\":\"m\",\"prompt\":\"x\"}"), &o, &b, &r) ==
               FMLX_SERVE_OK,
           "st completions");
    CK(r_status(&r) == 404 && HAS(r_body(&r), "\"type\":\"model_not_found\""),
       "/v1/completions -> 404 model_not_found (%d)\n%s", r_status(&r), r_body(&r));

    /* Il dialetto si vede da CIÒ CHE IL CLIENT MANDA (`anthropic-version`), non dal path:
     * `/v1/messages` e `count_tokens` sono Anthropic e il 404 prende la LORO forma. */
    {
        const char *hd[] = {"anthropic-version: 2023-06-01", NULL};
        CK(ask(rq("POST /v1/messages", hd,
                   "{\"model\":\"m\",\"max_tokens\":8,"
                   "\"messages\":[{\"role\":\"user\",\"content\":\"x\"}]}"),
               &o, &b, &r) == FMLX_SERVE_OK,
           "st messages");
        CK(r_status(&r) == 404 && HAS(r_body(&r), "\"type\":\"error\"") &&
               HAS(r_body(&r), "not_found_error"),
           "messages -> 404 nella forma Anthropic (%d): %s", r_status(&r), r_body(&r));
        ck_length(&r);

        CK(ask(rq("POST /v1/messages/count_tokens", hd,
                   "{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"x\"}]}"),
               &o, &b, &r) == FMLX_SERVE_OK,
           "st count_tokens");
        CK(r_status(&r) == 404 && HAS(r_body(&r), "not_found_error"),
           "count_tokens -> 404 nella forma Anthropic (%d): %s", r_status(&r), r_body(&r));
    }
    fmlx_serve_buf_free(&b);
}

/* ---- la validazione che non chiede il motore ---- */

static void case_validation(void) {
    FmlxServeBuf b;
    FmlxServeOpts o = opts_default();
    Resp r;

    CK(fmlx_serve_buf_init(&b, 0, 0) == 0, "buf_init");

    /* JSON che JSON non è: 400 con l'offset. Non 501: qui il motore non c'entra niente. */
    CK(ask(rq("POST /v1/chat/completions", NULL, "{moddd"), &o, &b, &r) == FMLX_SERVE_OK,
       "st json rotto");
    CK(r_status(&r) == 400, "corpo non JSON -> 400 (%d)\n%s", r_status(&r), r.s);
    CK(HAS(r_body(&r), FMLX_SERVE_CODE_BODY_INVALID) && HAS(r_body(&r), "offset"), "%s",
       r_body(&r));

    /* Un ARRAY è JSON valido e NON è un corpo accettabile: due cose diverse, due messaggi. */
    CK(ask(rq("POST /v1/chat/completions", NULL, "[1]"), &o, &b, &r) == FMLX_SERVE_OK &&
               r_status(&r) == 400 && HAS(r_body(&r), "must be a JSON object"),
       "radice non oggetto -> 400 con la ragione giusta");

    /* `model` mancante: lo dice l'adapter, e la SUA frase (inglese) può andare in rete. */
    CK(ask(rq("POST /v1/chat/completions", NULL, "{\"messages\":[1]}"), &o, &b, &r) ==
               FMLX_SERVE_OK &&
           r_status(&r) == 400 && HAS(r_body(&r), "model is required"),
       "la validazione dell'adapter gira OGGI: %s", r_body(&r));

    /* Versione Anthropic non capita: 400 con il SUO codice, PRIMA del 501. */
    {
        const char *hd[] = {"anthropic-version: 1999-01-01", NULL};
        CK(ask(rq("POST /v1/messages", hd, "{\"model\":\"m\"}"), &o, &b, &r) == FMLX_SERVE_OK,
           "st version");
        CK(r_status(&r) == 400 && HAS(r_body(&r), "\"type\":\"invalid_request_error\"") &&
               HAS(r_body(&r), "1999-01-01"),
           "versione sconosciuta -> 400 che NOMINA la versione (%d): %s", r_status(&r),
           r_body(&r));
    }

    /* Il tetto sul corpo, risolto dal responder su ciò che HA in mano (il loop lo risolve PRIMA di
     * leggere: la stessa funzione chiamata da due punti, non due letture del campo). */
    {
        FmlxServeOpts small = o;
        small.max_body = 8;
        CK(ask(rq("POST /v1/chat/completions", NULL, "{\"model\":\"m\",\"n\":1234}"), &small, &b,
               &r) == FMLX_SERVE_OK,
           "st body large");
        CK(r_status(&r) == 400 && HAS(r_body(&r), FMLX_SERVE_CODE_BODY_LARGE),
           "corpo oltre il tetto -> 400 + body_too_large (%d)", r_status(&r));
        CK(r.ka == 0, "dopo un corpo troppo grande la connessione si chiude");
    }

    /* Rotta ignota e metodo sbagliato: due informazioni diverse, e la 404 ECHEGGIA il target. */
    CK(ask(rq("GET /v1/rotta-che-non-c-e", NULL, NULL), &o, &b, &r) == FMLX_SERVE_OK, "st 404");
    CK(r_status(&r) == 404 && HAS(r_body(&r), "/v1/rotta-che-non-c-e"), "404 con il target: %s",
       r_body(&r));
    CK(ask(rq("POST /v1/models", NULL, "{}"), &o, &b, &r) == FMLX_SERVE_OK && r_status(&r) == 400 &&
               HAS(r_body(&r), "wrong_method"),
       "metodo sbagliato -> 400 con il SUO codice (%d)", r_status(&r));
    fmlx_serve_buf_free(&b);
}

/* ---- keep-alive dichiarato, e il buffer che non basta ---- */

static void case_framing(void) {
    FmlxServeBuf b, tiny;
    FmlxServeOpts o = opts_default();
    const char *ka[] = {"Connection: keep-alive", NULL};
    const char *cl[] = {"Connection: close", NULL};
    Resp r;

    CK(fmlx_serve_buf_init(&b, 0, 0) == 0, "buf_init");
    /* HTTP/1.0: la forma va ricomposta a mano, `rq` scrive 1.1 perché è quella che i client
     * mandano, e il default di versione NON è una cosa su cui il test deve poter mentire. */
    CK(ask("GET /health HTTP/1.0\r\nHost: x\r\n\r\n", &o, &b, &r) == FMLX_SERVE_OK, "st 1.0");
    CK(r.ka == 0, "HTTP/1.0 senza `Connection: keep-alive` chiude (ka=%d)", r.ka);
    CK(HAS(r.s, "Connection: close"), "la testata dice quello che il loop fa");
    CK(strncmp(r.s, "HTTP/1.1 200", 12) != 0, "la risposta ecoa la versione del client");
    CK(ask(rq("GET /health", ka, NULL), &o, &b, &r) == FMLX_SERVE_OK && r.ka == 1,
       "il client che lo chiede HA il keep-alive");
    CK(ask(rq("GET /health", cl, NULL), &o, &b, &r) == FMLX_SERVE_OK && r.ka == 0 &&
               HAS(r.s, "Connection: close"),
       "e il close è detto nella testata quanto nel ritorno");
    fmlx_serve_buf_free(&b);

    /* Un buffer di risposta che non basta: BUF_SMALL e `*out_len` 0 — NON una risposta a metà.
     * Il loop chiude la connessione; mandare i primi byte di un JSON sarebbe il corpo illeggibile
     * che il codec vieta esplicitamente in testa al suo header. */
    CK(fmlx_serve_buf_init(&tiny, 64, 4) == 0, "buf_init tiny");
    CK(ask(rq("GET /health", NULL, NULL), &o, &tiny, &r) == FMLX_SERVE_BUF_SMALL,
       "buffer minuscolo -> BUF_SMALL");
    CK(r.n == 0, "con BUF_SMALL non c'è nulla da inviare (out_len=%zu)", r.n);
    fmlx_serve_buf_free(&tiny);
}

/* ---- la risoluzione delle opzioni (piano §14: flag > ambiente > default) ---- */

static void case_env(void) {
    FmlxServeOpts o;
    char msg[128];
    const char *k;

    /* Si toglie ciò che l'ambiente di chi legge potrebbe aver messo: un test che passa solo perché
     * la shell era vuota non sta provando niente. */
    unsetenv("FASTLLM_HOST");
    unsetenv("FASTLLM_PORT");
    unsetenv("FASTLLM_API_KEY");
    memset(&o, 0, sizeof o);
    CK(fmlx_serve_apply_env(&o, msg, sizeof msg) == FMLX_SERVE_OK && o.host == NULL &&
           o.port == 0 && o.api_key == NULL,
       "niente ambiente, niente da riempire: i default li dà il loop");

    setenv("FASTLLM_HOST", "0.0.0.0", 1);
    setenv("FASTLLM_PORT", "8123", 1);
    setenv("FASTLLM_API_KEY", "dda", 1);
    memset(&o, 0, sizeof o);
    CK(fmlx_serve_apply_env(&o, msg, sizeof msg) == FMLX_SERVE_OK, "apply_env");
    CK(o.host && !strcmp(o.host, "0.0.0.0") && o.port == 8123 && o.api_key &&
           !strcmp(o.api_key, "dda"),
       "l'ambiente riempie ciò che il flag non ha detto");
    /* Il flag VANCE: `--host` deve valere più di un FASTLLM_HOST lasciato in un file di shell. */
    o.host = "127.0.0.1";
    o.port = 9009;
    CK(fmlx_serve_apply_env(&o, msg, sizeof msg) == FMLX_SERVE_OK && !strcmp(o.host, "127.0.0.1") &&
           o.port == 9009,
       "ciò che è già impostato NON viene sovrascritto");

    setenv("FASTLLM_PORT", "8000x", 1);
    memset(&o, 0, sizeof o);
    CK(fmlx_serve_apply_env(&o, msg, sizeof msg) == FMLX_SERVE_BAD_ARG && HAS(msg, "FASTLLM_PORT"),
       "`8000x` NON è 8000: è un typo, e va detto (%s)", msg);
    setenv("FASTLLM_PORT", "0", 1);
    memset(&o, 0, sizeof o);
    CK(fmlx_serve_apply_env(&o, msg, sizeof msg) == FMLX_SERVE_BAD_ARG, "porta 0 rifiutata");
    setenv("FASTLLM_PORT", "70000", 1);
    memset(&o, 0, sizeof o);
    CK(fmlx_serve_apply_env(&o, msg, sizeof msg) == FMLX_SERVE_BAD_ARG, "porta oltre 65535");
    unsetenv("FASTLLM_HOST");
    unsetenv("FASTLLM_PORT");
    unsetenv("FASTLLM_API_KEY");
    /* Una chiave vuota È "nessuna auth", non "la chiave è la stringa vuota": due stati che qui
     * devono coincidere, o `/health` direbbe `enabled` su un server aperto. */
    setenv("FASTLLM_API_KEY", "", 1);
    k = "qualcosa";
    CK(fmlx_serve_key_from_env(&k) == 0 && k == NULL, "chiave vuota = nessuna chiave");
    unsetenv("FASTLLM_API_KEY");
}

/* ---- il loop vero: un socket, una richiesta, la coerenza dei byte ---- */

/* Il `serve` gira nel FIGLIO e si ferma da solo: chiamarlo in linea significa sedersi su `accept`
 * per sempre, e un test che non finisce non è un test che fallisce — è un blocco da uccidere a
 * mano. `-1` se il fork non riesce (e il chiamante chiude il caso, senza appendersi). */
static pid_t serve_child(int port, unsigned max_requests) {
    pid_t pid;

    fflush(stdout); /* il figlio eredita il buffer: senza, le righe già scritte uscirebbero due volte */
    pid = fork();
    if (pid < 0) {
        CK(0, "fork: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        FmlxServeOpts o = opts_default();
        volatile sig_atomic_t stop = 0;
        o.host = "127.0.0.1";
        o.port = port;
        o.max_conns = max_requests;
        /* `buf == NULL`: il loop alloca i propri buffer, ed è il cammino che `fastllm serve`
         * percorre davvero. `stop` dato noi: così il figlio non tocca i segnali. */
        _exit(fmlx_serve(&o, NULL, NULL, &stop, NULL) == FMLX_SERVE_OK ? 0 : 1);
    }
    return pid;
}

/* La loopback con la pazienza che serve: il figlio deve arrivare a `listen` prima che la `connect`
 * vada a buon fine. `SO_RCVTIMEO` NON è cortesia: senza, un server che non risponde mai renderebbe
 * il test un processo da uccidere, e il sintomo sarebbe "la suite è lenta" invece che "il framing è
 * rotto". */
static int connect_child(int port) {
    int tries = 0;

    while (tries++ < 200) {
        struct timeval tv = {2, 0};
        struct sockaddr_in sa;
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_port = htons((unsigned short)port);
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0) return fd;
        close(fd);
        {
            struct timespec ts = {0, 25 * 1000 * 1000};
            nanosleep(&ts, NULL);
        }
    }
    return -1;
}

/* Legge finché ha `want` byte, il timeout scade, o il peer chiude. Ritorna i byte letti. */
static size_t read_up_to(int fd, char *buf, size_t cap, size_t want) {
    size_t have = 0;

    while (have < want && have + 1 < cap) {
        size_t room = cap - 1 - have, need = want - have;
        ssize_t n = recv(fd, buf + have, room < need ? room : need, 0);
        if (n <= 0) break;
        have += (size_t)n;
    }
    buf[have] = '\0';
    return have;
}

/* Legge FINO ALLA FINE della risposta (il peer chiude: `Connection: close`). */
static size_t read_all(int fd, char *buf, size_t cap) {
    size_t have = 0;

    while (have + 1 < cap) {
        ssize_t n = recv(fd, buf + have, cap - 1 - have, 0);
        if (n <= 0) break;
        have += (size_t)n;
    }
    buf[have] = '\0';
    return have;
}

/* La coerenza che sta in piedi SU UN SOLO NUMERO: i byte di corpo ricevuti dopo la testata devono
 * coincidere con `Content-Length`. È l'assioma del framing, ed è la ragione per cui questa sezione
 * esiste: in memoria il responder lo garantisce per costruzione, sul socket lo garantisce il loop. */
static void ck_framing(const char *raw, size_t have, const char *want_start) {
    const char *endhdr = strstr(raw, "\r\n\r\n");
    const char *cl = strstr(raw, "Content-Length: ");

    CK(strncmp(raw, want_start, strlen(want_start)) == 0, "esito atteso %s, risposta grezza:\n%s",
       want_start, raw);
    CK(endhdr && cl, "manca la testata completa:\n%s", raw);
    if (!endhdr || !cl) return;
    CK((size_t)atol(cl + 16) == have - (size_t)(endhdr + 4 - raw),
       "Content-Length=%ld, byte ricevuti=%zu: la promessa non coincide", (long)atol(cl + 16),
       have - (size_t)(endhdr + 4 - raw));
}

static void case_round_trip(void) {
    int port = 8100 + (int)(getpid() % 300);
    char req[256];
    char got[8192];
    pid_t pid;
    size_t have;
    int fd, w = 0;

    pid = serve_child(port, 1);
    if (pid < 0) return;
    snprintf(req, sizeof req,
             "GET /v1/models HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nConnection: close\r\n\r\n", port);
    fd = connect_child(port);
    if (fd < 0) {
        CK(0, "il server non ha ascoltato su 127.0.0.1:%d", port);
        kill(pid, SIGKILL);
        waitpid(pid, &w, 0);
        return;
    }
    if (write(fd, req, strlen(req)) != (ssize_t)strlen(req)) CK(0, "invio della richiesta");
    have = read_all(fd, got, sizeof got);
    close(fd);
    waitpid(pid, &w, 0);
    CK(WIFEXITED(w) && WEXITSTATUS(w) == 0, "il figlio esce 0 (stato %d: bind fallito?)",
       WEXITSTATUS(w));

    /* Qui si asserisce sul FRAMING, non sul contenuto (già provato in memoria). */
    ck_framing(got, have, "HTTP/1.1 200");
    CK(HAS(got, "\"id\":\"org/uno\""), "il modello non arriva dal socket: %s", got);
    CK(HAS(got, "Connection: close"), "il client chiedeva close, la testata lo dice");
}

/* `Expect: 100-continue` è l'unico punto del loop in cui il SERVER parla PRIMA di rispondere: se la
 * riga non parte, il client che rispetta il protocollo NON manda il corpo, e il server che rispetta
 * il protocollo non lo aspetta — uno stallo che dall'esterno sembra lentezza. Il caso è su un socket
 * VERO perché l'ordine dei byte È la prova: si legge il via libera, e solo DOPO si scrive il corpo.
 * Il contrario (corpo da zero byte: nessun via libera) sta nella stessa connessione successiva, così
 * che il confronto non dipenda da due build diverse del server. */
static void case_continue_100(void) {
    static const char interim[] = "HTTP/1.1 100 Continue\r\n\r\n";
    /* Un corpo CHE VALIDA: con `messages` vuoto sarebbe il layer ② a rispondere 400 PRIMA di
     * arrivare al motore, e questo caso smetterebbe di provare il 501 senza che nessuno lo dica.
     * (È successo: la prima versione aveva `"messages":[]` e il 400 era arrivato al posto del 501.) */
    static const char body[] = "{\"model\":\"org/uno\",\"messages\":[{\"role\":\"user\","
                               "\"content\":\"ciao\"}]}";
    int port = 8500 + (int)(getpid() % 300);
    char head[256], got[4096];
    pid_t pid;
    size_t have;
    int fd, w = 0;

    pid = serve_child(port, 2);
    if (pid < 0) return;
    fd = connect_child(port);
    if (fd < 0) {
        CK(0, "il server non ha ascoltato su 127.0.0.1:%d", port);
        kill(pid, SIGKILL);
        waitpid(pid, &w, 0);
        return;
    }

    snprintf(head, sizeof head,
             "POST /v1/chat/completions HTTP/1.1\r\nHost: 127.0.0.1\r\n"
             "Content-Type: application/json\r\nExpect: 100-continue\r\n"
             "Content-Length: %zu\r\nConnection: close\r\n\r\n",
             sizeof body - 1);
    if (write(fd, head, strlen(head)) != (ssize_t)strlen(head)) CK(0, "invio della testata");
    have = read_up_to(fd, got, sizeof got, sizeof interim - 1);
    CK(have == sizeof interim - 1 && memcmp(got, interim, sizeof interim - 1) == 0,
       "il via libera non arriva (letti %zu byte): %s", have, got);
    /* Il corpo parte ADESSO: se il server avesse già risposto senza aspettare il via libera, qui
     * troveremmo la risposta finale al posto dell'interim e l'asserzione sopra sarebbe fallita. */
    if (write(fd, body, sizeof body - 1) != (ssize_t)(sizeof body - 1)) CK(0, "invio del corpo");
    {
        /* L'interim occupa i primi 25 byte del buffer: la RISPOSTA comincia subito dopo, ed è su di
         * lei che va controllato `Content-Length` — il via libera non ha corpo e non conta. */
        const char *fin = got + have;
        size_t n = read_all(fd, got + have, sizeof got - have);
        close(fd);
        CK(strncmp(fin, "HTTP/1.1 404", 12) == 0, "manca il 404 dopo il via libera:\n%s", fin);
        ck_framing(fin, n, "HTTP/1.1 404");
        CK(HAS(fin, "\"type\":\"model_not_found\""), "il 404 non nomina il tipo: %s", fin);
    }

    /* Il secondo pezzo della regola: un corpo da zero byte NON merita il via libera. Se lo mandassi
     * lo stesso, un client che aspetta prima di scrivere si prenderebbe un interim che non ha
     * chiesto; e se il server lo saltasse quando il corpo c'è, quello è lo stallo qui sopra. */
    fd = connect_child(port);
    if (fd < 0) {
        CK(0, "seconda connessione refused su 127.0.0.1:%d", port);
        kill(pid, SIGKILL);
        waitpid(pid, &w, 0);
        return;
    }
    snprintf(head, sizeof head,
             "POST /v1/chat/completions HTTP/1.1\r\nHost: 127.0.0.1\r\n"
             "Expect: 100-continue\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    if (write(fd, head, strlen(head)) != (ssize_t)strlen(head)) CK(0, "invio della testata vuota");
    have = read_all(fd, got, sizeof got);
    close(fd);
    CK(strncmp(got, interim, sizeof interim - 1) != 0,
       "via libera mandato per un corpo da zero byte:\n%s", got);
    ck_framing(got, have, "HTTP/1.1 400");
    waitpid(pid, &w, 0);
    CK(WIFEXITED(w) && WEXITSTATUS(w) == 0, "il figlio esce 0 dopo due richieste (stato %d)",
       WEXITSTATUS(w));
}

/* ============================================================================ */

/* ---- ciò che decide il SO: il bind rifiutato, e il segnale che ferma il loop ----
 *
 * Questi due percorsi non si possono provare in memoria: finché il `serve` del test aveva sempre
 * dato una propria `stop`, `install_handlers` non era mai girato, e finché la porta era sempre
 * libera il ramo dei dinieghi non era mai stato preso. Sono anche gli unici due punti in cui il
 * server può UCCIDERSI (il segnale non catturato) o ACCUSARE IL CORRETTO (un `BAD_ARG` per una
 * porta scivolata sotto un altro processo). */

/* Il figlio che logga su una pipe: «in ascolto» è l'UNICO segnale onesto che il bind è riuscito e
 * il loop è entrato. La pipe e non un file: si legge non-blocking e si vede la FINE (EOF) quando il
 * figlio muore — un file vuoto non distingue «non ha ancora parlato» da «è morto in silenzio». */
static pid_t serve_child_logged(int port, unsigned max_conns, int *logfd) {
    int fds[2];
    pid_t pid;

    *logfd = -1;
    if (pipe(fds) != 0) {
        CK(0, "pipe: %s", strerror(errno));
        return -1;
    }
    fflush(stdout);
    pid = fork();
    if (pid < 0) {
        CK(0, "fork: %s", strerror(errno));
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        FmlxServeOpts o = opts_default();
        FILE *lf;

        close(fds[0]);
        lf = fdopen(fds[1], "w");
        o.host = "127.0.0.1";
        o.port = port;
        o.max_conns = max_conns;
        /* `stop == NULL`: è il cammino del binario `fastllm serve`, e l'unico in cui gli handler
         * si installano DAVVERO. Se `fdopen` fallisce la log viene NULL e il padre non legge nulla:
         * il caso fallisce dicendo il vero, invece che passare per un niente. */
        _exit(fmlx_serve(&o, NULL, lf, NULL, NULL) == FMLX_SERVE_OK ? 0 : 1);
    }
    close(fds[1]);
    *logfd = fds[0];
    {
        int fl = fcntl(fds[0], F_GETFL, 0);
        if (fl >= 0) fcntl(fds[0], F_SETFL, fl | O_NONBLOCK);
    }
    return pid;
}

/* Legge la pipe finché trova `needle`, o fino alla fine del tempo dato (10 s: la prima iterazione
 * del loop di un server locale si conta in millisecondi). Il tempo è CONTATO di proposito: un
 * `read` blocking su un server che non parte non è un test che fallisce, è una suite appesa. */
static int wait_log_line(int fd, const char *needle, char *buf, size_t cap) {
    size_t have = 0;
    int tries;

    buf[0] = '\0';
    for (tries = 0; tries < 400; tries++) {
        struct timespec ts = {0, 25 * 1000 * 1000};
        ssize_t n;
        if (have + 1 < cap) {
            n = read(fd, buf + have, cap - 1 - have);
            if (n > 0) {
                have += (size_t)n;
                buf[have] = '\0';
            } else if (n == 0) {
                return 0; /* il figlio è morto: EOF, non «ancora niente» */
            }
        }
        if (strstr(buf, needle)) return 1;
        nanosleep(&ts, NULL);
    }
    return 0;
}

/* Una chiamata in-processo a `fmlx_serve` che NON può appendersi: gli si passa `stop` GIÀ alzato,
 * così se il bind riuscisse il loop esce al primo giro. Il codice che si asserisce nasce tutto
 * dalla bind/listen, che sta PRIMA del loop: questa scelta non copre nulla di ciò che stiamo
 * provando, toglie solo il rischio di un test che non finisce mai. */
static FmlxServeSt serve_once_now(FmlxServeOpts *o, char **log_out, size_t *log_len) {
    volatile sig_atomic_t stop = 1;
    FmlxServeSt st;
    FILE *lf;

    *log_out = NULL;
    *log_len = 0;
    lf = open_memstream(log_out, log_len);
    st = fmlx_serve(o, NULL, lf, &stop, NULL);
    fclose(lf); /* è QUI che `*log_out` prende il contenuto: prima è indefinito */
    return st;
}

/* La stessa porta di un ascoltatore vivo: è il caso più frequente di server che non parte (un
 * `fastllm serve` già aperto in un altro terminale) e fino a ieri usciva come `BAD_ARG`, che
 * manda l'operatore a rileggere i flag quando i flag erano giusti. */
static void case_addr_in_use(void) {
    int port = 8700 + (int)(getpid() % 300), logfd = -1, w = 0;
    size_t lgn = 0;
    char line[1024], *lg;
    FmlxServeOpts o = opts_default();
    FmlxServeSt st;
    pid_t pid;

    pid = serve_child_logged(port, 0, &logfd);
    if (pid < 0) return;
    if (!wait_log_line(logfd, "in ascolto", line, sizeof line)) {
        CK(0, "il figlio non dichiara di ascoltare su %d (log: '%s')", port, line);
        close(logfd);
        kill(pid, SIGKILL);
        waitpid(pid, &w, 0);
        return;
    }
    close(logfd);

    o.host = "127.0.0.1";
    o.port = port;
    st = serve_once_now(&o, &lg, &lgn);
    CK(st == FMLX_SERVE_ADDR_IN_USE, "porta occupata: atteso ADDR_IN_USE, arrivato '%s'",
       fmlx_serve_status_str(st));
    /* Qui la FRASE è il prodotto, non un ornamento: la distinzione fra i tre codici esiste perché
     * l'operatore legga un da-farsi diverso («cambia porta» non è «dammi i privilegi»). È l'unico
     * punto del file dove si asserisce su una stringa umana, ed è per questo che si nomina il pezzo
     * che cambia comportamento e non la punteggiatura. */
    CK(HAS(lg, "già presa"), "il log non dice il da-farsi: %s", lg);
    CK(HAS(lg, "Address") || HAS(lg, "EADDRINUSE"),
       "il log non cita il diniego del SO: %s", lg);
    free(lg);
    kill(pid, SIGKILL);
    waitpid(pid, &w, 0);
}

/* Porta 1: sotto i 1024 non si ascolta senza privilegi. Il numero è MISURATO su questa macchina
 * (`bind(:80)` → `errno 13 Permission denied`), non indovinato. Se chi esegue è root il caso NON È
 * PROVABILE, e lo si DICE: un'asserzione saltata in silenzio è peggio di una riga in più. */
static void case_no_perm(void) {
    size_t lgn = 0;
    char *lg;
    FmlxServeOpts o = opts_default();
    FmlxServeSt st;

    if (geteuid() == 0) {
        printf("  nota: stiamo girando come root: il diniego EACCES non è provabile qui\n");
        return;
    }
    o.host = "127.0.0.1";
    o.port = 1;
    st = serve_once_now(&o, &lg, &lgn);
    if (st == FMLX_SERVE_ADDR_IN_USE) {
        printf("  nota: sulla porta 1 c'è già qualcuno: EACCES non è distinguibile (log: %s)\n",
               lg ? lg : "");
        free(lg);
        return;
    }
    CK(st == FMLX_SERVE_NO_PERM, "porta 1 da non-root: atteso NO_PERM, arrivato '%s'",
       fmlx_serve_status_str(st));
    CK(HAS(lg, "1024"), "il log non dice il limite delle porte privilegiate: %s", lg);
    free(lg);
}

/* Il segnale che ferma il loop. Due asserzioni DISTINTE, e l'ordine conta: se il processo è morto
 * DI segnale, `WEXITSTATUS` non vuol dire niente e il solo controllarlo darebbe un verde bugiardo
 * (un handler assente lascia morire il figlio con il default, e il default non è «esce 0»). */
static void case_signal_exit(int sig, const char *name, int port) {
    static const char req[] =
        "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    char line[1024], got[2048];
    int logfd = -1, w = 0, fd, tries = 0, reaped = 0;
    pid_t pid;

    pid = serve_child_logged(port, 0, &logfd);
    if (pid < 0) return;
    if (!wait_log_line(logfd, "in ascolto", line, sizeof line)) {
        CK(0, "%s: il figlio non ascolta su %d (log: '%s')", name, port, line);
        close(logfd);
        kill(pid, SIGKILL);
        waitpid(pid, &w, 0);
        return;
    }
    close(logfd);

    /* Una richiesta VERA e servita prima di sparare il segnale: «ho chiuso perché me l'hai
     * chiesto» ha un senso solo se il server era vivo e rispondeva. Questo consuma una richiesta,
     * e per questo qui `max_conns` è 0 — con un contingente il figlio sarebbe uscito da sé e
     * l'exit code 0 non sarebbe più una prova del segnale. */
    fd = connect_child(port);
    if (fd < 0) {
        CK(0, "%s: il server non accetta su 127.0.0.1:%d", name, port);
        kill(pid, SIGKILL);
        waitpid(pid, &w, 0);
        return;
    }
    if (write(fd, req, sizeof req - 1) != (ssize_t)(sizeof req - 1)) CK(0, "%s: invio", name);
    read_all(fd, got, sizeof got);
    close(fd);
    CK(strncmp(got, "HTTP/1.1 200", 12) == 0, "%s: prima del segnale non rispondeva: %s", name,
       got);

    if (kill(pid, sig) != 0) CK(0, "%s: kill: %s", name, strerror(errno));
    while (tries++ < 400) {
        struct timespec ts = {0, 25 * 1000 * 1000};
        int r = waitpid(pid, &w, WNOHANG);
        if (r == pid) {
            reaped = 1;
            break;
        }
        if (r < 0 && errno != EINTR) {
            CK(0, "%s: waitpid: %s", name, strerror(errno));
            return;
        }
        nanosleep(&ts, NULL);
    }
    if (!reaped) {
        CK(0, "%s: il loop non si è fermato entro 10 s: `accept` non torna EINTR?", name);
        kill(pid, SIGKILL);
        waitpid(pid, &w, 0);
        return;
    }
    CK(!WIFSIGNALED(w), "%s: il processo è stato UCISO dal segnale %d: l'handler non gira", name,
       WIFSIGNALED(w) ? WTERMSIG(w) : 0);
    CK(WIFEXITED(w) && WEXITSTATUS(w) == 0,
       "%s: atteso lo stato 0 (il loop ha finito di pulirsi), arrivato %d", name, WEXITSTATUS(w));
}

static void case_stop_signal(void) {
    int base = (int)(getpid() % 200);
    case_signal_exit(SIGTERM, "SIGTERM", 8900 + base);
    case_signal_exit(SIGINT, "SIGINT", 9200 + base);
}

/* ============================================================================ */

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("uso: %s <directory-di-lavoro>\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0); /* un crash deve dire QUALE sezione, non niente */
    snprintf(ROOT, sizeof ROOT, "%s", argv[1]);
    unlock_fixture(); /* un giro morto a directory murata non è wipeabile: si riapre prima di pulire */
    wipe(ROOT);
    build_cache();

    case_models();
    case_health();
    case_health_unavailable();
    case_catalog_too_many();
    case_auth();
    case_engine_refusals();
    case_validation();
    case_framing();
    case_env();
    case_round_trip();
    case_continue_100();
    case_addr_in_use();
    case_no_perm();
    case_stop_signal();

    unlock_fixture();
    wipe(ROOT);
    printf("test_serve: %d verifiche, %d fallite\n", checks, failures);
    return failures ? 1 : 0;
}
