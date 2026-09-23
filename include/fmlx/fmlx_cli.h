/* fmlx_cli.h — il binario `fastmlx`: argv, `pull`, e ciò che il motore non ha ancora (piano §10.3, §13).

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
    const char *revision;        /* default "main": il default di Hugging Face, non una scelta nostra */
    FmlxPullOpts pull;
    FmlxServeOpts serve; /* le opzioni di `serve` stanno LÌ: una bandiera che esiste a CLI deve
                          * esistere anche nel server, o sarebbe una bandiera che mente (§1.1) */
    int quiet;
    int want_help; /* -h/--help: NON è un errore, e l'uscita è 0 */
} FmlxCliArgs;

/* ---- models / list ---- */

/* Elenca ciò che il cache contiene già, SENZA rete: non prende un transport perché la lane che
 * legge il disco un transport non ce l'ha (`fmlx_hf_catalog`). `a->pull.fetch.cache_root` è la
 * radice data con `--cache`, NULL = la regola d'ambiente della lane hf.
 * Con `a->quiet` una riga per repo (id e basta): la forma per i parse di una shell.
 * `out`/`err` separate come per il `pull`: l'elenco è dato, i problemi no. */
FmlxCliStatus fmlx_models_exec(const FmlxCliArgs *a, FILE *out, FILE *err);

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
