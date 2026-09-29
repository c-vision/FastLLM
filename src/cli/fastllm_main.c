/* fastllm_main.c — il `main` del binario: cinque righe, e NON è un caso.
 *
 * Tutta la logica della CLI è in fmlx_cli.c e si prova lì, con argv finti e due FILE* in memoria.
 * Questo file esiste perché un eseguibile ha bisogno di `main`, e serve a una cosa sola: tradurre
 * lo stato in un codice d'uscita. Mettere qui del comportamento vorrebbe dire avere una parte del
 * comando provabile solo lanciando il processo.
 */
#include "fmlx/fmlx_cli.h"

#include <stdio.h>

int main(int argc, char **argv) {
    switch (fmlx_cli_run(argc, argv, stdout, stderr)) {
        case FMLX_CLI_OK: return 0;
        case FMLX_CLI_USAGE: return 2; /* la convenzione di getopt: è l'argv, non il mondo */
        default: return 1;             /* NOT_WIRED e FAILED: non si è fatto ciò che è stato chiesto */
    }
}
