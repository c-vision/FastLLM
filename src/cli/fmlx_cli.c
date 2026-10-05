/* fmlx_cli.c — l'argv di `fastllm` e il dispatch (piano §10.3, §13).
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
 * Il `main` sta in fastllm_main.c: qui non c'è nessun simbolo che impedisca a un test di chiamare
 * `fmlx_cli_run` con un transport finto e due FILE* catturati in memoria.
 */
#include "fmlx/fmlx_cli.h"

#include "engine/fmlx_engine.h" /* fmlx_engine_set_interactive (prompt Y/N per --offload-experts) */

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
            "\n"
            "uso: fastllm <comando> [argomenti] [opzioni]\n"
            "\n"
            "GESTIONE MODELLI\n"
            "  pull <repo>            scarica (o ritrova) TUTTO il repo nel cache HF, con resume\n"
            "  models [<filtro>]      cosa c'e' gia' nel cache HF, senza rete\n"
            "  list, ls               i modelli DISPONIBILI, NUMERATI (registro locale + cache HF)\n"
            "  add <nome> <path>      registra un modello locale per percorso (dir MLX, .gguf, VLM)\n"
            "  rm <nome|#n>           toglie un modello dal registro (--purge: cancella i file)\n"
            "  show <nome|#n>         i dettagli di una voce del registro\n"
            "  paths [add <dir>|rm <n>|scan]   percorsi di ricerca dei modelli (file di config):\n"
            "                         `paths` li elenca (numerati), `add <dir>` ne aggiunge uno,\n"
            "                         `rm <n>` toglie quello con quel progressivo, `scan [--json]`\n"
            "                         inventaria dove ESISTONO modelli\n"
            "  search [<dir>]         trova i MODELLI (--roots: le cartelle radice); default: tutti i dischi\n"
            "  test <nome|#n|path>    verifica che il modello sia COMPLETO e non corrotto su disco\n"
            "\n"
            "GENERAZIONE\n"
            "  run <nome|#n> [prompt] una generazione col motore; il testo esce token-live\n"
            "  chat <nome|#n>         conversazione multi-turno col motore\n"
            "\n"
            "SERVER E UTILITY\n"
            "  serve                  server OpenAI/Anthropic (/v1/models, /v1/chat/completions, /v1/messages)\n"
            "  ps                     dice se un `serve` e' in ascolto e i modelli residenti\n"
            "  doctor [<filtro>]      hasha i blob del cache HF e dice se SONO quelli promessi\n"
            "  options                elenca TUTTE le opzioni del motore (registro) con un esempio\n"
            "  help [<comando>]       questo testo, o gli esempi di UN comando\n"
            "\n"
            "OPZIONI COMUNI\n"
            "  --opt <nome>=<valore>  imposta un parametro del motore (registro, no env); elenco:\n"
            "                         `fastllm options` (tutte le opzioni con esempi)\n"
            "  --offload-experts      streama gli esperti MoE da disco (per modelli piu' grandi della\n"
            "                         RAM: residente piccolo, piu' lento). Se il modello supera META'\n"
            "                         della memoria libera+reclamabile e l'opzione non e' data, run/chat\n"
            "                         chiedono [Y/n] sul terminale. Si disabilita la domanda con\n"
            "                         FASTLLM_NO_OFFLOAD_PROMPT=1.\n"
            "  --cache <dir>          radice del cache (pull/models/list/serve/doctor)\n"
            "  --quiet                niente righe di progresso/riepilogo\n"
            "\n"
            "ESEMPI COMPLETI:  fastllm help <comando>   (oppure  fastllm <comando> --help)\n"
            "                  Esempio: fastllm help pull\n"
            "\n"
            "Il token si passa SOLO con la variabile d'ambiente HF_TOKEN.\n"
            "Modelli locali registrati in $FASTLLM_HOME/models.tsv (default ~/.fastllm/models.tsv).\n");
}

/* L'aiuto di UN comando: le sue bandiere E almeno un esempio con TUTTI i parametri. */
static void usage_verb(FILE *out, const char *v) {
    fprintf(out, "\n");
    if (!strcmp(v, "pull"))
        fprintf(out,
            "fastllm pull <repo> [opzioni]\n"
            "  <repo>                 id Hugging Face (org/nome)\n"
            "  --revision <ref>       ramo o commit (default: main)\n"
            "  --cache <dir>          radice del cache (default: quella di Hugging Face)\n"
            "  --endpoint <url>       server diverso da huggingface.co\n"
            "  --dest <dir>           specchia in <dir>/<nome-repo> (hardlink, mai symlink)\n"
            "  --max-bytes <n>        tetto di byte per UN file\n"
            "  --stall-timeout <s>    secondi senza byte prima di riprovare (default 60)\n"
            "  --stall-retries <n>    riprese con Range dopo uno stall (default 3)\n"
            "  --offline              mai rete: se manca un file, errore\n"
            "  --repair               permesso di sostituire un blob che non verifica\n"
            "  --quiet                niente righe di progresso\n"
            "  -h, --help             questo testo\n"
            "\n"
            "esempi:\n"
            "  fastllm pull mlx-community/Qwen3-8B-4bit\n"
            "  fastllm pull org/modello --revision main --cache ~/.cache/huggingface \\\n"
            "      --endpoint https://huggingface.co --dest ~/ai/Models --max-bytes 5000000000 \\\n"
            "      --stall-timeout 60 --stall-retries 3 --repair --quiet\n"
            "  fastllm pull org/modello --offline\n");
    else if (!strcmp(v, "models"))
        fprintf(out,
            "fastllm models [<filtro>] [opzioni]\n"
            "  <filtro>               uno solo: org/nome, nome, o org/*\n"
            "  --cache <dir>          radice del cache (default: quella di Hugging Face)\n"
            "  --quiet                solo gli id, uno per riga\n"
            "  --json                 la stessa lista in JSON\n"
            "  -h, --help             questo testo\n"
            "\n"
            "esempi:\n"
            "  fastllm models\n"
            "  fastllm models qwen\n"
            "  fastllm models mlx-community/* --cache ~/.cache/huggingface --json\n"
            "  fastllm models --quiet\n");
    else if (!strcmp(v, "paths"))
        fprintf(out,
            "fastllm paths [add <dir> | rm <n> | scan [--json]]\n"
            "  (nessun argomento)     elenca i percorsi configurati, NUMERATI (progressivo per rm)\n"
            "  add <dir>              aggiunge una directory ai percorsi di ricerca\n"
            "  rm <n>                 rimuove il percorso con il progressivo <n>\n"
            "  scan [--json]          scansiona i percorsi e dice dove sono i modelli\n"
            "  --json                 per `scan`: inventario JSON\n"
            "  -h, --help             questo testo\n"
            "\n"
            "I percorsi stanno in $FASTLLM_HOME/paths (default ~/.fastllm/paths), una directory per\n"
            "riga. Sono cercati, IN ORDINE, dopo il path esplicito e il cache Hugging Face.\n"
            "Nessun default implicito legato alla macchina: le directory di ricerca le decidi qui.\n"
            "\n"
            "esempi:\n"
            "  fastllm paths\n"
            "  fastllm paths add ~/ai/Models\n"
            "  fastllm paths add /Volumes/Armor5TB/ai/Models\n"
            "  fastllm paths rm 2\n"
            "  fastllm paths scan --json\n");
    else if (!strcmp(v, "search"))
        fprintf(out,
            "fastllm search [<dir>] [--roots] [--type <t>] [--walk] [--max-depth <n>] [--json]\n"
            "  <dir>                  radice da cui partire (default: TUTTI i dischi accessibili)\n"
            "  --roots                mostra le CARTELLE RADICE (tree coi modelli dentro)\n"
            "  --type <mlx|gguf|hf>   filtra per tipo (default: tutti)\n"
            "  --walk                 forza il walk del filesystem (niente Spotlight)\n"
            "  --max-depth <n>        tetto di profondita' del walk (default: NESSUN tetto)\n"
            "  --json                 la stessa lista in JSON\n"
            "  -h, --help             questo testo\n"
            "\n"
            "DEFAULT: lista NUMERATA e ALFABETICA dei MODELLI. Il NOME su una riga, il PERCORSO\n"
            "sotto (stile tree), senza accorciature; i DUPLICATI si mostrano tutti.\n"
            "CON --roots: un TREE delle cartelle RADICE, coi modelli annidati sotto ciascuna.\n"
            "\n"
            "Un MODELLO e': una dir con `config.json` + un file di pesi (*.safetensors/.bin/.npz/...),\n"
            "un FILE `*.gguf` (>= 1 MB, non `ggml-vocab-*`) — una cartella con N gguf da' N modelli —\n"
            "oppure un repo-cache HF (`models--org--nome`).\n"
            "Senza <dir>: Spotlight (mdfind) + walk di ogni volume montato + $HOME + i percorsi di `paths`.\n"
            "\n"
            "esempi:\n"
            "  fastllm search\n"
            "  fastllm search ~/ai --max-depth 4\n"
            "  fastllm search ~/ai --roots --max-depth 4\n"
            "  fastllm search /Volumes/Armor5TB --type gguf --json\n");
    else if (!strcmp(v, "test"))
        fprintf(out,
            "fastllm test <nome|#n|percorso>\n"
            "  <nome|#n|percorso>     alias del registro, indice (#n), o path/id/`paths`\n"
            "  -h, --help             questo testo\n"
            "\n"
            "Verifica che il modello sia COMPLETO su disco e non corrotto, SENZA caricarlo:\n"
            "  - GGUF (file): header col lettore canonico + i tensori che ci stanno nel file;\n"
            "  - MLX/HF (dir): config.json, pesi presenti, header safetensors validi;\n"
            "  - repo-cache HF: blob e snapshot presenti (nessun symlink rotto);\n"
            "  - in tutti: marker di download incompleto (*.part/*.incomplete/*.download/*.tmp).\n"
            "Uscita 0 = COMPLETO, 1 = problema (col motivo).\n"
            "\n"
            "esempi:\n"
            "  fastllm test ds4\n"
            "  fastllm test ~/ai/Models/Qwen3-8B-4bit\n"
            "  fastllm test /Volumes/Armor5TB/LocalLLM/ai/models/Qwen3.8-27B-DFlash2\n");
    else if (!strcmp(v, "list") || !strcmp(v, "ls"))
        fprintf(out,
            "fastllm list   (alias: ls)   [opzioni]\n"
            "  --cache <dir>          radice del cache HF da unire al registro\n"
            "  --json                 la stessa lista in JSON\n"
            "  --quiet                (accettata; la lista e' gia' compatta)\n"
            "  -h, --help             questo testo\n"
            "\n"
            "esempi:\n"
            "  fastllm list\n"
            "  fastllm list --cache ~/.cache/huggingface --json\n");
    else if (!strcmp(v, "add"))
        fprintf(out,
            "fastllm add <nome> <path> [opzioni]\n"
            "  <nome>                 l'alias con cui sceglierlo (run/chat/show/rm)\n"
            "  <path>                 dir MLX | file .gguf (ds4) | modello VLM\n"
            "  --mmproj <path>        proiettore multimodale (per --modality vlm|ocr)\n"
            "  --modality <m>         chat | vlm | ocr | audio (default chat)\n"
            "  -h, --help             questo testo\n"
            "\n"
            "esempi:\n"
            "  fastllm add llama1b ~/ai/Models/Llama-3.2-1B-Instruct-4bit\n"
            "  fastllm add ds4 ~/ds4/ds4flash.gguf --modality chat\n"
            "  fastllm add doodle-ocr ~/ai/Models/gguf/PaddleOCR-VL-1.6-GGUF.gguf \\\n"
            "      --mmproj ~/ai/Models/gguf/PaddleOCR-VL-1.6-GGUF-mmproj.gguf --modality ocr\n");
    else if (!strcmp(v, "rm"))
        fprintf(out,
            "fastllm rm <nome|#n> [opzioni]\n"
            "  <nome|#n>              l'alias o l'indice dalla `list`\n"
            "  --purge                cancella anche i file (default: solo la voce)\n"
            "  -h, --help             questo testo\n"
            "\n"
            "esempi:\n"
            "  fastllm rm ds4\n"
            "  fastllm rm 2\n"
            "  fastllm rm doodle-ocr --purge\n");
    else if (!strcmp(v, "show"))
        fprintf(out,
            "fastllm show <nome|#n>\n"
            "  <nome|#n>              l'alias o l'indice dalla `list`\n"
            "  -h, --help             questo testo\n"
            "\n"
            "esempi:\n"
            "  fastllm show ds4\n"
            "  fastllm show 0\n");
    else if (!strcmp(v, "run"))
        fprintf(out,
            "fastllm run <nome|#n> [prompt] [opzioni]\n"
            "  <nome|#n>              alias del registro, indice (#n), path/id HF, o dir di `paths`\n"
            "  [prompt]               one-shot; senza, legge una riga per volta da stdin\n"
            "  --cache <dir>          radice da cui risolvere il modello\n"
            "  --system <testo>       messaggio di sistema anteposto\n"
            "  --max-tokens <n>       tetto sui token generati\n"
            "  --think                abilita il thinking e lo mostra (default: thinking DISATTIVATO)\n"
            "  --offload-experts      streama gli esperti MoE da disco (modelli > RAM)\n"
            "  -h, --help             questo testo\n"
            "\n"
            "esempi:\n"
            "  fastllm run llama1b \"Ciao, presentati in una frase.\"\n"
            "  fastllm run 0 --cache ~/.cache/huggingface --system \"Sei conciso.\" \\\n"
            "      --max-tokens 256 --think --offload-experts\n");
    else if (!strcmp(v, "chat"))
        fprintf(out,
            "fastllm chat <nome|#n> [opzioni]   (REPL: /exit /quit /clear /new /system /thinking /prompt /compact /help)\n"
            "  <nome|#n>              alias del registro, indice (#n), path/id HF, o dir di `paths`\n"
            "  --cache <dir>          radice da cui risolvere il modello\n"
            "  --system <testo>       system iniziale (modificabile con /system)\n"
            "  --max-tokens <n>       tetto sui token generati\n"
            "  --think                abilita il thinking e lo mostra (default: thinking DISATTIVATO)\n"
            "  --offload-experts      streama gli esperti MoE da disco (modelli > RAM)\n"
            "  -h, --help             questo testo\n"
            "\n"
            "esempi:\n"
            "  fastllm chat llama1b\n"
            "  fastllm chat 0 --cache ~/.cache/huggingface --system \"Sei un assistente.\" \\\n"
            "      --max-tokens 512 --think --offload-experts\n");
    else if (!strcmp(v, "serve"))
        fprintf(out,
            "fastllm serve [opzioni]\n"
            "  --host <if>            interfaccia di ascolto (default 127.0.0.1)\n"
            "  --port <n>             porta TCP (default 8000)\n"
            "  --cache <dir>          radice del cache che /v1/models elenca\n"
            "  --max-body <n>         tetto di byte di un corpo di richiesta\n"
            "  --parallel <n>         pool I/O + worker engine (n thread I/O; default 1 = single-thread)\n"
            "  --batch                lotto continuo (M3): N richieste decodificano insieme (implica --parallel)\n"
            "  --once                 serve UNA richiesta e esce (smoke test)\n"
            "  --offload-experts      streama gli esperti MoE da disco (modelli > RAM)\n"
            "  --vision-model <gguf>  VLM per l'input immagini (richiede --vision-mmproj)\n"
            "  --vision-mmproj <gguf> proiettore multimodale del VLM\n"
            "  --quiet                niente riepilogo finale su stdout (i log restano su stderr)\n"
            "  -h, --help             questo testo\n"
            "\n"
            "esempi:\n"
            "  fastllm serve\n"
            "  fastllm serve --host 0.0.0.0 --port 8000 --cache ~/.cache/huggingface \\\n"
            "      --max-body 64000000 --offload-experts --quiet\n"
            "  fastllm serve --once\n");
    else if (!strcmp(v, "ps"))
        fprintf(out,
            "fastllm ps [opzioni]\n"
            "  --host <if>            dove cercare il server (default 127.0.0.1)\n"
            "  --port <n>             porta (default FASTLLM_PORT, poi 8000)\n"
            "  -h, --help             questo testo\n"
            "\n"
            "esempi:\n"
            "  fastllm ps\n"
            "  fastllm ps --host 127.0.0.1 --port 8000\n");
    else if (!strcmp(v, "doctor"))
        fprintf(out,
            "fastllm doctor [<filtro>] [opzioni]\n"
            "  <filtro>               org/nome, nome, o org/*; nessuno = tutto il cache\n"
            "  --cache <dir>          radice del cache\n"
            "  --quiet                SOLO le righe di problema (uscita 0 = niente corruzione)\n"
            "  -h, --help             questo testo\n"
            "\n"
            "esempi:\n"
            "  fastllm doctor\n"
            "  fastllm doctor qwen\n"
            "  fastllm doctor mlx-community/* --cache ~/.cache/huggingface --quiet\n");
    else if (!strcmp(v, "options"))
        fprintf(out,
            "fastllm options   [--json]\n"
            "  Elenca TUTTE le opzioni del motore (registro, parametri SENZA env), ognuna con\n"
            "  valore corrente e un ESEMPIO d'uso. Non carica alcun modello.\n"
            "  --json                 la stessa lista in JSON (name/desc/value/example)\n"
            "  -h, --help             questo testo\n"
            "\n"
            "Per impostare un parametro (prima del load, ripetibile):\n"
            "  fastllm --opt <nome>=<valore> run <modello>\n"
            "\n"
            "esempi:\n"
            "  fastllm options\n"
            "  fastllm --opt context=131072 --opt thinking=on run ds4\n"
            "  fastllm --opt kvquant=8:8192 run ds4\n"
            "  fastllm --opt dflash_model=~/ai/Models/Qwen3.8-27B-DFlash2 run ds4\n");
    else
        fmlx_cli_usage(out);
}

/* Cosa risponde un verbo che oggi non può girare, e PERCHÉ. È il punto in cui la CLI smette di
 * essere una lista di stringhe: nomina il pezzo mancante, che è poi il piano di lavoro. `run`,
 * `chat` e `ps` NON sono più qui: il motore è collegato e i tre verbi girano davvero. */
static const char *missing_piece(const char *verb) {
    if (!strcmp(verb, "cp") || !strcmp(verb, "rm"))
        return "manca chi scrive/cancella nel cache al di fuori del percorso di download";
    return "il comando non esiste in questa versione";
}

static int is_verb(const char *v) {
    static const char *const known[] = {"pull", "run", "chat",  "serve", "models",
                                        "list", "ls",  "ps",    "cp",    "rm",    "doctor",
                                        "add",  "show", "help",  "options", "paths", "search", "test"};
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
 * stesso numero (0), e fidarsi del valore lascerebbe passare `fastllm serve --stall-retries 0`
 * come se fosse una cosa detta per sbaglio. `--quiet` e `--cache` hanno un bit ma non stanno in
 * nessun insieme ESCLUSIVO: valgono per ogni verbo. */
enum {
    F_QUIET = 0, F_CACHE,                                  /* shared */
    F_REVISION, F_ENDPOINT, F_MAXBYTES, F_STALLTO, F_STALLRE, F_DEST, F_OFFLINE, F_REPAIR,
    F_HOST, F_PORT, F_MAXBODY, F_ONCE,
    F_VISION_MODEL, F_VISION_MMPROJ,
    F_JSON, F_MAXTOKENS, F_SYSTEM, F_THINK, F_OFFLOAD,
    F_MMPROJ, F_MODALITY, F_PURGE,
    F_OPT,
    F_WALK, F_MAXDEPTH, F_ROOTS, F_TYPE, F_PARALLEL, F_BATCH,
    F_N
};
#define F_PULL_ONLY                                                                                            \
    ((1u << F_REVISION) | (1u << F_ENDPOINT) | (1u << F_MAXBYTES) | (1u << F_STALLTO) |                        \
     (1u << F_STALLRE) | (1u << F_DEST) | (1u << F_OFFLINE) | (1u << F_REPAIR))
#define F_SERVE_ONLY ((1u << F_HOST) | (1u << F_PORT) | (1u << F_MAXBODY) | (1u << F_ONCE) | \
                      (1u << F_OFFLOAD) | (1u << F_VISION_MODEL) | (1u << F_VISION_MMPROJ) | \
                      (1u << F_PARALLEL) | (1u << F_BATCH))
#define F_MODELS_ONLY ((1u << F_JSON))
#define F_RUNCHAT_ONLY ((1u << F_MAXTOKENS) | (1u << F_SYSTEM) | (1u << F_THINK) | (1u << F_OFFLOAD))
/* `add` porta il proiettore e la modalità di un modello locale; `rm` la cancellazione dei file. */
#define F_ADD_ONLY ((1u << F_MMPROJ) | (1u << F_MODALITY))
#define F_RM_ONLY ((1u << F_PURGE))
/* `ps` non ha una superficie sua: guarda dove `serve` ascolta, quindi prende gli STESSI due bit
 * (`--host`/`--port`). Due maschere con gli stessi bit sono la stessa decisione (dove sta il
 * server), detta una volta e usata da due verbi. */
#define F_PS_ONLY ((1u << F_HOST) | (1u << F_PORT))
/* Le bandiere che SONO un fatto, non un dato: la forma `--flag=valore` è un errore per tutte con
 * UNA regola, invece che una rinuncia per una. `--quiet=1` e `--quiet=0` sono la stessa cosa, e
 * `--offline=` (vuoto) direbbe che qualcuno ha dato un valore a una parola che non lo prende: due
 * semantiche con la stessa grafia sono peggio di un rifiuto. */
#define F_BOOLEANI                                                                                             \
    ((1u << F_QUIET) | (1u << F_OFFLINE) | (1u << F_REPAIR) | (1u << F_ONCE) | (1u << F_JSON) |                \
     (1u << F_THINK) | (1u << F_OFFLOAD) | (1u << F_PURGE) | (1u << F_WALK) | (1u << F_ROOTS) |      \
     (1u << F_BATCH))

static const char *const FLN[F_N] = {
    "--quiet",    "--cache",        "--revision", "--endpoint", "--max-bytes", "--stall-timeout",
    "--stall-retries", "--dest",     "--offline",  "--repair",   "--host",      "--port",
    "--max-body", "--once",         "--vision-model", "--vision-mmproj", "--json", "--max-tokens", "--system", "--think",
    "--offload-experts", "--mmproj", "--modality", "--purge", "--opt", "--walk", "--max-depth", "--roots", "--type",
    "--parallel", "--batch"};

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
                if (!is_verb(a)) {
                    /* Non è un verbo: è un NOME DI MODELLO -> si assume `chat <nome>`.
                     * Se il modello non esiste, l'errore lo dà la risoluzione del nome (registry). */
                    snprintf(out->verb, sizeof out->verb, "chat");
                    snprintf(out->repo, sizeof out->repo, "%s", a);
                    verb_seen = 1;
                    repo_seen = 1;
                    continue;
                }
                snprintf(out->verb, sizeof out->verb, "%s", a);
                verb_seen = 1;
                continue;
            }
            if (repo_seen) {
                /* `run` ha DUE posizionali: `<modello> [prompt]`. La seconda parola non è un
                 * secondo modello (sarebbe un errore, come per `models`) ma il prompt one-shot:
                 * deciderlo dall'ordine, non da un flag, è ciò che rende `fastllm run m "ciao"`
                 * scrivibile. Per ogni altro verbo il secondo posizionale resta un errore. */
                if (!strcmp(out->verb, "run") && !out->prompt[0]) {
                    if (snprintf(out->prompt, sizeof out->prompt, "%s", a) >=
                        (int)sizeof out->prompt)
                        return fail(out, msg, msg_len, "run: il prompt supera %d byte",
                                    (int)sizeof out->prompt);
                    continue;
                }
                /* `add <nome> <path>`: il secondo posizionale è il PERCORSO del modello. */
                if (!strcmp(out->verb, "add") && !out->path[0]) {
                    if (snprintf(out->path, sizeof out->path, "%s", a) >= (int)sizeof out->path)
                        return fail(out, msg, msg_len, "add: il percorso supera %d byte",
                                    (int)sizeof out->path);
                    continue;
                }
                /* `paths add <dir>` / `paths rm <n>`: il sottocomando è `repo`, l'argomento è `path`. */
                if (!strcmp(out->verb, "paths") && !out->path[0]) {
                    if (snprintf(out->path, sizeof out->path, "%s", a) >= (int)sizeof out->path)
                        return fail(out, msg, msg_len, "paths: l'argomento supera %d byte",
                                    (int)sizeof out->path);
                    continue;
                }
                return fail(out, msg, msg_len, "troppi argomenti posizionali: %s", a);
            }
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
        if (!strcmp(name, "--json")) { out->json = 1; continue; }
        if (!strcmp(name, "--think")) { out->show_think = 1; continue; }
        if (!strcmp(name, "--offload-experts")) { out->offload_experts = 1; continue; }
        if (!strcmp(name, "--purge")) { out->purge = 1; continue; }
        if (!strcmp(name, "--walk")) { out->search_walk = 1; continue; }
        if (!strcmp(name, "--batch")) { out->serve.batch = 1; continue; }
        if (!strcmp(name, "--roots")) { out->search_roots = 1; continue; }

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
        } else if (!strcmp(name, "--vision-model")) {
            out->serve.vision_model = val;  /* `val` vive nell'argv del processo (come --host) */
        } else if (!strcmp(name, "--vision-mmproj")) {
            out->serve.vision_mmproj = val;
        } else if (!strcmp(name, "--port")) {
            /* 1..65535 e solo cifre: la stessa regola di `FASTLLM_PORT` (piano §14), perché due
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
        } else if (!strcmp(name, "--parallel")) {
            if (!parse_num(val, 1, 64, &n))
                return fail(out, msg, msg_len, "--parallel: '%s' non è un numero 1..64", val);
            out->serve.parallel = (unsigned)n;
        } else if (!strcmp(name, "--max-tokens")) {
            /* Il tetto vero lo applica il motore (`FMLX_ENGINE_MAX_NEW`): qui si valida solo che
             * sia un intero >= 1, perché `--max-tokens 0` è "genera zero token", cioè nessuna
             * risposta, e un limite che non lascia passare niente è un typo, non una richiesta. */
            if (!parse_num(val, 1, LLONG_MAX, &n))
                return fail(out, msg, msg_len, "--max-tokens: '%s' non è un numero >= 1", val);
            out->max_tokens = n;
        } else if (!strcmp(name, "--type")) {
            if (strcmp(val, "mlx") && strcmp(val, "gguf") && strcmp(val, "hf"))
                return fail(out, msg, msg_len, "--type: '%s' non è mlx|gguf|hf", val);
            snprintf(out->search_type, sizeof out->search_type, "%s", val);
        } else if (!strcmp(name, "--max-depth")) {
            if (!parse_num(val, 1, 4096, &n))
                return fail(out, msg, msg_len, "--max-depth: '%s' non è un numero >= 1", val);
            out->search_max_depth = (int)n;
        } else if (!strcmp(name, "--system")) {
            out->system = val;
        } else if (!strcmp(name, "--opt")) {
            /* Parametro del motore `nome=valore` (registro; nessuna env). Ripetibile. */
            if (!val || !strchr(val, '='))
                return fail(out, msg, msg_len, "--opt vuole nome=valore");
            if (out->n_opts < (int)(sizeof out->opts / sizeof out->opts[0]))
                snprintf(out->opts[out->n_opts++], sizeof out->opts[0], "%s", val);
            else
                return fail(out, msg, msg_len, "--opt: troppe opzioni");
        } else if (!strcmp(name, "--mmproj")) {
            snprintf(out->mmproj, sizeof out->mmproj, "%s", val);
        } else if (!strcmp(name, "--modality")) {
            if (strcmp(val, "chat") && strcmp(val, "vlm") && strcmp(val, "ocr") && strcmp(val, "audio"))
                return fail(out, msg, msg_len, "--modality: '%s' non è chat|vlm|ocr|audio", val);
            snprintf(out->modality, sizeof out->modality, "%s", val);
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
    if ((!strcmp(out->verb, "run") || !strcmp(out->verb, "chat")) && !out->repo[0]) {
        char v[16];
        snprintf(v, sizeof v, "%s", out->verb); /* `fail` azzera `out`: vedi sopra */
        return fail(out, msg, msg_len, "%s: manca il modello (es. org/modello)", v);
    }
    /* Registro locale (stile Ollama): `add <nome> <path>`; `rm`/`show <nome|#n>`. */
    if (!strcmp(out->verb, "add") && (!out->repo[0] || !out->path[0]))
        return fail(out, msg, msg_len,
                    "add: uso: add <nome> <path> [--mmproj <p>] [--modality chat|vlm|ocr|audio]");
    if ((!strcmp(out->verb, "rm") || !strcmp(out->verb, "show")) && !out->repo[0]) {
        char v[16];
        snprintf(v, sizeof v, "%s", out->verb);
        return fail(out, msg, msg_len, "%s: manca <nome|#n>", v);
    }
    /* `serve` e `ps` non hanno un argomento posizionale: accettare `serve mlx-model` e ascoltare
     * comunque sarebbe una bandiera che mente, solo scritta senza trattini, e per `ps` il modello
     * non c'entra — lui guarda dove ascolta `serve`. `models`/`list` LO hanno dal 2026-09-24 (il
     * filtro per repo) e `doctor` da subito: per loro UNO solo, il secondo è un errore — due filtri
     * non vogliono dire niente. */
    if (repo_seen && (!strcmp(out->verb, "serve") || !strcmp(out->verb, "ps"))) {
        char v[16];
        snprintf(v, sizeof v, "%s", out->verb); /* `fail` azzera `out`: vedi sopra */
        return fail(out, msg, msg_len, "%s: non prende argomenti (le opzioni di %s sono bandiere)", v,
                    v);
    }
    /* Una bandiera che IL VERBO NON LEGGE è un errore, non un'opzione ignorata: il motivo è lo
     * stesso per cui una bandiera sconosciuta lo è. `fastllm serve --dest x` sembra un comando che
     * dice due cose e ne fa una, e chi legge l'output non ha modo di sapere quale delle due è
     * saltata. Le bandiere condivise (`--quiet`, `--cache`) valgono per tutti i verbi e non stanno
     * in nessun insieme esclusivo.
     *
     * LA REGOLA VALE FRA I VERBI CHE HANNO UNA SUPERFICIE DI OPZIONI. `run`/`chat` e `ps` ne hanno
     * una da quando il motore è cablato: `--port` dato a `run` È un errore, perché `run` non ascolta
     * su nessuna porta. Restano fuori i verbi SENZA opzioni proprie (`cp`, `rm`): per loro la
     * risposta onesta è ancora una sola — non esistono — ed è `NOT_WIRED` nel dispatch, non un
     * elenco di bandiere che NON accettano. */
    if (verb_seen) {
        unsigned allowed = (1u << F_QUIET) | (1u << F_CACHE) | (1u << F_OPT); /* condivise: sempre */
        int has_surface = 1, k;
        if (!strcmp(out->verb, "pull")) allowed |= F_PULL_ONLY;
        else if (!strcmp(out->verb, "serve")) allowed |= F_SERVE_ONLY;
        else if (!strcmp(out->verb, "models") || !strcmp(out->verb, "list") || !strcmp(out->verb, "ls"))
            allowed |= F_MODELS_ONLY;
        else if (!strcmp(out->verb, "doctor")) allowed |= 0; /* solo le condivise */
        else if (!strcmp(out->verb, "run") || !strcmp(out->verb, "chat")) allowed |= F_RUNCHAT_ONLY;
        else if (!strcmp(out->verb, "ps")) allowed |= F_PS_ONLY;
        else if (!strcmp(out->verb, "add")) allowed |= F_ADD_ONLY;
        else if (!strcmp(out->verb, "paths")) allowed |= (1u << F_JSON);
        else if (!strcmp(out->verb, "search"))
            allowed |= (1u << F_JSON) | (1u << F_WALK) | (1u << F_MAXDEPTH) | (1u << F_ROOTS) | (1u << F_TYPE);
        else if (!strcmp(out->verb, "test")) allowed |= 0; /* solo le condivise (--quiet) */
        else if (!strcmp(out->verb, "rm")) allowed |= F_RM_ONLY;
        else if (!strcmp(out->verb, "show") || !strcmp(out->verb, "help")) allowed |= 0;
        else has_surface = 0; /* cp: nessuna superficie da difendere */
        for (k = 0; has_surface && k < F_N; k++)
            if (!(allowed & (1u << k)) && (seen & (1u << k))) {
                char vv[16];
                snprintf(vv, sizeof vv, "%s", out->verb); /* `fail` azzera `out`: vedi sopra */
                return fail(out, msg, msg_len, "%s non è un'opzione di %s", FLN[k], vv);
            }
    }
    return FMLX_CLI_OK;
}

static void fput_json_escaped(FILE *out, const char *s) {
    if (!s) return;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { fputc('\\', out); fputc(c, out); }
        else if (c == '\n') fputs("\\n", out);
        else if (c < 0x20) fprintf(out, "\\u%04x", c);
        else fputc(c, out);
    }
}

static FmlxCliStatus fmlx_options_exec(const FmlxCliArgs *a, FILE *out, FILE *err) {
    int n, i;
    (void)err;
    if (!out) out = stdout;
    n = fmlx_engine_opt_count();
    if (a && a->json) {
        fprintf(out, "[\n");
        for (i = 0; i < n; i++) {
            const char *nm = NULL, *desc = NULL;
            char val[64] = "", ex[96] = "";
            fmlx_engine_opt_at(i, &nm, &desc, val, sizeof val);
            fmlx_engine_opt_example(i, ex, sizeof ex);
            fprintf(out, "  {\"name\":\"");      fput_json_escaped(out, nm ? nm : "");
            fprintf(out, "\",\"value\":\"");     fput_json_escaped(out, val);
            fprintf(out, "\",\"example\":\"");   fput_json_escaped(out, ex);
            fprintf(out, "\",\"desc\":\"");      fput_json_escaped(out, desc ? desc : "");
            fprintf(out, "\"}%s\n", (i + 1 < n) ? "," : "");
        }
        fprintf(out, "]\n");
        return FMLX_CLI_OK;
    }
    fprintf(out, "\nOpzioni del motore (registro: parametri SENZA variabili d'ambiente).\n"
                 "Si impostano con `--opt <nome>=<valore>` (ripetibile), prima del load, o da BASIC.\n"
                 "Ogni voce ha il valore corrente e un ESEMPIO d'uso.\n\n");
    for (i = 0; i < n; i++) {
        const char *nm = NULL, *desc = NULL;
        char val[64] = "", ex[96] = "";
        fmlx_engine_opt_at(i, &nm, &desc, val, sizeof val);
        fmlx_engine_opt_example(i, ex, sizeof ex);
        fprintf(out, "  %-22s = %-12s %s\n", nm ? nm : "?", val, desc ? desc : "");
        if (nm && ex[0])
            fprintf(out, "  %-22s   es.  fastllm --opt %s=%s run <modello>\n", "", nm, ex);
    }
    fprintf(out, "\nQuesti parametri valgono per TUTTI i comandi che caricano il modello (run, chat,\n"
                 "serve). Vedi la guida completa: LLMEnv/docs/fastllm-guide.md.\n"
                 "\nEsempi combinati:\n"
                 "  fastllm options                              # questo elenco\n"
                 "  fastllm run ds4                              # default (contesto auto, thinking off)\n"
                 "  fastllm --opt context=131072 run ds4         # contesto 128k\n"
                 "  fastllm --opt thinking=on --opt temp=0 run ds4\n"
                 "  fastllm --opt offload=on --opt offload_cache_mb=16384 run ds4\n"
                 "  fastllm --opt kvquant=8:8192 run ds4         # KV int8 oltre 8192 token\n"
                 "  fastllm --opt dtype=bf16 --opt kvdisk=on run ds4\n"
                 "  fastllm --opt prefix_cache=off run ds4       # prefix-cache solo in-RAM (niente disco)\n"
                 "  fastllm --opt dflash_model=~/ai/Models/Qwen3.8-27B-DFlash2 run ds4\n");
    return FMLX_CLI_OK;
}

FmlxCliStatus fmlx_cli_exec_with(const FmlxCliArgs *a_in, const FmlxHfTransport *t, FILE *out,
                                 FILE *err) {
    FmlxCliArgs ac;
    const FmlxCliArgs *a;
    FmlxPullReport rep;
    FmlxHfStatus s;

    if (!a_in) return FMLX_CLI_USAGE;
    ac = *a_in;
    a = &ac;
    /* Parametri del motore (`--opt nome=valore`): applicati PRIMA di ogni load (registro, no env). */
    for (int oi = 0; oi < a->n_opts; oi++) {
        char kv[192];
        char *eq;
        snprintf(kv, sizeof kv, "%s", a->opts[oi]);
        eq = strchr(kv, '=');
        if (!eq) continue;
        *eq = '\0';
        {
            int rc = fmlx_engine_opt_set(kv, eq + 1);
            if (rc != 0) {
                if (err) fprintf(err, "fastllm: opzione '%s' %s\n", kv,
                                 rc == -1 ? "sconosciuta" : "valore non valido");
                return FMLX_CLI_USAGE;
            }
        }
    }
    /* `offload` del registro = stesso effetto del parametro `--offload-experts`. */
    {
        int off = 0;
        if (fmlx_engine_opt_get_bool("offload", &off) == 0 && off) ac.offload_experts = 1;
    }
    /* Solo `run`/`chat` sono interattivi: possono CHIEDERE (Y/N) di attivare `--offload-experts`
     * e usano il default "thinking OFF". `serve` no. */
    {
        int rc_verb = (!strcmp(a->verb, "run") || !strcmp(a->verb, "chat"));
        fmlx_engine_set_interactive(rc_verb ? 1 : 0);
        /* thinking: il PARAMETRO del registro vince se impostato, altrimenti `--think`. */
        {
            int th = -1;
            int use = fmlx_engine_opt_get_bool("thinking", &th);
            if (rc_verb) fmlx_engine_set_think(use == 0 ? th : a->show_think);
        }
    }
    /* `run`/`chat`: `<nome|#n>` del registro → percorso. Se non è nel registro resta com'è (path
     * locale, cache HF, $FMLX_MODELS_DIR): un nome non registrato non è un errore, è un'altra via. */
    if ((!strcmp(a->verb, "run") || !strcmp(a->verb, "chat")) && a->repo[0]) {
        char rp[FMLX_HF_PATH_MAX];
        if (fmlx_registry_resolve(a->repo, rp, sizeof rp, NULL, 0, NULL, 0))
            snprintf(ac.repo, sizeof ac.repo, "%s", rp);
    }
    if (a->want_help) {
        if (a->verb[0] && strcmp(a->verb, "help"))
            usage_verb(out ? out : stdout, a->verb);
        else
            fmlx_cli_usage(out ? out : stdout);
        return FMLX_CLI_OK;
    }
    if (!a->verb[0]) {
        fmlx_cli_usage(err ? err : stderr);
        return FMLX_CLI_USAGE;
    }
    if (!strcmp(a->verb, "help")) {
        if (a->repo[0]) usage_verb(out ? out : stdout, a->repo);
        else fmlx_cli_usage(out ? out : stdout);
        return FMLX_CLI_OK;
    }
    /* Registro locale (stile Ollama). */
    if (!strcmp(a->verb, "add")) return fmlx_registry_add_exec(a, out, err);
    if (!strcmp(a->verb, "rm")) return fmlx_registry_rm_exec(a, out, err);
    if (!strcmp(a->verb, "show")) return fmlx_registry_show_exec(a, out, err);
    if (!strcmp(a->verb, "list") || !strcmp(a->verb, "ls"))
        return fmlx_registry_list_exec(a, out, err);
    if (!strcmp(a->verb, "models"))
        return fmlx_models_exec(a, out, err);
    if (!strcmp(a->verb, "paths")) return fmlx_paths_exec(a, out, err);
    if (!strcmp(a->verb, "search")) return fmlx_search_exec(a, out, err);
    if (!strcmp(a->verb, "test")) return fmlx_test_exec(a, out, err);
    if (!strcmp(a->verb, "doctor"))
        /* Nemmeno qui il transport viene guardato: l'audit hashà ciò che è già su disco, e una
         * rete finta non aggiungerebbe nessuna prova — semmai ne toglierebbe, perché il verdetto
         * dovrebbe dipendere da qualcosa che non c'entra con i byte da controllare. */
        return fmlx_doctor_exec(a, out, err);
    if (!strcmp(a->verb, "serve"))
        /* Nemmeno qui `t` viene guardato: il `serve` legge il cache da disco e scrive su socket.
         * Passare un transport falso a `serve` non è un test, è un'illusione di test. */
        return fmlx_serve_exec(a, out, err);
    if (!strcmp(a->verb, "run"))
        /* Il motore è cablato: nessun transport di rete (la generazione non è un download), quindi
         * `t` non viene guardato — come per `serve`. */
        return fmlx_run_exec(a, out, err);
    if (!strcmp(a->verb, "chat")) return fmlx_chat_exec(a, out, err);
    if (!strcmp(a->verb, "options")) return fmlx_options_exec(a, out, err);
    if (!strcmp(a->verb, "ps"))
        /* `ps` PARLA in rete, ma non con Hugging Face: usa un socket verso il server locale, e il
         * transport HF non ha niente a che vedere. */
        return fmlx_ps_exec(a, out, err);
    if (strcmp(a->verb, "pull")) {
        fprintf(err ? err : stderr,
                "fastllm %s: %s\n", a->verb, missing_piece(a->verb));
        return FMLX_CLI_NOT_WIRED;
    }

    memset(&rep, 0, sizeof rep);
    s = fmlx_pull_with(a->repo, a->revision, &a->pull, t, &rep, a->quiet ? NULL : (out ? out : stdout));
    if (s != FMLX_HF_OK) {
        /* Il report porta già il messaggio della lane che ha fallito; qui si aggiunge solo il
         * codice, perché "pull fallito" senza dire perché è il tipo di riga che fa perdere ore. */
        fprintf(err ? err : stderr, "fastllm pull: %s [%s]\n", rep.error_msg,
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
        fprintf(err ? err : stderr, "fastllm: %s\n", msg);
        return s;
    }
    return fmlx_cli_exec_with(&a, fmlx_hf_transport_curl(), out, err);
}
