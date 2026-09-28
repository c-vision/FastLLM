/* fmlx_chat.c — `fastmlx chat <modello>`: una conversazione multi-turno sullo stesso motore.
 *
 * La differenza da `run` è UNA: qui la conversazione è ricordata. Ogni turno si rigenera il prompt
 * dalla cronologia INTERA con il chat template del modello, ed è per questo che la prefix cache ha
 * senso — il prefisso (system + turni vecchi) è già in KV, e il motore pre-filla solo la coda. Se la
 * cronologia non arrivasse al motore, il modello non ricorderebbe nulla e questo verbo sarebbe `run`
 * con un prompt decorativo.
 *
 * I comandi sono una manciata, e sono slash per la ragione di sempre: una riga che comincia con `/`
 * non può essere confusa con un messaggio, quindi non esiste il caso in cui l'utente vuole dire
 * `/clear` al modello e il terminale lo interpreta. I comandi che alterano il contesto lo DICONO
 * (`/clear` e `/system`), perché una modifica silenziosa alla cronologia è il modo in cui una
 * risposta sorprende chi l'ha chiesta.
 *
 * Il prompt utente è scritto su `out` PRIMA di leggere, così l'eco di un terminale e la riga
 * riscritta in un log concordano su chi ha detto cosa.
 */
#include "fmlx_cli_internal.h"

#include <stdlib.h>
#include <string.h>
#include <sys/types.h> /* ssize_t */

static void chat_help(FILE *out) {
    fprintf(out,
            "comandi:\n"
            "  /exit, /quit     esci\n"
            "  /clear           dimentica la conversazione (il system resta)\n"
            "  /system <testo>  imposta il messaggio di sistema (senza testo lo cancella)\n"
            "  /help            questo testo\n");
}

/* Toglie l'ultimo turno: serve a non lasciare un `user` orfano quando la generazione fallisce —
 * un contesto con una domanda e nessuna risposta confonderebbe il modello al turno successivo. */
static void hist_pop(FmlxChatHist *h) {
    if (!h || h->n == 0) return;
    h->n--;
    free(h->v[h->n].role);
    free(h->v[h->n].content);
}

FmlxCliStatus fmlx_chat_exec(const FmlxCliArgs *a, FILE *out, FILE *err) {
    FmlxChatHist hist;
    char *system = NULL;
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    FmlxCliStatus last = FMLX_CLI_OK;

    if (!a || !a->repo[0]) {
        fprintf(err ? err : stderr, "fastmlx chat: manca il modello (es. org/modello)\n");
        return FMLX_CLI_USAGE;
    }
    if (!out) out = stdout;
    if (!err) err = stderr;
    fmlx_chat_hist_init(&hist);
    if (a->system && *a->system) {
        system = malloc(strlen(a->system) + 1);
        if (system) strcpy(system, a->system);
    }

    for (;;) {
        fputs("you> ", out);
        fflush(out);
        n = getline(&line, &cap, stdin);
        if (n < 0) {
            fputc('\n', out);
            break;
        }
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (n == 0) continue;

        if (line[0] == '/') {
            if (!strcmp(line, "/exit") || !strcmp(line, "/quit")) break;
            if (!strcmp(line, "/help")) {
                chat_help(out);
                continue;
            }
            if (!strcmp(line, "/clear")) {
                fmlx_chat_hist_clear(&hist);
                fprintf(out, "cronologia cancellata\n");
                continue;
            }
            if (!strncmp(line, "/system", 7) && (line[7] == '\0' || line[7] == ' ')) {
                const char *arg = line[7] ? line + 8 : "";
                while (*arg == ' ') arg++;
                free(system);
                system = *arg ? strdup(arg) : NULL;
                fprintf(out, "system %s\n", system ? "impostato" : "cancellato");
                continue;
            }
            /* Un comando sconosciuto NON è un messaggio: dirlo evita che `/exitt` (col refuso)
             * parta come testo al modello e l'utente creda di essere uscito. */
            fprintf(out, "comando sconosciuto: %s (prova /help)\n", line);
            continue;
        }

        if (fmlx_chat_hist_push(&hist, "user", line) != 0) {
            fprintf(err, "fastmlx chat: out of memory\n");
            last = FMLX_CLI_FAILED;
            break;
        }
        {
            char *text = NULL;
            FmlxCliStatus st = fmlx_cli_generate_stream(a->pull.fetch.cache_root, a->repo, system,
                                                        &hist, a->max_tokens, out, err, &text);
            if (st != FMLX_CLI_OK) {
                hist_pop(&hist); /* il turno fallito non entra nella cronologia */
                last = st;
                if (st == FMLX_CLI_FAILED) break; /* modello irraggiungibile: non c'è un turno dopo */
                continue;
            }
            if (fmlx_chat_hist_push(&hist, "assistant", text ? text : "") != 0) {
                fprintf(err, "fastmlx chat: out of memory\n");
                free(text);
                last = FMLX_CLI_FAILED;
                break;
            }
            free(text);
        }
    }

    free(line);
    free(system);
    fmlx_chat_hist_free(&hist);
    return last;
}
