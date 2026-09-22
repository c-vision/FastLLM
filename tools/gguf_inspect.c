/* gguf_inspect.c — legge la SOLA testata di file GGUF reali e stampa quello che il
 * lettore di riferimento vede. Serve a confrontare il parser con i file che sono già su
 * questa macchina (nessun download): numero di tensori, tipi presenti, allineamento
 * dichiarato, inizio della data section, e il CODICE distinto per ogni rifiuto.
 *
 * Uso: fmlx_gguf_inspect <file.gguf> [...]
 * Uscita: 0 se tutti i file si aprono, 1 se anche uno solo viene rifiutato.
 */
#include "fmlx/fmlx_gguf.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int one(const char *path) {
    FmlxGgufFile f;
    FmlxGgufStatus st = fmlx_gguf_open_header(path, &f);
    uint64_t i;
    unsigned hist[FMLX_GGML_TYPE_MAX + 2]; /* l'ultimo bucket raccoglie i tipi ignoti */
    const FmlxGgufKV *arch;

    if (st != FMLX_GGUF_OK) {
        printf("%s\n  RIFIUTATO: %d (%s) — %s\n", path, (int)st, fmlx_gguf_status_str(st),
               fmlx_gguf_last_error(&f) ? fmlx_gguf_last_error(&f) : "(nessun messaggio)");
        fmlx_gguf_close(&f);
        return 1;
    }
    printf("%s\n  v%u  tensori=%" PRIu64 "  kv=%" PRIu64 "  alignment=%" PRIu64
           "  data_start=%" PRIu64 "  byte_file=%zu\n", path, f.version, f.n_tensors, f.n_kv,
           f.alignment, f.data_start, f.len);

    memset(hist, 0, sizeof hist);
    for (i = 0; i < f.n_tensors; i++) {
        int t = f.tensors[i].ggml_type;
        hist[(t >= 0 && t <= FMLX_GGML_TYPE_MAX) ? (unsigned)t : FMLX_GGML_TYPE_MAX + 1]++;
    }
    printf("  tipi:");
    for (i = 0; i <= (uint64_t)(FMLX_GGML_TYPE_MAX + 1); i++) {
        const FmlxGgmlType *ty;
        if (!hist[i]) continue;
        ty = fmlx_ggml_type((int)i);
        printf(" %s(%u)x%u", ty ? ty->name : "IGNOTO", (unsigned)i, hist[i]);
    }
    printf("\n");

    arch = fmlx_gguf_find_kv(&f, "general.architecture");
    if (arch && arch->s) printf("  arch=%s", arch->s);
    printf("\n");

    /* Fine della data section usata + coda: è il controllo forte sui NUMERI della tabella
     * tipi. Se un byte-per-blocco fosse sbagliato, gli end si accumulerebbe o avanzerebbe
     * una coda enorme rispetto alla dimensione reale del file. */
    {
        uint64_t max_end = 0, sum = 0;
        for (i = 0; i < f.n_tensors; i++) {
            uint64_t end = f.tensors[i].offset + f.tensors[i].nbytes;
            if (end > max_end) max_end = end;
            sum += f.tensors[i].nbytes;
        }
        printf("  data_usata=%" PRIu64 " (somma tensori=%" PRIu64 ")  coda=%" PRId64 "\n",
               max_end, sum,
               (int64_t)f.len - (int64_t)f.data_start - (int64_t)max_end);
    }

    for (i = 0; i < f.n_tensors && i < 3; i++) {
        const FmlxGgufTensor *t = &f.tensors[i];
        printf("  [%" PRIu64 "] \"%s\" n_dims=%u dim0=%" PRIu64 " tipo=%d numel=%" PRIu64
               " offset=%" PRIu64 " nbytes=%" PRIu64 "\n", i, t->name ? t->name : "(nulla)",
               t->n_dims, t->dims[0], t->ggml_type, t->numel, t->offset, t->nbytes);
    }
    fmlx_gguf_close(&f);
    return 0;
}

int main(int argc, char **argv) {
    int i, bad = 0;
    if (argc < 2) {
        fprintf(stderr, "uso: %s <file.gguf> [...]\n", argv[0]);
        return 2;
    }
    for (i = 1; i < argc; i++) bad += one(argv[i]);
    return bad ? 1 : 0;
}
