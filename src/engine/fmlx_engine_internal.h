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
#define FMLX_ENGINE_EOS_MAX 8

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
    /* L'ABI accetta UN solo eos: `eos` è quello passato all'engine, `eos_list` la lista
     * completa di `config.json` (fermarsi su UNO QUALSIASI). Gli eos secondari li fa
     * scattare il consumer, token per token, nel ciclo di decode. */
    int32_t eos;
    int32_t eos_list[FMLX_ENGINE_EOS_MAX];
    int n_eos;
    FmlxTplKind tpl;
    /* Chat template REALE (Jinja) dal tokenizer_config.json, o NULL se il modello non ne
     * ha uno: in quel caso si usa il fallback di famiglia (`tpl`). */
    char *chat_tpl;
    size_t chat_tpl_len;
    /* `bos_token` (stringa) del config, "" se assente: alcune template lo rendono. */
    char bos[64];
};

/* Una generazione INCREMENTALE: un token per chiamata di `fmlx_engine_next`, così il testo
 * esce appena prodotto (TTFT = primo token) invece che a generazione finita. */
struct FmlxEngineGen {
    const FmlxEngineModel *m;
    LlmEngineLlmSession session;
    const FmlxApiChat *c; /* per le stop sequences (vive quanto la richiesta) */

    int32_t *ids; /* token generati finora */
    int max_new;
    int n_gen;
    int ended; /* la generazione è finita (eos/stop/lunghezza) */

    /* Decodifica incrementale: `full` è il decode dell'intera sequenza generata; `emitted`
     * dice quanti byte sono già stati consegnati al consumer (in `text`). Un nuovo token
     * può cambiare la coda del decode, mai il prefisso già consegnato. */
    char *full;
    size_t full_len;
    size_t emitted;

    /* Testo in attesa di essere emesso (copia). `fmlx_engine_next` lo consegna a pezzi. */
    char *text;
    size_t text_len;
    size_t pos;

    uint64_t prompt_tokens;
    int n_out;
    int done; /* DONE già emesso */
    int err;  /* il motore ha fallito: ERR alla prossima chiamata */
    FmlxApiFinish finish;
    char stop_seq[FMLX_API_MAX_NAME];
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
