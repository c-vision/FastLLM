/* fmlx_media.c — `fastllm run <alias> <file>` per i modelli registrati con --modality ocr|vlm|audio.
 *
 * Un modello OCR/VLM/audio non e' un modello di chat MLX: passa dalla lane vision (llama.cpp/mtmd di LLMEnv,
 * vedi engine/fmlx_vision.h). Il secondo posizionale di `run` e' il FILE (immagine o audio); l'istruzione al modello
 * (es. "OCR:", "Table Recognition:", "Descrivi l'immagine") si passa con --system, default "OCR:" per ocr. */
#include "fmlx/fmlx_cli.h"
#include "engine/fmlx_vision.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

FmlxCliStatus fmlx_media_run_exec(const FmlxCliArgs *a, const char *model, const char *mmproj, const char *modality,
                                  FILE *out, FILE *err) {
    struct stat st;
    char emsg[512] = "";
    FmlxVision *v;
    char *text;
    int audio = !strcmp(modality, "audio");
    int max_tokens = a->max_tokens > 0 ? (int)a->max_tokens : 1024;

    if (!out) out = stdout;
    if (!err) err = stderr;
    if (!a->prompt[0]) {
        fprintf(err, "fastllm run %s <file>: il modello e' '%s', serve il file %s come secondo argomento\n", a->repo,
                modality, audio ? "audio" : "immagine");
        return FMLX_CLI_USAGE;
    }
    if (stat(a->prompt, &st) != 0 || !S_ISREG(st.st_mode)) {
        fprintf(err, "fastllm run: file non trovato: %s\n", a->prompt);
        return FMLX_CLI_FAILED;
    }
    if (!fmlx_vision_available()) {
        fprintf(err, "fastllm run: la lane vision (llama.cpp/mtmd) non e' disponibile in questo build\n");
        return FMLX_CLI_NOT_WIRED;
    }
    v = fmlx_vision_load(model, mmproj, 4096, 99, emsg, sizeof emsg);
    if (!v) { fprintf(err, "fastllm run: %s\n", emsg); return FMLX_CLI_FAILED; }
    text = audio ? fmlx_vision_read_audio(v, a->prompt, a->system, max_tokens, emsg, sizeof emsg)
                 : fmlx_vision_read_image(v, a->prompt, a->system, max_tokens, emsg, sizeof emsg);
    if (!text) {
        fprintf(err, "fastllm run: %s\n", emsg[0] ? emsg : "nessun testo riconosciuto");
        fmlx_vision_free(v);
        return FMLX_CLI_FAILED;
    }
    fprintf(out, "%s\n", text);
    fmlx_vision_free_str(text);
    fmlx_vision_free(v);
    return FMLX_CLI_OK;
}
