/* fmlx_serve.c (CLI) — `fastllm serve`: risolvo le opzioni, faccio girare il loop, dico all'
 * operatore cosa è successo.
 *
 * Questo file è VOLUTAMENTE senza decisioni sul contenuto delle risposte: quella è la metà pura
 * (`src/server/fmlx_serve_respond.c`). Qui ci sono tre cose che un terminale deve fare e un loop
 * non può:
 *  1. risolvere host/porta/chiave secondo la precedenza del piano §14 (flag > ambiente > default)
 *     e dire ALL'OPERATORE quale è stata usata — un server che ascolta sulla porta sbagliata in
 *     silenzio è il bug che si scopre dal log di un altro processo;
 *  2. mettere in guardia contro l'unica configurazione davvero pericolosa: aprirsi oltre il
 *     loopback senza chiave;
 *  3. scegliere i canali. Gli errori e i log per-richiesta vanno su `err` (il loop li scrive
 *     italiano, e sono log); su `out` finisce UNA riga sola, il conto di ciò che è stato servito.
 *     Un server non ha un risultato da stampare: chi si aspetta output su stdout di un `serve`
 *     sta confondendo un demone con un comando.
 */
#include "fmlx/fmlx_cli.h"
#include "fmlx_cli_internal.h" /* fmlx_banner */
#include "engine/fmlx_engine.h" /* offload esperti */

#include <stdio.h>
#include <string.h>
#include <unistd.h> /* isatty: banner/clear solo su terminale */

/* 1 = l'host è raggiungibile solo dalla macchina. Confrontato letteralmente: `localhost` resolve
 * per il sistema, e una risoluzione DI QUI sarebbe una seconda autorità sulla stessa decisione
 * che ha già `getaddrinfo`. */
static int is_loopback_name(const char *h) {
    return !strcmp(h, "127.0.0.1") || !strcmp(h, "localhost") || !strcmp(h, "::1");
}

FmlxCliStatus fmlx_serve_exec(const FmlxCliArgs *a, FILE *out, FILE *err) {
    FmlxServeOpts o;
    FmlxServeBuf buf;
    FmlxServeSt st;
    char why[256] = "";
    unsigned served = 0;
    const char *host;

    if (!a) return FMLX_CLI_USAGE;
    fmlx_engine_set_offload_experts(a->offload_experts);
    o = a->serve;
    if (fmlx_serve_apply_env(&o, why, sizeof why) != FMLX_SERVE_OK) {
        fprintf(err ? err : stderr, "fastllm serve: %s\n", why);
        return FMLX_CLI_USAGE;
    }
    host = o.host ? o.host : FMLX_SERVE_HOST_DEFAULT;
    if (!is_loopback_name(host) && !(o.api_key && o.api_key[0])) {
        fprintf(err ? err : stderr,
                "fastllm serve: attenzione, ascolti su %s senza FASTLLM_API_KEY: chiunque raggiunga "
                "la porta legge i tuoi modelli e il path del cache\n",
                host);
    }
    if (fmlx_serve_buf_init(&buf, 0, 0) != 0) {
        fprintf(err ? err : stderr, "fastllm serve: memoria negata per i buffer del server\n");
        return FMLX_CLI_FAILED;
    }
    if (!a->quiet && out && isatty(fileno(out))) {
        char where[128];
        snprintf(where, sizeof where, "%s:%d", host,
                 o.port ? o.port : FMLX_SERVE_PORT_DEFAULT);
        fmlx_banner(out, "ascolto", where, 1);
    }
    st = fmlx_serve(&o, &buf, err ? err : stderr, NULL, &served);
    fmlx_serve_buf_free(&buf);
    if (st != FMLX_SERVE_OK) {
        /* Il perché lo ha già scritto il loop (`strerror` alla mano): qui si dice solo l'esito,
         * perché ripetere la causa due volte in due lingue diverse è il principio di una
         * divergenza. */
        fprintf(err ? err : stderr, "fastllm serve: non in ascolto (%s)\n",
                fmlx_serve_status_str(st));
        return FMLX_CLI_FAILED;
    }
    if (!a->quiet && out) fprintf(out, "fastllm serve: fermate %u richieste\n", served);
    return FMLX_CLI_OK;
}
