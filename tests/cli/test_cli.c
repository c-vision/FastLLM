/* test_cli.c — l'argv di `fastmlx` e il dispatch: due casi per regola, nessuno decorativo.
 *
 * Il `pull` VERO è provato in test_pull.c (lí ci sono i blob, gli symlink, gl'inode). Qui si prova
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

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
 * ricostruire il finto server di test_pull.c (che è un altro oggetto, e già provato lí). */
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
 * manca è il lavoro del piano. */
static void case_motori_non_collegati(void) {
    static const char *const verbs[] = {"run", "chat", "serve", "models",
                                        "list", "ps",   "doctor", "cp",     "rm"};
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
    /* Nove verbi, almeno TRE messaggi diversi: è la prova che il testo dipende dal verbo e non è
     * una stringa sola ripetuta. */
    CK(distinct >= 3, "messaggi distinti=%d su 9 verbi", distinct);
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
    CK(b && strstr(b, "in attesa del motore") != NULL, "l'aiuto non distingue i verbi non cablati");
    CK(b && strstr(b, "HF_TOKEN") != NULL, "l'aiuto non dice come si passa il token");
    free(b);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    /* L'ambiente di chi esegue non decide i risultati. */
    unsetenv("HF_TOKEN");
    unsetenv("HF_HUB_OFFLINE");
    unsetenv("TRANSFORMERS_OFFLINE");
    unsetenv("HF_ENDPOINT");
    unsetenv("HF_HOME");
    unsetenv("HF_HUB_CACHE");

    case_pull_minimo();
    case_pull_ogni_bandiera();
    case_bandiera_lunga();
    case_bandiere_rifiutate();
    case_motori_non_collegati();
    case_canali();
    case_aiuto();

    printf("test_cli: %d verifiche, %d fallimenti\n", checks, failures);
    return failures ? 1 : 0;
}
