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
 * NOT_WIRED sarebbe comunque il codice "giusto" per la lista sbagliata). */
static void case_motori_non_collegati(void) {
    static const char *const verbs[] = {"run", "chat", "serve", "ps", "doctor", "cp", "rm"};
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
    /* Sette verbi, almeno TRE messaggi diversi: è la prova che il testo dipende dal verbo e non è
     * una stringa sola ripetuta. */
    CK(distinct >= 3, "messaggi distinti=%d su 7 verbi", distinct);
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
 * mente: 1000 + 500 + 3 = 1503 byte. */
static void build_cache_fixture(void) {
    dir("cache/models--mm--uno/refs");
    put_bytes("cache/models--mm--uno/blobs/b1", 1000);
    put_bytes("cache/models--mm--uno/blobs/b2", 500);
    dir("cache/models--mm--ref/refs");
    put_bytes("cache/models--mm--ref/refs/main", 40);
    dir("cache/datasets--mm--data");
    put_bytes("cache/datasets--mm--data/blobs/z", 3);
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
     * `fail`) e il solo frammento "non prende argomenti" lo stava a bere. */
    CK_PARSE(FMLX_CLI_USAGE, "fastmlx", "models", "mm/uno");
    CK(strstr(msg, "non prende argomenti") != NULL, "msg=%s", msg);
    CK(strncmp(msg, "models:", 7) == 0, "il verbo non è nominato per esteso: '%s'", msg);
    CK_PARSE(FMLX_CLI_USAGE, "fastmlx", "list", "mm/uno");
    CK(strncmp(msg, "list:", 5) == 0, "alias senza il suo nome: '%s'", msg);

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
    CK(outb && strstr(outb, "3 repo") != NULL, "riga finale col conteggio:\n%s", outb ? outb : "(vuoto)");
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
    CK(lines == 3, "righe=%zu, attese 3 (una per repo)\n%s", lines, outb ? outb : "(vuoto)");
    /* `list` alias di `models`: stessa tabella, stesse colonne. */
    CK(en == 0, "l'alias ha scritto sull'errore: %s", errb ? errb : "(null)");
    free(outb);
    free(errb);
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
    }
    CK(b && strstr(b, "HF_TOKEN") != NULL, "l'aiuto non dice come si passa il token");
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
    case_models_tabella();
    case_models_quiet();
    case_models_senza_cache();
    case_canali();
    case_aiuto();

    printf("test_cli: %d verifiche, %d fallimenti\n", checks, failures);
    return failures ? 1 : 0;
}
