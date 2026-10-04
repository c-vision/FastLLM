/* fmlx_paths_cmd.c — `fastllm paths`: lista/add/rm (progressivo numerico) dei percorsi di ricerca
 * modelli, e `scan` che inventaria in JSON dove ESISTONO modelli (disco interno o esterno).
 *
 * Il file di configurazione e le primitive stanno nel motore (`fmlx/fmlx_paths.h`), perché è
 * `resolve_dir` a doverli usare: qui c'è solo il verbo e la presentazione. */
#include "fmlx/fmlx_cli.h"
#include "fmlx/fmlx_hf.h"
#include "fmlx/fmlx_json.h"
#include "fmlx/fmlx_paths.h"

#include "fmlx_cli_internal.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static int p_is_dir(const char *p) { struct stat s; return stat(p, &s) == 0 && S_ISDIR(s.st_mode); }
static int p_is_file(const char *p) { struct stat s; return stat(p, &s) == 0 && S_ISREG(s.st_mode); }
static int p_is_gguf(const char *p) {
    size_t n = strlen(p);
    return n > 5 && strcmp(p + n - 5, ".gguf") == 0;
}
/* Una dir è un modello MLX/HF se ha config.json. */
static int p_is_model_dir(const char *dir) {
    char f[FMLX_PATHS_DIR_MAX + 32];
    return snprintf(f, sizeof f, "%s/config.json", dir) < (int)sizeof f && p_is_file(f);
}
/* Un repo del cache HF (layout `models--org--name`). */
static int p_is_hf_repo(const char *id) { return strncmp(id, "models--", 8) == 0; }
static int p_add_root(char (*roots)[FMLX_PATHS_DIR_MAX], int max, int *n, const char *p) {
    int i;
    if (!p || !*p || *n >= max) return 0;
    for (i = 0; i < *n; i++)
        if (strcmp(roots[i], p) == 0) return 0; /* dedup */
    snprintf(roots[*n], FMLX_PATHS_DIR_MAX, "%s", p);
    (*n)++;
    return 1;
}

/* I roots da scandire: i percorsi di configurazione + il cache HF (standard, non legato a una
 * macchina). Nessun default implicito tipo ~/ai/Models: sta a chi configura `paths add` deciderlo. */
static int p_collect_roots(char (*roots)[FMLX_PATHS_DIR_MAX], int max) {
    int n = 0, i, np = fmlx_paths_count();
    char p[FMLX_PATHS_DIR_MAX];
    for (i = 0; i < np && n < max; i++)
        if (fmlx_paths_get(i, p, sizeof p) == 0) p_add_root(roots, max, &n, p);
    { char hf[FMLX_HF_PATH_MAX]; if (fmlx_hf_cache_root(hf, sizeof hf) == FMLX_HF_OK) p_add_root(roots, max, &n, hf); }
    return n;
}

/* Conta i modelli immediatamente dentro `root` e (se richiesto) scrive le voci JSON. */
static int p_scan_root(const char *root, FmlxJsonW *w, int emit, int *out_n) {
    DIR *d;
    struct dirent *e;
    int n = 0;

    if (out_n) *out_n = 0;
    if (!p_is_dir(root)) return 0;
    d = opendir(root);
    if (!d) return 0;
    if (emit && fmlx_jsonw_obj_begin(w) != FMLX_JSON_OK) { closedir(d); return 0; }
    if (emit) fmlx_jsonw_key(w, "path"); fmlx_jsonw_cstr(w, root);
    if (emit) fmlx_jsonw_key(w, "models"); fmlx_jsonw_arr_begin(w);
    while ((e = readdir(d))) {
        char full[FMLX_PATHS_DIR_MAX];
        const char *id = e->d_name;
        if (id[0] == '.') continue;
        if (snprintf(full, sizeof full, "%s/%s", root, id) >= (int)sizeof full) continue;
        if (p_is_model_dir(full) || p_is_gguf(full) || p_is_hf_repo(id)) {
            const char *kind = p_is_gguf(full) ? "gguf" : (p_is_model_dir(full) ? "mlx" : "hf");
            n++;
            if (emit) {
                fmlx_jsonw_obj_begin(w);
                fmlx_jsonw_key(w, "id"); fmlx_jsonw_cstr(w, id);
                fmlx_jsonw_key(w, "path"); fmlx_jsonw_cstr(w, full);
                fmlx_jsonw_key(w, "kind"); fmlx_jsonw_cstr(w, kind);
                fmlx_jsonw_obj_end(w);
            }
        }
    }
    closedir(d);
    if (emit) { fmlx_jsonw_arr_end(w); fmlx_jsonw_obj_end(w); }
    if (out_n) *out_n = n;
    return n;
}

static FmlxCliStatus paths_scan(FILE *out, FILE *err, int json) {
    char(*roots)[FMLX_PATHS_DIR_MAX] = malloc((size_t)FMLX_PATHS_MAX * FMLX_PATHS_DIR_MAX);
    int nr, i, total = 0;
    if (!roots) { fprintf(err, "fastllm paths: memoria esaurita\n"); return FMLX_CLI_FAILED; }
    nr = p_collect_roots(roots, FMLX_PATHS_MAX);

    if (json) {
        FmlxJsonW w;
        size_t blen = 0;
        int ok = 0;
        fmlx_jsonw_init(&w, NULL, 0, 1u << 24);
        do {
            if (fmlx_jsonw_obj_begin(&w) != FMLX_JSON_OK) break;
            if (fmlx_jsonw_key(&w, "generated") != FMLX_JSON_OK || fmlx_jsonw_i64(&w, (int64_t)time(NULL)) != FMLX_JSON_OK) break;
            if (fmlx_jsonw_key(&w, "roots") != FMLX_JSON_OK || fmlx_jsonw_arr_begin(&w) != FMLX_JSON_OK) break;
            for (i = 0; i < nr; i++) { int cnt = 0; p_scan_root(roots[i], &w, 1, &cnt); total += cnt; }
            if (fmlx_jsonw_arr_end(&w) != FMLX_JSON_OK || fmlx_jsonw_obj_end(&w) != FMLX_JSON_OK ||
                fmlx_jsonw_finish(&w, &blen) != FMLX_JSON_OK) break;
            ok = 1;
        } while (0);
        if (!ok) { fmlx_jsonw_free(&w); free(roots); fprintf(err, "fastllm paths: JSON troppo grande\n"); return FMLX_CLI_FAILED; }
        fwrite(w.buf ? w.buf : "{}", 1, blen, out);
        fputc('\n', out);
        fmlx_jsonw_free(&w);
    } else {
        fprintf(out, "%zu percorsi di ricerca scansionati:\n", (size_t)nr);
        for (i = 0; i < nr; i++) { int cnt = 0; p_scan_root(roots[i], NULL, 0, &cnt); total += cnt;
            fprintf(out, "  %2d. %-60s %s%d modelli\n", i + 1, roots[i], p_is_dir(roots[i]) ? "" : "(assente) ", cnt); }
        fprintf(out, "\n%zu modelli totali.  (--json per l'inventario macchina-leggibile)\n", (size_t)total);
    }
    free(roots);
    return FMLX_CLI_OK;
}

FmlxCliStatus fmlx_paths_exec(const FmlxCliArgs *a, FILE *out, FILE *err) {
    const char *sub = (a && a->repo[0]) ? a->repo : NULL;
    char msg[512];

    if (!a) return FMLX_CLI_USAGE;
    if (!out) out = stdout;
    if (!err) err = stderr;

    if (!sub) { /* lista numerata */
        int n = fmlx_paths_count(), i;
        char p[FMLX_PATHS_DIR_MAX], f[FMLX_PATHS_DIR_MAX + 16];
        fmlx_paths_file(f, sizeof f);
        fprintf(out, "Percorsi di ricerca modelli (config: %s):\n", f[0] ? f : "(unknown)");
        if (n == 0) fprintf(out, "  (nessuno: aggiungi con `fastllm paths add <dir>`)\n");
        for (i = 0; i < n; i++) { p[0] = 0; fmlx_paths_get(i, p, sizeof p);
            fprintf(out, "  [%d] %s%s\n", i + 1, p, p_is_dir(p) ? "" : "  (assente)"); }
        fprintf(out,
                "\nNessun default legato alla macchina. Oltre ai percorsi qui sopra e' sempre\n"
                "cercato il cache Hugging Face (HF_HUB_CACHE / HF_HOME), dove `fastllm pull` scarica.\n"
                "\nUso: fastllm paths [add <dir> | rm <n> | scan [--json]]\n");
        return FMLX_CLI_OK;
    }
    if (!strcmp(sub, "add")) {
        if (!a->path[0]) { fprintf(err, "fastllm paths add: manca la directory\n"); return FMLX_CLI_USAGE; }
        if (fmlx_paths_add(a->path, msg, sizeof msg) != 0) { fprintf(err, "fastllm paths add: %s\n", msg); return FMLX_CLI_FAILED; }
        fprintf(out, "percorso aggiunto: %s\n", a->path);
        return FMLX_CLI_OK;
    }
    if (!strcmp(sub, "rm") || !strcmp(sub, "remove")) {
        long idx;
        char *end = NULL;
        if (!a->path[0]) { fprintf(err, "fastllm paths rm: manca l'indice numerico\n"); return FMLX_CLI_USAGE; }
        idx = strtol(a->path, &end, 10);
        if (!end || *end || idx < 1) { fprintf(err, "fastllm paths rm: '%s' non e' un indice (serve il progressivo da `fastllm paths`)\n", a->path); return FMLX_CLI_USAGE; }
        if (fmlx_paths_rm((int)idx, msg, sizeof msg) != 0) { fprintf(err, "fastllm paths rm: %s\n", msg); return FMLX_CLI_FAILED; }
        fprintf(out, "percorso #%ld rimosso\n", idx);
        return FMLX_CLI_OK;
    }
    if (!strcmp(sub, "scan")) return paths_scan(out, err, a->json);
    fprintf(err, "fastllm paths: sottocomando '%s' sconosciuto (add | rm | scan)\n", sub);
    return FMLX_CLI_USAGE;
}
