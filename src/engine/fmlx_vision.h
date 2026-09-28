/* fmlx_vision.h — lane VISION/OCR di FastLLM.
 *
 * I modelli multimodali (VLM/OCR/ASR) NON passano dal motore MLX (la cui vtable ha
 * `vision_* = NULL`): passano dal motore llama.cpp/mtmd dell'engine LLMEnv, esposto
 * dall'ABI comune `llmengine_llm_engine_llamacpp()`. Questo file è il ponte:
 * carica un VLM (GGUF del testo + GGUF `mmproj` del proiettore) e legge immagini/audio.
 *
 * È una lane a sé: il modello multimodal NON è un modello di chat MLX, e la sua cache
 * (contesto + proiettore) vive qui, non nella cache residente dei modelli di testo. */
#ifndef FMLX_VISION_H
#define FMLX_VISION_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FmlxVision FmlxVision;

/* 1 se la lane vision è disponibile in questo build (motore llama presente e con vision). */
int fmlx_vision_available(void);

/* Carica un VLM: `model` = GGUF del modello testo (o path multimodale), `mmproj` = GGUF del
 * proiettore multimodale. Ritorna NULL e scrive la causa in `err`. */
FmlxVision *fmlx_vision_load(const char *model, const char *mmproj, int n_ctx, int n_gpu_layers,
                             char *err, size_t errcap);

/* OCR/vision: testo riconosciuto in `image_path`. Ritorna una stringa malloc (liberare con
 * `fmlx_vision_free_str`) o NULL con la causa in `err`. `prompt` NULL = "OCR:" (default mtmd). */
char *fmlx_vision_read_image(FmlxVision *v, const char *image_path, const char *prompt,
                             int max_tokens, char *err, size_t errcap);

/* Audio (ASR): trascrizione di `audio_path`. Stessa ownership di `fmlx_vision_read_image`. */
char *fmlx_vision_read_audio(FmlxVision *v, const char *audio_path, const char *prompt,
                             int max_tokens, char *err, size_t errcap);

void fmlx_vision_free_str(char *s);
void fmlx_vision_free(FmlxVision *v);

#ifdef __cplusplus
}
#endif

#endif /* FMLX_VISION_H */
