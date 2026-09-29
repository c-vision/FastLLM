/* fmlx_api_openai_embeddings.c — `/v1/embeddings` (OpenAI): parse della richiesta e forma della
 * risposta. NON calcola gli embedding: quelli sono del motore (layer ①); qui c'è solo il
 * contratto del dialetto. L'embedding è un vettore di `dim` float, uno per input.
 *
 * ONESTÀ (piano:474): i campi che non onoriamo (`user`, e in generale le chiavi ignote) finiscono
 * in `ignored_params[]`; `encoding_format` è solo "float" (base64 non implementato → 400);
 * `dimensions` (troncatura) non è supportato → 400, mai un vettore di lunghezza diversa da quella
 * che il client crede.
 */
#include "fmlx/fmlx_api.h"

#include <string.h>

static void set_err(FmlxApiErrCtx *err, FmlxApiErr e, const char *msg, const char *param) {
    if (!err) return;
    memset(err, 0, sizeof *err);
    err->err = e;
    err->message = msg;
    err->param = param;
}

static int str_of(const FmlxJsonValue *v, FmlxSlice *out) {
    const char *p;
    size_t n;
    if (!fmlx_json_as_str(v, &p, &n)) return 0;
    if (n > 0xffffffffu) return 0;
    out->p = p;
    out->n = (uint32_t)n;
    return 1;
}

/* Scrive un array JSON di stringhe a partire da una lista di `FmlxSlice`. */
static int write_ignored(FmlxJsonW *w, const FmlxApiEmbeddings *e) {
    if (e->n_ignored == 0) return 0; /* campo assente: "non glielo dico" != "non ha mandato nulla" */
    if (fmlx_jsonw_key(w, "ignored_params") || fmlx_jsonw_arr_begin(w)) return -1;
    for (uint32_t i = 0; i < e->n_ignored; i++)
        if (fmlx_jsonw_str(w, e->ignored[i].p, e->ignored[i].n)) return -1;
    return fmlx_jsonw_arr_end(w) == FMLX_JSON_OK ? 0 : -1;
}

FmlxApiErr fmlx_api_openai_embeddings_parse(const FmlxJsonValue *root, FmlxApiEmbeddings *e,
                                            FmlxApiErrCtx *err) {
    const FmlxJsonValue *v;
    if (!e) return FMLX_API_INTERNAL;
    memset(e, 0, sizeof *e);
    if (!root || root->type != FMLX_JSON_OBJ) {
        set_err(err, FMLX_API_INVALID_REQUEST, "the request body must be a JSON object", NULL);
        return FMLX_API_INVALID_REQUEST;
    }
    /* Campi ignoti (tranne quelli noti/gestiti). `user` è standard ma non usato: dichiarato. */
    for (size_t i = 0; i < fmlx_json_obj_n(root); i++) {
        const FmlxJsonValue *k = fmlx_json_obj_key_at(root, i);
        const char *kp;
        size_t kn;
        if (!fmlx_json_as_str(k, &kp, &kn)) continue;
        if ((kn == 5 && !memcmp(kp, "model", 5)) || (kn == 5 && !memcmp(kp, "input", 5)) ||
            (kn == 15 && !memcmp(kp, "encoding_format", 15)) ||
            (kn == 10 && !memcmp(kp, "dimensions", 10)))
            continue;
        if (e->n_ignored >= FMLX_API_MAX_IGNORED) {
            e->overflow = 1;
            break;
        }
        e->ignored[e->n_ignored].p = kp;
        e->ignored[e->n_ignored].n = (uint32_t)kn;
        e->n_ignored++;
    }

    v = fmlx_json_obj_get(root, "model");
    if (!str_of(v, &e->model) || e->model.n == 0) {
        set_err(err, FMLX_API_INVALID_REQUEST, "model is required and must be a non-empty string",
                "model");
        return FMLX_API_INVALID_REQUEST;
    }

    v = fmlx_json_obj_get(root, "input");
    if (!v || fmlx_json_is_null(v)) {
        set_err(err, FMLX_API_INVALID_REQUEST, "input is required", "input");
        return FMLX_API_INVALID_REQUEST;
    }
    if (v->type == FMLX_JSON_STR) {
        if (!str_of(v, &e->input[0])) {
            set_err(err, FMLX_API_INVALID_REQUEST, "input must be a string or an array of strings",
                    "input");
            return FMLX_API_INVALID_REQUEST;
        }
        e->n_input = 1;
    } else if (v->type == FMLX_JSON_ARR) {
        size_t n = fmlx_json_arr_n(v);
        if (n == 0) {
            set_err(err, FMLX_API_INVALID_REQUEST, "input must not be empty", "input");
            return FMLX_API_INVALID_REQUEST;
        }
        if (n > FMLX_API_MAX_INPUTS) {
            e->overflow = 1;
            set_err(err, FMLX_API_INVALID_REQUEST, "too many inputs in one request", "input");
            return FMLX_API_INVALID_REQUEST;
        }
        for (size_t i = 0; i < n; i++) {
            const FmlxJsonValue *it = fmlx_json_arr_at(v, i);
            if (!str_of(it, &e->input[i])) {
                set_err(err, FMLX_API_INVALID_REQUEST,
                        "each input must be a string (token-id arrays and images are not "
                        "supported)",
                        "input");
                return FMLX_API_INVALID_REQUEST;
            }
        }
        e->n_input = (uint32_t)n;
    } else {
        set_err(err, FMLX_API_INVALID_REQUEST, "input must be a string or an array of strings",
                "input");
        return FMLX_API_INVALID_REQUEST;
    }

    v = fmlx_json_obj_get(root, "encoding_format");
    if (v && !fmlx_json_is_null(v)) {
        FmlxSlice s;
        if (!str_of(v, &s) || !(s.n == 5 && !memcmp(s.p, "float", 5))) {
            set_err(err, FMLX_API_INVALID_REQUEST,
                    "encoding_format must be \"float\" (base64 is not supported)",
                    "encoding_format");
            return FMLX_API_INVALID_REQUEST;
        }
    }
    v = fmlx_json_obj_get(root, "dimensions");
    if (v && !fmlx_json_is_null(v)) {
        set_err(err, FMLX_API_INVALID_REQUEST,
                "dimensions is not supported: this server returns the model's full width",
                "dimensions");
        return FMLX_API_INVALID_REQUEST;
    }
    return FMLX_API_OK;
}

FmlxApiErr fmlx_api_openai_embeddings_render(const FmlxApiEmbeddings *e, const float *data, int n,
                                             int dim, uint64_t tokens, FmlxJsonW *w) {
    if (!e || !data || n <= 0 || dim <= 0 || !w) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_obj_begin(w)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "object") || fmlx_jsonw_cstr(w, "list")) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "data") || fmlx_jsonw_arr_begin(w)) return FMLX_API_INTERNAL;
    for (int i = 0; i < n; i++) {
        if (fmlx_jsonw_obj_begin(w)) return FMLX_API_INTERNAL;
        if (fmlx_jsonw_key(w, "object") || fmlx_jsonw_cstr(w, "embedding")) return FMLX_API_INTERNAL;
        if (fmlx_jsonw_key(w, "index") || fmlx_jsonw_i64(w, i)) return FMLX_API_INTERNAL;
        if (fmlx_jsonw_key(w, "embedding") || fmlx_jsonw_arr_begin(w)) return FMLX_API_INTERNAL;
        for (int d = 0; d < dim; d++)
            if (fmlx_jsonw_f64(w, (double)data[(size_t)i * dim + d])) return FMLX_API_INTERNAL;
        if (fmlx_jsonw_arr_end(w) || fmlx_jsonw_obj_end(w)) return FMLX_API_INTERNAL;
    }
    if (fmlx_jsonw_arr_end(w)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "model") || fmlx_jsonw_str(w, e->model.p, e->model.n))
        return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "usage") || fmlx_jsonw_obj_begin(w)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "prompt_tokens") || fmlx_jsonw_u64(w, tokens)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "total_tokens") || fmlx_jsonw_u64(w, tokens)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_obj_end(w)) return FMLX_API_INTERNAL;
    if (write_ignored(w, e) != 0) return FMLX_API_INTERNAL;
    return fmlx_jsonw_obj_end(w) == FMLX_JSON_OK ? FMLX_API_OK : FMLX_API_INTERNAL;
}
