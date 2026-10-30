/* test_cli.c — l'argv di `fastllm` e il dispatch: due casi per regola, nessuno decorativo.
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

#include <errno.h>
#include <ftw.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h> /* geteuid: un `chmod 000` non frena root, e il caso del repo murato sarebbe falso */

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
 * file ha INCONTRATO davvero: `{"fastllm","pull"}` con argc=3 non fallisce in modo visibile,
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

    CK_PARSE(FMLX_CLI_OK, "fastllm", "pull", "org/repo");
    CK(strcmp(a.verb, "pull") == 0, "verb=%s", a.verb);
    CK(strcmp(a.repo, "org/repo") == 0, "repo=%s", a.repo);
    CK(a.revision && strcmp(a.revision, "main") == 0, "revision=%s", a.revision ? a.revision : "(null)");
    CK(a.pull.dest == NULL, "--dest non dato deve restare NULL, non una stringa vuota");
    CK(a.want_help == 0, "want_help=%d", a.want_help);
}

static void case_pull_ogni_bandiera(void) {
    FmlxCliArgs a;
    char msg[256];

    CK_PARSE(FMLX_CLI_OK, "fastllm", "pull", "mlx-community/R repo", "--revision", "refs/pr/7",
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
    CK_PARSE(FMLX_CLI_USAGE, "fastllm", "pull", evil);
    CK(strstr(msg, "lunga") != NULL, "msg=%s", msg);

    /* Stessa forma, nome corto: la bandiera sconosciuta è nominata per nome, non per posizione. */
    CK_PARSE(FMLX_CLI_USAGE, "fastllm", "pull", "o/r", "--niente=1");
    CK(strstr(msg, "--niente") != NULL, "msg=%s", msg);
}

static void case_bandiere_rifiutate(void) {
    FmlxCliArgs a;
    char msg[256];

    CK_PARSE(FMLX_CLI_USAGE, "fastllm", "pull", "o/r", "--force");
    CK(strstr(msg, "--force") != NULL, "la bandiera non è nominata: %s", msg);

    CK_PARSE(FMLX_CLI_USAGE, "fastllm", "pull", "o/r", "--max-bytes", "abc");
    CK(strstr(msg, "max-bytes") != NULL, "msg=%s", msg);

    /* 0 non è "nessun limite": è un valore, e `--max-bytes 0` vorrebbe dire "zero byte". */
    CK_PARSE(FMLX_CLI_USAGE, "fastllm", "pull", "o/r", "--max-bytes", "0");

    /* 0 di --stall-retries INVECE è un valore legittimo: "una prova sola". */
    CK_PARSE(FMLX_CLI_OK, "fastllm", "pull", "o/r", "--stall-retries", "0");
    CK(a.pull.fetch.stall_retries == 0, "stall_retries=%d", a.pull.fetch.stall_retries);

    CK_PARSE(FMLX_CLI_USAGE, "fastllm", "pull", "o/r", "--dest"); /* valore mancante in coda */
    CK(strstr(msg, "valore") != NULL, "msg=%s", msg);

    CK_PARSE(FMLX_CLI_USAGE, "fastllm", "pull", "o/r", "--dest", ""); /* vuoto ≠ assente */

    CK_PARSE(FMLX_CLI_USAGE, "fastllm", "pull", "o/r", "extra");
    CK(strstr(msg, "posizionali") != NULL, "msg=%s", msg);

    CK_PARSE(FMLX_CLI_USAGE, "fastllm", "pull");
    CK(strstr(msg, "repo") != NULL, "msg=%s", msg);

    /* Un primo token che NON è un verbo è un NOME DI MODELLO: la CLI assume `chat <nome>` (è ciò
     * che rende scrivibile `fastllm <modello>`), e l'ingresso non è un errore di parse. Il secondo
     * posizionale, invece, resta un errore. */
    CK_PARSE(FMLX_CLI_OK, "fastllm", "fetch");
    CK(strcmp(a.verb, "chat") == 0 && strcmp(a.repo, "fetch") == 0, "verb=%s repo=%s", a.verb, a.repo);

    CK_PARSE(FMLX_CLI_USAGE, "fastllm", "fetch", "o/r");
    CK(strstr(msg, "posizionali") != NULL, "msg=%s", msg);

    CK_PARSE(FMLX_CLI_USAGE, "fastllm");

    /* l'aiuto non è un errore, e NON serve un comando */
    CK_PARSE(FMLX_CLI_OK, "fastllm", "--help");
    CK(a.want_help == 1, "want_help=%d", a.want_help);

    /* un parse fallito azzera tutto: un verbo residuo verrebbe eseguito */
    CK_PARSE(FMLX_CLI_USAGE, "fastllm", "pull", "o/r", "--mass-bytes", "1");
    CK(a.verb[0] == '\0' && a.repo[0] == '\0', "args non azzerati: %s / %s", a.verb, a.repo);
}

/* ---- dispatch ---- */

/* I verbi che chiedono il motore e NON lo hanno ancora. Dopo il passo 5 (CLI run/chat/ps) la lista
 * è scesa a `cp`/`rm`: sono gli unici che aspettano una capacità che il motore non fornisce (chi
 * scrive/cancella nel cache). `run`/`chat`/`ps` sono USCITI da qui perché ora GENERANO davvero:
 * tenerli in questa lista passerebbe anche se il dispatch fosse rotto, perché NOT_WIRED sarebbe il
 * codice "giusto" per il verbo sbagliato. Le prove di run/chat stanno nella loro sezione.
 *
 * `cp` è l'ULTIMO verbo non cablato (manca chi scrive/cancella nel cache fuori dal download):
 * qui si prova che il messaggio nomini il PROPRIO verbo e dica che un pezzo manca. `rm` è USCITO
 * da questa lista: toglie una voce dal REGISTRO locale ed è cablato. Tenerlo qui passerebbe anche
 * col dispatch rotto, perché NOT_WIRED sarebbe il codice "giusto" per il verbo sbagliato. */
static void case_motori_non_collegati(void) {
    static const char *const verbs[] = {"cp"};
    size_t i;

    for (i = 0; i < sizeof verbs / sizeof verbs[0]; i++) {
        FmlxCliArgs a;
        char msg[256];
        char *outb = NULL, *errb = NULL;
        size_t en = 0, on = 0;
        FILE *out = open_memstream(&outb, &on), *err = open_memstream(&errb, &en);

        CK_PARSE(FMLX_CLI_OK, "fastllm", (char *)verbs[i], "qualcosa");
        CK_ST(fmlx_cli_exec_with(&a, &DEAD, out, err), FMLX_CLI_NOT_WIRED);
        fclose(out);
        fclose(err); /* il buffer di open_memstream è valido solo dopo la chiusura */
        CK(strstr(errb, verbs[i]) != NULL, "%s: il messaggio non nomina il verbo: %s", verbs[i], errb);
        CK(strstr(errb, "non") != NULL || strstr(errb, "manca") != NULL,
           "%s: il messaggio non dice che il pezzo manca: %s", verbs[i], errb);
        /* Niente stdout: un comando che non può girare non emette mezzo risultato. */
        CK(on == 0, "%s: ha scritto anche su stdout (%zu byte)", verbs[i], on);
        free(outb);
        free(errb);
    }

    /* `rm` è cablato (registro): su una voce ASSENTE fallisce per la registrazione, non con
     * NOT_WIRED — è il codice che distingue "non ancora possibile" da "possibile e fallito". */
    {
        FmlxCliArgs a;
        char msg[256];
        char *outb = NULL, *errb = NULL;
        size_t en = 0, on = 0;
        FILE *out = open_memstream(&outb, &on), *err = open_memstream(&errb, &en);
        CK_PARSE(FMLX_CLI_OK, "fastllm", "rm", "qualcosa");
        CK_ST(fmlx_cli_exec_with(&a, &DEAD, out, err), FMLX_CLI_FAILED);
        fclose(out);
        fclose(err);
        free(outb);
        free(errb);
    }
}

/* ---- serve ---- */

/* `serve` è cablato: quello che si prova QUI è l'argv e la precedenza delle opzioni. Il loop TCP
 * vero è provato in `test_serve.c` (che ha un socket suo, su 127.0.0.1, con `max_conns=1`):
 * ripetere quel giro in questo file vorrebbe dire incollare un test nel posto sbagliato — e un
 * `serve` chiamato qui senza quel limite si sederebbe su `accept` per sempre. */
static void case_serve_bandiere(void) {
    FmlxCliArgs a;
    char msg[256];

    CK_PARSE(FMLX_CLI_OK, "fastllm", "serve", "--host", "0.0.0.0", "--port", "8137", "--max-body",
             "4096", "--once");
    CK(!strcmp(a.serve.host, "0.0.0.0"), "--host non arrivata al server: %s",
       a.serve.host ? a.serve.host : "(null)");
    CK(a.serve.port == 8137, "--port=%d", a.serve.port);
    CK(a.serve.max_body == 4096, "--max-body=%zu", a.serve.max_body);
    /* `--once` È `max_conns = 1`: una bandiera che non arriva al loop è una bandiera che mente, e
     * il sintomo è un `serve` che non finisce mai in una shell di prova. */
    CK(a.serve.max_conns == 1, "--once -> max_conns=%u", a.serve.max_conns);

    CK_PARSE(FMLX_CLI_OK, "fastllm", "serve");
    CK(a.serve.port == 0 && a.serve.host == NULL && a.serve.max_conns == 0,
       "senza bandiere il server usa i suoi default, non zero a caso");

    /* La stessa severità della bandiera e dell'ambiente: `--port abc` non è "porta non impostata",
     * è un typo, e accettarlo vorrebbe dire ascoltare sulla porta sbagliata in silenzio. */
    CK_PARSE(FMLX_CLI_USAGE, "fastllm", "serve", "--port", "abc");
    CK_PARSE(FMLX_CLI_USAGE, "fastllm", "serve", "--port", "0");
    CK_PARSE(FMLX_CLI_USAGE, "fastllm", "serve", "--port", "65536");
    CK_PARSE(FMLX_CLI_USAGE, "fastllm", "serve", "--max-body", "0");
    CK_PARSE(FMLX_CLI_USAGE, "fastllm", "serve", "--dest", "/tmp/x");
    CK_PARSE(FMLX_CLI_USAGE, "fastllm", "serve", "org/modello");
    /* E il contrario: una bandiera DEL SERVER data al verbo sbagliato. `run` sarebbe stato un
     * esempio peggiore di `models`, perché `missing_piece("run")` nomina `run` alla lettera e
     * l'asserzione sul messaggio passerebbe anche con argv accettato. */
    CK_PARSE(FMLX_CLI_USAGE, "fastllm", "models", "--once");
    CK(HAS(msg, "--once"), "il rifiuto deve nominare la bandiera: %s", msg);
    CK(HAS(msg, "models"), "e il verbo: %s", msg);

    /* Il CONFINE NUOVO della regola: `run` e `chat` HANNO ora una superficie di opzioni (il motore
     * è cablato), quindi una bandiera che non leggono è un errore anche per loro. `--port` di
     * `serve` non ha senso per `run` (non ascolta su nessuna porta) e `--once` nemmeno. Se questa
     * regola non li coprisse, `fastllm run m --port 8000` passerebbe col flag ignorato — la stessa
     * menzogna che la regola esiste per impedire. */
    CK_PARSE(FMLX_CLI_USAGE, "fastllm", "run", "m", "--port", "8000");
    CK(HAS(msg, "--port"), "il rifiuto deve nominare la bandiera: %s", msg);
    CK(HAS(msg, "run"), "e il verbo: %s", msg);
    CK_PARSE(FMLX_CLI_USAGE, "fastllm", "chat", "m", "--once");
    /* E il verso positivo: le bandiere che run/chat LEGGONO passano e arrivano all'exec. */
    CK_PARSE(FMLX_CLI_OK, "fastllm", "run", "m", "--max-tokens", "7", "--system", "sii breve");
    CK(a.max_tokens == 7, "--max-tokens non arrivato a run: %lld", a.max_tokens);
    CK(a.system && !strcmp(a.system, "sii breve"), "--system non arrivato a run: %s",
       a.system ? a.system : "(null)");
    CK_PARSE(FMLX_CLI_OK, "fastllm", "chat", "m", "--max-tokens", "9");
    CK(a.max_tokens == 9, "--max-tokens non arrivato a chat: %lld", a.max_tokens);
    /* `ps` guarda dove ascolta `serve`, quindi prende --host/--port. */
    CK_PARSE(FMLX_CLI_OK, "fastllm", "ps", "--host", "127.0.0.1", "--port", "8123");
    CK(a.serve.port == 8123, "ps: --port non arrivata: %d", a.serve.port);
    CK_PARSE(FMLX_CLI_USAGE, "fastllm", "ps", "un-modello");
    CK(HAS(msg, "non prende argomenti"), "ps con un posizionale: %s", msg);

    /* Una booleana NON prende valore, e la forma `--flag=x` ESISTE nel parser: `--once=0` sarebbe
     * letto come "dato", con lo STESSO effetto di `--once=1`. Due semantiche con la stessa grafia
     * è il tipo di banda che un utente mette in uno script e non scopre mai. */
    CK_PARSE(FMLX_CLI_USAGE, "fastllm", "serve", "--once=0");
    CK_PARSE(FMLX_CLI_USAGE, "fastllm", "pull", "o/r", "--quiet=1");
    CK_PARSE(FMLX_CLI_USAGE, "fastllm", "pull", "o/r", "--offline=");
    CK_PARSE(FMLX_CLI_USAGE, "fastllm", "pull", "o/r", "--repair=no");
    CK(HAS(msg, "valore"), "il rifiuto deve dire COSA non va: %s", msg);
    /* Il contrario, per non avere appena spostato il bug: la stessa bandiera SENZA `=` deve
     * passare, o la regola avrebbe trasformato un caso legittimo in un errore. */
    CK_PARSE(FMLX_CLI_OK, "fastllm", "pull", "o/r", "--offline", "--repair", "--quiet");
    CK(a.quiet && a.pull.fetch.offline && a.pull.fetch.repair, "booleani senza = persi per strada");

    /* Il `--cache` vale per TUTTI i verbi che guardano il disco: darlo a `serve` e vedere il
     * server leggere HF_HOME sarebbe il contrario della regola. */
    CK_PARSE(FMLX_CLI_OK, "fastllm", "serve", "--cache", "/tmp/due");
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
        setenv("FASTLLM_PORT", "80x", 1);
        out = open_memstream(&ob, &on);
        err = open_memstream(&eb, &en);
        CK_ST(fmlx_serve_exec(&b, out, err), FMLX_CLI_USAGE);
        fclose(out);
        fclose(err);
        if (eb) eb[en] = '\0';
        CK(HAS(eb, "FASTLLM_PORT"), "l'errore deve nominare la variabile: %s", eb ? eb : "(vuoto)");
        CK(on == 0, "un `serve` che non parte non scrive su stdout (%zu byte)", on);
        unsetenv("FASTLLM_PORT");
        free(ob);
        free(eb);
    }
}

/* ---- run / chat / ps: l'argv (l'esecuzione ha bisogno del motore) ---- */

/* Il parse è l'unica parte di run/chat/ps provabile senza un modello: l'esecuzione GENERA, e un
 * test che genera dipenderebbe da un modello installato (che è il contrario di deterministico). Qui
 * si prova il CONTRATTO: `run` ha due posizionali, il resto uno; le bandiere giuste arrivano; quelle
 * sbagliate sono rifiutate nominandole. */
static void case_run_chat_parse(void) {
    FmlxCliArgs a;
    char msg[256];

    CK_PARSE(FMLX_CLI_OK, "fastllm", "run", "org/m", "ciao mondo");
    CK(!strcmp(a.verb, "run") && !strcmp(a.repo, "org/m") && !strcmp(a.prompt, "ciao mondo"),
       "run: verbo/repo/prompt = %s/%s/%s", a.verb, a.repo, a.prompt);
    CK_PARSE(FMLX_CLI_OK, "fastllm", "run", "org/m");
    CK(a.prompt[0] == '\0', "run senza prompt: prompt='%s'", a.prompt);

    /* Il primo posizionale è OBBLIGATORIO: `run` senza modello non ha cosa caricare. */
    CK_PARSE(FMLX_CLI_USAGE, "fastllm", "run");
    CK(HAS(msg, "modello"), "run senza modello non lo dice: %s", msg);
    CK_PARSE(FMLX_CLI_USAGE, "fastllm", "chat");
    CK(HAS(msg, "modello"), "chat senza modello non lo dice: %s", msg);

    /* Un TERZO posizionale su run, e un secondo su chat: sono errori, non una cosa ignorata. */
    CK_PARSE(FMLX_CLI_USAGE, "fastllm", "run", "m", "a", "b");
    CK(HAS(msg, "posizionali"), "run con tre posizionali: %s", msg);
    CK_PARSE(FMLX_CLI_USAGE, "fastllm", "chat", "m", "extra");
    CK(HAS(msg, "posizionali"), "chat con due posizionali: %s", msg);

    /* `--max-tokens 0` è "genera zero token": un limite così è un typo. */
    CK_PARSE(FMLX_CLI_USAGE, "fastllm", "run", "m", "--max-tokens", "0");

    /* `--json` è di `models` e NON di pull: la regola «una bandiera che il verbo non legge è un
     * errore» vale anche per le bandiere nuove. */
    CK_PARSE(FMLX_CLI_OK, "fastllm", "models", "--json");
    CK(a.json == 1, "models --json non ha impostato il campo");
    CK_PARSE(FMLX_CLI_USAGE, "fastllm", "pull", "o/r", "--json");
    CK(HAS(msg, "--json"), "il rifiuto deve nominare la bandiera: %s", msg);
    CK_PARSE(FMLX_CLI_USAGE, "fastllm", "run", "m", "--json");
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
    CK_PARSE(FMLX_CLI_OK, "fastllm", "models", "--cache", "x");
    CK(a.pull.fetch.cache_root != NULL, "--cache non arrivata a models");
    /* L'argomento posizionale su un verbo che non lo usa è un errore, non una cosa ignorata. Si
     * asserisce ANCHE il nome del verbo: il primo giro sul cache reale ha stampato
     * `fastllm: : non prende argomenti` (il messaggio leggeva `out->verb` DOPO l'azzzeramento di
     * `fail`) e il solo frammento "non prende argomenti" lo stava a bere. `models` l'argomento lo
     * USA (è il filtro), e il verbo che oggi non lo prende è `serve`. */
    CK_PARSE(FMLX_CLI_USAGE, "fastllm", "serve", "mm/uno");
    CK(strstr(msg, "non prende argomenti") != NULL, "msg=%s", msg);
    CK(strncmp(msg, "serve:", 6) == 0, "il verbo non è nominato per esteso: '%s'", msg);
    /* Due filtri non vogliono dire niente: il secondo è un errore, non un AND implicito. */
    CK_PARSE(FMLX_CLI_USAGE, "fastllm", "models", "mm/uno", "mm/ue");
    CK(strstr(msg, "troppi argomenti posizionali") != NULL, "due filtri accettati: '%s'", msg);
    /* Il filtro arriva DAVVERO all'exec: `a->repo` è lo stesso campo che legge `pull`, e un parse
     * che lo riempie per un verbo e lo dimentica per l'altro si vede solo qui. */
    CK_PARSE(FMLX_CLI_OK, "fastllm", "models", "mm/uno");
    CK(strcmp(a.repo, "mm/uno") == 0, "il filtro non arriva all'exec: '%s'", a.repo);
    CK_PARSE(FMLX_CLI_OK, "fastllm", "list", "uno");
    CK(strcmp(a.repo, "uno") == 0, "l'alias perde il filtro: '%s'", a.repo);

    CK_PARSE(FMLX_CLI_OK, "fastllm", "models", "--cache", at("cache"));
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
 * bandiera silenziasse solo il progresso senza togliere l'intestazione, `$(fastllm models --quiet)`
 * passerebbe "NOME" a chi aspetta un repo. */
static void case_models_quiet(void) {
    FmlxCliArgs a;
    char msg[256], *outb = NULL, *errb = NULL;
    size_t on = 0, en = 0, lines = 0;
    FILE *out = open_memstream(&outb, &on), *err = open_memstream(&errb, &en);

    CK_PARSE(FMLX_CLI_OK, "fastllm", "models", "--cache", at("cache"), "--quiet");
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
    /* `models --quiet`: SOLO gli id, nell'ordine del catalogo, zero colonne. (`list` è il registro
     * NUMERATO — registro locale + cache — e ha una tabella propria: non è un alias di `models`, e
     * il suo header resta anche con `--quiet`.) */
    CK(en == 0, "l'elenco ha scritto sull'errore: %s", errb ? errb : "(null)");
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
    char *ex[] = {"fastllm", "models", "--cache", NULL, "mm/uno"};
    char *bare[] = {"fastllm", "models", "--cache", NULL, "uno"};
    char *glob[] = {"fastllm", "models", "--cache", NULL, "zz/*"};
    char *up[] = {"fastllm", "models", "--cache", NULL, "MM/UNO"};
    char *none[] = {"fastllm", "models", "--cache", NULL, "nope/nope"};
    char *star[] = {"fastllm", "models", "--cache", NULL, "or*x"};
    char *q[] = {"fastllm", "models", "--quiet", "--cache", NULL, "uno"};
    char *qnone[] = {"fastllm", "models", "--quiet", "--cache", NULL, "nope/nope"};

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

/* La lista in JSON esiste per uno script, quindi si prova la FORMA, non una frase: si parsà l'output
 * con lo STESSO parser dei client (un JSON che il nostro parser rifiuta non è un JSON) e si
 * controllano i campi che la tabella mostra. */
static void case_models_json(void) {
    FmlxCliArgs a;
    char msg[256], *outb = NULL, *errb = NULL;
    size_t on = 0, en = 0;
    FILE *out, *err;
    FmlxJsonDoc doc;

    build_cache_fixture();
    CK_PARSE(FMLX_CLI_OK, "fastllm", "models", "--cache", at("cache"), "--json");
    out = open_memstream(&outb, &on);
    err = open_memstream(&errb, &en);
    CK_ST(fmlx_cli_exec_with(&a, &DEAD, out, err), FMLX_CLI_OK);
    fclose(out);
    fclose(err);
    if (outb) outb[on] = '\0';
    if (errb) errb[en] = '\0';
    CK(en == 0, "models --json ha scritto sull'errore: %s", errb ? errb : "(null)");
    fmlx_json_doc_init(&doc);
    CK(outb && fmlx_json_parse(outb, on, 0, &doc) == FMLX_JSON_OK,
       "l'uscita --json non è JSON valido: %s", outb ? outb : "(vuoto)");
    if (outb && fmlx_json_root(&doc)) {
        const FmlxJsonValue *root = fmlx_json_root(&doc);
        const FmlxJsonValue *models = fmlx_json_obj_get(root, "models");
        CK(fmlx_json_arr_n(models) == 4, "attesi 4 repo nel JSON, trovati %zu",
           fmlx_json_arr_n(models));
        CK(HAS(outb, "\"refs_only\":true"), "il marker «solo ref» non è nel JSON:\n%s", outb);
        CK(HAS(outb, "\"id\":\"mm/uno\""), "manca un repo nel JSON:\n%s", outb);
    }
    fmlx_json_doc_free(&doc);
    free(outb);
    free(errb);

    /* Zero match con filtro: JSON VUOTO ma valido, e uscita 1 — come la tabella, che non stampa
     * nulla. Il codice d'uscita è l'unica cosa che distingue «non c'è» da «va tutto bene». */
    out = open_memstream(&outb, &on);
    err = open_memstream(&errb, &en);
    CK_PARSE(FMLX_CLI_OK, "fastllm", "models", "--cache", at("cache"), "--json", "nope/nope");
    CK_ST(fmlx_cli_exec_with(&a, &DEAD, out, err), FMLX_CLI_FAILED);
    fclose(out);
    fclose(err);
    if (outb) outb[on] = '\0';
    CK(outb && strstr(outb, "\"models\":[]") != NULL, "zero match: atteso un array vuoto:\n%s",
       outb ? outb : "(vuoto)");
    free(outb);
    free(errb);
}

/* La radice che non esiste NON è "cache vuoto": due messaggi diversi, perché sono due consigli
 * diversi. E l'elenco vuoto deve restare un SUCCESSO (exit 0): `fastllm models` su un cache appena
 * creato non è un fallimento. */
static void case_models_senza_cache(void) {
    FmlxCliArgs a;
    char msg[256], *outb = NULL, *errb = NULL;
    size_t on = 0, en = 0;
    FILE *out = open_memstream(&outb, &on), *err = open_memstream(&errb, &en);

    CK_PARSE(FMLX_CLI_OK, "fastllm", "models", "--cache", at("radice-che-non-c-e"));
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
    CK_PARSE(FMLX_CLI_OK, "fastllm", "models", "--cache", at("cache-vuoto"));
    CK_ST(fmlx_cli_exec_with(&a, &DEAD, out, err), FMLX_CLI_OK);
    fclose(out);
    fclose(err);
    if (outb) outb[on] = '\0';
    CK(outb && strstr(outb, "(nessun repo)") != NULL, "cache vuoto senza una parola: %s",
       outb ? outb : "(vuoto)");
    free(outb);
    free(errb);
}

/* ---- un repo che il filesystem rifiuta ---- */

/* Tre modi di dire la STESSA cosa — «questi byte non li abbiamo guardati» — e ognuno sparisce per
 * conto suo se il codice degenera: il marker `[!]` sulla riga, i trattini al posto dei numeri, il
 * conteggio «N repo NON LETTI» nella riga finale. Il primo se lo perde chi filtra la tabella, il
 * secondo se lo perde chi ha fretta di stampare `0 byte`, il terzo se lo perde chi somma e basta. La
 * riga finale è quella che conta per uno script: `du` dice un numero, `models` deve poter dire QUANTO
 * di quel numero non è misurato — senza che nessuno debba leggere la colonna NOME. */
static void case_models_murato(void) {
    FmlxCliArgs a;
    char msg[256], *outb = NULL, *errb = NULL, line[1024];
    size_t on = 0, en = 0;
    FILE *out, *err;

    if (geteuid() == 0) {
        printf("  (root: nessun chmod frena nessuno: caso del repo murato saltato)\n");
        return;
    }
    /* Si riapre PRIMA di tutto: se un giro precedente è crashato a directory murata, il `wipe`
     * dell'avvio non ci è potuto entrare e il fixture di quest'anno sarebbe sporco. */
    (void)chmod(at("mur/models--org--murato"), 0755);
    dir("mur/models--org--buono");
    put_bytes("mur/models--org--buono/blobs/b1", 100);
    dir("mur/models--org--murato/refs");
    put_bytes("mur/models--org--murato/refs/main", 40);
    put_bytes("mur/models--org--murato/blobs/b1", 7);
    if (chmod(at("mur/models--org--murato"), 0) != 0) {
        printf("  FIXTURE: chmod sul repo murato: %s\n", strerror(errno));
        failures++;
        return;
    }

    CK_PARSE(FMLX_CLI_OK, "fastllm", "models", "--cache", at("mur"));
    out = open_memstream(&outb, &on);
    err = open_memstream(&errb, &en);
    CK_ST(fmlx_cli_exec_with(&a, &DEAD, out, err), FMLX_CLI_OK);
    fclose(out);
    fclose(err);
    if (outb) outb[on] = '\0';
    if (errb) errb[en] = '\0';
    /* L'elenco COMPLETO non è un fallimento: «uno non l'ho aperto» è una riga della tabella, non un
     * codice d'uscita — e `en == 0` tiene pulito chi legge stdout in un pipe. */
    CK(en == 0, "il repo murato ha scritto sull'errore: %s", errb ? errb : "(vuoto)");
    CK(HAS(outb, "2 repo"), "l'elenco si è fermato a uno:\n%s", outb ? outb : "(vuoto)");

    CK(row_with(outb, "org/buono", line, sizeof line) != NULL, "manca il repo leggibile:\n%s",
       outb ? outb : "(vuoto)");
    CK(HAS(line, "100 byte"), "il leggibile ha perso i suoi numeri: %s", line);
    CK(!HAS(line, "[!]"), "il marker è sul repo che apre: %s", line);

    CK(row_with(outb, "org/murato", line, sizeof line) != NULL, "il repo murato non è in lista:\n%s",
       outb ? outb : "(vuoto)");
    CK(HAS(line, "[!]"), "manca il marker [!]: %s", line);
    CK(HAS(line, "—"), "le colonne numeriche non sono trattini: %s", line);
    /* Il `0 byte` è LA bugia di questo caso: direbbe «liberabili» di una directory che non abbiamo
     * potuto aprire, ed è il motivo per cui i tre numeri diventano un trattino. */
    CK(!HAS(line, "byte"), "la riga di un non-letto stampa una dimensione: %s", line);
    CK(!HAS(line, "[solo ref]"), "un murato con i ref marcati «solo ref»: %s", line);
    CK(HAS(line, "non letto dir: Permission denied"), "il motivo non è nominato: %s", line);
    CK(HAS(line, "models--org--murato"), "il colpevole non è il dirname: %s", line);
    CK(HAS(line, "model"), "il TIPO si sa anche così (viene dal prefisso): %s", line);
    free(outb);
    free(errb);

    /* La somma è «ciò che ho letto»: 100 byte, non 107. Se il `notread` si perdesse, uno script
     * confronta 100 con il `du` e non sa da dove viene la differenza. */
    out = open_memstream(&outb, &on);
    err = open_memstream(&errb, &en);
    CK_PARSE(FMLX_CLI_OK, "fastllm", "models", "--cache", at("mur"));
    CK_ST(fmlx_cli_exec_with(&a, &DEAD, out, err), FMLX_CLI_OK);
    fclose(out);
    fclose(err);
    if (outb) outb[on] = '\0';
    CK(HAS(outb, "100 byte in"), "la riga finale somma anche il non letto:\n%s",
       outb ? outb : "(vuoto)");
    CK(HAS(outb, "1 repo NON LETTI"), "la riga finale non confessa: %s", outb ? outb : "(vuoto)");
    CK(HAS(outb, "[!] ="), "nessuna legenda del marker: %s", outb ? outb : "(vuoto)");
    free(outb);
    free(errb);

    /* Il FILTRO non deve poter nascondere la confessione: `[!]` si perde con la riga se si filtra
     * l'altro repo, e un totale senza quella frase è il cache presentato come i match. */
    out = open_memstream(&outb, &on);
    err = open_memstream(&errb, &en);
    CK_PARSE(FMLX_CLI_OK, "fastllm", "models", "--cache", at("mur"), "murato");
    CK_ST(fmlx_cli_exec_with(&a, &DEAD, out, err), FMLX_CLI_OK);
    fclose(out);
    fclose(err);
    if (outb) outb[on] = '\0';
    CK(HAS(outb, "1 repo che matchano"), "il filtro conta i repo del cache: %s",
       outb ? outb : "(vuoto)");
    CK(HAS(outb, "1 repo NON LETTI"), "il filtro nasconde la confessione: %s",
       outb ? outb : "(vuoto)");
    CK(HAS(outb, "2 repo nel cache"), "il filtro non dice quanti erano: %s", outb ? outb : "(vuoto)");
    free(outb);
    free(errb);

    /* `--quiet`: l'id C'È, e la lista è una lista di repo. Il fallimento arriva dopo, quando ci si
     * prova a lavorare — ed è il codice DISTINTO di `catalog_repo` («non si è potuto aprire», non
     * «non è nel cache»), che è ciò che il caso della lane va a provare. */
    out = open_memstream(&outb, &on);
    err = open_memstream(&errb, &en);
    CK_PARSE(FMLX_CLI_OK, "fastllm", "models", "--cache", at("mur"), "--quiet");
    CK_ST(fmlx_cli_exec_with(&a, &DEAD, out, err), FMLX_CLI_OK);
    fclose(out);
    fclose(err);
    if (outb) outb[on] = '\0';
    CK(HAS(outb, "org/murato\n"), "`--quiet` ha perso il repo murato: %s", outb ? outb : "(vuoto)");
    CK(!HAS(outb, "["), "`--quiet` stampa decorazioni: %s", outb ? outb : "(vuoto)");
    free(outb);
    free(errb);

    chmod(at("mur/models--org--murato"), 0755); /* si riapre: un fixture murato non si wipea */
}

/* L'errore va sul canale d'errore, il progresso su stdout: in un pipe `fastllm pull … | wc -l`
 * deve contare i file, non le lamentele. */
static void case_canali(void) {
    FmlxCliArgs a;
    char msg[256], *outb = NULL, *errb = NULL;
    size_t on = 0, en = 0;
    FILE *out = open_memstream(&outb, &on), *err = open_memstream(&errb, &en);

    CK_PARSE(FMLX_CLI_OK, "fastllm", "pull", "org/repo");
    CK_ST(fmlx_cli_exec_with(&a, &DEAD, out, err), FMLX_CLI_FAILED);
    fclose(out);
    fclose(err);
    if (outb) outb[on] = '\0';
    if (errb) errb[en] = '\0';
    CK(errb && strstr(errb, "fastllm pull:") != NULL, "errore non sul canale d'errore: %s",
       errb ? errb : "(nessuno)");
    CK(outb && strstr(outb, "fastllm pull:") == NULL, "l'errore è anche su stdout: %s",
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

    CK_PARSE(FMLX_CLI_OK, "fastllm", "-h");
    CK_ST(fmlx_cli_exec_with(&a, &DEAD, out, out), FMLX_CLI_OK);
    fclose(out);
    if (b) b[n] = '\0';
    CK(b && strstr(b, "pull <repo>") != NULL, "l'aiuto non nomina pull: %s", b ? b : "(vuoto)");
    CK(b && strstr(b, "models [<filtro>]") != NULL, "l'aiuto non nomina models: %s", b ? b : "(vuoto)");
    CK(b && strstr(b, "list, ls") != NULL, "l'aiuto non nomina list/ls: %s", b ? b : "(vuoto)");
    /* Un verbo che gira DEVE comparire nell'aiuto, e non tra quelli "in attesa": l'aiuto dice ciò
     * che ESISTE. Oggi nell'aiuto non c'è alcuna lista di verbi in attesa. */
    CK(b && strstr(b, "in attesa del motore") == NULL, "l'aiuto promette verbi in attesa che girano");
    CK(b && strstr(b, "  serve ") != NULL, "l'aiuto non nomina serve: %s", b ? b : "(vuoto)");
    CK(b && strstr(b, "  run <nome|#n>") != NULL, "l'aiuto non nomina run: %s", b ? b : "(vuoto)");
    CK(b && strstr(b, "  chat <nome|#n>") != NULL, "l'aiuto non nomina chat: %s", b ? b : "(vuoto)");
    CK(b && strstr(b, "  ps ") != NULL, "l'aiuto non nomina ps: %s", b ? b : "(vuoto)");
    CK(b && strstr(b, "  doctor ") != NULL, "l'aiuto non nomina doctor: %s", b ? b : "(vuoto)");
    /* L'auth e il token arrivano SOLO da ambiente: la help lo dice, perché una chiave in argv
     * finirebbe nella cronologia della shell e nei processi altrui. */
    CK(b && strstr(b, "HF_TOKEN") != NULL, "l'aiuto non dice come si passa il token");
    CK(b && strstr(b, "$FASTLLM_HOME/models.tsv") != NULL, "l'aiuto non dice dove sta il registro");
    /* Il filtro e la sua regola d'uscita stanno qui dentro: chi digita `models xyz` e riceve 1 deve
     * poter leggere PERCHÉ senza aprire `fmlx_models.c`. */
    CK(b && strstr(b, "[<filtro>]") != NULL, "l'aiuto non nomina il filtro di `models`");
    free(b);

    /* Le bandiere di run/chat e di serve stanno nell'aiuto PER-VERBO (`fastllm help <verbo>`):
     * là devono essere leggibili, o esisterebbero solo per chi legge il sorgente. */
    {
        char *rb = NULL;
        size_t rn = 0;

        FILE *ro = open_memstream(&rb, &rn);
        CK_PARSE(FMLX_CLI_OK, "fastllm", "help", "run");
        CK_ST(fmlx_cli_exec_with(&a, &DEAD, ro, ro), FMLX_CLI_OK);
        fclose(ro);
        if (rb) rb[rn] = '\0';
        CK(rb && strstr(rb, "--max-tokens") != NULL && strstr(rb, "--system") != NULL,
           "help run non elenca le bandiere di run/chat");
        free(rb);
    }
    {
        char *sb = NULL;
        size_t sn = 0;

        FILE *so = open_memstream(&sb, &sn);
        CK_PARSE(FMLX_CLI_OK, "fastllm", "help", "serve");
        CK_ST(fmlx_cli_exec_with(&a, &DEAD, so, so), FMLX_CLI_OK);
        fclose(so);
        if (sb) sb[sn] = '\0';
        CK(sb && strstr(sb, "--port") != NULL && strstr(sb, "--max-body") != NULL &&
               strstr(sb, "--once") != NULL,
           "help serve non elenca le bandiere di serve");
        free(sb);
    }
}

/* `run <alias> <file>` per i modelli ocr|vlm|audio: senza file e' un errore d'uso, con un file inesistente un fallimento,
 * e in entrambi i casi il messaggio dice cosa manca (il modello non viene nemmeno caricato). */
static void case_media_run(void) {
    FmlxCliArgs a;
    FILE *out = tmpfile(), *err = tmpfile();
    char buf[512] = "";
    size_t n;

    memset(&a, 0, sizeof a);
    snprintf(a.repo, sizeof a.repo, "pocr");
    CK(fmlx_media_run_exec(&a, "/nonexistent.gguf", "/nonexistent-mmproj.gguf", "ocr", out, err) == FMLX_CLI_USAGE,
       "run ocr senza file non e' un errore d'uso");
    rewind(err);
    n = fread(buf, 1, sizeof buf - 1, err);
    buf[n] = '\0';
    CK(HAS(buf, "immagine"), "run ocr senza file non dice che serve l'immagine: %s", buf);

    snprintf(a.prompt, sizeof a.prompt, "/nonexistent/foto.png");
    CK(fmlx_media_run_exec(&a, "/nonexistent.gguf", "/nonexistent-mmproj.gguf", "ocr", out, err) == FMLX_CLI_FAILED,
       "run ocr con file inesistente non fallisce");
    snprintf(a.repo, sizeof a.repo, "parakeet");
    a.prompt[0] = '\0';
    rewind(err);
    CK(fmlx_media_run_exec(&a, "/nonexistent.gguf", NULL, "audio", out, err) == FMLX_CLI_USAGE, "run audio senza file");
    {   /* il rilevamento per architettura non tocca cio' che non e' un GGUF esistente */
        char md[16] = "x", mp[64] = "x";
        CK(fmlx_media_detect("/nonexistent.gguf", md, sizeof md, mp, sizeof mp) == 0, "detect su un file inesistente");
        CK(fmlx_media_detect("modello-mlx-dir", md, sizeof md, mp, sizeof mp) == 0, "detect su un nome non GGUF");
    }
    fclose(out);
    fclose(err);
}

/* installazione: i verbi `version`, `upgrade`, `doctor --install` esistono, prendono SOLO le loro bandiere, e il controllo di
 * compatibilita' col motore non blocca una build di sviluppo (nessun LLMENV_VERSION accanto alla libreria). */
static void case_installazione(void) {
    FmlxCliArgs a;
    char msg[256], *b = NULL;
    size_t n = 0;
    FILE *out = open_memstream(&b, &n);

    CK_PARSE(FMLX_CLI_OK, "fastllm", "version");
    CK_ST(fmlx_cli_exec_with(&a, &DEAD, out, out), FMLX_CLI_OK);
    fflush(out);
    CK(b && strstr(b, "fastllm ") == b, "version non comincia con il nome e la versione: %s", b ? b : "(vuoto)");
    CK(b && strstr(b, "engine LLMEnv") != NULL, "version non nomina il motore: %s", b ? b : "(vuoto)");

    CK_PARSE(FMLX_CLI_OK, "fastllm", "doctor", "--install");
    CK(a.check_install == 1, "doctor --install non imposta il campo");
    CK_PARSE(FMLX_CLI_OK, "fastllm", "upgrade", "--check");
    CK(a.upgrade_check == 1, "upgrade --check non imposta il campo");
    /* una bandiera che il verbo non legge e' un errore, non un'opzione ignorata */
    CK_PARSE(FMLX_CLI_USAGE, "fastllm", "models", "--install");
    CK_PARSE(FMLX_CLI_USAGE, "fastllm", "run", "m", "--check");
    CK_PARSE(FMLX_CLI_USAGE, "fastllm", "version", "--check");
    CK_PARSE(FMLX_CLI_USAGE, "fastllm", "doctor", "--install=1");   /* e' un fatto, non un dato */

    /* il controllo di compatibilita' lascia passare una build di sviluppo */
    CK(fmlx_install_gate(out) == 1, "il gate blocca una build di sviluppo");
    fclose(out);
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
    unsetenv("FASTLLM_HOST");
    unsetenv("FASTLLM_PORT");
    unsetenv("FASTLLM_API_KEY");

    /* Il work directory è OBBLIGATORIO (lo passa CMake): un test che scrivesse il suo cache finto
     * nella directory corrente finirebbe per farlo nella root del repo, che è il punto dove il
     * cache VERO di questa macchina non deve essere confuso con un fixture. */
    if (argc < 2) {
        printf("uso: %s <directory-di-lavoro>\n", argv[0]);
        return 2;
    }
    snprintf(WORK, sizeof WORK, "%s", argv[1]);
    wipe(WORK);

    /* Il registro è `$FASTLLM_HOME/models.tsv` (default `~/.fastllm/models.tsv`): senza un home
     * controllato i test di `models`/`list` leggerebbero il registro VERO della macchina e
     * conterebbero i modelli di chi esegue — l'ambiente di chi esegue non deve decidere i
     * risultati. `list`/`models` uniscono registro + cache `--cache`, quindi qui il registro è
     * VUOTO e la sola sorgente è il fixture. */
    {
        char h[FMLX_HF_PATH_MAX];
        snprintf(h, sizeof h, "%s/home", WORK);
        fmlx_hf_mkdir_p(h);
        setenv("FASTLLM_HOME", h, 1);
    }

    case_pull_minimo();
    case_pull_ogni_bandiera();
    case_bandiera_lunga();
    case_bandiere_rifiutate();
    case_motori_non_collegati();
    case_serve_bandiere();
    case_run_chat_parse();
    case_models_tabella();
    case_models_quiet();
    case_models_filtro();
    case_models_json();
    case_models_senza_cache();
    case_models_murato();
    case_canali();
    case_aiuto();
    case_media_run();
    case_installazione();

    printf("test_cli: %d verifiche, %d fallimenti\n", checks, failures);
    return failures ? 1 : 0;
}
