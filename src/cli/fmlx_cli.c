/* fmlx_cli.c — l'argv di `fastmlx` e il dispatch (piano §10.3, §13).
 *
 * Due regole tengono insieme questo file.
 *
 *  1. UNA bandiera sconosciuta È un errore, non una cosa ignorata. Il motivo non è di stile: una
 *     bandiera ignorata in un comando che scarica modelli vuol dire `--force` che non forza,
 *     `--max-bytes` che non limita, e chi guarda il risultato non ha modo di saperlo. (È lo stesso
 *     principio con cui il §11.2 del piano tratta i campi ignoti di una richiesta API.)
 *  2. I verbi che chiedono il motore rispondono `FMLX_CLI_NOT_WIRED` con il PERCHÉ, e il perché è
 *     diverso verbo da verbo: `run`/`chat`/`serve` aspettano la libreria del motore, `models`/`ps`
 *     aspettano il registro delle sessioni, `rm`/`cp` aspettano chi scrive nel cache. Dire tutti
 *     "non implementato" farebbe perdere all'utente l'informazione che gli serve.
 *
 * Il `main` sta in fastmlx_main.c: qui non c'è nessun simbolo che impedisca a un test di chiamare
 * `fmlx_cli_run` con un transport finto e due FILE* catturati in memoria.
 */
#include "fmlx/fmlx_cli.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *fmlx_cli_status_str(FmlxCliStatus s) {
    switch (s) {
        case FMLX_CLI_OK: return "ok";
        case FMLX_CLI_USAGE: return "argomento non valido";
        case FMLX_CLI_NOT_WIRED: return "comando non ancora possibile";
        case FMLX_CLI_FAILED: return "fallito";
    }
    return "?"; /* irraggiungibile: nessun `default` nello switch, così -Wextra obbliga a nominare
                 * ogni nuovo stato prima che esista un cammino che non lo tratta. */
}

void fmlx_cli_usage(FILE *out) {
    fprintf(out,
            "uso: fastmlx <comando> [argomenti] [opzioni]\n"
            "\n"
            "  pull <repo>            scarica (o ritrova) TUTTO il repo nel cache HF\n"
            "\n"
            "per pull:\n"
            "  --revision <ref>       ramo o commit (default: main)\n"
            "  --dest <dir>           specchia il modello in <dir>/<nome-del-repo> con hardlink\n"
            "  --cache <dir>          radice del cache (default: quella di Hugging Face)\n"
            "  --endpoint <url>       server diverso da huggingface.co\n"
            "  --max-bytes <n>        tetto di byte per UN singolo file\n"
            "  --stall-timeout <s>    secondi senza un byte prima di riprovare (default 60)\n"
            "  --stall-retries <n>    quante volte ripartire con Range dopo uno stall\n"
            "  --offline              mai rete: se manca un file, errore\n"
            "  --repair             permesso di sostituire un blob che non verifica\n"
            "  --quiet              niente righe di progresso\n"
            "  -h, --help             questo testo\n"
            "\n"
            "in attesa del motore (rispondono un errore esplicito, mai un risultato finto):\n"
            "  run, chat, serve, models, list, ps, cp, rm, doctor\n"
            "\n"
            "il token si passa SOLO con la variabile d'ambiente HF_TOKEN.\n");
}

/* Cosa risponde un verbo che oggi non può girare, e PERCHÉ. È il punto in cui la CLI smette di
 * essere una lista di stringhe: nomina il pezzo mancante, che è poi il piano di lavoro. */
static const char *missing_piece(const char *verb) {
    if (!strcmp(verb, "run") || !strcmp(verb, "chat") || !strcmp(verb, "serve"))
        return "la libreria del motore (mlxengine) non è ancora collegata: i token non si generano";
    if (!strcmp(verb, "models") || !strcmp(verb, "list"))
        return "manca chi legge il cache e stampa i modelli con il loro ramo (lanes/hf sa già farlo)";
    if (!strcmp(verb, "ps")) return "manca il registro delle sessioni, che non esiste fuori dal motore";
    if (!strcmp(verb, "cp") || !strcmp(verb, "rm"))
        return "manca chi scrive/cancella nel cache al di fuori del percorso di download";
    if (!strcmp(verb, "doctor"))
        return "manca la ricognizione (quale lane, quale ramo, quale budget) da esporre";
    return "il comando non esiste in questa versione";
}

static int is_verb(const char *v) {
    static const char *const known[] = {"pull", "run", "chat",  "serve", "models",
                                        "list", "ps",   "cp",    "rm",    "doctor"};
    size_t i;
    for (i = 0; i < sizeof known / sizeof known[0]; i++)
        if (!strcmp(v, known[i])) return 1;
    return 0;
}

/* Un numero intero dalla riga di comando, senza scorciatoie: `0` di `strtol` su "abc" è il bug che
 * trasforma `--max-bytes=abc` in "nessun limite". Si accetta SOLO il formato decimale tutto cifre,
 * entro il range richiesto. */
static int parse_num(const char *s, long long lo, long long hi, long long *out) {
    long long v;
    char *end;
    if (!s || !*s) return 0;
    errno = 0;
    v = strtoll(s, &end, 10);
    if (errno == ERANGE || !end || *end || v < lo || v > hi) return 0;
    *out = v;
    return 1;
}

static FmlxCliStatus fail(FmlxCliArgs *out, char *msg, size_t msg_len, const char *fmt, ...) {
    va_list ap;
    memset(out, 0, sizeof *out);
    if (msg && msg_len) {
        va_start(ap, fmt);
        vsnprintf(msg, msg_len, fmt, ap);
        va_end(ap);
    }
    return FMLX_CLI_USAGE;
}

FmlxCliStatus fmlx_cli_parse(int argc, char **argv, FmlxCliArgs *out, char *msg, size_t msg_len) {
    int i, verb_seen = 0, repo_seen = 0;
    long long n;

    memset(out, 0, sizeof *out);
    out->revision = "main";
    if (!argv) return fail(out, msg, msg_len, "argv assente");

    for (i = 1; i < argc; i++) {
        char namebuf[32]; /* `name` può puntarci: vive quanto tutto il giro di ciclo */
        const char *a = argv[i];
        const char *name;
        const char *val = NULL;
        const char *eq;

        if (!a) return fail(out, msg, msg_len, "argomento NULL alla posizione %d", i);

        /* -h/--help in qualunque punto: non è un errore invocare l'aiuto con altre bandiere. */
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            out->want_help = 1;
            continue;
        }
        if (a[0] != '-' || !a[1]) {
            if (!verb_seen) {
                if (!is_verb(a))
                    return fail(out, msg, msg_len, "comando sconosciuto: %s", a);
                snprintf(out->verb, sizeof out->verb, "%s", a);
                verb_seen = 1;
                continue;
            }
            if (repo_seen) return fail(out, msg, msg_len, "troppo argomenti posizionali: %s", a);
            snprintf(out->repo, sizeof out->repo, "%s", a);
            repo_seen = 1;
            continue;
        }

        name = a;
        if ((eq = strchr(a, '=')) != NULL) {
            size_t len = (size_t)(eq - a);
            if (len >= sizeof namebuf) return fail(out, msg, msg_len, "bandiera troppo lunga: %s", a);
            memcpy(namebuf, a, len);
            namebuf[len] = '\0';
            name = namebuf;
            val = eq + 1;
        }
        if (!strcmp(name, "--quiet")) { out->quiet = 1; continue; }
        if (!strcmp(name, "--offline")) { out->pull.fetch.offline = 1; continue; }
        if (!strcmp(name, "--repair")) { out->pull.fetch.repair = 1; continue; }
        if (strcmp(name, "--revision") && strcmp(name, "--dest") && strcmp(name, "--cache") &&
            strcmp(name, "--endpoint") && strcmp(name, "--max-bytes") &&
            strcmp(name, "--stall-timeout") && strcmp(name, "--stall-retries"))
            return fail(out, msg, msg_len, "bandiera sconosciuta: %s", name);

        /* Una bandiera che vuole un valore e non ce l'ha è un errore ANCHE se il valore sarebbe
         * vuoto: `--dest ""` non significa "dest: nessuna", significa che qualcuno ha costruito il
         * comando sbagliato. */
        if (!val) {
            if (i + 1 >= argc) return fail(out, msg, msg_len, "%s vuole un valore", name);
            val = argv[++i];
        }
        if (!*val) return fail(out, msg, msg_len, "%s: valore vuoto", name);

        if (!strcmp(name, "--revision")) {
            out->revision = val;
        } else if (!strcmp(name, "--dest")) {
            out->pull.dest = val;
        } else if (!strcmp(name, "--cache")) {
            out->pull.fetch.cache_root = val;
        } else if (!strcmp(name, "--endpoint")) {
            out->pull.fetch.endpoint = val;
        } else if (!strcmp(name, "--max-bytes")) {
            if (!parse_num(val, 1, LLONG_MAX, &n))
                return fail(out, msg, msg_len, "--max-bytes: '%s' non è un numero >= 1", val);
            out->pull.fetch.max_bytes = n;
        } else if (!strcmp(name, "--stall-timeout")) {
            if (!parse_num(val, 1, INT_MAX, &n))
                return fail(out, msg, msg_len, "--stall-timeout: '%s' non è un numero >= 1", val);
            out->pull.fetch.stall_timeout_s = (int)n;
        } else { /* --stall-retries: 0 qui È un valore, e vuol dire "una prova sola" */
            if (!parse_num(val, 0, INT_MAX, &n))
                return fail(out, msg, msg_len, "--stall-retries: '%s' non è un numero >= 0", val);
            out->pull.fetch.stall_retries = (int)n;
        }
    }

    if (out->want_help) return FMLX_CLI_OK; /* l'aiuto vince: è una richiesta legittima */
    if (!verb_seen) return fail(out, msg, msg_len, "nessun comando");
    if (!strcmp(out->verb, "pull") && !out->repo[0])
        return fail(out, msg, msg_len, "pull: manca il repo (es. org/modello)");
    return FMLX_CLI_OK;
}

FmlxCliStatus fmlx_cli_exec_with(const FmlxCliArgs *a, const FmlxHfTransport *t, FILE *out,
                                 FILE *err) {
    FmlxPullReport rep;
    FmlxHfStatus s;

    if (!a) return FMLX_CLI_USAGE;
    if (a->want_help) {
        fmlx_cli_usage(out ? out : stdout);
        return FMLX_CLI_OK;
    }
    if (!a->verb[0]) {
        fmlx_cli_usage(err ? err : stderr);
        return FMLX_CLI_USAGE;
    }
    if (strcmp(a->verb, "pull")) {
        fprintf(err ? err : stderr,
                "fastmlx %s: %s\n", a->verb, missing_piece(a->verb));
        return FMLX_CLI_NOT_WIRED;
    }

    memset(&rep, 0, sizeof rep);
    s = fmlx_pull_with(a->repo, a->revision, &a->pull, t, &rep, a->quiet ? NULL : (out ? out : stdout));
    if (s != FMLX_HF_OK) {
        /* Il report porta già il messaggio della lane che ha fallito; qui si aggiunge solo il
         * codice, perché "pull fallito" senza dire perché è il tipo di riga che fa perdere ore. */
        fprintf(err ? err : stderr, "fastmlx pull: %s [%s]\n", rep.error_msg,
                fmlx_hf_status_str(rep.error));
        return FMLX_CLI_FAILED;
    }
    return FMLX_CLI_OK;
}

FmlxCliStatus fmlx_cli_exec(const FmlxCliArgs *a, FILE *out, FILE *err) {
    return fmlx_cli_exec_with(a, fmlx_hf_transport_curl(), out, err);
}

FmlxCliStatus fmlx_cli_run(int argc, char **argv, FILE *out, FILE *err) {
    FmlxCliArgs a;
    char msg[FMLX_HF_PATH_MAX] = "";
    FmlxCliStatus s = fmlx_cli_parse(argc, argv, &a, msg, sizeof msg);

    if (s != FMLX_CLI_OK) {
        fprintf(err ? err : stderr, "fastmlx: %s\n", msg);
        return s;
    }
    return fmlx_cli_exec_with(&a, fmlx_hf_transport_curl(), out, err);
}
