/* fmlx_cli.h — il binario `fastllm`: argv, `pull`, e ciò che il motore non ha ancora (piano §10.3, §13).

 * Questa lane NON decide niente sui byte: le regole del cache stanno nella lane hf, il trasporto in
 * curl. Qui vivono DUE cose, ed è il motivo per cui esiste una lane separata:
 *  1. `pull`: prende il MANIFESTO (un repo, tutti i file), passa ciascuno alla lane download allo
 *     STESSO commit, e — se viene chiesto `--dest` — specchia il risultato fuori dal cache;
 *  2. l'argv: quali bandiere esistono, cosa vuol dire una bandiera sconosciuta, e cosa risponde un
 *     comando che oggi non può girare.
 *
 * Sul punto 2 c'è una regola più importante della comodità: un comando che richiede il motore
 * (`run`, `chat`, `serve`) risponde un errore CON IL SUO NOME ed esce 1. Non stampa un testo vuoto,
 * non finge di generare, non esce 0: un `serve` che rispondesse "ciao" farebbe scoprire al client
 * che la risposta è finta solo guardando output incoerenti — lo stesso divieto del §11.2 del piano.
 *
 * Il token NON ha una bandiera: arriva solo da `HF_TOKEN`. Metterlo negli argomenti vorrebbe dire
 * lasciarlo visibile nei processi altrui e nella cronologia della shell.
 */
#ifndef FMLX_CLI_H
#define FMLX_CLI_H

#include "fmlx/fmlx_fetch.h"
#include "fmlx/fmlx_serve.h"

#include <stdio.h>

/* Uscita del processo: 0 = fatto, 1 = fallito o non possibile, 2 = argv sbagliato (la convenzione
 * di getopt). `USAGE` non è lo stesso codice di un download fallito: uno è un errore di chi
 *digita, l'altro è il mondo esterno. */
#define FMLX_CLI_PROMPT_MAX 131072 /* il prompt one-shot di `run`; oltre, è un argv che non si legge */

typedef enum {
    FMLX_CLI_OK = 0,
    FMLX_CLI_USAGE = 1,     /* argv malformato, verbo sconosciuto, argomento obbligatorio assente */
    FMLX_CLI_NOT_WIRED = 2, /* il verbo esiste, ma chiede il motore: NON è un fallimento di rete */
    FMLX_CLI_FAILED = 3     /* la lane ha risposto un errore (il codice HF è nel report) */
} FmlxCliStatus;

const char *fmlx_cli_status_str(FmlxCliStatus s);

/* Testo d'uso: la lista di ciò che ESISTE e di ciò che non esiste ancora, senza farli sembrare
 *equivalenti. */
void fmlx_cli_usage(FILE *out);

/* ---- pull ---- */

/* Le opzioni del `pull` sono le opzioni della lane download PIÙ il `--dest`: nessuna copia di
 * campo, così `--stall-timeout` non può esistere in un posto e non nell'altro. */
typedef struct {
    FmlxHfFetchOpts fetch;
    const char *dest; /* NULL = il modello resta nel cache: non c'è un default implicito */
    /* `--file <glob>`, ripetibile (16 è un tetto dichiarato, non un silenzio): scarica SOLO i file
     * del manifesto che corrispondono ad ALMENO uno dei pattern. Vuoto = tutto il repo, che è il
     * comportamento storico e l'unico sensato per un repo di un modello solo.
     * Perché serve: i repo di ds4 contengono 4-8 varianti dello stesso modello (Q2/Q4/MXFP4/vision),
     * ~100-190 GB l'una; `pull` senza filtro le scaricherebbe TUTTE. I pattern usano `*` (qualsiasi
     * sequenza, anche `/`) e `?` (un carattere): `*.gguf`, `*Q4*`, e i file dentro una sottocartella.
     * L'esempio con la barra (`vision/` + `*.gguf`) non si può scrivere qui: la sequenza barra-asterisco
     * aprirebbe un commento dentro il commento. */
    const char *files[16];
    int n_files;
} FmlxPullOpts;

typedef struct {
    char commit[64];                     /* lo `sha` del manifesto: il commit di TUTTI i file */
    char snapshot_dir[FMLX_HF_PATH_MAX]; /* <root>/models--…/snapshots/<commit> */
    char dest_dir[FMLX_HF_PATH_MAX];     /* <dest>/<nome-del-repo>, "" se `--dest` è assente */
    size_t n_files;                      /* voci del manifesto */
    size_t n_ok;                         /* file raggiunti (nel cache o scaricati) */
    size_t n_mirrored;                   /* file specchiati in `dest_dir` */
    int64_t bytes_declared;              /* somma dei `size` dichiarati; gli sconosciuti contano 0 */
    FmlxHfStatus error;                  /* OK se è andato tutto bene */
    char error_file[FMLX_HF_PATH_MAX];   /* il file su cui `error` è accaduto, "" se non un file */
    char error_msg[FMLX_HF_PATH_MAX];    /* messaggio: della lane hf se viene da lì, nostro altrimenti */
} FmlxPullReport;

/*scarica (o ritrova) TUTTI i file del manifesto di `repo_id` alla `revision`.
 * Il commit è uno solo — quello del manifesto — e non la HEAD di ciascun file: è ciò che rende
 * possibile uno snapshot coerente se qualcuno pubblica durante il pull (huggingface_hub fa così:
 * risolve `repo_info.sha` una volta e scarica a quel commit).
 * Ferma al PRIMO errore: un modello a metà non è un modello, e `n_ok` dice dove si è arrivati.
 * `log` non-NULL riceve una riga per file e le righe finali con i path; NULL = silenzioso.
 * `t == NULL` usa il transport curl di sistema. */
FmlxHfStatus fmlx_pull_with(const char *repo_id, const char *revision, const FmlxPullOpts *opts,
                            const FmlxHfTransport *t, FmlxPullReport *rep, FILE *log);
FmlxHfStatus fmlx_pull(const char *repo_id, const char *revision, const FmlxPullOpts *opts,
                       FmlxPullReport *rep, FILE *log);

/* ---- argv ---- */

typedef struct {
    char verb[16];
    char repo[FMLX_HF_PATH_MAX]; /* l'argomento posizionale: vuoto se il verbo non ne vuole */
    char prompt[FMLX_CLI_PROMPT_MAX]; /* il SECONDO posizionale, solo di `run`: il prompt one-shot */
    char path[FMLX_HF_PATH_MAX]; /* il SECONDO posizionale di `add`: il percorso del modello */
    char mmproj[FMLX_HF_PATH_MAX]; /* --mmproj di `add`: il proiettore di un VLM/OCR */
    char modality[16];             /* --modality di `add`: chat|vlm|ocr|audio (default chat) */
    int purge;                     /* --purge di `rm`: cancella anche i file, non solo la voce */
    const char *revision;        /* default "main": il default di Hugging Face, non una scelta nostra */
    const char *system;          /* --system di run/chat: il testo di sistema, NULL se assente */
    long long max_tokens;        /* --max-tokens di run/chat: 0 = il default del motore */
    int show_think;              /* --think di run/chat: mostra il ragionamento (attenuato) */
    int offload_experts;         /* --offload-experts: streama gli esperti MoE da disco (niente env) */
    FmlxPullOpts pull;
    FmlxServeOpts serve; /* le opzioni di `serve` stanno LÌ: una bandiera che esiste a CLI deve
                          * esistere anche nel server, o sarebbe una bandiera che mente (§1.1) */
    int quiet;
    int json;      /* `models --json`: la stessa tabella in forma macchina-leggibile */
    int diffable;  /* `tensors --diffable`: omette offset/byte -> output deterministico (golden/diff) */
    int check_install;   /* `doctor --install`: controlla l'installazione (binario, motore LLMEnv, metallib, ABI) invece del cache */
    int upgrade_check;   /* `upgrade --check`: dice se c'e' una versione piu' nuova, non installa */
    int want_help; /* -h/--help: NON è un errore, e l'uscita è 0 */
    int search_walk;      /* `search --walk`: forza il walk del filesystem (niente Spotlight) */
    int search_max_depth; /* `search --max-depth N`: tetto di profondita' (0 = nessun tetto) */
    int search_roots;     /* `search --roots`: mostra le cartelle RADICE (tree), non i singoli modelli */
    char search_type[8];  /* `search --type`: filtra mlx|gguf|hf (vuoto = tutti) */
    const char *glob;     /* `compare --glob`: filtro glob sui nomi tensore, NULL = tutti */
    /* Parametri del motore (registro, NIENTE env): `--opt nome=valore`, ripetibile. */
    char opts[16][192];
    int  n_opts;
} FmlxCliArgs;

/* ---- run ---- */

/* `run <modello> [prompt]`: UNA generazione con il motore (chat template del modello, non una
 * stringa composta a mano), e il testo esce token-live su `out`. Con `[prompt]` è one-shot; senza,
 * legge una riga per volta da `stdin` e genera per ciascuna. La risoluzione del modello è quella
 * di `serve` (`fmlx_engine_model`: path, cache HF, $FMLX_MODELS_DIR), non una seconda regola. */
FmlxCliStatus fmlx_run_exec(const FmlxCliArgs *a, FILE *out, FILE *err);

/* ---- chat ---- */

/* `chat <modello>`: REPL multi-turno sopra lo STESSO motore. La cronologia è tenuta qui in
 * `FmlxChatHist` e resa a ogni turno col chat template (Jinja) del modello, così la prefix cache
 * riusa il prefisso. Comandi slash: `/exit` (o `/quit`), `/clear`, `/system <testo>`, `/help`. */
FmlxCliStatus fmlx_chat_exec(const FmlxCliArgs *a, FILE *out, FILE *err);

/* ---- ps ---- */

/* `ps`: se un `serve` è in ascolto sulla porta nota (`--port`/`FASTLLM_PORT`, default 8000) e quali
 * modelli sono residenti (interrogando `/v1/models/status`). La cache residente vive NEL processo del
 * server: questo verbo la OSSERVA via HTTP, non la possiede — è il criterio onesto finché non esiste
 * un registro di processi esterno. Nessun server in ascolto NON è un fallimento: è la risposta. */
FmlxCliStatus fmlx_ps_exec(const FmlxCliArgs *a, FILE *out, FILE *err);

/* ---- models / list ---- */

/* Elenca ciò che il cache contiene già, SENZA rete: non prende un transport perché la lane che
 * legge il disco un transport non ce l'ha (`fmlx_hf_catalog`). `a->pull.fetch.cache_root` è la
 * radice data con `--cache`, NULL = la regola d'ambiente della lane hf.
 * Con `a->quiet` una riga per repo (id e basta): la forma per i parse di una shell.
 * `out`/`err` separate come per il `pull`: l'elenco è dato, i problemi no. */
FmlxCliStatus fmlx_models_exec(const FmlxCliArgs *a, FILE *out, FILE *err);

/* `fastllm paths` — percorsi di ricerca modelli (file di config): list/add/rm/scan. */
FmlxCliStatus fmlx_paths_exec(const FmlxCliArgs *a, FILE *out, FILE *err);

/* `fastllm search [<path>]` — trova le cartelle-modello; senza <path>, su tutti i dischi accessibili. */
FmlxCliStatus fmlx_search_exec(const FmlxCliArgs *a, FILE *out, FILE *err);

/* `fastllm test <nome|#n|path>` — il modello e' completo su disco e non corrotto? */
FmlxCliStatus fmlx_test_exec(const FmlxCliArgs *a, FILE *out, FILE *err);

/* `fastllm compare <a.gguf> <b.gguf> [--glob G]` — confronto NUMERICO per-tensore tra due GGUF
 * (dequant + rel_l2 + avg_diff% normalizzato, stile antirez/gguf-tools). Lineage/frozen-layer e
 * breakdown per-ruolo. Core in LLMEnv/LLMCodec (`llmcodec_gguf_compare`), UNA SOLA SORGENTE. */
FmlxCliStatus fmlx_compare_exec(const FmlxCliArgs *a, FILE *out, FILE *err);

/* `fastllm tensors <model.gguf> [--diffable]` — dump di KV + tabella tensori (port di antirez
 * gguf-tools `show`). `--diffable` omette offset/byte -> output deterministico per golden/diff. */
FmlxCliStatus fmlx_tensors_exec(const FmlxCliArgs *a, FILE *out, FILE *err);

/* `fastllm asr <model_dir> <file.wav>` — speech-to-text Qwen3-ASR (engine nativo C, inference).
 * Il testo esce TOKEN-LIVE su `out` (streaming). Engine in LLMEnv/LLMEngine/asr (target `llm_asr`),
 * la STESSA sorgente usata da GMBasic (superficie BASIC `ASR.RECOGNIZE`). */
FmlxCliStatus fmlx_asr_exec(const FmlxCliArgs *a, FILE *out, FILE *err);
/* `gmbarch <url|org/nome|cartella>`: crea il descrittore `.gmbarch` dell'architettura nella cartella ABI di LLMEnv se manca. */
FmlxCliStatus fmlx_gmbarch_exec(const FmlxCliArgs *a, FILE *out, FILE *err);
/* Lo stesso controllo per una cartella modello gia' scaricata: lo chiamano `pull` e `add`. Scrive una riga `[abi] ...`. */
void fmlx_gmbarch_ensure_dir(const char *dir, FILE *out);
/* 1 se `path` e' un GGUF multimodale riconosciuto da general.architecture (parakeet -> audio, paddleocr -> ocr con il
 * `*-mmproj.gguf` accanto): scrive modality e mmproj. 0 = un normale modello di chat. */
int fmlx_media_detect(const char *path, char *modality, size_t mcap, char *mmproj, size_t pcap);
/* `run <alias> <file>` per i modelli ocr|vlm|audio: lane vision (llama.cpp/mtmd) oppure, per parakeet, l'engine ASR di LLMEnv. */
FmlxCliStatus fmlx_media_run_exec(const FmlxCliArgs *a, const char *model, const char *mmproj, const char *modality,
                                  FILE *out, FILE *err);

/* ---- registro locale (stile Ollama) — src/cli/fmlx_registry.c ---- */

/* `add <nome> <path> [--mmproj <p>] [--modality chat|vlm|ocr|audio]`: registra un modello locale
 * per percorso (dir MLX, `.gguf` ds4, o VLM con mmproj). Ri-aggiungere un nome lo aggiorna. */
FmlxCliStatus fmlx_registry_add_exec(const FmlxCliArgs *a, FILE *out, FILE *err);
/* `rm <nome|#n> [--purge]`: toglie la voce dal registro; `--purge` cancella anche i file. */
FmlxCliStatus fmlx_registry_rm_exec(const FmlxCliArgs *a, FILE *out, FILE *err);
/* `show <nome|#n>`: i dettagli di una voce. */
FmlxCliStatus fmlx_registry_show_exec(const FmlxCliArgs *a, FILE *out, FILE *err);
/* `list`: registro NUMERATO + i repo del cache HF non registrati. */
FmlxCliStatus fmlx_registry_list_exec(const FmlxCliArgs *a, FILE *out, FILE *err);
/* Risolve `<nome|#n>` dal registro: 1 = trovato (path/mmproj/modality scritti), 0 = non registrato.
 * Usato da `run`/`chat`/`serve` per accettare un modello per nome o per numero. */
int fmlx_registry_resolve(const char *spec, char *out_path, size_t cap, char *out_mmproj,
                          size_t mcap, char *out_modality, size_t modcap);

/* ---- doctor ---- */

/* Verifica l'integrità dei repo che il filtro sceglie, hashando i blob. Come `models` non prende
 * un transport: legge solo il disco, e la rete non c'è nemmeno da falsificare. L'uscita è la parte
 * che conta per uno script: `FAILED` vuol dire «c'è una corruzione» (o «un repo non ho potuto
 * guardarlo»), mentre le NOTE non la muovono — un `.incomplete` lasciato a metà download è un fatto.
 * Con `a->quiet` restano SOLO le righe di PROBLEMA su stdout, più il messaggio d'errore su stderr:
 * il silenzio in `--quiet` deve voler dire una cosa sola, «non c'è niente di rotto». */
FmlxCliStatus fmlx_doctor_exec(const FmlxCliArgs *a, FILE *out, FILE *err);
/* `doctor --install`: binario, motore LLMEnv (versione, variante, metallib, descrittori ABI), copie duplicate. */
FmlxCliStatus fmlx_doctor_install_exec(const FmlxCliArgs *a, FILE *out, FILE *err);
/* `version`: versione di fastllm e del motore caricato. `upgrade [--check]`: aggiorna fastllm e il motore. */
FmlxCliStatus fmlx_version_exec(const FmlxCliArgs *a, FILE *out, FILE *err);
FmlxCliStatus fmlx_upgrade_exec(const FmlxCliArgs *a, FILE *out, FILE *err);
/* 1 = il motore LLMEnv caricato e' compatibile con questo binario (o e' una build di sviluppo); 0 = no (messaggio su err). */
int fmlx_install_gate(FILE *err);

/* ---- serve ---- */

/* Il `serve` da terminale: risolve host/port/chiave (flag > ambiente > default, piano §14),
 * stampa UNA riga all'operatore e passa tutto al loop. NON ha un transport: non tocca la rete,
 * la ascolta. Con `a->quiet` niente riga di ascolto, ma i log per-richiesta restano su `err`:
 * un server che tace del tutto non è silenzioso, è opaco. */
FmlxCliStatus fmlx_serve_exec(const FmlxCliArgs *a, FILE *out, FILE *err);

/* Azzera SEMPRE `out`, anche quando fallisce: un args mezzo riempito in mano al dispatch è il modo
 * migliore per eseguire il verbo sbagliato. `msg` riceve la ragione (può essere NULL). */
FmlxCliStatus fmlx_cli_parse(int argc, char **argv, FmlxCliArgs *out, char *msg, size_t msg_len);

/* Esegue gli argomenti già parseati. `err` riceve gli errori, `out` il progresso e l'uso: separate
 * perché un `pull` in un pipe deve restare leggibile mentre l'errore va sul canale d'errore. */
FmlxCliStatus fmlx_cli_exec_with(const FmlxCliArgs *a, const FmlxHfTransport *t, FILE *out,
                                 FILE *err);
FmlxCliStatus fmlx_cli_exec(const FmlxCliArgs *a, FILE *out, FILE *err);

/* parse + exec: è ciò che chiama `main`. */
FmlxCliStatus fmlx_cli_run(int argc, char **argv, FILE *out, FILE *err);

#endif /* FMLX_CLI_H */
