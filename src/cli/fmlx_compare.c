/* fmlx_compare.c — `fastllm compare <a.gguf> <b.gguf> [--glob G]`.
 *
 * Confronto NUMERICO per-tensore tra due GGUF (dequant + metriche stile antirez/gguf-tools).
 * Il CORE è di LLMCodec (`llmcodec_gguf_compare`, Apache/LLMEnv): UNA SOLA SORGENTE, la stessa che
 * usa la superficie BASIC `LLM TENSOR COMPARE` di GMBasic. Qui solo: argomenti + formattazione CLI. */
#include "fmlx/fmlx_cli.h"

#include "llmcodec_gguf_compare.h"
#include "llmcodec_ggml_quants.h"

#include <stdio.h>
#include <string.h>

static int compare_cb(const char *name, const LlmCodecGgufCompareStat *st, void *ud) {
    FILE *fo = (FILE *)ud;
    char ta[32] = "?", tb[32] = "?";
    llmcodec_ggml_type_name(st->ggml_type_a, ta, sizeof ta);
    llmcodec_ggml_type_name(st->ggml_type_b, tb, sizeof tb);
    fprintf(fo, "  [%s] %s/%s n=%llu rel_l2=%.6g avg_diff=%.4f%% max_abs=%.6g\n",
            name, ta, tb, (unsigned long long)st->numel, st->rel_l2, st->avg_diff_pct, st->max_abs);
    return 0;
}

FmlxCliStatus fmlx_compare_exec(const FmlxCliArgs *a, FILE *out, FILE *err) {
    FILE *fo = out ? out : stdout;
    FILE *fe = err ? err : stderr;
    if (!a->repo[0] || !a->path[0]) {
        fprintf(fe, "fastllm compare: attesi <a.gguf> e <b.gguf>\n");
        return FMLX_CLI_USAGE;
    }

    LlmCodecGgufCompareSummary s;
    /* --quiet: solo la riga di riepilogo (dettaglio per-tensore soppresso). */
    if (llmcodec_gguf_compare(a->repo, a->path, (a->glob && *a->glob) ? a->glob : NULL,
                              a->quiet ? NULL : compare_cb, fo, &s) != 0) {
        fprintf(fe, "fastllm compare: impossibile leggere '%s' o '%s' come GGUF\n", a->repo, a->path);
        return FMLX_CLI_FAILED;
    }

    fprintf(fo,
            "compare %s vs %s: n_a=%llu n_b=%llu matched=%llu only_a=%llu only_b=%llu "
            "mismatch=%llu skipped=%llu rel_l2=%.6g avg_diff=%.4f%% max_diff=%.4f%% @%s\n",
            a->repo, a->path, (unsigned long long)s.n_a, (unsigned long long)s.n_b,
            (unsigned long long)s.matched, (unsigned long long)s.only_a, (unsigned long long)s.only_b,
            (unsigned long long)s.mismatch, (unsigned long long)s.skipped,
            s.rel_l2_agg, s.avg_diff_pct, s.max_diff_pct, s.max_diff_name[0] ? s.max_diff_name : "-");
    return FMLX_CLI_OK;
}
