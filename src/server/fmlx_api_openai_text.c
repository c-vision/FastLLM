/* fmlx_api_openai_text.c — il dialetto OpenAI per `/v1/completions` (legacy, testo grezzo).
 *
 * PERCHÉ UN FILE. `/v1/completions` NON è `/v1/chat/completions` con un altro nome: cambia
 * l'oggetto (`text_completion`), cambia il campo della scelta (`text`, non `message`) e cambia
 * il frame di stream (niente `delta`, niente ruolo). Tenere i due in un file solo avrebbe
 * portato a un flag `is_text` sparso in ogni ramo, cioè a due dialetti intrecciati: qui il
 * confine è di FORMATO, come fra openai e anthropic.
 */
#include "fmlx/fmlx_api.h"

#include <string.h>

#define TEXT_CHUNK_CAP 8192u /* un frame di stream: un token, non una risposta intera */

/* L'oggetto `logprobs` per UN token, forma legacy di `/v1/completions`:
 * `{tokens:[t], token_logprobs:[p], top_logprobs:[null], text_offset:[o]}`. `top_logprobs`
 * ha un `null` per token: l'ABI dà solo la logprob del token scelto, non le alternative. */
static FmlxApiErr write_text_lp_token(FmlxJsonW *w, const char *tok, uint32_t n, double lp,
                                      uint32_t off) {
    if (fmlx_jsonw_obj_begin(w)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "tokens") || fmlx_jsonw_arr_begin(w) || fmlx_jsonw_str(w, tok, n) ||
        fmlx_jsonw_arr_end(w))
        return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "token_logprobs") || fmlx_jsonw_arr_begin(w) || fmlx_jsonw_f64(w, lp) ||
        fmlx_jsonw_arr_end(w))
        return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "top_logprobs") || fmlx_jsonw_arr_begin(w) || fmlx_jsonw_null(w) ||
        fmlx_jsonw_arr_end(w))
        return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "text_offset") || fmlx_jsonw_arr_begin(w) || fmlx_jsonw_i64(w, off) ||
        fmlx_jsonw_arr_end(w))
        return FMLX_API_INTERNAL;
    return fmlx_jsonw_obj_end(w) == FMLX_JSON_OK ? FMLX_API_OK : FMLX_API_INTERNAL;
}

/* `logprobs` dell'intera risposta legacy: le quattro liste parallele. */
static FmlxApiErr write_text_logprobs(FmlxJsonW *w, const FmlxApiChat *c, const FmlxApiAcc *a) {
    if (fmlx_jsonw_key(w, "logprobs")) return FMLX_API_INTERNAL;
    if (!c->want_logprobs || a->n_logprobs == 0)
        return fmlx_jsonw_null(w) == FMLX_JSON_OK ? FMLX_API_OK : FMLX_API_INTERNAL;
    if (fmlx_jsonw_obj_begin(w)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "tokens") || fmlx_jsonw_arr_begin(w)) return FMLX_API_INTERNAL;
    for (int i = 0; i < a->n_logprobs; i++)
        if (fmlx_jsonw_str(w, a->text + a->log_off[i], a->log_len[i])) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_arr_end(w)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "token_logprobs") || fmlx_jsonw_arr_begin(w)) return FMLX_API_INTERNAL;
    for (int i = 0; i < a->n_logprobs; i++)
        if (fmlx_jsonw_f64(w, (double)a->logprob[i])) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_arr_end(w)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "top_logprobs") || fmlx_jsonw_arr_begin(w)) return FMLX_API_INTERNAL;
    for (int i = 0; i < a->n_logprobs; i++)
        if (fmlx_jsonw_null(w)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_arr_end(w)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "text_offset") || fmlx_jsonw_arr_begin(w)) return FMLX_API_INTERNAL;
    for (int i = 0; i < a->n_logprobs; i++)
        if (fmlx_jsonw_i64(w, (int64_t)a->log_off[i])) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_arr_end(w) || fmlx_jsonw_obj_end(w)) return FMLX_API_INTERNAL;
    return FMLX_API_OK;
}

FmlxApiErr fmlx_api_openai_completion_text(const FmlxApiChat *c, const FmlxApiMeta *m,
                                           const FmlxApiAcc *a, FmlxJsonW *w) {
    if (!c || !m || !a || !w) return FMLX_API_INTERNAL;
    if (a->overflow) return FMLX_API_INTERNAL;

    if (fmlx_jsonw_obj_begin(w)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "id") || fmlx_jsonw_cstr(w, m->id ? m->id : "cmpl-unknown"))
        return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "object") || fmlx_jsonw_cstr(w, "text_completion"))
        return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "created") || fmlx_jsonw_i64(w, m->created)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "model") || fmlx_jsonw_str(w, c->model.p, c->model.n))
        return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "choices") || fmlx_jsonw_arr_begin(w)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_obj_begin(w)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "text") || fmlx_jsonw_str(w, a->text, a->text_len))
        return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "index") || fmlx_jsonw_i64(w, 0)) return FMLX_API_INTERNAL;
    if (write_text_logprobs(w, c, a) != FMLX_API_OK) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "finish_reason") ||
        fmlx_jsonw_cstr(w, fmlx_api_finish_openai(a->have_finish ? a->finish : FMLX_API_FINISH_STOP)))
        return FMLX_API_INTERNAL;
    if (fmlx_jsonw_obj_end(w) || fmlx_jsonw_arr_end(w)) return FMLX_API_INTERNAL;

    if (fmlx_jsonw_key(w, "usage") || fmlx_jsonw_obj_begin(w)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "prompt_tokens") || fmlx_jsonw_u64(w, a->prompt_tokens))
        return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "completion_tokens") || fmlx_jsonw_u64(w, a->completion_tokens))
        return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "total_tokens") ||
        fmlx_jsonw_u64(w, a->prompt_tokens + a->completion_tokens))
        return FMLX_API_INTERNAL;
    if (fmlx_jsonw_obj_end(w)) return FMLX_API_INTERNAL;
    if (fmlx_api_write_ignored(w, c)) return FMLX_API_INTERNAL;
    return fmlx_jsonw_obj_end(w) ? FMLX_API_INTERNAL : FMLX_API_OK;
}

/* Un frame: `{"id","object":"text_completion","created","model","choices":[{"text",...,
 * "finish_reason":null}]}`; sull'ultimo `finish_reason` valorizzato + `usage` se chiesto. */
static FmlxApiErr text_chunk(FmlxHttpRes *w, const FmlxApiMeta *m, const FmlxApiChat *c,
                             const char *text, size_t tn, const char *finish, const FmlxApiAcc *a,
                             int usage, const FmlxApiEvent *tev, uint32_t lp_off) {
    char buf[TEXT_CHUNK_CAP];
    FmlxJsonW j;
    fmlx_jsonw_init(&j, buf, sizeof buf, sizeof buf);
    if (fmlx_jsonw_obj_begin(&j)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(&j, "id") || fmlx_jsonw_cstr(&j, m->id ? m->id : "cmpl-unknown"))
        return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(&j, "object") || fmlx_jsonw_cstr(&j, "text_completion"))
        return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(&j, "created") || fmlx_jsonw_i64(&j, m->created)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(&j, "model") || fmlx_jsonw_str(&j, c->model.p, c->model.n))
        return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(&j, "choices") || fmlx_jsonw_arr_begin(&j)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_obj_begin(&j)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(&j, "text") || fmlx_jsonw_str(&j, text, tn)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(&j, "index") || fmlx_jsonw_i64(&j, 0)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(&j, "logprobs")) return FMLX_API_INTERNAL;
    if (tev && tev->has_logprob) {
        if (write_text_lp_token(&j, text, (uint32_t)tn, (double)tev->logprob, lp_off))
            return FMLX_API_INTERNAL;
    } else if (fmlx_jsonw_null(&j)) {
        return FMLX_API_INTERNAL;
    }
    if (fmlx_jsonw_key(&j, "finish_reason")) return FMLX_API_INTERNAL;
    if (finish ? fmlx_jsonw_cstr(&j, finish) : fmlx_jsonw_null(&j)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_obj_end(&j) || fmlx_jsonw_arr_end(&j)) return FMLX_API_INTERNAL;
    if (usage && a) {
        if (fmlx_jsonw_key(&j, "usage") || fmlx_jsonw_obj_begin(&j)) return FMLX_API_INTERNAL;
        if (fmlx_jsonw_key(&j, "prompt_tokens") || fmlx_jsonw_u64(&j, a->prompt_tokens))
            return FMLX_API_INTERNAL;
        if (fmlx_jsonw_key(&j, "completion_tokens") || fmlx_jsonw_u64(&j, a->completion_tokens))
            return FMLX_API_INTERNAL;
        if (fmlx_jsonw_key(&j, "total_tokens") ||
            fmlx_jsonw_u64(&j, a->prompt_tokens + a->completion_tokens))
            return FMLX_API_INTERNAL;
        if (fmlx_jsonw_obj_end(&j)) return FMLX_API_INTERNAL;
    }
    if (fmlx_jsonw_obj_end(&j)) return FMLX_API_INTERNAL;
    size_t n = 0;
    if (fmlx_jsonw_finish(&j, &n)) return FMLX_API_INTERNAL;
    FmlxSlice d = {buf, (uint32_t)n};
    return fmlx_http_sse_write(w, NULL, d) == FMLX_HTTP_OK ? FMLX_API_OK : FMLX_API_INTERNAL;
}

FmlxApiErr fmlx_api_openai_stream_text(const FmlxApiChat *c, const FmlxApiMeta *m, FmlxApiNext next,
                                       void *gen_ctx, FmlxHttpRes *w) {
    if (!c || !m || !next || !w) return FMLX_API_INTERNAL;
    /* Il testo accumulato NON serve per i frame (il testo esce come `text` grep-grezzo negli
     * eventi), ma l'accumulatore tiene i conteggi e il finish: stessa disciplina dello stream
     * chat, un solo posto che capisce gli eventi. */
    char text[1], think[1], args[1];
    FmlxApiAcc a;
    fmlx_api_acc_init(&a, text, sizeof text, think, sizeof think, args, sizeof args);

    FmlxApiErr err = FMLX_API_OK;
    int done_seen = 0;
    uint32_t lp_off = 0;
    for (;;) {
        FmlxApiEvent ev;
        memset(&ev, 0, sizeof ev);
        FmlxApiErrCtx ec;
        memset(&ec, 0, sizeof ec);
        FmlxApiGenSt gs = next(gen_ctx, &ev, &ec);
        if (gs == FMLX_API_GEN_ERR) {
            err = ec.err ? ec.err : FMLX_API_INTERNAL;
            break;
        }
        if (gs == FMLX_API_GEN_END) break;
        if (ev.ev == FMLX_API_EV_DONE) {
            /* Il conteggio del DONE va nei campi dell'accumulatore senza consumare il buffer
             * testo: `acc_push` va bene anche con un buffer da 1 byte perché l'evento DONE non
             * porta testo. */
            if (fmlx_api_acc_push(&a, &ev) != FMLX_API_OK) {
                err = FMLX_API_INTERNAL;
                break;
            }
            done_seen = 1;
            break;
        }
        if (ev.ev == FMLX_API_EV_TEXT) {
            if (text_chunk(w, m, c, ev.text, ev.n, NULL, NULL, 0,
                           ev.has_logprob ? &ev : NULL, lp_off)) {
                err = FMLX_API_INTERNAL;
                break;
            }
            lp_off += ev.n;
        }
    }
    if (err == FMLX_API_OK) {
        FmlxApiFinish fin = a.have_finish ? a.finish : FMLX_API_FINISH_STOP;
        if (text_chunk(w, m, c, "", 0, fmlx_api_finish_openai(fin), &a,
                       c->stream_usage && done_seen, NULL, 0))
            err = FMLX_API_INTERNAL;
    }
    if (err == FMLX_API_OK) {
        static const char DONE[] = "[DONE]";
        FmlxSlice s = {DONE, 6};
        if (fmlx_http_sse_write(w, NULL, s) != FMLX_HTTP_OK) err = FMLX_API_INTERNAL;
    }
    if (fmlx_http_sse_end(w) != FMLX_HTTP_OK && err == FMLX_API_OK) err = FMLX_API_INTERNAL;
    return err;
}
