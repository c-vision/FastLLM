/* hf_fetch_probe.c — la lane download contro il server VERO.
 *
 * Non è un test: tocca la rete, e quindi non entra in ctest. Serve a una cosa sola — scoprire se
 * le regole che ho copiato da huggingface_hub sono le regole del SERVER, non solo del sorgente.
 * Scarica un file PICCOLO (la soglia di sicurezza è nel programma), lo verifica, e dice quali
 * intestazioni ha visto davvero.
 *
 * Uso:  hf_fetch_probe [repo_id] [revisione] [path] [max_byte]
 * Default: bert-base-uncased main config.json 4194304
 * Il token viene SOLO da HF_TOKEN; non viene mai stampato, nemmeno mascherato.
 */
#include "fmlx/fmlx_fetch.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Che tipo di promessa fa un nome di blob. Locale: non si aggiunge un'API pubblica per un probe. */
static const char *kind_name(FmlxHfHashKind k) {
    switch (k) {
        case FMLX_HF_HASH_GIT_BLOB: return "git blob id (SHA1 di 'blob <len>\\0' + contenuto)";
        case FMLX_HF_HASH_SHA256: return "sha256";
        case FMLX_HF_HASH_NONE: return "nessuna";
    }
    return "?";
}

int main(int argc, char **argv) {    const char *repo = argc > 1 ? argv[1] : "google-bert/bert-base-uncased";
    const char *rev = argc > 2 ? argv[2] : "main";
    const char *rel = argc > 3 ? argv[3] : "config.json";
    int64_t max_bytes = argc > 4 ? strtoll(argv[4], NULL, 10) : 4 * 1024 * 1024;

    char root[FMLX_HF_PATH_MAX];
    if (fmlx_hf_cache_root(root, sizeof root) != FMLX_HF_OK) {
        printf("cache root: %s\n", fmlx_hf_last_error());
        return 2;
    }
    printf("cache = %s\nrepo  = %s@%s\nfile  = %s\nlimite  = %" PRId64 " byte\n\n", root, repo, rev,
           rel, max_bytes);

    const FmlxHfTransport *t = fmlx_hf_transport_curl();
    if (!t) {
        printf("transport curl non disponibile\n");
        return 2;
    }

    FmlxHfFetchOpts o;
    memset(&o, 0, sizeof o);
    o.max_bytes = max_bytes;
    o.wait_lock_ms = 5000;

    FmlxHfRef ref;
    FmlxHfStatus s = fmlx_hf_download_with(repo, rev, rel, &o, t, &ref);
    printf("download -> %s", fmlx_hf_status_str(s));
    if (s != FMLX_HF_OK && fmlx_hf_last_error()) printf(" (%s)", fmlx_hf_last_error());
    printf("\n");
    if (s != FMLX_HF_OK) return 1;

    printf("path   = %s\nblob   = %s (%s)\nis_link= %d\nsize   = %" PRId64 "\n", ref.path,
           ref.blob_name, kind_name(ref.hash_kind), ref.is_blob_link, ref.size);

    int64_t hashed = 0;
    FmlxHfStatus v = fmlx_hf_verify(&ref, &hashed);
    printf("verify -> %s (%" PRId64 " byte hashati)%s", fmlx_hf_status_str(v), hashed,
           v == FMLX_HF_ERR_NOT_VERIFIABLE ? " — il nome non promette un hash" : "\n");
    if (v != FMLX_HF_OK && v != FMLX_HF_ERR_NOT_VERIFIABLE) return 1;

    /* Il secondo colpo deve risolversi SENZA rete: è la prova che il cache è scritto bene, non
     * solo che il download è andato a buon fine. */
    FmlxHfFetchOpts q;
    memset(&q, 0, sizeof q);
    q.offline = 1;
    FmlxHfStatus s2 = fmlx_hf_download_with(repo, rev, rel, &q, t, NULL);
    printf("richiamata offline -> %s  (deve essere ok)\n", fmlx_hf_status_str(s2));
    return s2 == FMLX_HF_OK ? 0 : 1;
}
