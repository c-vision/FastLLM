/* fmlx_run.c — `fastmlx run <modello> [prompt]`: UNA generazione, subito, sul terminale.
 *
 * Il verbo che rende il motore usabile senza montare un server. Con `[prompt]` è one-shot; senza,
 * legge una riga per volta da stdin e genera per ciascuna — NON è `chat`: qui non c'è cronologia,
 * ogni riga è una richiesta nuova. La differenza non è di comodità, è di cosa significa: `chat`
 * costruisce contesto, `run` risponde e basta, e confonderli farebbe sembrare che la seconda riga
 * ricordi la prima quando non lo fa.
 *
 * L'uscita è il TESTO, e basta: niente intestazioni, niente contatori, niente prompt decorativi su
 * stdout — `fastmlx run m "..." | altro` deve ricevere esattamente ciò che il modello ha scritto.
 * I messaggi d'errore vanno su `err`, come per ogni altro verbo di questa CLI.
 *
 * La risoluzione del modello NON è una regola nuova: `fmlx_cli_generate_stream` chiama la stessa
 * `fmlx_engine_model` di `serve`, quindi path, cache HF e `$FMLX_MODELS_DIR` si comportano allo
 * stesso modo nei due verbi per costruzione, non per disciplina.
 */
#include "fmlx_cli_internal.h"

#include <stdlib.h>
#include <string.h>
#include <sys/types.h> /* ssize_t: il tipo di ritorno di `getline` */

FmlxCliStatus fmlx_run_exec(const FmlxCliArgs *a, FILE *out, FILE *err) {
    FmlxChatHist hist;
    FmlxCliStatus st = FMLX_CLI_OK; /* stdin senza righe non è un fallimento */

    if (!a || !a->repo[0]) {
        fprintf(err ? err : stderr, "fastmlx run: manca il modello (es. org/modello)\n");
        return FMLX_CLI_USAGE;
    }
    if (!out) out = stdout;
    if (!err) err = stderr;
    fmlx_chat_hist_init(&hist);

    if (a->prompt[0]) {
        char *text = NULL;
        if (fmlx_chat_hist_push(&hist, "user", a->prompt) != 0) {
            fmlx_chat_hist_free(&hist);
            fprintf(err, "fastmlx run: out of memory\n");
            return FMLX_CLI_FAILED;
        }
        st = fmlx_cli_generate_stream(a->pull.fetch.cache_root, a->repo, a->system, &hist,
                                      a->max_tokens, out, err, &text);
        free(text);
        fmlx_chat_hist_free(&hist);
        return st;
    }

    /* Modalità interattiva: una riga, una generazione, zero cronologia. `getline` invece di un
     * buffer fisso perché un prompt lungo non deve essere spezzato in due richieste silenziose. */
    {
        char *line = NULL;
        size_t cap = 0;
        ssize_t n;

        while ((n = getline(&line, &cap, stdin)) >= 0) {
            while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
            if (n == 0) continue;
            fmlx_chat_hist_clear(&hist);
            if (fmlx_chat_hist_push(&hist, "user", line) != 0) {
                fprintf(err, "fastmlx run: out of memory\n");
                free(line);
                fmlx_chat_hist_free(&hist);
                return FMLX_CLI_FAILED;
            }
            /* Un fallimento su una riga NON ferma le altre: è una sessione interattiva, e chi ha
             * sbagliato una riga vuole poter provare la successiva. Il codice d'uscita però resta
             * quello dell'ultimo esito, come per un `make` che continua e alla fine dice se è andata
             * male. */
            st = fmlx_cli_generate_stream(a->pull.fetch.cache_root, a->repo, a->system, &hist,
                                          a->max_tokens, out, err, NULL);
        }
        free(line);
        fmlx_chat_hist_free(&hist);
        return st;
    }
}
