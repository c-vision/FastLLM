/* engine_abi_probe.c — un consumatore ESTERNO di MLXEngine: un solo header, una sola dylib.
 *
 * Perché esiste. `test_engine` prova l'ABI dall'interno del progetto, con il sorgente sotto
 * mano e lo stesso compilatore. Questo file no: è compilato contro il solo
 * `include/mlxengine.h` e linkato contro `libmlxengine.1.dylib`. È il modo in cui GMBasic — o
 * qualunque programma che non ha i ns. sorgenti — userà il motore, quindi è l'unico prova che
 * l'ABI non è un'estensione dei test.
 *
 * Due dettagli che questo file ha già trovato, e sono la ragione della sua forma.
 *
 * (1) Chiedere 8 elementi di un Q8_0 o di un Q4_K è illegale (8 non è multiplo del blocco):
 *     l'ABI lo dice con il SUO codice e NON tocca `dst`. Chi stampa il buffer dopo un rifiuto
 *     stampa byte stale credendoli pesi — il «risultato che sembra quello che non è» in forma
 *     di probe. Quindi qui i valori si stampano SOLO con esito OK.
 * (2) Per sapere quale `n` è legale un chiamante esterno deve poter chiedere il blocco: da
 *     qui `mlxengine_tensor_block_vals`. Tutto quello che il probe usa è nel header pubblico:
 *     se questo file smettesse di compilarsi, il motore avrebbe un buco nell'ABI e i ns. test
 *     non se ne accorgerebbero.
 */
#include "mlxengine.h"

#include <stdio.h>
#include <string.h>

// Si compila e si lancia così, dalla root del progetto (niente CMake: è il punto).
//   cc -std=c17 -Wall -Wextra -Werror -I include tools/engine_abi_probe.c \
//      -L build -lmlxengine -o build/engine_abi_probe
//   DYLD_LIBRARY_PATH=build ./build/engine_abi_probe ~/ai/Models/gguf/*.gguf

#define PROBE_OK 0   /* il file si è aperto e nulla ha contraddetto l'ABI */
#define PROBE_SKIP 1 /* il rifiuto d'apertura è un ESITO, non un difetto: dichiarato */
#define PROBE_BAD 2  /* l'ABI ha mentito: riletura divergente o un rifiuto che ha scritto */

/* Gli elementi di cui stampare i primi valori: un blocco intero, arrotondato ad almeno 8 per i
 * tipi non bloccati (F32/F16/BF16, `block_vals` = 1). */
static uint64_t probe_span(uint64_t block_vals) {
    uint64_t n = block_vals ? block_vals : 1;
    while (n < 8) n *= 2;
    return n;
}

static int probe(const char *path) {
    MlxEngineModel *m = NULL;
    const char *arch = NULL;
    int rc = PROBE_OK;
    MlxEngineStatus st = mlxengine_model_open(path, &m);
    uint64_t i, n_tensors;

    if (st != FMLX_GGUF_OK) {
        printf("%s\n  APERTURA RIFIUTATA: %s\n", path, mlxengine_status_str(st));
        return PROBE_SKIP;
    }
    printf("%s\n  abi %u · %s · tensori %llu · kv %llu · alignment %llu · architettura %s\n", path,
           mlxengine_abi_version(), mlxengine_version_string(),
           (unsigned long long)mlxengine_model_n_tensors(m),
           (unsigned long long)mlxengine_model_n_kv(m),
           (unsigned long long)mlxengine_model_alignment(m),
           mlxengine_model_kv_str(m, "general.architecture", &arch) == FMLX_GGUF_OK && arch
               ? arch : "(assente)");

    /* I primi tre tensori, e per ognuno una copia di COERENZA: la stessa finestra chiesta due
     * volte deve dare gli stessi bit. Un byte diverso significherebbe che qualcuno legge da un
     * buffer transitorio invece che dal file mappato. */
    n_tensors = mlxengine_model_n_tensors(m);
    for (i = 0; i < n_tensors && i < 3; i++) {
        static float a[4096], b[4096];
        uint64_t numel = mlxengine_tensor_numel(m, i);
        uint64_t bv = mlxengine_tensor_block_vals(m, i);
        uint64_t n = probe_span(bv);
        MlxEngineStatus r1, r2;

        if (bv == 0) { /* tipo fuori dalla tabella: nessuna richiesta è calcolabile da qui */
            printf("  %-30s tipo %3d  numel %llu: tipo sconosciuto, blocco non calcolabile\n",
                   mlxengine_tensor_name(m, i), mlxengine_tensor_type(m, i),
                   (unsigned long long)numel);
            continue;
        }
        if (n > numel) n = numel - (numel % bv); /* mai oltre la fine: è il caso dei tensori corti */
        if (n == 0 || n > sizeof a / sizeof a[0]) {
            printf("  %-30s tipo %3d  numel %llu: nessun blocco intero da leggere\n",
                   mlxengine_tensor_name(m, i), mlxengine_tensor_type(m, i),
                   (unsigned long long)numel);
            continue;
        }
        memset(a, 0x5a, n * sizeof(float));
        memset(b, 0xa5, n * sizeof(float));
        r1 = mlxengine_tensor_dequant(m, i, 0, n, a, sizeof a);
        r2 = mlxengine_tensor_dequant(m, i, 0, n, b, sizeof b);
        printf("  %-30s tipo %3d · copr=%d · blocco %llu · numel %llu · n %llu → %s",
               mlxengine_tensor_name(m, i), mlxengine_tensor_type(m, i),
               mlxengine_tensor_is_covered(m, i), (unsigned long long)bv,
               (unsigned long long)numel, (unsigned long long)n, mlxengine_status_str(r1));
        /* Prima dei bit, il verdetto: la STESSA chiamata ripetuta non deve cambiare idea. Un
         * esito che oscilla tra due invocate identiche racconta un rifiuto appoggiato a uno
         * stato interno, e su quello stato un chiamante esterno non può contare. */
        if (r1 != r2) {
            printf(" · esito non riproducibile: %s poi %s\n", mlxengine_status_str(r1),
                   mlxengine_status_str(r2));
            rc = PROBE_BAD;
        }
        if (r1 == FMLX_GGUF_OK) {
            int same = memcmp(a, b, n * sizeof(float)) == 0;
            printf(" · %.6g %.6g %.6g · riletura %s\n", a[0], a[1], a[2],
                   same ? "identica" : "DIVERGENTE");
            if (!same) {
                printf("      i due passi non concordano: l'ABI non è deterministica\n");
                rc = PROBE_BAD;
            }
        } else {
            /* E questo è il caso giusto, non un fallimento: il rifiuto non deve aver scritto. */
            int untouched = 1;
            uint64_t k;
            for (k = 0; k < n; k++)
                if (((unsigned char *)a)[k * 4] != 0x5a || ((unsigned char *)b)[k * 4] != 0xa5)
                    untouched = 0;
            printf(" · dst intatto %s\n", untouched ? "sì" : "NO — rifiuto che scrive");
            if (!untouched) rc = PROBE_BAD;
        }
    }
    mlxengine_model_close(m);
    return rc;
}

int main(int argc, char **argv) {
    int i, ok = 0, skip = 0, bad = 0;
    if (argc < 2) {
        fprintf(stderr, "uso: %s <file.gguf>...\n", argv[0]);
        return 2;
    }
    for (i = 1; i < argc; i++) {
        int r = probe(argv[i]);
        if (r == PROBE_OK) ok++;
        else if (r == PROBE_SKIP) skip++;
        else bad++;
    }
    printf("\n%d file provati · %d puliti · %d non aperti · %d con l'ABI in contraddizione\n",
           argc - 1, ok, skip, bad);
    return bad ? 1 : 0;
}
