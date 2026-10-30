/* fmlx_vision.c — implementazione della lane VISION/OCR (vedi fmlx_vision.h).
 *
 * Usa l'ABI comune `llmengine` del motore llama.cpp/mtmd di LLMEnv:
 *   llmengine_llm_engine_llamacpp()->vision_load / vision_read_image / vision_read_audio /
 *   vision_unload / free_str
 * Nessuna logica di formato qui: il dequant e il forward multimodale stanno nel motore.
 *
 * PERCHE' SEMPRE llama.cpp E NON IL MOTORE DEL MODELLO. Ne' il motore MLX ne' i due motori ds4
 * espongono `vision_*` nell'ABI: la visione vive in mtmd (llama.cpp) e li' resta. Questa scelta e'
 * deliberata — un `CAP_VISION` dichiarato e non implementato sarebbe una promessa falsa, e il
 * percorso multimodale di GMBasic (che usa `llm_ai_llamacpp`) non va toccato. */
#include "fmlx_vision.h"

#include "llmengine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct FmlxVision {
    const LlmEngineLlmEngine *eng;
    LlmEngineLlmVision h;
};

static void set_err(char *err, size_t cap, const char *msg) {
    if (err && cap) snprintf(err, cap, "%s", msg ? msg : "");
}

int fmlx_vision_available(void) {
    const LlmEngineLlmEngine *e = llmengine_llm_engine_llamacpp();
    return (e && e->vision_load && e->vision_read_image) ? 1 : 0;
}

FmlxVision *fmlx_vision_load(const char *model, const char *mmproj, int n_ctx, int n_gpu_layers,
                             char *err, size_t errcap) {
    const LlmEngineLlmEngine *e = llmengine_llm_engine_llamacpp();
    LlmEngineLlmVision h;
    FmlxVision *v;

    if (!model || !*model) { set_err(err, errcap, "vision: model path missing"); return NULL; }
    if (!e || !e->vision_load) {
        set_err(err, errcap,
                "vision: the llama.cpp/mtmd engine is not available in this build "
                "(MLX and ds4 have no vision: use llama.cpp/mtmd)");
        return NULL;
    }
    h = e->vision_load(model, (mmproj && *mmproj) ? mmproj : NULL, n_ctx, n_gpu_layers);
    if (!h) { set_err(err, errcap, "vision: the engine could not load the multimodal model"); return NULL; }
    v = calloc(1, sizeof *v);
    if (!v) { if (e->vision_unload) e->vision_unload(h); set_err(err, errcap, "vision: out of memory"); return NULL; }
    v->eng = e;
    v->h = h;
    return v;
}

static char *read_common(FmlxVision *v, const char *path, const char *prompt, int max_tokens,
                         int audio, char *err, size_t errcap) {
    const char *p;
    char *out;

    if (!v || !v->h) { set_err(err, errcap, "vision: no model loaded"); return NULL; }
    if (!path || !*path) { set_err(err, errcap, "vision: file path missing"); return NULL; }
    p = (prompt && *prompt) ? prompt : "OCR:";
    out = audio ? (v->eng->vision_read_audio ? v->eng->vision_read_audio(v->h, path, p, max_tokens) : NULL)
                : (v->eng->vision_read_image ? v->eng->vision_read_image(v->h, path, p, max_tokens) : NULL);
    if (!out) set_err(err, errcap, "vision: the engine returned no text");
    return out;
}

char *fmlx_vision_read_image(FmlxVision *v, const char *image_path, const char *prompt,
                             int max_tokens, char *err, size_t errcap) {
    return read_common(v, image_path, prompt, max_tokens, 0, err, errcap);
}

char *fmlx_vision_read_audio(FmlxVision *v, const char *audio_path, const char *prompt,
                             int max_tokens, char *err, size_t errcap) {
    return read_common(v, audio_path, prompt, max_tokens, 1, err, errcap);
}

void fmlx_vision_free_str(char *s) {
    const LlmEngineLlmEngine *e = llmengine_llm_engine_llamacpp();
    if (!s) return;
    if (e && e->free_str) e->free_str(s);
    else free(s);
}

void fmlx_vision_free(FmlxVision *v) {
    if (!v) return;
    if (v->eng && v->eng->vision_unload && v->h) v->eng->vision_unload(v->h);
    free(v);
}
