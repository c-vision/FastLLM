/* fmlx_asr.c — `fastllm asr <model_dir> <file.wav>`: speech-to-text Qwen3-ASR.
 *
 * Engine nativo C in LLMEnv/LLMEngine/asr (target `llm_asr`) — la STESSA sorgente usata da GMBasic
 * (superficie BASIC `ASR.RECOGNIZE`): nessuna copia. Il testo esce TOKEN-LIVE su stdout (streaming:
 * chunk 2 s + cache finestre encoder 8 s + prefix rollback). Al termine, una riga vuota. */
#include "fmlx/fmlx_cli.h"
#include "fmlx/fmlx_paths.h"
#include "gmb_asr.h"
#include "gmb_asr_tok.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int asr_is_dir(const char *p) { struct stat s; return stat(p, &s) == 0 && S_ISDIR(s.st_mode); }
static const char *asr_base(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }

/* Risolve la dir del modello: percorso diretto, registro locale (stile Ollama), oppure `paths`. */
static int asr_resolve(const char *spec, char *out, size_t cap) {
    struct stat st;
    char rp[FMLX_HF_PATH_MAX], root[FMLX_HF_PATH_MAX], t[FMLX_HF_PATH_MAX];
    const char *base = asr_base(spec);
    int np, i;
    if (stat(spec, &st) == 0) { snprintf(out, cap, "%s", spec); return 0; }
    if (fmlx_registry_resolve(spec, rp, sizeof rp, NULL, 0, NULL, 0)) { snprintf(out, cap, "%s", rp); return 0; }
    np = fmlx_paths_count();
    for (i = 0; i < np; i++) {
        if (fmlx_paths_get(i, root, sizeof root) != 0) continue;
        snprintf(t, sizeof t, "%s/%s", root, spec); if (stat(t, &st) == 0) { snprintf(out, cap, "%s", t); return 0; }
        snprintf(t, sizeof t, "%s/%s", root, base); if (stat(t, &st) == 0) { snprintf(out, cap, "%s", t); return 0; }
    }
    return -1;
}

typedef struct { FILE *out; } AsrSink;
static void asr_emit(const char *piece, int len, void *ctx) {
    AsrSink *s = (AsrSink *)ctx;
    if (s && s->out && piece && len > 0) { fwrite(piece, 1, (size_t)len, s->out); fflush(s->out); }
}
static const char *asr_piece(int id, void *ctx) { return gmb_asr_tok_decode((GmbAsrTok *)ctx, id); }

FmlxCliStatus fmlx_asr_exec(const FmlxCliArgs *a, FILE *out, FILE *err) {
    char model[FMLX_HF_PATH_MAX];
    GmbAsrModel *m;
    GmbAsrTok *t;
    float *samples;
    int ns = 0;
    AsrSink sink;
    char *text;

    if (!out) out = stdout;
    if (!err) err = stderr;
    if (!a || !a->repo[0] || !a->path[0]) {
        fprintf(err, "fastllm asr <model_dir> <file.wav>: servono il modello e il WAV\n");
        return FMLX_CLI_USAGE;
    }
    if (asr_resolve(a->repo, model, sizeof model) != 0 || !asr_is_dir(model)) {
        fprintf(err, "fastllm asr: modello '%s' non trovato (percorso/registro/`paths`)\n", a->repo);
        return FMLX_CLI_FAILED;
    }
    m = gmb_asr_load_model(model);
    if (!m) { fprintf(err, "fastllm asr: load modello fallito (%s)\n", model); return FMLX_CLI_FAILED; }
    t = gmb_asr_tok_load(model);
    if (!t) { fprintf(err, "fastllm asr: load tokenizer fallito (%s)\n", model); gmb_asr_model_free(m); return FMLX_CLI_FAILED; }
    samples = gmb_asr_wav_load(a->path, &ns);
    if (!samples) { fprintf(err, "fastllm asr: load WAV fallito (%s)\n", a->path); gmb_asr_tok_free(t); gmb_asr_model_free(m); return FMLX_CLI_FAILED; }

    sink.out = out;
    text = gmb_asr_transcribe_stream(gmb_asr_model_ecfg(m), gmb_asr_model_encoder(m),
                                     gmb_asr_model_dcfg(m), gmb_asr_model_decoder(m),
                                     samples, ns, NULL, 0, NULL, 0, asr_piece, t, 5, 2.0f, asr_emit, &sink);
    if (!text) { fprintf(err, "fastllm asr: trascrizione fallita\n"); free(samples); gmb_asr_tok_free(t); gmb_asr_model_free(m); return FMLX_CLI_FAILED; }
    fprintf(out, "\n");
    free(text);
    free(samples);
    gmb_asr_tok_free(t);
    gmb_asr_model_free(m);
    return FMLX_CLI_OK;
}
