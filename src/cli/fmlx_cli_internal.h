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

#include <stdio.h>
#include <stdint.h>

/* --- il formatore di dimensioni (definito in fmlx_models.c) --- */
void fmlx_cli_fmt_bytes(int64_t b, char *out, size_t n);

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
/* 1 se il chiamante NON deve stampare la propria riga per-è-file (lo fa il progressore). */
int fmlx_pull_progress_inplace(const FmlxPullProgress *p);

#endif /* FMLX_CLI_INTERNAL_H */
