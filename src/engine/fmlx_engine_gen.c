/* fmlx_engine_gen.c — render del prompt, prefill/decode via l'ABI `llmengine`, e l'iteratore
 * di eventi che il layer ② consuma.
 *
 * UN SOLO decode. `generate_ids` produce l'intera sequenza (l'ABI MLX non espone un callback
 * per-token); qui si decodifica UNA volta e si emettono i pezzi a valle. È una scelta, non un
 * ripiego: decodificare i prefissi token per token farebbe passare per il filo byte UTF-8
 * spezzati a metà, che il writer JSON rifiuta — la risposta corretta è tenere l'intero testo e
 * tagliarlo SOLO sui confini di codepoint. Il risultato su filo è SSE valido con più `delta`.
 */
#include "fmlx_engine_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- contenuto dei messaggi ---- */

static void trim_span(const char **p, size_t *n) {
    const char *s = *p;
    size_t len = *n;
    while (len && (s[0] == ' ' || s[0] == '\t' || s[0] == '\n' || s[0] == '\r')) {
        s++;
        len--;
    }
    while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\n' ||
                   s[len - 1] == '\r'))
        len--;
    *p = s;
    *n = len;
}

/* Aggiunge il testo di un `content` (stringa, oppure array di stringhe/blocchi `{text}`).
 * Ritorna 0/-1. `trim` applica il trim del template (Llama-3 fa `| trim`). */
static int content_text(const FmlxJsonValue *content, FmlxSb *out, int trim) {
    if (!content) return 0;
    if (content->type == FMLX_JSON_STR) {
        const char *p = fmlx_json_str(content);
        size_t n = fmlx_json_str_len(content);
        if (!p) return -1;
        if (trim) trim_span(&p, &n);
        return fmlx_sb_add(out, p, n);
    }
    if (content->type != FMLX_JSON_ARR) return 0;
    for (size_t i = 0; i < fmlx_json_arr_n(content); i++) {
        const FmlxJsonValue *b = fmlx_json_arr_at(content, i);
        if (!b) continue;
        if (b->type == FMLX_JSON_STR) {
            const char *p = fmlx_json_str(b);
            size_t n = fmlx_json_str_len(b);
            if (p && fmlx_sb_add(out, p, n) != 0) return -1;
        } else if (b->type == FMLX_JSON_OBJ) {
            /* blocco testo (Anthropic e OpenAI a blocchi): si prende `text`, il resto
             * (immagini/tool) non è testo e in questa prima lane non è reso. */
            const FmlxJsonValue *t = fmlx_json_obj_get(b, "text");
            if (t && t->type == FMLX_JSON_STR) {
                const char *p = fmlx_json_str(t);
                size_t n = fmlx_json_str_len(t);
                if (p && fmlx_sb_add(out, p, n) != 0) return -1;
            }
        }
    }
    return 0;
}

static void role_of(const FmlxJsonValue *msg, const char **role, size_t *rn) {
    const FmlxJsonValue *r = fmlx_json_obj_get(msg, "role");
    *role = NULL;
    *rn = 0;
    if (r && r->type == FMLX_JSON_STR) {
        *role = fmlx_json_str(r);
        *rn = fmlx_json_str_len(r);
    }
}

static int is_system_role(const char *role, size_t rn) {
    return (rn == 6 && !memcmp(role, "system", 6)) ||
           (rn == 9 && !memcmp(role, "developer", 9));
}

/* ---- render del prompt ---- */

int fmlx_engine_render_chat(const FmlxEngineModel *m, const FmlxApiChat *c, FmlxSb *b, char *err,
                            size_t errcap) {
    char *sysmem = NULL;
    FmlxSlice sys = {NULL, 0};

    if (!m || !c || !b) return -1;
    sysmem = malloc(1u << 20);
    if (!sysmem) {
        snprintf(err, errcap, "out of memory building the system prompt");
        return -1;
    }
    if (fmlx_api_join_system(c, sysmem, 1u << 20, &sys) != FMLX_API_OK) {
        free(sysmem);
        snprintf(err, errcap, "the system prompt does not fit the render buffer");
        return -1;
    }

    if (m->tpl == FMLX_TPL_LLAMA3) {
        /* Mirror del template Llama-3 senza tool: BOS, blocco system (con le date di default
         * del template), i turni non-system, e l'opener assistant. */
        if (fmlx_sb_cstr(b, "<|begin_of_text|>") != 0) goto oom;
        if (fmlx_sb_cstr(b, "<|start_header_id|>system<|end_header_id|>\n\n"
                            "Cutting Knowledge Date: December 2023\nToday Date: 26 Jul 2024\n\n") != 0)
            goto oom;
        if (sys.p && fmlx_sb_add(b, sys.p, sys.n) != 0) goto oom;
        if (fmlx_sb_cstr(b, "<|eot_id|>") != 0) goto oom;
        for (size_t i = 0; c->messages && i < fmlx_json_arr_n(c->messages); i++) {
            const FmlxJsonValue *msg = fmlx_json_arr_at(c->messages, i);
            const char *role;
            size_t rn;
            role_of(msg, &role, &rn);
            if (!role || is_system_role(role, rn)) continue;
            if (fmlx_sb_cstr(b, "<|start_header_id|>") != 0 ||
                fmlx_sb_add(b, role, rn) != 0 ||
                fmlx_sb_cstr(b, "<|end_header_id|>\n\n") != 0 ||
                content_text(fmlx_json_obj_get(msg, "content"), b, 1) != 0 ||
                fmlx_sb_cstr(b, "<|eot_id|>") != 0)
                goto oom;
        }
        if (fmlx_sb_cstr(b, "<|start_header_id|>assistant<|end_header_id|>\n\n") != 0) goto oom;
    } else if (m->tpl == FMLX_TPL_CHATML) {
        if (fmlx_sb_cstr(b, "<|im_start|>system\n") != 0) goto oom;
        if (sys.p && fmlx_sb_add(b, sys.p, sys.n) != 0) goto oom;
        if (fmlx_sb_cstr(b, "<|im_end|>\n") != 0) goto oom;
        for (size_t i = 0; c->messages && i < fmlx_json_arr_n(c->messages); i++) {
            const FmlxJsonValue *msg = fmlx_json_arr_at(c->messages, i);
            const char *role;
            size_t rn;
            role_of(msg, &role, &rn);
            if (!role || is_system_role(role, rn)) continue;
            if (fmlx_sb_cstr(b, "<|im_start|>") != 0 || fmlx_sb_add(b, role, rn) != 0 ||
                fmlx_sb_cstr(b, "\n") != 0 ||
                content_text(fmlx_json_obj_get(msg, "content"), b, 0) != 0 ||
                fmlx_sb_cstr(b, "<|im_end|>\n") != 0)
                goto oom;
        }
        if (fmlx_sb_cstr(b, "<|im_start|>assistant\n") != 0) goto oom;
    } else {
        /* Fallback generico (template sconosciuto): NON è il template del modello, ed è per
         * questo che il server logga la famiglia scelta. Forma minima e leggibile. */
        if (sys.p && sys.n) {
            if (fmlx_sb_add(b, sys.p, sys.n) != 0 || fmlx_sb_cstr(b, "\n\n") != 0) goto oom;
        }
        for (size_t i = 0; c->messages && i < fmlx_json_arr_n(c->messages); i++) {
            const FmlxJsonValue *msg = fmlx_json_arr_at(c->messages, i);
            const char *role;
            size_t rn;
            role_of(msg, &role, &rn);
            if (!role || is_system_role(role, rn)) continue;
            if (fmlx_sb_add(b, role, rn) != 0 || fmlx_sb_cstr(b, ": ") != 0 ||
                content_text(fmlx_json_obj_get(msg, "content"), b, 0) != 0 ||
                fmlx_sb_cstr(b, "\n") != 0)
                goto oom;
        }
        if (fmlx_sb_cstr(b, "assistant:") != 0) goto oom;
    }
    free(sysmem);
    return 0;
oom:
    free(sysmem);
    snprintf(err, errcap, "out of memory building the prompt");
    return -1;
}

/* ---- tokenizzazione ---- */

static int32_t *tokenize(llmtoken_handle tok, const char *text, int *out_n, char *err,
                         size_t errcap) {
    int n = llmtoken_encode(tok, text, NULL, 0);
    int32_t *ids;

    if (n < 0) {
        snprintf(err, errcap, "the tokenizer rejected the prompt");
        return NULL;
    }
    if (n == 0) {
        snprintf(err, errcap, "the prompt tokenizes to zero tokens");
        return NULL;
    }
    ids = malloc((size_t)n * sizeof *ids);
    if (!ids) {
        snprintf(err, errcap, "out of memory allocating %d prompt tokens", n);
        return NULL;
    }
    if (llmtoken_encode(tok, text, ids, n) != n) {
        free(ids);
        snprintf(err, errcap, "the tokenizer produced an inconsistent token count");
        return NULL;
    }
    *out_n = n;
    return ids;
}

/* Decodifica la sequenza COMPLETA. Ritorna una stringa malloc (NUL-terminata) + lunghezza. */
static char *detokenize(llmtoken_handle tok, const int32_t *ids, int n, size_t *out_len) {
    int need = llmtoken_decode(tok, ids, n, NULL, 0);
    char *s;

    if (need < 0) return NULL;
    s = malloc((size_t)need + 1);
    if (!s) return NULL;
    if (llmtoken_decode(tok, ids, n, s, need + 1) != need) {
        free(s);
        return NULL;
    }
    *out_len = (size_t)need;
    return s;
}

/* ---- stop sequences: tronca al primo match e dice quale. ---- */

static size_t apply_stop(const FmlxApiChat *c, char *text, size_t len, const char **which) {
    size_t best = (size_t)-1;
    const char *best_p = NULL;

    for (uint32_t i = 0; i < c->n_stop; i++) {
        const char *s = c->stop[i].p;
        uint32_t sn = c->stop[i].n;
        if (!s || sn == 0 || sn > len) continue;
        for (size_t k = 0; k + sn <= len; k++) {
            if (memcmp(text + k, s, sn) == 0) {
                if (k < best) {
                    best = k;
                    best_p = s;
                }
                break;
            }
        }
    }
    if (best == (size_t)-1) return len;
    *which = best_p;
    return best;
}

/* ---- avvio della generazione ---- */

static FmlxEngineGen *start(const FmlxEngineModel *m, const int32_t *ids, int n_prompt,
                            int max_tokens, const LlmEngineLlmSampling *sp, const FmlxApiChat *c,
                            char *err, size_t errcap) {
    LlmEngineLlmSession s;
    int32_t *out;
    int got;
    FmlxEngineGen *g;

    if (n_prompt <= 0) {
        snprintf(err, errcap, "empty prompt");
        return NULL;
    }
    if (max_tokens < 1) max_tokens = 1;
    if (max_tokens > FMLX_ENGINE_MAX_NEW) max_tokens = FMLX_ENGINE_MAX_NEW;

    s = m->eng->create_session(m->model);
    if (!s) {
        snprintf(err, errcap, "the engine could not create a session");
        return NULL;
    }
    if (sp && m->eng->set_sampling) m->eng->set_sampling(s, sp);
    if (m->eng->append_ids && m->eng->append_ids(s, ids, n_prompt) != 0) {
        m->eng->destroy_session(s);
        snprintf(err, errcap, "the engine failed to ingest the prompt");
        return NULL;
    }
    out = malloc((size_t)max_tokens * sizeof *out);
    if (!out) {
        m->eng->destroy_session(s);
        snprintf(err, errcap, "out of memory for %d output tokens", max_tokens);
        return NULL;
    }
    got = m->eng->generate_ids(s, max_tokens, m->eos, out);
    m->eng->destroy_session(s);
    if (got < 0) {
        free(out);
        snprintf(err, errcap, "the engine failed while generating");
        return NULL;
    }
    if (got > max_tokens) got = max_tokens; /* contratto: mai più del richiesto */

    g = calloc(1, sizeof *g);
    if (!g) {
        free(out);
        snprintf(err, errcap, "out of memory for the generation state");
        return NULL;
    }
    g->prompt_tokens = (uint64_t)n_prompt;
    g->n_out = got;
    g->finish = got >= max_tokens ? FMLX_API_FINISH_LENGTH : FMLX_API_FINISH_STOP;
    g->text = detokenize(m->tok, out, got, &g->text_len);
    free(out);
    if (!g->text) {
        free(g);
        snprintf(err, errcap, "the tokenizer could not decode the output");
        return NULL;
    }
    if (c) {
        const char *which = NULL;
        size_t cut = apply_stop(c, g->text, g->text_len, &which);
        if (cut < g->text_len) {
            g->text_len = cut;
            g->text[cut] = '\0';
            g->finish = FMLX_API_FINISH_STOPSEQ;
            if (which) snprintf(g->stop_seq, sizeof g->stop_seq, "%s", which);
        }
    }
    return g;
}

/* ---- API pubblica ---- */

static void sampling_of(const FmlxApiChat *c, LlmEngineLlmSampling *sp) {
    memset(sp, 0, sizeof *sp);
    sp->temperature = c->has_temperature ? (float)c->temperature : 1.0f;
    sp->top_p = c->has_top_p ? (float)c->top_p : 1.0f;
    sp->top_k = c->has_top_k ? (int)c->top_k : 0;
    sp->repetition_penalty = 1.0f;
    sp->frequency_penalty = c->has_frequency ? (float)c->frequency_penalty : 0.0f;
    sp->presence_penalty = c->has_presence ? (float)c->presence_penalty : 0.0f;
    sp->seed = c->has_seed ? c->seed : 0;
}

FmlxEngineGen *fmlx_engine_chat(FmlxEngineModel *m, const FmlxApiChat *c, char *err,
                                size_t errcap) {
    FmlxSb prompt = {0};
    int32_t *ids;
    int n = 0;
    int max_tokens;
    LlmEngineLlmSampling sp;
    FmlxEngineGen *g;

    if (!m || !c) return NULL;
    if (fmlx_engine_render_chat(m, c, &prompt, err, errcap) != 0) {
        fmlx_sb_free(&prompt);
        return NULL;
    }
    ids = tokenize(m->tok, prompt.p ? prompt.p : "", &n, err, errcap);
    fmlx_sb_free(&prompt);
    if (!ids) return NULL;
    max_tokens = c->has_max_tokens ? (int)c->max_tokens : 256;
    sampling_of(c, &sp);
    g = start(m, ids, n, max_tokens, &sp, c, err, errcap);
    free(ids);
    return g;
}

FmlxEngineGen *fmlx_engine_prompt(FmlxEngineModel *m, const char *prompt, int max_tokens,
                                  float temperature, float top_p, int top_k, float presence,
                                  float frequency, int64_t seed, char *err, size_t errcap) {
    int32_t *ids;
    int n = 0;
    LlmEngineLlmSampling sp;
    FmlxEngineGen *g;

    if (!m || !prompt) return NULL;
    ids = tokenize(m->tok, prompt, &n, err, errcap);
    if (!ids) return NULL;
    memset(&sp, 0, sizeof sp);
    sp.temperature = temperature;
    sp.top_p = top_p;
    sp.top_k = top_k;
    sp.repetition_penalty = 1.0f;
    sp.presence_penalty = presence;
    sp.frequency_penalty = frequency;
    sp.seed = seed;
    g = start(m, ids, n, max_tokens, &sp, NULL, err, errcap);
    free(ids);
    return g;
}

int fmlx_engine_count_tokens(FmlxEngineModel *m, const FmlxApiChat *c, int *out, char *err,
                             size_t errcap) {
    FmlxSb prompt = {0};
    int n;

    if (!m || !c || !out) return -1;
    if (fmlx_engine_render_chat(m, c, &prompt, err, errcap) != 0) {
        fmlx_sb_free(&prompt);
        return -1;
    }
    n = llmtoken_encode(m->tok, prompt.p ? prompt.p : "", NULL, 0);
    fmlx_sb_free(&prompt);
    if (n < 0) {
        snprintf(err, errcap, "the tokenizer rejected the prompt");
        return -1;
    }
    *out = n;
    return 0;
}

/* Avanza di una sequenza UTF-8 completa (o fino a `limit`). */
static size_t utf8_step(const char *s, size_t i, size_t len) {
    unsigned char c = (unsigned char)s[i];
    size_t need = 1;
    if (c >= 0xF0) need = 4;
    else if (c >= 0xE0) need = 3;
    else if (c >= 0xC0) need = 2;
    if (i + need > len) return len; /* tronco: si emette quel che c'è */
    for (size_t k = 1; k < need; k++)
        if (((unsigned char)s[i + k] & 0xC0) != 0x80) return i + 1; /* byte non valido: 1 */
    return i + need;
}

#define FMLX_ENGINE_CHUNK 96u

FmlxApiGenSt fmlx_engine_next(void *ctx, FmlxApiEvent *ev, FmlxApiErrCtx *err) {
    FmlxEngineGen *g = ctx;

    (void)err;
    if (!g || !ev) return FMLX_API_GEN_ERR;
    if (g->pos < g->text_len) {
        size_t end = g->pos;
        while (end < g->text_len && end - g->pos < FMLX_ENGINE_CHUNK)
            end = utf8_step(g->text, end, g->text_len);
        ev->ev = FMLX_API_EV_TEXT;
        ev->text = g->text + g->pos;
        ev->n = (uint32_t)(end - g->pos);
        g->pos = end;
        return FMLX_API_GEN_MORE;
    }
    if (!g->done) {
        g->done = 1;
        ev->ev = FMLX_API_EV_DONE;
        ev->finish = g->finish;
        ev->prompt_tokens = g->prompt_tokens;
        ev->completion_tokens = (uint64_t)g->n_out;
        ev->cached_tokens = 0;
        ev->stop_sequence = g->finish == FMLX_API_FINISH_STOPSEQ && g->stop_seq[0] ? g->stop_seq
                                                                                   : NULL;
        return FMLX_API_GEN_MORE;
    }
    return FMLX_API_GEN_END;
}

void fmlx_engine_gen_free(FmlxEngineGen *g) {
    if (!g) return;
    free(g->text);
    free(g);
}
