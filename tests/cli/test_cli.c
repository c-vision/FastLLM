/* test_cli.c — l'argv di `fastmlx` e il dispatch: due casi per regola, nessuno decorativo.
 *
 * Il `pull` VERO è provato in test_pull.c (lì ci sono i blob, gli symlink, gl'inode). Qui si prova
 * ciò che esiste solo in questa lane:
 *  - una bandiera sconosciuta NON è ignorata: è un errore che NOMINA la bandiera;
 *  - un numero non è un numero: `--max-bytes abc` e `--max-bytes 0` sono errori, non "nessun limite"
 *    (è il bug classico di `strtol`, che restituisce 0 anche quando non ha letto nulla);
 *  - i verbi che chiedono il motore rispondono NOT_WIRED con il PERCHÉ, e il perché è diverso da
 *    verbo a verbo — se fosse uno solo, il messaggio sarebbe una scusa;
 *  - l'argomento posizionale in più è un errore: `pull a b` non deve scaricare `a` e tacere su `b`.
 *
 * L'uscita catturata con `open_memstream` serve a una cosa sola: provare SU COSA si scrive, non
 * solo con che codice si torna. Un messaggio sul canale sbagliato (l'errore su stdout) rende il
 * comando illeggibile in un pipe, e nessun codice d'uscita lo dice.
 */
#include "fmlx/fmlx_cli.h"

#include <ftw.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int checks, failures;

#define CK(cond, ...)                                                                                              \
    do {                                                                                                           \
        checks++;                                                                                                  \
        if (!(cond)) {                                                                                             \
            failures++;                                                                                            \
            printf("  FALLIMENTO %s:%d: ", __FILE__, __LINE__);                                                    \
            printf(__VA_ARGS__);                                                                                   \
            printf("\n");                                                                                          \
        }                                                                                                          \
    } while (0)

#define CK_ST(call, want)                                                                                          \
    do {                                                                                                           \
        FmlxCliStatus got_ = (call);                                                                               \
        checks++;                                                                                                  \
        if (got_ != (want)) {                                                                                      \
            failures++;                                                                                            \
            printf("  FALLIMENTO %s:%d: %s -> %s, atteso %s (%s)\n", __FILE__, __LINE__, #call,                   \
                   fmlx_cli_status_str(got_), fmlx_cli_status_str(want), msg);                                     \
        }                                                                                                          \
    } while (0)

/* Il numero degli argomenti lo conta il compilatore. Un `argc` scritto a mano è il bug che questo
 * file ha INCONTRATO davvero: `{"fastmlx","pull"}` con argc=3 non fallisce in modo visibile,
 * legge la memoria accanto e racconta una storia falsa sul parse. */
#define CK_PARSE(want, ...)                                                                        \
    do {                                                                                           \
        char *av_[] = {__VA_ARGS__};                                                               \
        CK_ST(fmlx_cli_parse((int)(sizeof av_ / sizeof av_[0]), av_, &a, msg, sizeof msg), (want)); \
    } while (0)

/* NULL = il buffer non è stato nemmeno allocato: `strstr` non lo accetta, e un test che crasha su
 * una `open_memstream` fallita racconterebbe un fallimento di sintassi dove c'è una risorse. */
#define HAS(hay, needle) ((hay) && strstr((hay), (needle)) != NULL)

/* Un transport che non sa fare niente: serve a provare il CAMMINO d'errore della CLI senza
 * ricostruire il finto server di test_pull.c (che è un altro oggetto, e già provato lì). */
static FmlxHfStatus dead_meta(void *c, const char *u, const char *t, FmlxHfMeta *o) {
    (void)c; (void)u; (void)t; (void)o;
    return FMLX_HF_ERR_IO;
}
static FmlxHfStatus dead_get(void *c, const char *u, const char *t, int64_t f, int64_t l, int fd,
                             int64_t *w) {
    (void)c; (void)u; (void)t; (void)f; (void)l; (void)fd; (void)w;
    return FMLX_HF_ERR_IO;
}
static FmlxHfStatus dead_buf(void *c, const char *u, const char *t, char *b, size_t cap,
                             size_t *w) {
    (void)c; (void)u; (void)t; (void)b; (void)cap; (void)w;
    return FMLX_HF_ERR_IO;
}
static const FmlxHfTransport DEAD = {NULL, dead_meta, dead_get, NULL, dead_buf};

/* ---- parse ---- */

static void case_pull_minimo(void) {
    FmlxCliArgs a;
    char msg[256];

    CK_PARSE(FMLX_CLI_OK, "fastmlx", "pull", "org/repo");
    CK(strcmp(a.verb, "pull") == 0, "verb=%s", a.verb);
    CK(strcmp(a.repo, "org/repo") == 0, "repo=%s", a.repo);
    CK(a.revision && strcmp(a.revision, "main") == 0, "revision=%s", a.revision ? a.revision : "(null)");
    CK(a.pull.dest == NULL, "--dest non dato deve restare NULL, non una stringa vuota");
    CK(a.want_help == 0, "want_help=%d", a.want_help);
}

static void case_pull_ogni_bandiera(void) {
    FmlxCliArgs a;
    char msg[256];

    CK_PARSE(FMLX_CLI_OK, "fastmlx", "pull", "mlx-community/R repo", "--revision", "refs/pr/7",
             "--dest", "/tmp/Models", "--cache=/tmp/cache", "--endpoint", "https://x.example",
             "--max-bytes=1000", "--stall-timeout", "5", "--stall-retries", "1", "--offline",
             "--repair", "--quiet");
    CK(strcmp(a.repo, "mlx-community/R repo") == 0, "repo=%s", a.repo);
    CK(strcmp(a.revision, "refs/pr/7") == 0, "revision=%s", a.revision);
    CK(strcmp(a.pull.dest, "/tmp/Models") == 0, "dest=%s", a.pull.dest);
    CK(strcmp(a.pull.fetch.cache_root, "/tmp/cache") == 0, "cache=%s", a.pull.fetch.cache_root);
    CK(strcmp(a.pull.fetch.endpoint, "https://x.example") == 0, "endpoint=%s",
       a.pull.fetch.endpoint);
    CK(a.pull.fetch.max_bytes == 1000, "max_bytes=%" PRId64, a.pull.fetch.max_bytes);
    CK(a.pull.fetch.stall_timeout_s == 5 && a.pull.fetch.stall_retries == 1, "stall %d/%d",
       a.pull.fetch.stall_timeout_s, a.pull.fetch.stall_retries);
    CK(a.pull.fetch.offline == 1 && a.pull.fetch.repair == 1 && a.quiet == 1, "flag booleani persi");
    CK(a.pull.fetch.token == NULL, "il token non puo' arrivare dall'argv");
}

/* La forma `--bandiera=valore` con un nome più lungo del buffer di copia: è il punto in cui si
 * troncherebbe in silenzio, e il bordo va provato (sopra la soglia: dentro, la bandiera viene
 * letta per intero e riconosciuta come sconosciuta). */
static void case_bandiera_lunga(void) {
    FmlxCliArgs a;
    char msg[256];
    char evil[160];

    memset(evil, '-', sizeof evil);
    evil[sizeof evil - 1] = '\0';
    evil[158] = '='; /* un nome che NON sta nel buffer di copia: deve essere un errore, non un troncamento */
    CK_PARSE(FMLX_CLI_USAGE, "fastmlx", "pull", evil);
    CK(strstr(msg, "lunga") != NULL, "msg=%s", msg);

    /* Stessa forma, nome corto: la bandiera sconosciuta è nominata per nome, non per posizione. */
    CK_PARSE(FMLX_CLI_USAGE, "fastmlx", "pull", "o/r", "--niente=1");
    CK(strstr(msg, "--niente") != NULL, "msg=%s", msg);
}

static void case_bandiere_rifiutate(void) {
    FmlxCliArgs a;
    char msg[256];

    CK_PARSE(FMLX_CLI_USAGE, "fastmlx", "pull", "o/r", "--force");
    CK(strstr(msg, "--force") != NULL, "la bandiera non è nominata: %s", msg);

    CK_PARSE(FMLX_CLI_USAGE, "fastmlx", "pull", "o/r", "--max-bytes", "abc");
    CK(strstr(msg, "max-bytes") != NULL, "msg=%s", msg);

    /* 0 non è "nessun limite": è un valore, e `--max-bytes 0` vorrebbe dire "zero byte". */
    CK_PARSE(FMLX_CLI_USAGE, "fastmlx", "pull", "o/r", "--max-bytes", "0");

    /* 0 di --stall-retries INVECE è un valore legittimo: "una prova sola". */
    CK_PARSE(FMLX_CLI_OK, "fastmlx", "pull", "o/r", "--stall-retries", "0");
    CK(a.pull.fetch.stall_retries == 0, "stall_retries=%d", a.pull.fetch.stall_retries);

    CK_PARSE(FMLX_CLI_USAGE, "fastmlx", "pull", "o/r", "--dest"); /* valore mancante in coda */
    CK(strstr(msg, "valore") != NULL, "msg=%s", msg);

    CK_PARSE(FMLX_CLI_USAGE, "fastmlx", "pull", "o/r", "--dest", ""); /* vuoto ≠ assente */

    CK_PARSE(FMLX_CLI_USAGE, "fastmlx", "pull", "o/r", "extra");
    CK(strstr(msg, "posizionali") != NULL, "msg=%s", msg);

    CK_PARSE(FMLX_CLI_USAGE, "fastmlx", "pull");
    CK(strstr(msg, "repo") != NULL, "msg=%s", msg);

    CK_PARSE(FMLX_CLI_USAGE, "fastmlx", "fetch", "o/r");
    CK(strstr(msg, "comando sconosciuto") != NULL, "msg=%s", msg);

    CK_PARSE(FMLX_CLI_USAGE, "fastmlx");

    /* l'aiuto non è un errore, e NON serve un comando */
    CK_PARSE(FMLX_CLI_OK, "fastmlx", "--help");
    CK(a.want_help == 1, "want_help=%d", a.want_help);

    /* un parse fallito azzera tutto: un verbo residuo verrebbe eseguito */
    CK_PARSE(FMLX_CLI_USAGE, "fastmlx", "pull", "o/r", "--mass-bytes", "1");
    CK(a.verb[0] == '\0' && a.repo[0] == '\0', "args non azzerati: %s / %s", a.verb, a.repo);
}

/* ---- dispatch ---- */

/* Ogni verbo in attesa del motore: stessa classe di esito, messaggi DIVERSI. Se il messaggio non
 * cambiasse al cambiare del verbo, l'utente non potrebbe sapere cosa manca — e la lista di ciò che
 * manca è il lavoro del piano.
 *
 * `models`/`list` NON sono in questa lista da quando esiste `fmlx_models_exec`: sono verbi che
 * girano, e un test che li lasciasse qui passerebbe anche se il dispatch fosse rotto (perché
 * NOT_WIRED sarebbe comunque il codice "giusto" per la lista sbagliata).
 * `serve` è uscito dalla lista per lo stesso motivo, ma con un sintomo diverso: ora è cablato, e
 * chiamarlo qui NON darebbe NOT_WIRED — si siederebbe su `accept` in attesa di un client che non
 * arriva, che è il modo in cui un test diventa un blocco da uccidere a mano. `doctor` è uscito
 * infine con un TERZO sintomo: legge il disco e scrive su `out`, e tenerlo qui avrebbe chiesto
 * NOT_WIRED su una cache che sa aprire — verde per il motivo sbagliato. Le sue prove stanno in
 * `test_doctor.c`. */
static void case_motori_non_collegati(void) {
    static const char *const verbs[] = {"run", "chat", "ps", "cp", "rm"};
    size_t i;
    char first[FMLX_HF_PATH_MAX];
    int distinct = 0;

    first[0] = '\0';
    for (i = 0; i < sizeof verbs / sizeof verbs[0]; i++) {
        FmlxCliArgs a;
        char msg[256];
        char *outb = NULL, *errb = NULL;
        size_t en = 0, on = 0;
        FILE *out = open_memstream(&outb, &on), *err = open_memstream(&errb, &en);

        CK_PARSE(FMLX_CLI_OK, "fastmlx", (char *)verbs[i], "qualcosa");
        CK_ST(fmlx_cli_exec_with(&a, &DEAD, out, err), FMLX_CLI_NOT_WIRED);
        fclose(out);
        fclose(err); /* il buffer di open_memstream è valido solo dopo la chiusura */
        CK(strstr(errb, verbs[i]) != NULL, "%s: il messaggio non nomina il verbo: %s", verbs[i], errb);
        CK(strstr(errb, "non") != NULL || strstr(errb, "manca") != NULL,
           "%s: il messaggio non dice che il pezzo manca: %s", verbs[i], errb);
        /* Niente stdout: un comando che non può girare non emette mezzo risultato. */
        CK(on == 0, "%s: ha scritto anche su stdout (%zu byte)", verbs[i], on);
        if (i == 0) snprintf(first, sizeof first, "%s", errb);
        else if (strcmp(first, errb) != 0) distinct++;
        free(outb);
        free(errb);
    }
    /* Cinque verbi, almeno TRE messaggi diversi: è la prova che il testo dipende dal verbo e non è
     * una stringa sola ripetuta. */
    CK(distinct >= 3, "messaggi distinti=%d su %d verbi", distinct,
       (int)(sizeof verbs / sizeof verbs[0]));
}

/* ---- serve ---- */

/* `serve` è cablato: quello che si prova QUI è l'argv e la precedenza delle opzioni. Il loop TCP
 * vero è provato in `test_serve.c` (che ha un socket suo, su 127.0.0.1, con `max_conns=1`):
 * ripetere quel giro in questo file vorrebbe dire incollare un test nel posto sbagliato — e un
 * `serve` chiamato qui senza quel limite si sederebbe su `accept` per sempre. */
static void case_serve_bandiere(void) {
    FmlxCliArgs a;
    char msg[256];

    CK_PARSE(FMLX_CLI_OK, "fastmlx", "serve", "--host", "0.0.0.0", "--port", "8137", "--max-body",
             "4096", "--once");
    CK(!strcmp(a.serve.host, "0.0.0.0"), "--host non arrivata al server: %s",
       a.serve.host ? a.serve.host : "(null)");
    CK(a.serve.port == 8137, "--port=%d", a.serve.port);
    CK(a.serve.max_body == 4096, "--max-body=%zu", a.serve.max_body);
    /* `--once` È `max_conns = 1`: una bandiera che non arriva al loop è una bandiera che mente, e
     * il sintomo è un `serve` che non finisce mai in una shell di prova. */
    CK(a.serve.max_conns == 1, "--once -> max_conns=%u", a.serve.max_conns);

    CK_PARSE(FMLX_CLI_OK, "fastmlx", "serve");
    CK(a.serve.port == 0 && a.serve.host == NULL && a.serve.max_conns == 0,
       "senza bandiere il server usa i suoi default, non zero a caso");

    /* La stessa severità della bandiera e dell'ambiente: `--port abc` non è "porta non impostata",
     * è un typo, e accettarlo vorrebbe dire ascoltare sulla porta sbagliata in silenzio. */
    CK_PARSE(FMLX_CLI_USAGE, "fastmlx", "serve", "--port", "abc");
    CK_PARSE(FMLX_CLI_USAGE, "fastmlx", "serve", "--port", "0");
    CK_PARSE(FMLX_CLI_USAGE, "fastmlx", "serve", "--port", "65536");
    CK_PARSE(FMLX_CLI_USAGE, "fastmlx", "serve", "--max-body", "0");
    CK_PARSE(FMLX_CLI_USAGE, "fastmlx", "serve", "--dest", "/tmp/x");
    CK_PARSE(FMLX_CLI_USAGE, "fastmlx", "serve", "org/modello");
    /* E il contrario: una bandiera DEL SERVER data al verbo sbagliato. `run` sarebbe stato un
     * esempio peggiore di `models`, perché `missing_piece("run")` nomina `run` alla lettera e
     * l'asserzione sul messaggio passerebbe anche con argv accettato. */
    CK_PARSE(FMLX_CLI_USAGE, "fastmlx", "models", "--once");
    CK(HAS(msg, "--once"), "il rifiuto deve nominare la bandiera: %s", msg);
    CK(HAS(msg, "models"), "e il verbo: %s", msg);

    /* Il CONFINE della regola: `run` e `chat` non hanno ancora UNA superficie di opzioni, e
     * «--port non è un'opzione di run» sarebbe un'affermazione sul futuro di un verbo che non è
     * scritto. Il parse deve passare e lasciare al dispatch l'unica cosa vera oggi: manca il
     * motore. (`models` invece le opzioni le HA, ed è per lui che la regola esiste.) */
    CK_PARSE(FMLX_CLI_OK, "fastmlx", "run", "--port", "8000");
    CK_PARSE(FMLX_CLI_OK, "fastmlx", "chat", "--once");

    /* Una booleana NON prende valore, e la forma `--flag=x` ESISTE nel parser: `--once=0` sarebbe
     * letto come "dato", con lo STESSO effetto di `--once=1`. Due semantiche con la stessa grafia
     * è il tipo di banda che un utente mette in uno script e non scopre mai. */
    CK_PARSE(FMLX_CLI_USAGE, "fastmlx", "serve", "--once=0");
    CK_PARSE(FMLX_CLI_USAGE, "fastmlx", "pull", "o/r", "--quiet=1");
    CK_PARSE(FMLX_CLI_USAGE, "fastmlx", "pull", "o/r", "--offline=");
    CK_PARSE(FMLX_CLI_USAGE, "fastmlx", "pull", "o/r", "--repair=no");
    CK(HAS(msg, "valore"), "il rifiuto deve dire COSA non va: %s", msg);
    /* Il contrario, per non avere appena spostato il bug: la stessa bandiera SENZA `=` deve
     * passare, o la regola avrebbe trasformato un caso legittimo in un errore. */
    CK_PARSE(FMLX_CLI_OK, "fastmlx", "pull", "o/r", "--offline", "--repair", "--quiet");
    CK(a.quiet && a.pull.fetch.offline && a.pull.fetch.repair, "booleani senza = persi per strada");

    /* Il `--cache` vale per TUTTI i verbi che guardano il disco: darlo a `serve` e vedere il
     * server leggere HF_HOME sarebbe il contrario della regola. */
    CK_PARSE(FMLX_CLI_OK, "fastmlx", "serve", "--cache", "/tmp/due");
    CK(a.serve.cache_root && !strcmp(a.serve.cache_root, "/tmp/due"),
       "serve: --cache non arrivato al server");

    /* L'unica uscita di `fmlx_serve_exec` provabile SENZA toccare una porta: un ambiente che non
     * risolve. Dice che la CLI passa davvero le opzioni al server e non stampa un esito suo. */
    {
        FILE *out, *err;
        char *ob = NULL, *eb = NULL;
        size_t on = 0, en = 0;
        FmlxCliArgs b;

        memset(&b, 0, sizeof b);
        snprintf(b.verb, sizeof b.verb, "serve");
        b.serve.port = 0;
        setenv("FASTMLX_PORT", "80x", 1);
        out = open_memstream(&ob, &on);
        err = open_memstream(&eb, &en);
        CK_ST(fmlx_serve_exec(&b, out, err), FMLX_CLI_USAGE);
        fclose(out);
        fclose(err);
        if (eb) eb[en] = '\0';
        CK(HAS(eb, "FASTMLX_PORT"), "l'errore deve nominare la variabile: %s", eb ? eb : "(vuoto)");
        CK(on == 0, "un `serve` che non parte non scrive su stdout (%zu byte)", on);
        unsetenv("FASTMLX_PORT");
        free(ob);
        free(eb);
    }
}

/* ---- models ---- */

/* Un cache finto costruito su disco: `models` legge il disco, quindi il disco se lo fa da sé. Qui
 * non c'è un transport da falsificare — e infatti il `DEAD` di questo file NON viene usato: provare
 * che non c'è rete con un transport finto vorrebbe dire avere una rete da fingere. */
static int remove_one(const char *p, const struct stat *st, int flags, struct FTW *ftwbuf) {
    (void)st;
    (void)flags;
    (void)ftwbuf;
    return remove(p);
}

/* Il cache finto si ripete da sé se resta da un giro precedente (un repo in più = un conteggio in
 * più). FTW_PHYS: senza, un symlink verrebbe seguito e il suo bersaglio cancellato. */
static void wipe(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return;
    if (nftw(path, remove_one, 6, FTW_DEPTH | FTW_PHYS) != 0)
        printf("  ATTENZIONE: wipe di %s incompleto\n", path);
}

static char WORK[FMLX_HF_PATH_MAX];

/* Buffer statico NON const: gli argv di CK_PARSE sono `char *[]` (così sono i argv di un processo),
 * e un `const char *` che ci entra sarebbe un cast a nascondere — il disco, qui, lo scriviamo noi. */
static char *at(const char *rel) {
    static char buf[FMLX_HF_PATH_MAX * 2];
    snprintf(buf, sizeof buf, "%s/%s", WORK, rel);
    return buf;
}

static void dir(const char *rel) {
    if (fmlx_hf_mkdir_p(at(rel)) != 0) printf("  FIXTURE: mkdir %s\n", rel);
}

static void put_bytes(const char *rel, size_t n) {
    const char *p = at(rel);
    FILE *f;
    size_t i;
    if (fmlx_hf_ensure_parent(p) != 0) {
        printf("  FIXTURE: genitore di %s\n", rel);
        return;
    }
    f = fopen(p, "wb");
    if (!f) {
        printf("  FIXTURE: open %s\n", rel);
        return;
    }
    for (i = 0; i < n; i++) fputc('x', f);
    fclose(f);
}

/* Costruisce `<WORK>/cache` con i quattro casi che la tabella deve distinguere. Le dimensioni sono
 * CHOSEN per cadere su due unità diverse (kB e byte) e per avere un totale che si può controllare a
 * mente: 1000 + 500 + 3 = 1503 byte. `zz/uno` NON è un quarto caso della tabella: è il caso che il
 * filtro lo scopre — un nome che esiste in DUE org, dove `models uno` deve rispondere due righe e
 * non indovinarne una. */
static void build_cache_fixture(void) {
    dir("cache/models--mm--uno/refs");
    put_bytes("cache/models--mm--uno/blobs/b1", 1000);
    put_bytes("cache/models--mm--uno/blobs/b2", 500);
    dir("cache/models--mm--ref/refs");
    put_bytes("cache/models--mm--ref/refs/main", 40);
    dir("cache/datasets--mm--data");
    put_bytes("cache/datasets--mm--data/blobs/z", 3);
    dir("cache/models--zz--uno/blobs");
    put_bytes("cache/models--zz--uno/blobs/b1", 7);
    put_bytes("cache/version.txt", 2); /* un non-repo: si CONTA, non si tace */
}

/* Una riga della tabella, isolata: `strstr` su un frammento non prova che la colonna esista, prova
 * che da qualche parte quelle cifre compaiono. Si divide per righe e si cerca il NOME, poi si
 * controllano le colonne della STESSA riga. */
static const char *row_with(const char *text, const char *id, char *line, size_t n) {
    const char *p = text;
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len && len < n) {
            memcpy(line, p, len);
            line[len] = '\0';
            if (strncmp(line, id, strlen(id)) == 0) return line;
        }
        p = nl ? nl + 1 : NULL;
    }
    line[0] = '\0';
    return NULL;
}

static void case_models_tabella(void) {
    FmlxCliArgs a;
    char msg[256], *outb = NULL, *errb = NULL, line[1024];
    size_t on = 0, en = 0;
    FILE *out = open_memstream(&outb, &on), *err = open_memstream(&errb, &en);

    build_cache_fixture();
    CK_PARSE(FMLX_CLI_OK, "fastmlx", "models", "--cache", "x");
    CK(a.pull.fetch.cache_root != NULL, "--cache non arrivata a models");
    /* L'argomento posizionale su un verbo che non lo usa è un errore, non una cosa ignorata. Si
     * asserisce ANCHE il nome del verbo: il primo giro sul cache reale ha stampato
     * `fastmlx: : non prende argomenti` (il messaggio leggeva `out->verb` DOPO l'azzzeramento di
     * `fail`) e il solo frammento "non prende argomenti" lo stava a bere. `models` l'argomento lo
     * USA (è il filtro), e il verbo che oggi non lo prende è `serve`. */
    CK_PARSE(FMLX_CLI_USAGE, "fastmlx", "serve", "mm/uno");
    CK(strstr(msg, "non prende argomenti") != NULL, "msg=%s", msg);
    CK(strncmp(msg, "serve:", 6) == 0, "il verbo non è nominato per esteso: '%s'", msg);
    /* Due filtri non vogliono dire niente: il secondo è un errore, non un AND implicito. */
    CK_PARSE(FMLX_CLI_USAGE, "fastmlx", "models", "mm/uno", "mm/ue");
    CK(strstr(msg, "troppi argomenti posizionali") != NULL, "due filtri accettati: '%s'", msg);
    /* Il filtro arriva DAVVERO all'exec: `a->repo` è lo stesso campo che legge `pull`, e un parse
     * che lo riempie per un verbo e lo dimentica per l'altro si vede solo qui. */
    CK_PARSE(FMLX_CLI_OK, "fastmlx", "models", "mm/uno");
    CK(strcmp(a.repo, "mm/uno") == 0, "il filtro non arriva all'exec: '%s'", a.repo);
    CK_PARSE(FMLX_CLI_OK, "fastmlx", "list", "uno");
    CK(strcmp(a.repo, "uno") == 0, "l'alias perde il filtro: '%s'", a.repo);

    CK_PARSE(FMLX_CLI_OK, "fastmlx", "models", "--cache", at("cache"));
    CK_ST(fmlx_cli_exec_with(&a, &DEAD, out, err), FMLX_CLI_OK);
    fclose(out);
    fclose(err);
    if (outb) outb[on] = '\0';
    if (errb) errb[en] = '\0';
    CK(en == 0, "l'elenco ha scritto sull'errore: %s", errb ? errb : "(null)");
    CK(outb && strstr(outb, "NOME") != NULL, "nessuna intestazione: %s", outb ? outb : "(vuoto)");

    /* `mm/uno`: 1500 byte = "1.5 kB" in DECIMALE, 2 blob, 1 ref. */
    CK(outb && row_with(outb, "mm/uno", line, sizeof line) != NULL, "manca la riga di mm/uno:\n%s",
       outb ? outb : "(vuoto)");
    if (outb) {
        row_with(outb, "mm/uno", line, sizeof line);
        CK(strstr(line, "model") != NULL, "colonna TIPO: %s", line);
        CK(strstr(line, "1.5 kB") != NULL, "colonna DIM (1500 byte attesi): %s", line);
        CK(strstr(line, " 2 ") != NULL, "colonna BLOB: %s", line);
        CK(strstr(line, "[solo ref]") == NULL, "un repo CON i blob marcato [solo ref]: %s", line);
    }
    /* `mm/ref`: nessun byte, ma c'è — marcato, con una legenda che dice cosa vuol dire. */
    CK(outb && row_with(outb, "mm/ref", line, sizeof line) != NULL, "manca la riga di mm/ref:\n%s",
       outb ? outb : "(vuoto)");
    if (outb) {
        row_with(outb, "mm/ref", line, sizeof line);
        CK(strstr(line, "[solo ref]") != NULL, "manca il marker [solo ref]: %s", line);
        CK(strstr(line, "0 byte") != NULL, "DIM di un repo senza blob: %s", line);
    }
    CK(outb && row_with(outb, "mm/data", line, sizeof line) != NULL, "manca il dataset:\n%s",
       outb ? outb : "(vuoto)");
    if (outb) {
        row_with(outb, "mm/data", line, sizeof line);
        CK(strstr(line, "dataset") != NULL, "TIPO del dataset: %s", line);
        CK(strstr(line, "3 byte") != NULL, "DIM del dataset: %s", line);
    }
    CK(outb && strstr(outb, "[solo ref] =") != NULL, "nessuna legenda del marker:\n%s",
       outb ? outb : "(vuoto)");
    CK(outb && strstr(outb, "4 repo") != NULL, "riga finale col conteggio:\n%s", outb ? outb : "(vuoto)");
    CK(outb && strstr(outb, "1 entry che non sono un repo") != NULL,
       "i non-repo non si nominano:\n%s", outb ? outb : "(vuoto)");
    /* Il totale è 1503: se la tabella raddoppiasse qualcosa (come farebbe contando gli symlink) qui
     * si vedrebbe. */
    CK(outb && strstr(outb, "1.5 kB in") != NULL, "totale nella riga finale:\n%s", outb ? outb : "(vuoto)");
    free(outb);
    free(errb);
}

/* `--quiet` è la forma per le shell: SOLO gli id, nell'ordine del catalogo, zero colonna. Se la
 * bandiera silenziasse solo il progresso senza togliere l'intestazione, `$(fastmlx models --quiet)`
 * passerebbe "NOME" a chi aspetta un repo. */
static void case_models_quiet(void) {
    FmlxCliArgs a;
    char msg[256], *outb = NULL, *errb = NULL;
    size_t on = 0, en = 0, lines = 0;
    FILE *out = open_memstream(&outb, &on), *err = open_memstream(&errb, &en);

    CK_PARSE(FMLX_CLI_OK, "fastmlx", "list", "--cache", at("cache"), "--quiet");
    CK_ST(fmlx_cli_exec_with(&a, &DEAD, out, err), FMLX_CLI_OK);
    fclose(out);
    fclose(err);
    if (outb) outb[on] = '\0';
    CK(outb && strstr(outb, "NOME") == NULL, "--quiet stampa ancora l'intestazione:\n%s",
       outb ? outb : "(vuoto)");
    CK(outb && strstr(outb, "kB") == NULL, "--quiet stampa le dimensioni:\n%s", outb ? outb : "(vuoto)");
    CK(outb && strstr(outb, "mm/uno\n") != NULL, "manca l'id: %s", outb ? outb : "(vuoto)");
    CK(strncmp(outb, "mm/data\n", 8) == 0, "la prima riga non e' un id: %s", outb ? outb : "(vuoto)");
    for (const char *p = outb; p && *p; p++)
        if (*p == '\n') lines++;
    CK(lines == 4, "righe=%zu, attese 4 (una per repo)\n%s", lines, outb ? outb : "(vuoto)");
    /* `list` alias di `models`: stessa tabella, stesse colonne. */
    CK(en == 0, "l'alias ha scritto sull'errore: %s", errb ? errb : "(null)");
    free(outb);
    free(errb);
}

/* ---- models <filtro> ---- */

typedef struct {
    char *out, *err;
    size_t on, en;
} Cap;

/* Un giro completo parse+exec con i buffer NUOVI: il secondo `open_memstream` sulla stessa variabile
 * lascia lì il buffer precedente, e un `strstr` che legge la risposta vecchia è il modo in cui un
 * test verde non dice più niente. `msg` esce compilato perché la forma del filtro (USAGE) si prova
 * sul messaggio, non sul codice. */
static FmlxCliStatus run_models(Cap *c, char *msg, int argc, char **av) {
    FmlxCliArgs a;
    FILE *o, *e;
    FmlxCliStatus s;

    c->out = NULL;
    c->err = NULL;
    c->on = c->en = 0;
    msg[0] = '\0';
    o = open_memstream(&c->out, &c->on);
    e = open_memstream(&c->err, &c->en);
    s = fmlx_cli_parse(argc, av, &a, msg, 256);
    if (s == FMLX_CLI_OK) s = fmlx_cli_exec_with(&a, &DEAD, o, e);
    fclose(o);
    fclose(e);
    if (c->out) c->out[c->on] = '\0';
    if (c->err) c->err[c->en] = '\0';
    return s;
}

static int count_rows(const Cap *c, const char *id) {
    const char *p = c->out;
    char line[1024];
    int k = 0;

    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len && len < sizeof line) {
            memcpy(line, p, len);
            line[len] = '\0';
            if (strncmp(line, id, strlen(id)) == 0) k++;
        }
        p = nl ? nl + 1 : NULL;
    }
    return k;
}

static void case_models_filtro(void) {
    Cap c;
    char msg[256];
    char *cv;
    char *ex[] = {"fastmlx", "models", "--cache", NULL, "mm/uno"};
    char *bare[] = {"fastmlx", "models", "--cache", NULL, "uno"};
    char *glob[] = {"fastmlx", "models", "--cache", NULL, "zz/*"};
    char *up[] = {"fastmlx", "models", "--cache", NULL, "MM/UNO"};
    char *none[] = {"fastmlx", "models", "--cache", NULL, "nope/nope"};
    char *star[] = {"fastmlx", "models", "--cache", NULL, "or*x"};
    char *q[] = {"fastmlx", "list", "--quiet", "--cache", NULL, "uno"};
    char *qnone[] = {"fastmlx", "list", "--quiet", "--cache", NULL, "nope/nope"};

    build_cache_fixture();
    /* `at()` dà UN buffer statico: `cv` si cattura DOPO l'ultima chiamata della fixture, o punterebbe
     * a "cache/models--mm--uno/refs" invece che a "cache" — e il test fallirebbe per una ragione che
     * non c'entra niente con il filtro. */
    cv = at("cache");
    ex[3] = cv;
    bare[3] = cv;
    glob[3] = cv;
    up[3] = cv;
    none[3] = cv;
    star[3] = cv;
    q[4] = cv;
    qnone[4] = cv;

    /* `org/nome`: UNA riga, e il CONTEGGIO della riga finale è dei match (1), non del cache (4) —
     * che è il numero che un occhio leggerebbe come "ho elencato tutto". Qui NON si asserisce nulla
     * sui BYTE: `mm/uno` è 1503 B su 1510 B di cache, e la formattazione li rende identici ("1.5 kB")
     * per entrambi, quindi un'asserzione su quella stringa sarebbe verde in ogni caso. Il totale che
     * rispetta il filtro è provato più sotto, con `zz/` + `*` (7 byte contro 1.5 kB): è lì che la
     * mutazione «il `total +=` ignora il filtro» diventa rossa, ed è rosso che l'ho verificato
     * lanciandola. */
    CK_ST(run_models(&c, msg, 5, ex), FMLX_CLI_OK);
    CK(count_rows(&c, "mm/uno") == 1, "filtro esatto con piu' righe:\n%s", c.out ? c.out : "(vuoto)");
    CK(count_rows(&c, "zz/uno") == 0, "il filtro esatto ha lasciato passare zz/uno:\n%s",
       c.out ? c.out : "(vuoto)");
    CK(HAS(c.out, "1 repo che matchano"), "la riga finale non dice che è un filtro:\n%s",
       c.out ? c.out : "(vuoto)");
    CK(HAS(c.out, "4 repo nel cache"), "il filtro non nomina il totale del cache:\n%s",
       c.out ? c.out : "(vuoto)");
    free(c.out);
    free(c.err);

    /* `nome` senza `/`: il caso per cui esiste la regola — due org con lo stesso nome. Il filtro
     * NON ne sceglie una: le mostra tutte e due, che è l'unica risposta onesta. */
    CK_ST(run_models(&c, msg, 5, bare), FMLX_CLI_OK);
    CK(count_rows(&c, "mm/uno") == 1 && count_rows(&c, "zz/uno") == 1,
       "il nome nudo non ha matchato entrambe le org:\n%s", c.out ? c.out : "(vuoto)");
    CK(HAS(c.out, "2 repo che matchano"), "conteggio dei match assente:\n%s", c.out ? c.out : "(vuoto)");
    free(c.out);
    free(c.err);

    CK_ST(run_models(&c, msg, 5, glob), FMLX_CLI_OK);
    CK(count_rows(&c, "zz/uno") == 1 && count_rows(&c, "mm/uno") == 0, "org/* non filtra per org:\n%s",
       c.out ? c.out : "(vuoto)");
    CK(HAS(c.out, "7 byte in"), "il totale e' di tutto il cache, non dell'org:\n%s",
       c.out ? c.out : "(vuoto)");
    free(c.out);
    free(c.err);

    CK_ST(run_models(&c, msg, 5, up), FMLX_CLI_OK);
    CK(count_rows(&c, "mm/uno") == 1, "MM/UNO non matcha mm/uno:\n%s", c.out ? c.out : "(vuoto)");
    free(c.out);
    free(c.err);

    /* Zero match: USCITA 1 e stdout VUOTO. Un elenco con l'intestazione e exit 0 sarebbe il modo in
     * cui "non c'è" somiglia a "ho finito di elencare". */
    CK_ST(run_models(&c, msg, 5, none), FMLX_CLI_FAILED);
    CK(c.on == 0, "zero match hanno stampato una tabella (%zu byte):\n%s", c.on, c.out ? c.out : "");
    CK(HAS(c.err, "nope/nope"), "l'errore non nomina il filtro: %s", c.err ? c.err : "(vuoto)");
    free(c.out);
    free(c.err);

    /* La forma illeggibile è un errore di chi digita (USAGE), non un "non c'è niente" (FAILED): i
     * due casi chiedono due correzioni diverse. */
    CK_ST(run_models(&c, msg, 5, star), FMLX_CLI_USAGE);
    CK(HAS(c.err, "non ammesso"), "la forma rifiutata non lo dice: %s", c.err ? c.err : "(vuoto)");
    CK(c.on == 0, "un filtro rifiutato ha stampato qualcosa:\n%s", c.out ? c.out : "(vuoto)");
    free(c.out);
    free(c.err);

    CK_ST(run_models(&c, msg, 6, q), FMLX_CLI_OK);
    CK(c.on && strcmp(c.out, "mm/uno\nzz/uno\n") == 0, "--quiet col filtro: '%s'",
       c.out ? c.out : "(vuoto)");
    free(c.out);
    free(c.err);

    /* LO STESSO zero match, ma dalla parte `--quiet`. Non è un doppione: il ramo silenzioso ha un
     * `return` suo, prima della tabella, e la mutazione che rende "nessun match" un successo ne
     * ha colpito uno solo dei due. Chi consuma `--quiet` è uno script, ed è esattamente chi legge
     * il codice d'uscita invece dello schermo. */
    CK_ST(run_models(&c, msg, 6, qnone), FMLX_CLI_FAILED);
    CK(c.on == 0, "--quiet con zero match ha stampato id: '%s'", c.out ? c.out : "(vuoto)");
    CK(HAS(c.err, "nope/nope"), "--quiet non nomina il filtro che non ha trovato niente: %s",
       c.err ? c.err : "(vuoto)");
    free(c.out);
    free(c.err);
}

/* La radice che non esiste NON è "cache vuoto": due messaggi diversi, perché sono due consigli
 * diversi. E l'elenco vuoto deve restare un SUCCESSO (exit 0): `fastmlx models` su un cache appena
 * creato non è un fallimento. */
static void case_models_senza_cache(void) {
    FmlxCliArgs a;
    char msg[256], *outb = NULL, *errb = NULL;
    size_t on = 0, en = 0;
    FILE *out = open_memstream(&outb, &on), *err = open_memstream(&errb, &en);

    CK_PARSE(FMLX_CLI_OK, "fastmlx", "models", "--cache", at("radice-che-non-c-e"));
    CK_ST(fmlx_cli_exec_with(&a, &DEAD, out, err), FMLX_CLI_FAILED);
    fclose(out);
    fclose(err);
    if (outb) outb[on] = '\0';
    if (errb) errb[en] = '\0';
    CK(on == 0, "su cache assente ha stampato un elenco: %s", outb ? outb : "(vuoto)");
    CK(errb && strstr(errb, "radice-che-non-c-e") != NULL, "non nomina il path: %s",
       errb ? errb : "(vuoto)");
    free(outb);
    free(errb);

    out = open_memstream(&outb, &on);
    err = open_memstream(&errb, &en);
    dir("cache-vuoto");
    CK_PARSE(FMLX_CLI_OK, "fastmlx", "models", "--cache", at("cache-vuoto"));
    CK_ST(fmlx_cli_exec_with(&a, &DEAD, out, err), FMLX_CLI_OK);
    fclose(out);
    fclose(err);
    if (outb) outb[on] = '\0';
    CK(outb && strstr(outb, "(nessun repo)") != NULL, "cache vuoto senza una parola: %s",
       outb ? outb : "(vuoto)");
    free(outb);
    free(errb);
}

/* L'errore va sul canale d'errore, il progresso su stdout: in un pipe `fastmlx pull … | wc -l`
 * deve contare i file, non le lamentele. */
static void case_canali(void) {
    FmlxCliArgs a;
    char msg[256], *outb = NULL, *errb = NULL;
    size_t on = 0, en = 0;
    FILE *out = open_memstream(&outb, &on), *err = open_memstream(&errb, &en);

    CK_PARSE(FMLX_CLI_OK, "fastmlx", "pull", "org/repo");
    CK_ST(fmlx_cli_exec_with(&a, &DEAD, out, err), FMLX_CLI_FAILED);
    fclose(out);
    fclose(err);
    if (outb) outb[on] = '\0';
    if (errb) errb[en] = '\0';
    CK(errb && strstr(errb, "fastmlx pull:") != NULL, "errore non sul canale d'errore: %s",
       errb ? errb : "(nessuno)");
    CK(outb && strstr(outb, "fastmlx pull:") == NULL, "l'errore è anche su stdout: %s",
       outb ? outb : "(nessuno)");
    free(outb);
    free(errb);
}

/* L'aiuto stampato, e il testo che NON promette ciò che non c'è: `run` e `pull` non possono
 * comparire nella stessa lista senza distinzione. */
static void case_aiuto(void) {
    FmlxCliArgs a;
    char msg[256], *b = NULL;
    size_t n = 0;
    FILE *out = open_memstream(&b, &n);

    CK_PARSE(FMLX_CLI_OK, "fastmlx", "-h");
    CK_ST(fmlx_cli_exec_with(&a, &DEAD, out, out), FMLX_CLI_OK);
    fclose(out);
    if (b) b[n] = '\0';
    CK(b && strstr(b, "pull <repo>") != NULL, "l'aiuto non nomina pull: %s", b ? b : "(vuoto)");
    CK(b && strstr(b, "models, list") != NULL, "l'aiuto non nomina models: %s", b ? b : "(vuoto)");
    CK(b && strstr(b, "in attesa del motore") != NULL, "l'aiuto non distingue i verbi non cablati");
    /* Un verbo che ORA gira non può restare nella lista di quelli in attesa: sarebbe il contrario
     * della regola per cui l'aiuto dice ciò che esiste. */
    {
        const char *tail = strstr(b, "in attesa del motore");
        CK(tail && strstr(tail, "models") == NULL, "`models` ancora tra i verbi non cablati");
        /* Vale per `serve` come per `models`: lasciarlo nella lista dei verbi in attesa sarebbe
         * l'aiuto che promette male il lavoro già fatto (e un `serve` che invece ASCOLTA). */
        CK(tail && strstr(tail, "serve") == NULL, "`serve` ancora tra i verbi non cablati");
    }
    CK(b && strstr(b, "  serve ") != NULL, "l'aiuto non nomina serve: %s", b ? b : "(vuoto)");
    /* Le bandiere nuove devono essere leggibili: un `--port` che non è scritto nell'aiuto è una
     * bandiera che esiste solo per chi ha letto il sorgente. */
    CK(b && strstr(b, "--port") != NULL && strstr(b, "--max-body") != NULL &&
           strstr(b, "--once") != NULL,
       "l'aiuto non elenca le bandiere di serve");
    /* E l'auth DEVE dire da dove arriva: una chiave in argv finirebbe nella cronologia della
     * shell e nei processi altrui, la stessa ragione di HF_TOKEN. */
    CK(b && strstr(b, "FASTMLX_API_KEY") != NULL, "l'aiuto non dice come si passa la chiave");
    CK(b && strstr(b, "HF_TOKEN") != NULL, "l'aiuto non dice come si passa il token");
    /* Il filtro e la sua regola d'uscita stanno qui dentro, non solo nel sorgente: chi digita
     * `models xyz` e riceve 1 deve poter leggere PERCHÉ senza aprire `fmlx_models.c`. */
    CK(b && strstr(b, "[<filtro>]") != NULL, "l'aiuto non nomina il filtro di `models`");
    CK(b && strstr(b, "uscita 1") != NULL, "l'aiuto non dice che zero match sono un fallimento");
    /* E la politica dell'avanzamento: un carattere di controllo che arriva in una pipe è ciò che
     * l'utente legge come sporcizia, quindi il contratto si dichiara prima che si scopra. */
    CK(b && strstr(b, " UNA riga che si riscrive") != NULL,
       "l'aiuto non dice come il `pull` segna l'avanzamento");
    CK(b && strstr(b, "senza nessun carattere di controllo") != NULL,
       "l'aiuto non dice che su pipe/log non ci sono `\\r`");
    free(b);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    /* L'ambiente di chi esegue non decide i risultati. */
    unsetenv("HF_TOKEN");
    unsetenv("HF_HUB_OFFLINE");
    unsetenv("TRANSFORMERS_OFFLINE");
    unsetenv("HF_ENDPOINT");
    unsetenv("HF_HOME");
    unsetenv("HF_HUB_CACHE");
    /* `serve` legge il suo ambiente come `pull` legge HF_HOME: lasciarlo quello di chi esegue
     * renderebbe i risultati dipendenti dalla shell di chi li guarda. */
    unsetenv("FASTMLX_HOST");
    unsetenv("FASTMLX_PORT");
    unsetenv("FASTMLX_API_KEY");

    /* Il work directory è OBBLIGATORIO (lo passa CMake): un test che scrivesse il suo cache finto
     * nella directory corrente finirebbe per farlo nella root del repo, che è il punto dove il
     * cache VERO di questa macchina non deve essere confuso con un fixture. */
    if (argc < 2) {
        printf("uso: %s <directory-di-lavoro>\n", argv[0]);
        return 2;
    }
    snprintf(WORK, sizeof WORK, "%s", argv[1]);
    wipe(WORK);

    case_pull_minimo();
    case_pull_ogni_bandiera();
    case_bandiera_lunga();
    case_bandiere_rifiutate();
    case_motori_non_collegati();
    case_serve_bandiere();
    case_models_tabella();
    case_models_quiet();
    case_models_filtro();
    case_models_senza_cache();
    case_canali();
    case_aiuto();

    printf("test_cli: %d verifiche, %d fallimenti\n", checks, failures);
    return failures ? 1 : 0;
}
