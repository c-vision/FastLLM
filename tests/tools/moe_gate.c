/* moe_gate.c — Promotion Gate per il fix del forward batch MoE (n=2..7). Sostituisce moe_gate.py.
 *
 * Compila ed esegue `tq_moe_batch_repro.c` (LLMEnv/LLMEngine/tools) contro un modello MoE: verifica che l'append (prefill) con
 * n=1..8,14 ritorni rc=0 (prima del fix df73a23/fe3b190 falliva per n=2..7). Stampa un artefatto JSON; esce 0 se tutti gli rc sono 0.
 *
 * Uso: moe_gate [--dylib D] [--repro-src S] [--ids "100,200"] [--out f.json] <modello>
 * Default: build-release/llmengine_mlx/libllm_ai_mlx.dylib e ../LLMEnv/LLMEngine/tools/tq_moe_batch_repro.c (dalla radice del repo).
 * Compilazione: cc -O2 tools/moe_gate.c -o build-release/moe_gate */
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    const char *dylib = "build-release/llmengine_mlx/libllm_ai_mlx.dylib", *src = "../LLMEnv/LLMEngine/tools/tq_moe_batch_repro.c",
               *ids = "100,200", *out = NULL, *model = NULL;
    char adylib[PATH_MAX], asrc[PATH_MAX], amodel[PATH_MAX], exe[] = "/tmp/moe_gate_repro_XXXXXX", cmd[4 * PATH_MAX], cwd[PATH_MAX],
         dirbuf[PATH_MAX], line[2048];
    int rows_n[32], rows_rc[32], nr = 0, fails = 0;
    FILE *p;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dylib") && i + 1 < argc) dylib = argv[++i];
        else if (!strcmp(argv[i], "--repro-src") && i + 1 < argc) src = argv[++i];
        else if (!strcmp(argv[i], "--ids") && i + 1 < argc) ids = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (argv[i][0] != '-') model = argv[i];
        else { fprintf(stderr, "uso: moe_gate [--dylib D] [--repro-src S] [--ids \"1,2\"] [--out f.json] <modello>\n"); return 2; }
    }
    if (!model) { fprintf(stderr, "moe_gate: manca <modello>\n"); return 2; }
    if (!realpath(dylib, adylib) || !realpath(src, asrc) || !realpath(model, amodel)) {
        fprintf(stderr, "moe_gate: percorso non trovato (dylib, sorgente o modello)\n");
        return 2;
    }
    if (strpbrk(adylib, "'\"") || strpbrk(asrc, "'\"") || strpbrk(amodel, "'\"") || strpbrk(ids, "'\";&|$`")) {
        fprintf(stderr, "moe_gate: caratteri non ammessi nei percorsi\n");
        return 2;
    }
    close(mkstemp(exe));
    snprintf(cmd, sizeof cmd, "cc -O2 '%s' -o '%s' 2>&1", asrc, exe);
    if (system(cmd) != 0) { printf("{\"error\": \"compile failed\"}\n"); return 2; }
    /* cwd = cartella sopra la dylib (build-release, dove sta mlx.metallib colocato per il motore) */
    snprintf(dirbuf, sizeof dirbuf, "%s", adylib);
    snprintf(cwd, sizeof cwd, "%s", dirname(dirname(dirbuf)));
    snprintf(cmd, sizeof cmd, "cd '%s' && '%s' '%s' '%s' '%s' 2>&1", cwd, exe, adylib, amodel, ids);
    p = popen(cmd, "r");
    while (p && fgets(line, sizeof line, p) && nr < 32) {
        int n, rc;
        char *a = strstr(line, "append n=");
        if (a && sscanf(a, "append n=%d rc=%d", &n, &rc) == 2) {
            rows_n[nr] = n;
            rows_rc[nr] = rc;
            if (rc != 0) fails++;
            nr++;
        }
    }
    if (p) pclose(p);
    unlink(exe);
    for (int pass = 0; pass < 2; pass++) {
        FILE *w = pass == 0 ? stdout : (out ? fopen(out, "w") : NULL);
        const char *base = strrchr(amodel, '/');
        if (!w) continue;
        fprintf(w, "{\n \"model\": \"%s\",\n \"fix\": \"MoE forward batch n=2..7 (qwen3_5 df73a23 / qwen3_moe fe3b190)\",\n \"n_tested\": [",
                base ? base + 1 : amodel);
        for (int i = 0; i < nr; i++) fprintf(w, "%s%d", i ? ", " : "", rows_n[i]);
        fprintf(w, "],\n \"rc\": {");
        for (int i = 0; i < nr; i++) fprintf(w, "%s\"%d\": %d", i ? ", " : "", rows_n[i], rows_rc[i]);
        fprintf(w, "},\n \"failures\": %d,\n \"gate\": \"%s\"\n}\n", fails, (nr && !fails) ? "PASS" : "FAIL");
        if (pass == 1) fclose(w);
    }
    return (nr && !fails) ? 0 : 1;
}
