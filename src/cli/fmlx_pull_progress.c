/* fmlx_pull_progress.c — la riga di avanzamento del `pull` (piano §13, "stile Ollama").
 *
 * Tre scelte che tengono insieme il file.
 *
 *  1. SU UN NON-TTY NON ESCA NIENTE. Il terminale è l'unico posto dove `\r` significa "torna
 *     indietro"; in una pipe, in un log e in `$( )` vuol dire "sovrascrivi la riga che stai
 *     leggendo", e chi riapre il file si trova un caratterino invisibile ogni 100 ms. Il chiamante
 *     in quel caso stampa la propria riga per-file (esistente da prima) e qui non si muove nulla.
 *
 *  2. UN RIDISEGNO AL SECONDO, non uno per chunk. A 500 MB/s la callback di curl gira migliaia di
 *     volte al secondo: scrivere al terminale a ogni giro è più lento del download stesso, ed è il
 *     modo in cui una UI diventa il collo di bottiglia di ciò che mostra. L'ultima foto invece si
 *     scrive SEMPRE (`end`), perché "si è fermato al 40%" stampato per ultimo è una bugia — «sempre»
 *     significa che il capo riga arriva sempre, non che si riscrivono byte identici: due frame
 *     UGUALI di fila sono rumore, e il terminale vero del 2026-09-24 li ha mostrati (l'ultimo chunk
 *     di un file capitava nello stesso secondo in cui `end` ridisegna la stessa riga).
 *
 *  3. I NUMERI SONO MISURATI O NON CI SONO. Velocità ed eta compaiono solo dal SECONDO campione in
 *     poi: al primo giro il denominatore dell'operazione è zero, e "0 B/s, eta inf" è il modo in cui
 *     una CLI insegna all'utente a non fidarsi delle sue cifre. Stessa regola per la barra: senza
 *     una lunghezza dichiarata dal server non si disegna nessun progresso percentuale, si scrivono
 *     i byte — che sono un fatto, la percentuale sarebbe un'invenzione.
 */
#include "fmlx_cli_internal.h"

#include <string.h>
#include <inttypes.h>

#define BAR_CELLS 24 /* la larghezza della barra in caratteri: 24 sta in un terminale da 80 colonne
                      * insieme al nome del file, che è la riga più lunga che ci passa */

/* Ciò che si sa del file corrente, tenuto separato dal resto perché `end` deve poter scrivere la
 * foto finale anche dopo un `bytes` mai arrivato (file già nel cache: zero chunk, zero callback). */
typedef struct {
    int64_t done, total;
    double rate;   /* byte/s, <0 = non misurabile */
    int64_t eta;   /* secondi, <0 = non misurabile */
} Snap;

static void snap(const FmlxPullProgress *p, int64_t done, int64_t total, int64_t now, Snap *s) {
    int64_t el;
    s->done = done;
    s->total = total;
    s->rate = -1;
    s->eta = -1;
    if (p->t0 < 0 || now <= p->t0) return; /* un clock che non è avanzato non è una misura */
    el = now - p->t0;
    if (done > p->base) {
        s->rate = (double)(done - p->base) / (double)el;
        if (total > 0 && s->rate > 0) {
            int64_t left = total - done;
            s->eta = left > 0 ? (int64_t)((double)left / s->rate) : 0;
        }
    }
}

/* La coda della riga: byte, velocità, eta — ognuna delle tre SPUTATA se non c'è il numero. */
static void tail(Snap *s, char *out, size_t n) {
    char d[32], t[32];
    size_t used;

    fmlx_cli_fmt_bytes(s->done, d, sizeof d);
    if (s->total > 0) {
        fmlx_cli_fmt_bytes(s->total, t, sizeof t);
        snprintf(out, n, "%s/%s", d, t);
    } else {
        snprintf(out, n, "%s", d); /* il server non ha dichiarato una lunghezza: nessun "/di" */
    }
    used = strlen(out);
    if (s->rate >= 0) {
        char r[32];
        fmlx_cli_fmt_bytes((int64_t)s->rate, r, sizeof r);
        snprintf(out + used, n - used, "  %s/s", r);
        used = strlen(out);
    }
    if (s->eta >= 0) {
        if (s->eta < 60) snprintf(out + used, n - used, "  eta %llds", (long long)s->eta);
        else if (s->eta < 3600) snprintf(out + used, n - used, "  eta %lldm%02llds",
                                         (long long)(s->eta / 60), (long long)(s->eta % 60));
        else snprintf(out + used, n - used, "  eta %lldh%02lldm", (long long)(s->eta / 3600),
                      (long long)((s->eta % 3600) / 60));
    }
}

static void draw(FmlxPullProgress *p, int64_t done, int64_t total, int64_t now) {
    char body[512], rate[80];
    Snap s;
    int pct = -1, filled = 0;
    size_t i, used;

    snap(p, done, total, now, &s);
    if (s.total > 0) {
        /* `done` oltre la promessa è possibile (un server che ha dichiarato male): la barra si
         * FERMA al 100%, non disegna un indice negativo o una cella oltre la fine. */
        double f = (double)s.done / (double)s.total;
        if (f > 1.0) f = 1.0;
        pct = (int)(f * 100.0);
        filled = (int)(f * BAR_CELLS + 0.5);
    }
    tail(&s, rate, sizeof rate);
    if (pct < 0) snprintf(body, sizeof body, "  [%zu/%zu] %s  %s", p->idx, p->nfiles, p->name, rate);
    else {
        char bar[BAR_CELLS + 1];
        for (i = 0; i < BAR_CELLS; i++) bar[i] = (int)i < filled ? '#' : '-';
        bar[BAR_CELLS] = '\0';
        snprintf(body, sizeof body, "  [%zu/%zu] %s  %3d%% [%s] %s", p->idx, p->nfiles, p->name, pct,
                 bar, rate);
    }
    /* Il padding cancella ciò che la riga precedente aveva di più lungo: senza, il resto di una
     * riga da 90 colonne resta sullo schermo sotto una da 60. */
    used = strlen(body);
    if (p->drew && !strcmp(body, p->last_body)) return; /* la STESSA foto è già a schermo */
    fputc('\r', p->out);
    fputs(body, p->out);
    if (used < (size_t)p->wide)
        for (i = used; i < (size_t)p->wide; i++) fputc(' ', p->out);
    if ((int)used > p->wide) p->wide = (int)used;
    snprintf(p->last_body, sizeof p->last_body, "%s", body);
    p->drew = 1;
}

void fmlx_pull_progress_init(FmlxPullProgress *p, FILE *out, int tty) {
    memset(p, 0, sizeof *p);
    p->out = out;
    p->tty = tty;
    p->t0 = -1;
}

int fmlx_pull_progress_inplace(const FmlxPullProgress *p) { return p->tty; }

void fmlx_pull_progress_begin(FmlxPullProgress *p, size_t idx, size_t nfiles, const char *name,
                              int64_t total_hint, int64_t now) {
    if (!p->tty || !p->out) return;
    p->idx = idx;
    p->nfiles = nfiles;
    snprintf(p->name, sizeof p->name, "%s", name ? name : "?");
    p->total = total_hint;
    p->t0 = -1; /* il primo campione fissa l'istante, e solo quello: `now` qui non è una misura */
    p->base = 0;
    p->done = 0;
    p->last_t = now;
    p->wide = 0;
    p->drew = 0; /* ogni file è una riga nuova: il confronto «stessa foto» riparte da zero */
    p->last_body[0] = '\0';
    p->open = 1;
    draw(p, 0, p->total, now);
}

void fmlx_pull_progress_bytes(FmlxPullProgress *p, int64_t done, int64_t total_hint, int64_t now) {
    if (!p->tty || !p->out || !p->open) return;
    /* Il `total_hint` del chunk vince su quello del manifesto quando il manifesto tace: è ciò che
     * il server ha detto con la RISPOSTA, non con l'elenco dei file. */
    if (total_hint > 0) p->total = total_hint;
    if (p->t0 < 0) {
        p->t0 = now;
        p->base = done; /* ripresa: questi byte non li abbiamo fatti adesso */
    }
    p->done = done;
    if (now == p->last_t) return; /* UN ridisegno per secondo: vedi la regola 2 in testa al file */
    p->last_t = now;
    draw(p, done, p->total, now);
}

void fmlx_pull_progress_end(FmlxPullProgress *p, int64_t final_bytes, int64_t now) {
    if (!p->tty || !p->out) return;
    if (final_bytes > 0) p->done = final_bytes;
    /* L'ultima foto si scrive SEMPRE, anche se il secondo non è passato: è l'unica che l'utente
     * legge, ed è l'unica che deve dire il numero giusto. La percentuale resta quella di `p->total`,
     * che può essere PIÙ di `done` se il server ha dichiarato male: meglio una barra all'80% con
     * sotto un errore di verifica che un 100% inventato. */
    draw(p, p->done, p->total, now);
    fputc('\n', p->out);
    p->open = 0;
    p->wide = 0;
}

void fmlx_pull_progress_abort(FmlxPullProgress *p) {
    if (!p->tty || !p->out || !p->open) return;
    /* Si va a capo SENZA ridisegnare: ciò che la riga dice è ciò che si è fermato, e riscriverlo
     * diverso da com'era sullo schermo sarebbe inventare. Il messaggio d'errore del chiamante
     * arriva sulla riga dopo, non in mezzo. */
    fputc('\n', p->out);
    p->open = 0;
    p->wide = 0;
}
