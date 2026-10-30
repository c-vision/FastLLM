/* hf_manifest_probe.c — il manifesto dei file contro il server VERO (piano §12.1).
 *
 * Non è un test: tocca la rete e quindi non entra in ctest. Serve a una cosa sola — sapere se
 * l'URL e i NOMI DEI CAMPI che ho copiato da huggingface_hub sono quelli che il SERVER risponde.
 * Il test offline (tests/hf/test_hf_manifest.c) prova la nostra lettura di una forma; questo prova
 * la forma.
 *
 * Uso:  hf_manifest_probe [repo_id] [revisione]
 * Default: google-bert/bert-base-uncased main
 * Il token viene SOLO da HF_TOKEN (per i repo gated) e non è mai stampato, nemmeno mascherato.
 * Nessun byte del modello viene scaricato: questa è UNA richiesta di metadati.
 */
#include "fmlx/fmlx_fetch.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *repo = argc > 1 ? argv[1] : "google-bert/bert-base-uncased";
    const char *rev = argc > 2 ? argv[2] : "main";
    FmlxHfFetchOpts o;
    FmlxHfManifest m;
    size_t i, nlfs = 0, nosize = 0, noblob = 0, n40 = 0, naltro = 0;
    int64_t total = 0;

    memset(&o, 0, sizeof o);
    printf("repo  = %s@%s\n", repo, rev);
    fflush(stdout);

    FmlxHfStatus s = fmlx_hf_manifest(repo, rev, &o, &m);
    printf("manifesto -> %s", fmlx_hf_status_str(s));
    if (s != FMLX_HF_OK && fmlx_hf_last_error()) printf(" (%s)", fmlx_hf_last_error());
    printf("\n");
    if (s != FMLX_HF_OK) return 1;

    printf("commit = %s\nfile   = %zu\n", m.commit, m.n);
    for (i = 0; i < m.n; i++) {
        size_t bl = strlen(m.files[i].git_blob_id);
        if (m.files[i].is_lfs) nlfs++;
        if (m.files[i].size < 0) nosize++;
        else total += m.files[i].size;
        if (!bl) noblob++;
        else if (bl == 40) n40++;
        else naltro++;
    }
    /* La riga sotto è il PERCHÉ questa sonda esiste: `git_blob_id` arriva sempre a 40 hex (git
     * blob id), anche sui file LFS — che nel cache si chiamano però con lo sha256 preso
     * dall'etag. Misurarlo qui tiene viva la ragione per cui `pull` non deve usare questo campo
     * come nome di file. */
    printf("con lfs = %zu   senza size = %zu   blobId: 40-hex=%zu altri=%zu assenti=%zu   "
           "totale dichiarato = %" PRId64 " byte\n\n",
           nlfs, nosize, n40, naltro, noblob, total);

    /* Le prime VENTI voci per intero: su un repo da migliaia di file stamparli tutti non aggiunge
     * niente a ciò che si deve vedere — la FORMA dei nomi e quali campi il server popola. */
    for (i = 0; i < m.n && i < 20; i++) {
        printf("  %-58s %10" PRId64 "  lfs=%d blob=%.12s\n", m.files[i].path, m.files[i].size,
               m.files[i].is_lfs, m.files[i].git_blob_id);
    }
    if (m.n > 20) printf("  ... altre %zu voci\n", m.n - 20);
    fmlx_hf_manifest_free(&m);
    return 0;
}
