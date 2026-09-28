/* fmlx_engine_internal.h — il modello residente e la generazione: strutture condivise fra
 * `fmlx_engine.c` (caricamento/risoluzione) e `fmlx_engine_gen.c` (render + decode).
 * Header INTERNO alla lane: non fa parte della superficie esposta (`fmlx_engine.h`). */
#ifndef FMLX_ENGINE_INTERNAL_H
#define FMLX_ENGINE_INTERNAL_H

#include "fmlx_engine.h"

#include "llmengine.h"
#include "llmtoken.h"

#include <stdint.h>

#define FMLX_ENGINE_PATH_MAX 4096
#define FMLX_ENGINE_CHATTPL_MAX 65536u

typedef enum {
    FMLX_TPL_GENERIC = 0, /* fallback: "role: content\n" + opener assistant */
    FMLX_TPL_LLAMA3 = 1,  /* <|begin_of_text|> ... <|eot_id|> */
    FMLX_TPL_CHATML = 2   /* <|im_start|>role ... <|im_end|> */
} FmlxTplKind;

struct FmlxEngineModel {
    char name[FMLX_ENGINE_NAME_MAX];
    char dir[FMLX_ENGINE_PATH_MAX];
    const LlmEngineLlmEngine *eng;
    LlmEngineLlmModel model;
    llmtoken_handle tok;
    int32_t eos;
    FmlxTplKind tpl;
};

struct FmlxEngineGen {
    /* prompt/output contati in token */
    uint64_t prompt_tokens;
    int n_out;
    /* testo decodificato, emesso a pezzi */
    char *text;
    size_t text_len;
    size_t pos;
    int done;
    FmlxApiFinish finish;
    char stop_seq[FMLX_API_MAX_NAME];
    int stop_emitted;
};

/* Stringa dinamica minimale. */
typedef struct {
    char *p;
    size_t len, cap;
} FmlxSb;

int fmlx_sb_add(FmlxSb *b, const char *s, size_t n);
int fmlx_sb_cstr(FmlxSb *b, const char *s);
void fmlx_sb_free(FmlxSb *b);

/* Carica il modello (risoluzione + tokenizer + eos + template). Ritorna 0/-1 con `err`. */
int fmlx_engine_load(const char *cache_root, const char *name, FmlxEngineModel *out, char *err,
                     size_t errcap);

/* Render del prompt per una chat. Scrive in `b`. Ritorna 0/-1. */
int fmlx_engine_render_chat(const FmlxEngineModel *m, const FmlxApiChat *c, FmlxSb *b,
                            char *err, size_t errcap);

#endif /* FMLX_ENGINE_INTERNAL_H */
