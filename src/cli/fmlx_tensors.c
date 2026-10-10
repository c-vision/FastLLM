/* fmlx_tensors.c — `fastllm tensors <model.gguf> [--diffable]`: dump di KV + tabella tensori.
 *
 * Port di `gguf-tools show` (antirez, MIT). Con `--diffable` OMETTE offset e byte dei tensori
 * (assoluti, legati alla disposizione del file) e le dimensioni numeriche del file: l'output resta
 * deterministico per confronti/golden (diff stabili fra due build dello stesso modello). */
#include "fmlx/fmlx_cli.h"

#include "llmcodec_gguf_read.h"
#include "llmcodec_ggml_quants.h"

#include <stdio.h>
#include <string.h>

static const char *kv_type_name(int t) {
    switch (t) {
        case 4: return "uint32"; case 5: return "int32"; case 6: return "float32";
        case 7: return "bool"; case 8: return "string"; case 9: return "array";
        case 10: return "uint64"; case 11: return "int64"; case 12: return "float64";
        default: return "?";
    }
}

FmlxCliStatus fmlx_tensors_exec(const FmlxCliArgs *a, FILE *out, FILE *err) {
    FILE *fo = out ? out : stdout;
    FILE *fe = err ? err : stderr;
    if (!a->repo[0]) { fprintf(fe, "fastllm tensors: atteso <model.gguf>\n"); return FMLX_CLI_USAGE; }

    LlmCodecGgufFileInfo fi; LlmCodecGgufMetaKVOwned *kv = NULL; int nkv = 0;
    if (llmcodec_gguf_read_meta(a->repo, &kv, &nkv, &fi) != 0) {
        fprintf(fe, "fastllm tensors: '%s' non e' un GGUF leggibile\n", a->repo);
        return FMLX_CLI_FAILED;
    }

    fprintf(fo, "%s (ver %u): %d key-value, %llu tensori\n",
            a->repo, fi.version, nkv, (unsigned long long)fi.n_tensors);

    for (int i = 0; i < nkv; i++) {
        int t = kv[i].type;
        fprintf(fo, "%s: [%s] ", kv[i].key, kv_type_name(t));
        switch (t) {
            case 4:  fprintf(fo, "%u", kv[i].u32); break;
            case 5:  fprintf(fo, "%d", kv[i].i32); break;
            case 6:  fprintf(fo, "%g", (double)kv[i].f32); break;
            case 7:  fprintf(fo, "%s", kv[i].b ? "true" : "false"); break;
            case 8:  fprintf(fo, "%s", kv[i].str ? kv[i].str : ""); break;
            case 9:  fprintf(fo, "[%llu items]", (unsigned long long)kv[i].arr_count); break;
            case 10: fprintf(fo, "%llu", (unsigned long long)kv[i].u64); break;
            case 11: fprintf(fo, "%lld", (long long)kv[i].i64); break;
            case 12: fprintf(fo, "%g", kv[i].f64); break;
            default: break;
        }
        fprintf(fo, "\n");
    }

    for (uint64_t i = 0; i < fi.n_tensors; i++) {
        LlmCodecGgufTensorInfo *ti = &fi.tensors[i];
        char tn[32] = "?";
        llmcodec_ggml_type_name(ti->ggml_type, tn, sizeof tn);
        fprintf(fo, "%s %s", tn, ti->name);
        if (!a->diffable)
            fprintf(fo, " @%llu, %llu byte",
                    (unsigned long long)(fi.data_start + ti->offset),
                    (unsigned long long)llmcodec_gguf_tensor_bytes(ti));
        fprintf(fo, ", dims [");
        for (int d = 0; d < ti->ndim; d++) fprintf(fo, "%s%llu", d ? "," : "", (unsigned long long)ti->dims[d]);
        fprintf(fo, "]\n");
    }

    llmcodec_gguf_free_info(&fi);
    llmcodec_gguf_meta_free(kv, nkv);
    return FMLX_CLI_OK;
}
