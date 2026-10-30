/* kernel_tune.c — harness di autotuning dei kernel (model-free). Sostituisce kernel_tune.py.
 *
 * Chiama l'API del motore `llm_ai_mlx_core_bench` (nessun modello caricato) per misurare le primitive core (rms_norm / qlinear / sdpa)
 * e stampa un artefatto JSON (baseline di tuning, specifica dell'hardware) da usare per le decisioni di dispatch.
 *
 * Uso: kernel_tune [--dylib D] [--op all|rms_norm|qlinear|sdpa] [--iters N] [--out f.json]
 * Default dylib: build-release/llmengine_mlx/libllm_ai_mlx.dylib (relativa alla radice del repo). Esce 0 se il motore ha misurato qualcosa.
 * Compilazione: cc -O2 tools/kernel_tune.c -ldl -o build-release/kernel_tune */
#include <dlfcn.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef int (*bench_fn)(const char *op, int iters, char *out, size_t cap);

static void json_str(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') fputc('\\', f);
        if ((unsigned char)*s >= 0x20) fputc(*s, f);
    }
    fputc('"', f);
}

int main(int argc, char **argv) {
    const char *dylib = "build-release/llmengine_mlx/libllm_ai_mlx.dylib", *op = "all", *out = NULL;
    int iters = 200;
    char abs_dylib[PATH_MAX], abs_out[PATH_MAX] = "", dir[PATH_MAX], *buf;
    void *h;
    bench_fn f;
    int n;
    FILE *o;

    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--dylib")) dylib = argv[i + 1];
        else if (!strcmp(argv[i], "--op")) op = argv[i + 1];
        else if (!strcmp(argv[i], "--iters")) iters = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--out")) out = argv[i + 1];
        else { fprintf(stderr, "uso: kernel_tune [--dylib D] [--op all|rms_norm|qlinear|sdpa] [--iters N] [--out f.json]\n"); return 2; }
    }
    if (!realpath(dylib, abs_dylib)) { fprintf(stderr, "kernel_tune: dylib non trovata: %s\n", dylib); return 2; }
    if (out) {
        char tmp[PATH_MAX];
        snprintf(tmp, sizeof tmp, "%s", out);
        if (tmp[0] != '/') { char cwd[PATH_MAX]; if (getcwd(cwd, sizeof cwd)) snprintf(abs_out, sizeof abs_out, "%s/%s", cwd, out); }
        else snprintf(abs_out, sizeof abs_out, "%s", out);
    }
    /* cwd = cartella di build (mlx.metallib colocated per il runtime MLX) */
    snprintf(dir, sizeof dir, "%s", abs_dylib);
    snprintf(dir, sizeof dir, "%s", dirname(dirname(dir)));
    if (chdir(dir) != 0) { /* non fatale */ }
    h = dlopen(abs_dylib, RTLD_NOW);
    if (!h) { fprintf(stderr, "kernel_tune: dlopen: %s\n", dlerror()); return 2; }
    f = (bench_fn)dlsym(h, "llm_ai_mlx_core_bench");
    if (!f) { fprintf(stderr, "kernel_tune: il motore non espone llm_ai_mlx_core_bench\n"); return 2; }
    buf = (char *)calloc(1, 1 << 16);
    n = f(op, iters, buf, (size_t)1 << 16);

    o = abs_out[0] ? fopen(abs_out, "w") : NULL;
    for (int pass = 0; pass < 2; pass++) {
        FILE *w = pass == 0 ? stdout : o;
        char *line, *save;
        int first = 1;
        if (!w) continue;
        fprintf(w, "{\n \"op\": "); json_str(w, op);
        fprintf(w, ",\n \"iters\": %d,\n \"n_ops\": %d,\n \"dylib\": ", iters, n); json_str(w, strrchr(abs_dylib, '/') ? strrchr(abs_dylib, '/') + 1 : abs_dylib);
        fprintf(w, ",\n \"rows\": [");
        char *copy = strdup(buf);
        for (line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            char *tok, *s2, *l2 = strdup(line);
            if (strncmp(line, "OP ", 3) != 0) { free(l2); continue; }
            fprintf(w, "%s\n  {\"line\": ", first ? "" : ","); json_str(w, line);
            for (tok = strtok_r(l2, " \t", &s2); tok; tok = strtok_r(NULL, " \t", &s2)) {
                char *eq = strchr(tok, '=');
                if (!eq) continue;
                *eq = '\0';
                fprintf(w, ", "); json_str(w, tok); fprintf(w, ": "); json_str(w, eq + 1);
            }
            fprintf(w, "}");
            first = 0;
            free(l2);
        }
        free(copy);
        fprintf(w, "\n ]\n}\n");
    }
    if (o) fclose(o);
    free(buf);
    return n > 0 ? 0 : 1;
}
