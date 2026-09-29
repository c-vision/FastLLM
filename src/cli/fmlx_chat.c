/* fmlx_chat.c — `fastllm chat <modello>`: una conversazione multi-turno sullo stesso motore.
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
#include "engine/fmlx_engine.h" /* offload esperti */

#include <stdlib.h>
#include <string.h>
#include <sys/types.h> /* ssize_t */
#include <unistd.h>    /* isatty: clear screen/colore solo su terminale */

static void chat_help(FILE *out) {
    fprintf(out,
            "comandi:\n"
            "  /exit, /quit      esci\n"
            "  /clear            dimentica la conversazione (il system resta)\n"
            "  /system <testo>   imposta il messaggio di sistema (senza testo lo cancella)\n"
            "  /thinking [on|off] abilita/disabilita il reasoning (senza argomento: inverte; default off)\n"
            "  /prompt <n>       capienza del contesto/KV in token (es. /prompt 100000)\n"
            "  /compact          riassume la conversazione e la sostituisce col riassunto\n"
            "  /new              nuova sessione: svuota cronologia e cache KV\n"
            "  /help             questo testo\n");
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
    int thinking = a && a->show_think ? 1 : 0;   /* reasoning OFF di default; /thinking lo attiva */

    if (!a || !a->repo[0]) {
        fprintf(err ? err : stderr, "fastllm chat: manca il modello (es. org/modello)\n");
        return FMLX_CLI_USAGE;
    }
    if (!out) out = stdout;
    if (!err) err = stderr;
    fmlx_engine_set_offload_experts(a->offload_experts);
    fmlx_engine_set_think(thinking);
    fmlx_chat_hist_init(&hist);
    if (a->system && *a->system) {
        system = malloc(strlen(a->system) + 1);
        if (system) strcpy(system, a->system);
    }

    /* Banner + clear screen PRIMA del load: la diagnostica del motore esce all'avvio (sotto il
     * banner), non dopo il primo messaggio. SOLO su terminale: in una pipe il banner/clear
     * sporcherebbe l'output del modello. */
    if (isatty(fileno(out))) fmlx_banner(out, "modello", a->repo, 1);
    {
        FmlxCliStatus ps = fmlx_cli_preload(a->pull.fetch.cache_root, a->repo, err);
        if (ps != FMLX_CLI_OK) {
            free(line);
            free(system);
            fmlx_chat_hist_free(&hist);
            return ps;
        }
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
            if (!strncmp(line, "/thinking", 9) && (line[9] == '\0' || line[9] == ' ')) {
                const char *arg = line[9] ? line + 10 : "";
                while (*arg == ' ') arg++;
                if (!*arg) thinking = !thinking;
                else if (!strcmp(arg, "on") || !strcmp(arg, "1")) thinking = 1;
                else if (!strcmp(arg, "off") || !strcmp(arg, "0")) thinking = 0;
                else { fprintf(out, "uso: /thinking [on|off]\n"); continue; }
                fmlx_engine_set_think(thinking);
                fprintf(out, "reasoning %s\n", thinking ? "attivo" : "disattivato");
                continue;
            }
            if (!strncmp(line, "/prompt", 7) && (line[7] == '\0' || line[7] == ' ')) {
                const char *arg = line[7] ? line + 8 : "";
                char *end = NULL;
                long v;
                while (*arg == ' ') arg++;
                v = strtol(arg, &end, 10);
                if (!*arg || !end || *end || v <= 0) {
                    fprintf(out, "uso: /prompt <token> (es. /prompt 100000)\n");
                    continue;
                }
                {
                    char e[256];
                    FmlxEngineModel *m = fmlx_engine_model(a->pull.fetch.cache_root, a->repo,
                                                           e, sizeof e);
                    if (!m) { fprintf(out, "modello non risolto: %s\n", e); continue; }
                    {
                        int rc = fmlx_engine_set_ctx(m, (int)v);
                        if (rc == 0) fprintf(out, "contesto impostato a %ld token\n", v);
                        else fprintf(out, "contesto non impostabile (rc=%d: motore/ modello non lo supporta)\n", rc);
                    }
                }
                continue;
            }
            if (!strcmp(line, "/compact")) {
                if (hist.n == 0) { fprintf(out, "niente da compattare\n"); continue; }
                size_t tl = 0, i;
                char *tp = NULL, *summary = NULL;
                FmlxChatHist tmp;
                for (i = 0; i < hist.n; i++)
                    tl += strlen(hist.v[i].role) + strlen(hist.v[i].content) + 8;
                tp = malloc(tl + 512);
                if (!tp) { fprintf(err, "fastllm chat: out of memory\n"); last = FMLX_CLI_FAILED; break; }
                {
                    size_t o = 0, lim = tl + 512;
                    o += (size_t)snprintf(tp + o, lim - o,
                        "Riassumi in modo conciso e fedele la conversazione seguente: elenca fatti, "
                        "decisioni e contesto utili a proseguirla. Non aggiungere commenti e non "
                        "rispondere alla conversazione.\n\n=== Conversazione ===\n");
                    for (i = 0; i < hist.n; i++)
                        o += (size_t)snprintf(tp + o, lim - o, "%s: %s\n", hist.v[i].role, hist.v[i].content);
                }
                fmlx_chat_hist_init(&tmp);
                fmlx_chat_hist_push(&tmp, "user", tp);
                free(tp);
                fprintf(out, "compattazione della conversazione…\n");
                {
                    FmlxCliStatus cst = fmlx_cli_generate_stream(a->pull.fetch.cache_root, a->repo,
                                            NULL, &tmp, 1024, 0, out, err, &summary);
                    fmlx_chat_hist_free(&tmp);
                    if (cst != FMLX_CLI_OK || !summary || !*summary) {
                        fprintf(out, "compattazione fallita\n");
                        free(summary);
                        continue;
                    }
                }
                /* La conversazione diventa il solo riassunto: meno token, stesso contesto utile. */
                fmlx_chat_hist_clear(&hist);
                {
                    size_t ml = strlen(summary) + 96;
                    char *mm = malloc(ml);
                    if (mm) {
                        snprintf(mm, ml, "Contesto (riassunto della conversazione precedente):\n%s", summary);
                        fmlx_chat_hist_push(&hist, "user", mm);
                        fmlx_chat_hist_push(&hist, "assistant", "Ok, procedo con questo contesto.");
                        free(mm);
                    }
                }
                fprintf(out, "\nconversazione compattata\n");
                free(summary);
                continue;
            }
            if (!strcmp(line, "/new")) {
                free(system);
                system = (a->system && *a->system) ? strdup(a->system) : NULL;
                fmlx_chat_hist_clear(&hist);
                {
                    char e[256];
                    FmlxEngineModel *m = fmlx_engine_model(a->pull.fetch.cache_root, a->repo, e, sizeof e);
                    if (m) fmlx_engine_new_session(m);
                }
                fprintf(out, "nuova sessione (cronologia svuotata, cache KV azzerata)\n");
                continue;
            }
            /* Un comando sconosciuto NON è un messaggio: dirlo evita che `/exitt` (col refuso)
             * parta come testo al modello e l'utente creda di essere uscito. */
            fprintf(out, "comando sconosciuto: %s (prova /help)\n", line);
            continue;
        }

        if (fmlx_chat_hist_push(&hist, "user", line) != 0) {
            fprintf(err, "fastllm chat: out of memory\n");
            last = FMLX_CLI_FAILED;
            break;
        }
        {
            char *text = NULL;
            FmlxCliStatus st = fmlx_cli_generate_stream(a->pull.fetch.cache_root, a->repo, system,
                                                        &hist, a->max_tokens, thinking, out, err,
                                                        &text);
            if (st != FMLX_CLI_OK) {
                hist_pop(&hist); /* il turno fallito non entra nella cronologia */
                last = st;
                if (st == FMLX_CLI_FAILED) break; /* modello irraggiungibile: non c'è un turno dopo */
                continue;
            }
            if (fmlx_chat_hist_push(&hist, "assistant", text ? text : "") != 0) {
                fprintf(err, "fastllm chat: out of memory\n");
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
