/* fmlx_cli.c — l'argv di `fastmlx` e il dispatch (piano §10.3, §13).
 *
 * Due regole tengono insieme questo file.
 *
 *  1. UNA bandiera sconosciuta È un errore, non una cosa ignorata. Il motivo non è di stile: una
 *     bandiera ignorata in un comando che scarica modelli vuol dire `--force` che non forza,
 *     `--max-bytes` che non limita, e chi guarda il risultato non ha modo di saperlo. (È lo stesso
 *     principio con cui il §11.2 del piano tratta i campi ignoti di una richiesta API.)
 *  2. I verbi che chiedono il motore rispondono `FMLX_CLI_NOT_WIRED` con il PERCHÉ, e il perché è
 *     diverso verbo da verbo: `run`/`chat`/`serve` aspettano la libreria del motore, `ps` aspetta il
 *     registro delle sessioni, `rm`/`cp` aspettano chi scrive nel cache. Dire tutti "non
 *     implementato" farebbe perdere all'utente l'informazione che gli serve.
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
            "  models, list [<filtro>] cosa c'e' gia' nel cache, senza rete; <filtro> = org/nome,\n"
            "                         nome (l'ultima parte dell'id) o org/*\n"
            "  serve                  serve /v1/models e /health e GENERA: chat, completions e\n"
            "                         messages passano dal motore LLM; il resto e' 501\n"
            "  doctor [<filtro>]      hasha i blob del cache e dice se SONO quelli promessi;\n"
            "                         legge e basta: non cancella e non ripara\n"
            "\n"
            "per pull:\n"
            "  --revision <ref>       ramo o commit (default: main)\n"
            "  --dest <dir>           specchia il modello in <dir>/<nome-del-repo> con hardlink\n"
            "  --cache <dir>          radice del cache (default: quella di Hugging Face)\n"
            "  --endpoint <url>       server diverso da huggingface.co\n"
            "  --max-bytes <n>        tetto di byte per UN singolo file\n"
            "  --stall-timeout <s>    secondi senza un byte prima di riprovare (default 60;\n"
            "                         il taglio arriva ~6 s dopo: finestra di libcurl)\n"
            "  --stall-retries <n>    quante volte ripartire con Range dopo uno stall\n"
            "  --offline              mai rete: se manca un file, errore\n"
            "  --repair             permesso di sostituire un blob che non verifica\n"
            "  --quiet              niente righe di progresso\n"
            "                       Su terminale il progresso e' UNA riga che si riscrive; su pipe o\n"
            "                       log una riga per file, senza nessun carattere di controllo.\n"
            "  -h, --help             questo testo\n"
            "\n"
            "per models:\n"
            "  <filtro>               uno solo, a posizione; zero match = uscita 1 (come grep):\n"
            "                         «non c'e'» non puo' sembrare «va tutto bene»\n"
            "  --cache <dir>          radice del cache (default: quella di Hugging Face)\n"
            "  --quiet                solo gli id, uno per riga\n"
            "\n"
            "per serve:\n"
            "  --host <if>            con quale interfaccia ascoltare (default 127.0.0.1)\n"
            "  --port <n>             porta TCP (default 8000)\n"
            "  --cache <dir>          radice del cache che /v1/models elenca\n"
            "  --max-body <n>         tetto di byte di un corpo di richiesta\n"
            "  --once                 serve UNA richiesta e esce (per gli smoke test)\n"
            "  --quiet                niente riepilogo finale su stdout (i log restano su stderr)\n"
            "\n"
            "per doctor:\n"
            "  <filtro>               gli STESSI tre modi di `models` (org/nome, nome, org/*);\n"
            "                         nessuno = tutto il cache\n"
            "  --cache <dir>          radice del cache (default: quella di Hugging Face)\n"
            "  --quiet                SOLO le righe di PROBLEMA: nessun riepilogo, nessuna nota.\n"
            "                         Uscita: 0 = niente corruzione, 1 = problemi o repo non\n"
            "                         audibili, 2 = argv. Le note non muovono l'uscita.\n"
            "\n"
            "ascolta e risponde DAVVERO a /v1/models e /health, e GENERA su chat/completions,\n"
            "/v1/completions e /v1/messages. Le altre rotte di generazione rispondono 501.\n"
            "L'auth viene SOLO da FASTMLX_API_KEY (vuota = nessuna): una bandiera finirebbe in argv.\n"
            "\n"
            "in attesa del motore (rispondono un errore esplicito, mai un risultato finto):\n"
            "  run, chat, ps, cp, rm\n"
            "\n"
            "il token si passa SOLO con la variabile d'ambiente HF_TOKEN.\n");
}

/* Cosa risponde un verbo che oggi non può girare, e PERCHÉ. È il punto in cui la CLI smette di
 * essere una lista di stringhe: nomina il pezzo mancante, che è poi il piano di lavoro. */
static const char *missing_piece(const char *verb) {
    if (!strcmp(verb, "run") || !strcmp(verb, "chat"))
        return "la libreria del motore (mlxengine) non è ancora collegata: i token non si generano";
    if (!strcmp(verb, "ps")) return "manca il registro delle sessioni, che non esiste fuori dal motore";
    if (!strcmp(verb, "cp") || !strcmp(verb, "rm"))
        return "manca chi scrive/cancella nel cache al di fuori del percorso di download";
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

/* ---- le bandiere, come INSIEME DI PAROLE -------
 *
 * I nomi stanno in UNA tabella sola, con un bit ciascuna, perché la domanda «questa bandiera è
 * stata data?» non si può fare sul VALORE: `--stall-retries 0` e `--offline` non dato sono lo
 * stesso numero (0), e fidarsi del valore lascerebbe passare `fastmlx serve --stall-retries 0`
 * come se fosse una cosa detta per sbaglio. `--quiet` e `--cache` hanno un bit ma non stanno in
 * nessun insieme ESCLUSIVO: valgono per ogni verbo. */
enum {
    F_QUIET = 0, F_CACHE,                                  /* shared */
    F_REVISION, F_ENDPOINT, F_MAXBYTES, F_STALLTO, F_STALLRE, F_DEST, F_OFFLINE, F_REPAIR,
    F_HOST, F_PORT, F_MAXBODY, F_ONCE,
    F_N
};
#define F_PULL_ONLY                                                                                            \
    ((1u << F_REVISION) | (1u << F_ENDPOINT) | (1u << F_MAXBYTES) | (1u << F_STALLTO) |                        \
     (1u << F_STALLRE) | (1u << F_DEST) | (1u << F_OFFLINE) | (1u << F_REPAIR))
#define F_SERVE_ONLY ((1u << F_HOST) | (1u << F_PORT) | (1u << F_MAXBODY) | (1u << F_ONCE))
/* Le bandiere che SONO un fatto, non un dato: la forma `--flag=valore` è un errore per tutte e
 * quattro con UNA regola, invece che una rinuncia per una. `--quiet=1` e `--quiet=0` sono la
 * stessa cosa, e `--offline=` (vuoto) direbbe che qualcuno ha dato un valore a una parola che non
 * lo prende: due semantiche con la stessa grafia sono peggio di un rifiuto. */
#define F_BOOLEANI                                                                                             \
    ((1u << F_QUIET) | (1u << F_OFFLINE) | (1u << F_REPAIR) | (1u << F_ONCE))

static const char *const FLN[F_N] = {
    "--quiet",    "--cache",        "--revision", "--endpoint", "--max-bytes", "--stall-timeout",
    "--stall-retries", "--dest",     "--offline",  "--repair",   "--host",      "--port",
    "--max-body", "--once"};

/* Il bit della bandiera `n`, 0 se la bandiera non esiste. */
static unsigned flag_bit(const char *n) {
    int i;
    for (i = 0; i < F_N; i++)
        if (!strcmp(n, FLN[i])) return 1u << i;
    return 0;
}

FmlxCliStatus fmlx_cli_parse(int argc, char **argv, FmlxCliArgs *out, char *msg, size_t msg_len) {
    int i, verb_seen = 0, repo_seen = 0;
    long long n;
    unsigned seen = 0;

    memset(out, 0, sizeof *out);
    out->revision = "main";
    if (!argv) return fail(out, msg, msg_len, "argv assente");

    for (i = 1; i < argc; i++) {
        char namebuf[32]; /* `name` può puntarci: vive quanto tutto il giro di ciclo */
        const char *a = argv[i];
        const char *name;
        const char *val = NULL;
        const char *eq;
        unsigned b;

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
            if (repo_seen) return fail(out, msg, msg_len, "troppi argomenti posizionali: %s", a);
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
        /* Il BIT si conta PRIMA di qualunque gestione: `--once` e `--quiet` escono dal giro con un
         * `continue`, e «questa bandiera è stata data» deve restare vero anche per le booleane. */
        b = flag_bit(name);
        if (!b) return fail(out, msg, msg_len, "bandiera sconosciuta: %s", name);
        seen |= b;
        if ((b & F_BOOLEANI) && val) return fail(out, msg, msg_len, "%s non prende un valore", name);
        if (!strcmp(name, "--quiet")) { out->quiet = 1; continue; }
        if (!strcmp(name, "--offline")) { out->pull.fetch.offline = 1; continue; }
        if (!strcmp(name, "--repair")) { out->pull.fetch.repair = 1; continue; }
        if (!strcmp(name, "--once")) { out->serve.max_conns = 1; continue; }

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
            /* UNA bandiera, DUE lettori: il `pull` e il `serve` guardano lo stesso disco, e darla a
             * `serve` vedendola ignorata (con il server che legge HF_HOME) sarebbe il tipo di
             * opzione che mente. Le due lane risolvono NULL allo stesso modo, quindi chi non ne dà
             * resta nel comportamento di prima. */
            out->pull.fetch.cache_root = val;
            out->serve.cache_root = val;
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
        } else if (!strcmp(name, "--host")) {
            out->serve.host = val;
        } else if (!strcmp(name, "--port")) {
            /* 1..65535 e solo cifre: la stessa regola di `FASTMLX_PORT` (piano §14), perché due
             * modi di leggere la stessa cifra con severità diversa è il bug che rende un flag
             * inaffidabile più dell'ambiente. */
            if (!parse_num(val, 1, 65535, &n))
                return fail(out, msg, msg_len, "--port: '%s' non è una porta 1..65535", val);
            out->serve.port = (int)n;
        } else if (!strcmp(name, "--max-body")) {
            /* `LLONG_MAX`, non `SIZE_MAX`: l'ultimo è senza segno e come `long long` diventa -1,
             * cioè "nessun numero passa". `parse_num` ragiona in signed, e il tetto reale lo mette
             * la lane. */
            if (!parse_num(val, 1, LLONG_MAX, &n))
                return fail(out, msg, msg_len, "--max-body: '%s' non è un numero >= 1", val);
            out->serve.max_body = (size_t)n;
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
    /* `serve` non ha un argomento posizionale: accettare `serve mlx-model` e ascoltare comunque
     * sarebbe una bandiera che mente, solo scritta senza trattini. `models`/`list` LO hanno dal
     * 2026-09-24 (il filtro per repo), e la regola che resta è la stessa: UNO solo, il secondo è un
     * errore — due filtri non vogliono dire niente. */
    if (repo_seen && !strcmp(out->verb, "serve")) {
        char v[16];
        snprintf(v, sizeof v, "%s", out->verb); /* `fail` azzera `out`: vedi sopra */
        return fail(out, msg, msg_len, "%s: non prende argomenti (le opzioni di serve sono bandiere)",
                    v);
    }
    /* Una bandiera che IL VERBO NON LEGGE è un errore, non un'opzione ignorata: il motivo è lo
     * stesso per cui una bandiera sconosciuta lo è. `fastmlx serve --dest x` sembra un comando che
     * dice due cose e ne fa una, e chi legge l'output non ha modo di sapere quale delle due è
     * saltata. Le bandiere condivise (`--quiet`, `--cache`) non stanno in nessun insieme.
     *
     * LA REGOLA VALE FRA I VERBI CHE HANNO UNA SUPERFICIE DI OPZIONI (pull, models/list, serve,
     * doctor).
     * `fastmlx run --port 8000` non deve sentire «--port non è un'opzione di run»: `run` non ha
     * ancora opzioni, e dire quali NON ha è un'affermazione sul futuro di un verbo che non è scritto.
     * Per quelli la risposta onesta è UNA sola — manca il motore — ed è `NOT_WIRED` nel dispatch. */
    if (verb_seen && (!strcmp(out->verb, "pull") || !strcmp(out->verb, "serve") ||
                      !strcmp(out->verb, "models") || !strcmp(out->verb, "list") ||
                      !strcmp(out->verb, "doctor"))) {
        unsigned bad = 0;
        int k;
        if (strcmp(out->verb, "pull")) bad |= F_PULL_ONLY;
        if (strcmp(out->verb, "serve")) bad |= F_SERVE_ONLY;
        for (k = 0; k < F_N; k++)
            if ((bad & (1u << k)) && (seen & (1u << k))) {
                char vv[16];
                snprintf(vv, sizeof vv, "%s", out->verb); /* `fail` azzera `out`: vedi sopra */
                return fail(out, msg, msg_len, "%s non è un'opzione di %s", FLN[k], vv);
            }
    }
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
    if (!strcmp(a->verb, "models") || !strcmp(a->verb, "list"))
        /* Non passa dal transport: `t` qui non è nemmeno guardato, perché l'elenco viene dal disco.
         * È il motivo per cui il verboso `case_motori_non_collegati` di test_cli.c NON lo copre:
         * un verbo che non tocca la rete non si prova con un transport finto. */
        return fmlx_models_exec(a, out, err);
    if (!strcmp(a->verb, "doctor"))
        /* Nemmeno qui il transport viene guardato: l'audit hashà ciò che è già su disco, e una
         * rete finta non aggiungerebbe nessuna prova — semmai ne toglierebbe, perché il verdetto
         * dovrebbe dipendere da qualcosa che non c'entra con i byte da controllare. */
        return fmlx_doctor_exec(a, out, err);
    if (!strcmp(a->verb, "serve"))
        /* Nemmeno qui `t` viene guardato: il `serve` legge il cache da disco e scrive su socket.
         * Passare un transport falso a `serve` non è un test, è un'illusione di test. */
        return fmlx_serve_exec(a, out, err);
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
