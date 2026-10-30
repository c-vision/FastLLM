/* fmlx_cli_internal.h — ciò che due file di `src/cli/` condividono, senza farlo diventare API.
 *
 * Questi simboli NON stanno in `include/fmlx/`: una bandiera della CLI non è un'interfaccia del
 * library consumer, e pubblicarla vorrebbe dire doversi giustificare con chi usa la libreria di
 * dettagli che riguardano solo come si riempie uno schermo.
 *
 * `fmlx_cli_fmt_bytes` è qui per una ragione precisa: le unità DECIMALI (1 kB = 1000 byte, come
 * Hugging Face pubblica le dimensioni) sono una DECISIONE, non una formattazione. Copiarla in due
 * file vuol dire avere due decisioni, e il caso in cui divergono — `models` dice 9.8 GB e `pull`
 * dice 9.1 GiB per gli stessi byte — è esattamente quello che l'utente userebbe per capire quale
 * delle due è sbagliata.
 */
#ifndef FMLX_CLI_INTERNAL_H
#define FMLX_CLI_INTERNAL_H

#include "fmlx/fmlx_cli.h" /* FmlxCliStatus + FmlxCliArgs: la base condivisa da run/chat/gen */
#include "fmlx/fmlx_hf.h"  /* FmlxHfStatus: il codice che `copy_stream` ritorna */

#include <stdint.h>
#include <stdio.h>

/* --- il formatore di dimensioni (definito in fmlx_models.c) --- */
void fmlx_cli_fmt_bytes(int64_t b, char *out, size_t n);

/* --- il filtro per repo (definito in fmlx_models.c) -------------------------------------------
 *
 * Due verbi fanno la stessa domanda — `models qwen` e `doctor qwen` — e se le due semantiche
 * divergono, l'utente impara che il filtro è una cosa di cui non fidarsi. Le TRE forme (solo la `*`
 * finale è ammessa, `nome` = ultima parte dell'id, `org/nome` = id intero, tutto case-insensitive)
 * stanno quindi in un posto solo, con le ragioni scritte dove sono state trovate. */
int fmlx_cli_filter_shape_ok(const char *f);
int fmlx_cli_filter_match(const char *f, const char *id);

/* --- il filtro dei FILE del `pull` (definito in fmlx_pull.c) ----------------------------------
 *
 * `pull --file <glob>` seleziona i file del manifesto. Il match è deliberatamente SEMPLICE e
 * dichiarato: `*` = qualsiasi sequenza (anche `/`), `?` = un carattere, tutto il resto letterale,
 * case-sensitive (i nomi dei file HF lo sono). Niente classi `[...]`, niente `**`, niente escape:
 * ogni costrutto in più è una cosa che l'utente può sbagliare senza accorgersene, e i nomi che
 * dobbiamo selezionare (`*Q4*`, `*DSpark*`, `mmproj-*.gguf`) non ne hanno bisogno.
 *
 * Non è `static` per la stessa ragione di `fmlx_pull_copy_stream`: il caso «il filtro è sbagliato»
 * deve poter essere eseguito da un test, e un `*.gguf` che combacia con `*.gguf.part` è il tipo di
 * bug che si vede solo con un pattern vero. */
int fmlx_pull_match(const char *pattern, const char *path);

/* --- l'avanzamento del `pull` (definito in fmlx_pull_progress.c) -----------------------------
 *
 * Una riga sola per file, che si RISCRIVE da sola con `\r` quando lo schermo è un terminale, e che
 * non emette NESSUN carattere di controllo quando non lo è: un log, una pipe e un file possono
 * finire dove capita, e riempirli di `\r` vuol dire renderli illeggibili a chi li riapre.
 *
 * L'orologio è un PARAMETRO (`now`, in secondi) e non `time(NULL)` dentro: senza questo non c'è
 * modo di provare il throttling — «una riga al secondo» vorrebbe dire un test che dorme, e un test
 * che dorme non è un test, è un rituale.
 *
 * Contratto d'uso: `begin` → N volte `bytes` → `end` (o `abort` se il file è fallito). `end`/`abort`
 * chiudono la riga ANCHE se non è mai stata aperta: un `\r` a metà schermo non deve mai precedere
 * il messaggio di errore. */
typedef struct {
    FILE *out;
    int tty;         /* 0 = modo pipes: nessun carattere di controllo, nessun ridisegno */
    int open;        /* 1 = una riga è aperta e va chiusa */
    size_t idx, nfiles;
    char name[256];
    int64_t t0;      /* il `now` del primo campione: da qui si calcola la velocità */
    int64_t base;    /* byte già presenti al primo campione (ripresa): non sono merito di adesso */
    int64_t done;    /* ultimo `done` visto: è ciò che si stampa a fine file */
    int64_t total;   /* lunghezza promessa dal server per il file, -1 = non dichiarata */
    int64_t last_t;  /* ultimo ridisegno: il throttling è di UNO per secondo */
    int wide;        /* riga più lunga già disegnata: serve a cancellare il resto */
    int drew;        /* 1 = almeno un ridisegno è partito: `end` deve andare a capo */
    char last_body[512]; /* l'ultima foto VERAMENTE scritta: la stessa non si riscrive due volte */
} FmlxPullProgress;

void fmlx_pull_progress_init(FmlxPullProgress *p, FILE *out, int tty);
/* `total_hint` = lunghezza dichiarata del file, o -1. */
void fmlx_pull_progress_begin(FmlxPullProgress *p, size_t idx, size_t nfiles, const char *name,
                              int64_t total_hint, int64_t now);
void fmlx_pull_progress_bytes(FmlxPullProgress *p, int64_t done, int64_t total_hint, int64_t now);
/* `final_bytes` è ciò che la lane dice che il file MISURA (può essere diverso dall'ultimo
 * campione: l'ultimo chunk arriva prima della verifica). */
void fmlx_pull_progress_end(FmlxPullProgress *p, int64_t final_bytes, int64_t now);
void fmlx_pull_progress_abort(FmlxPullProgress *p);
/* 1 se il chiamante NON deve stampare la propria riga per-file (lo fa il progressore). */
int fmlx_pull_progress_inplace(const FmlxPullProgress *p);

/* --- la copia dello specchio (definita in fmlx_pull.c) ----------------------------------------
 *
 * Sta qui, e non resta `static`, per un motivo misurabile: il ramo che la invoca (`link()` che rende
 * `EXDEV`, cioè cache e `--dest` su due volumi) non esiste su questa macchina, e un test sul ramo
 * falso NON sarebbe una prova. Chiamarla direttamente è l'unico modo di sapere che i byte copiati
 * sono gli stessi, che il temporaneo `.fmlx-part` non resta in giro e che l'avanzamento dice il
 * numero giusto. `prog` può essere NULL: è il caso del terminale che non chiede righe. */
FmlxHfStatus fmlx_pull_copy_stream(const char *src, const char *dst, FmlxPullProgress *prog);

/* --- la cronologia e la generazione a terminale (definite in fmlx_gen.c) ----------------------
 *
 * `run` e `chat` sono due modi di usare lo STESSO motore: cambia come si compone la richiesta (una
 * riga contro una conversazione), non come si generano i token. La parte che sa parlare al motore e
 * consumare gli eventi sta in UN posto, così un miglioramento dello streaming non va scritto due
 * volte — che è il modo in cui `run` e `chat` divergono fino a non assomigliarsi più.
 *
 * La cronologia è di TURNI in memoria (role/content), non un DOM JSON: il DOM si costruisce al
 * momento della generazione, perché le slice del parser puntano nel documento e il documento deve
 * vivere quanto la richiesta — la conversazione invece sopravvive a tutti i turni. */

typedef struct {
    char *role;
    char *content;
} FmlxChatTurn;

typedef struct {
    FmlxChatTurn *v;
    size_t n, cap;
} FmlxChatHist;

void fmlx_chat_hist_init(FmlxChatHist *h);
/* Copia `role` e `content` (possono essere NULL = stringa vuota). Ritorna 0, -1 su memoria. */
int fmlx_chat_hist_push(FmlxChatHist *h, const char *role, const char *content);
void fmlx_chat_hist_clear(FmlxChatHist *h);
void fmlx_chat_hist_free(FmlxChatHist *h);

/* Genera una risposta dalla `hist` (+ `system`) col motore già integrato, stampa il testo
 * token-live su `out`, e — se `out_text` non è NULL — ne restituisce una COPIA mallocata del testo
 * completo (il chiamante la libera). `max_tokens` <= 0 usa il default del motore.
 *
 * Criterio identico a un client della porta: si risolve il modello con la stessa `cache_root` e lo
 * stesso `name` di `serve`, si rende il chat template (Jinja) del modello, e si consuma
 * `fmlx_engine_next`. Nessuna seconda via al motore, nessuna copia della logica di render. */
FmlxCliStatus fmlx_cli_generate_stream(const char *cache_root, const char *name, const char *system,
                                       const FmlxChatHist *hist, long long max_tokens, int show_think,
                                       FILE *out, FILE *err, char **out_text);

/* Carica il modello nella cache residente SENZA generare. Serve ai verbi interattivi (`chat`):
 * la diagnostica di load (stderr) esce all'AVVIO, prima del primo prompt, invece che dopo il primo
 * messaggio. Idempotente: la generazione successiva trova il modello già residente e non ricarica. */
FmlxCliStatus fmlx_cli_preload(const char *cache_root, const char *name, FILE *err);

/* --- il banner di avvio (definito in fmlx_banner.c) -------------------------------------------
 * Big text "FastLLM" (port dell'algoritmo GMBasic) + autore/versione/licenza + una riga extra
 * `<tag>: <value>` (es. `modello: …` o `ascolto: host:porta`). `tty` = 1 abilita clear screen e
 * colore; 0 scrive solo il testo (niente escape in una pipe). */
void fmlx_banner(FILE *out, const char *tag, const char *value, int tty);

#endif /* FMLX_CLI_INTERNAL_H */
