/* hf_probe.c — guarda il cache HF REALE di questa macchina, senza rete e senza scrivere.
 *
 * Esiste perché un parser testato solo su fixture sintetiche può essere perfettamente
 * coerente con se stesso e sbagliato col mondo (è esattamente ciò che è successo alla
 * tabella dei tipi ggml: docs/quant-coverage.md §5). Qui l'input è il cache scritto da
 * huggingface_hub e da Rapid-MLX su disco.
 *
 * Uso:
 *   build/hf_probe                                  -> il modello misurato nel README
 *   build/hf_probe <org>/<repo> <rev> <file>        -> uno specifico file
 *   build/hf_probe --tree <org>/<repo> [<rev>]      -> elenca le entry di uno snapshot
 */
#include "fmlx/fmlx_hf.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void show_env(void) {
    char root[FMLX_HF_PATH_MAX], ep[FMLX_HF_PATH_MAX];
    if (fmlx_hf_cache_root(root, sizeof root) == FMLX_HF_OK) printf("cache root : %s\n", root);
    else printf("cache root : (%s) %s\n", fmlx_hf_status_str(FMLX_HF_ERR_NO_HOME), fmlx_hf_last_error());
    fmlx_hf_endpoint(ep, sizeof ep);
    printf("endpoint   : %s\n", ep);
    printf("offline    : %s\n", fmlx_hf_offline() ? "sì" : "no");
}

static int g_resolved;

/* Ricorsivo perché HF mette DIRECTORY reali dentro uno snapshot (misurato:
 * models--bert-base-uncased/snapshots/<rev>/coreml/...). Fermarsi al primo livello
 * vorrebbe dire non vedere i symlink più profondi, che sono anche il caso in cui la
 * regola dei "../" si vede fallire. */
static void walk(const char *root, const char *repo_id, const char *sha, const char *dir,
                 const char *prefix, int depth) {
    if (depth > 6) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[FMLX_HF_PATH_MAX * 2];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        char rel[FMLX_HF_PATH_MAX * 2];
        if (prefix[0]) snprintf(rel, sizeof rel, "%s/%s", prefix, e->d_name);
        else snprintf(rel, sizeof rel, "%s", e->d_name);

        struct stat st;
        if (lstat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            printf("  %-52s [directory]\n", rel);
            walk(root, repo_id, sha, p, rel, depth + 1);
            continue;
        }
        FmlxHfRef ref;
        FmlxHfStatus s = fmlx_hf_locate(root, repo_id, sha, rel, &ref);
        if (s != FMLX_HF_OK) {
            printf("  %-52s %s\n", rel, fmlx_hf_status_str(s));
            continue;
        }
        int64_t hashed = 0;
        FmlxHfStatus v = fmlx_hf_verify(&ref, &hashed);
        printf("  %-52s %10lld  link=%d hash=%d  verifica=%s\n", rel, (long long)ref.size,
               ref.is_blob_link, (int)ref.hash_kind, fmlx_hf_status_str(v));
        if (v != FMLX_HF_OK && v != FMLX_HF_ERR_NOT_VERIFIABLE) printf("      %s\n", fmlx_hf_last_error());
        g_resolved++;
    }
    closedir(d);
}

static int tree(const char *root, const char *repo_id, const char *rev) {
    char repo[FMLX_HF_PATH_MAX], sha[64], snap[FMLX_HF_PATH_MAX * 2];
    if (fmlx_hf_repo_dir(root, repo_id, repo, sizeof repo) != FMLX_HF_OK) {
        printf("%s\n", fmlx_hf_last_error());
        return 1;
    }
    printf("repo dir   : %s\n", repo);
    if (fmlx_hf_resolve_ref(repo, rev, sha, sizeof sha) != FMLX_HF_OK) {
        printf("ref        : %s\n", fmlx_hf_last_error());
        return 1;
    }
    printf("revisione  : %s\n", sha);
    snprintf(snap, sizeof snap, "%s/snapshots/%s", repo, sha);
    if (access(snap, R_OK) != 0) {
        printf("snapshot   : assente (%s)\n", snap);
        return 1;
    }
    printf("snapshot   : %s\n", snap);
    walk(root, repo_id, sha, snap, "", 0);
    printf("%d entry risolte\n", g_resolved);
    return 0;
}

int main(int argc, char **argv) {
    show_env();
    char root[FMLX_HF_PATH_MAX];
    if (fmlx_hf_cache_root(root, sizeof root) != FMLX_HF_OK) return 2;

    int arg = 1;
    int want_tree = 0;
    if (arg < argc && strcmp(argv[arg], "--tree") == 0) { want_tree = 1; arg++; }
    const char *repo_id = arg < argc ? argv[arg++] : "mlx-community/Qwen3-0.6B-4bit";
    const char *rev = arg < argc ? argv[arg++] : "main";
    const char *file = arg < argc ? argv[arg++] : "config.json";

    if (want_tree) return tree(root, repo_id, rev);

    char url[FMLX_HF_PATH_MAX * 2];
    if (fmlx_hf_url(repo_id, rev, file, 1, url, sizeof url) == FMLX_HF_OK) printf("url        : %s\n", url);

    FmlxHfRef ref;
    FmlxHfStatus s = fmlx_hf_locate(root, repo_id, rev, file, &ref);
    if (s != FMLX_HF_OK) {
        printf("locate     : %s — %s\n", fmlx_hf_status_str(s),
               fmlx_hf_last_error() ? fmlx_hf_last_error() : "-");
        /* Solamente NOT_CACHED giustifica una richiesta di rete: è la distinzione per cui
         * i codici sono separati. */
        return s == FMLX_HF_ERR_NOT_CACHED ? 0 : 1;
    }
    printf("path       : %s\n", ref.path);
    printf("blob       : %s (hash atteso: %s)\n", ref.blob_name,
             ref.hash_kind == FMLX_HF_HASH_GIT_BLOB   ? "git blob id"
             : ref.hash_kind == FMLX_HF_HASH_SHA256   ? "sha256"
                                                      : "nessuno");
    printf("size       : %lld byte\n", (long long)ref.size);
    int64_t hashed = 0;
    FmlxHfStatus v = fmlx_hf_verify(&ref, &hashed);
    printf("verifica   : %s", fmlx_hf_status_str(v));
    if (v == FMLX_HF_OK) printf(" (%lld byte hashati)", (long long)hashed);
    else if (fmlx_hf_last_error()) printf("\n             %s", fmlx_hf_last_error());
    printf("\n");
    return v == FMLX_HF_OK || v == FMLX_HF_ERR_NOT_VERIFIABLE ? 0 : 1;
}
