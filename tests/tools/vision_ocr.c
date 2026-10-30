/* vision_ocr.c — tool della lane VISION/OCR di FastLLM.
 *
 * Carica un modello multimodale (VLM/OCR) dal binario GGUF + il suo proiettore `mmproj`, e
 * legge un'immagine (o un audio) via il motore llama.cpp/mtmd dell'engine LLMEnv. È il punto
 * d'ingresso testabile della lane: la stessa `fmlx_vision_*` che il server userà.
 *
 * USO:
 *   vision_ocr --model <model.gguf> [--mmproj <mmproj.gguf>] --image <img> [--prompt <t>]
 *              [--max-tokens N] [--ctx N] [--gpu N]
 *   vision_ocr --model <model.gguf> [--mmproj <mmproj.gguf>] --audio <file> [--prompt <t>]
 *
 * ESEMPI:
 *   vision_ocr --model ~/ai/Models/gguf/PaddleOCR-VL-1.6-GGUF.gguf \
 *              --mmproj ~/ai/Models/gguf/PaddleOCR-VL-1.6-GGUF-mmproj.gguf --image doc.png
 */
#include "engine/fmlx_vision.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *arg_val(int argc, char **argv, const char *name) {
    for (int i = 1; i + 1 < argc; i++)
        if (strcmp(argv[i], name) == 0) return argv[i + 1];
    return NULL;
}

int main(int argc, char **argv) {
    const char *model = arg_val(argc, argv, "--model");
    const char *mmproj = arg_val(argc, argv, "--mmproj");
    const char *image = arg_val(argc, argv, "--image");
    const char *audio = arg_val(argc, argv, "--audio");
    const char *prompt = arg_val(argc, argv, "--prompt");
    const char *mt = arg_val(argc, argv, "--max-tokens");
    const char *cx = arg_val(argc, argv, "--ctx");
    const char *gp = arg_val(argc, argv, "--gpu");
    int max_tokens = mt ? atoi(mt) : 512;
    int n_ctx = cx ? atoi(cx) : 4096;
    int n_gpu = gp ? atoi(gp) : 99;
    char err[512] = "";
    FmlxVision *v;
    char *text;

    if (!model || (!image && !audio)) {
        fprintf(stderr,
                "uso: vision_ocr --model <model.gguf> [--mmproj <mmproj.gguf>] "
                "(--image <img> | --audio <file>) [--prompt <t>] [--max-tokens N] "
                "[--ctx N] [--gpu N]\n");
        return 2;
    }
    if (!fmlx_vision_available()) {
        fprintf(stderr, "vision_ocr: la lane vision non è disponibile in questo build\n");
        return 1;
    }
    v = fmlx_vision_load(model, mmproj, n_ctx, n_gpu, err, sizeof err);
    if (!v) { fprintf(stderr, "vision_ocr: %s\n", err); return 1; }

    text = audio ? fmlx_vision_read_audio(v, audio, prompt, max_tokens, err, sizeof err)
                 : fmlx_vision_read_image(v, image, prompt, max_tokens, err, sizeof err);
    if (!text) { fprintf(stderr, "vision_ocr: %s\n", err); fmlx_vision_free(v); return 1; }

    fputs(text, stdout);
    fputc('\n', stdout);
    fmlx_vision_free_str(text);
    fmlx_vision_free(v);
    return 0;
}
