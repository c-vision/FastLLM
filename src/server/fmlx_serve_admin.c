/* fmlx_serve_admin.c — amministrazione dei modelli residenti: `/v1/models/status|load|unload`.
 * Separato da `fmlx_serve_respond.c` perché NON è il dispatch di una richiesta di inferenza: è
 * gestione della cache residente, con un corpo e dei codici propri. La messa in forma della
 * risposta è condivisa via `fmlx_serve_internal.h`.
 */
#include "fmlx_serve_internal.h"

#include "fmlx/fmlx_json.h"

#include <stdio.h>
#include <string.h>

/* ---- amministrazione modelli: /v1/models/status|load|unload ---- */

/* GET /v1/models/status: i modelli RESIDENTI in RAM (quelli che una richiesta non ri-paga). */
FmlxServeSt fmlx_serve_body_models_status(FmlxServeBuf *buf, int ka, size_t *out_len) {
    FmlxJsonW w;
    size_t blen = 0;
    int n = fmlx_engine_resident_count();

    fmlx_jsonw_init(&w, buf->resp + FMLX_SERVE_RESERVE, buf->resp_cap - FMLX_SERVE_RESERVE, buf->resp_cap - FMLX_SERVE_RESERVE);
    if (fmlx_jsonw_obj_begin(&w) != FMLX_JSON_OK || fmlx_jsonw_key(&w, "object") != FMLX_JSON_OK ||
        fmlx_jsonw_cstr(&w, "list") != FMLX_JSON_OK || fmlx_jsonw_key(&w, "data") != FMLX_JSON_OK ||
        fmlx_jsonw_arr_begin(&w) != FMLX_JSON_OK) {
        fmlx_jsonw_free(&w);
        return fmlx_serve_refuse_simple(buf, FMLX_API_INTERNAL, 0, NULL,
                             "the model status body does not fit this server's buffer", out_len);
    }
    for (int i = 0; i < n; i++) {
        const char *nm = fmlx_engine_resident_name(i);
        if (fmlx_jsonw_obj_begin(&w) || fmlx_jsonw_key(&w, "id") || fmlx_jsonw_cstr(&w, nm) ||
            fmlx_jsonw_key(&w, "object") || fmlx_jsonw_cstr(&w, "model") ||
            fmlx_jsonw_key(&w, "state") || fmlx_jsonw_cstr(&w, "loaded") ||
            fmlx_jsonw_obj_end(&w))
            break;
    }
    if (fmlx_jsonw_arr_end(&w) != FMLX_JSON_OK || fmlx_jsonw_obj_end(&w) != FMLX_JSON_OK ||
        fmlx_jsonw_finish(&w, &blen) != FMLX_JSON_OK) {
        fmlx_jsonw_free(&w);
        return fmlx_serve_refuse_simple(buf, FMLX_API_INTERNAL, 0, NULL,
                             "the model status body does not fit this server's buffer", out_len);
    }
    fmlx_jsonw_free(&w);
    return fmlx_serve_finish_ok(buf, 200, "application/json", blen, ka, out_len);
}

/* Legge `model` (stringa) dal corpo di load/unload. 0 su errore. */
static int model_field(const char *body, size_t body_len, char *out, size_t cap, FmlxJsonDoc *doc,
                       size_t *out_len_used) {
    (void)out_len_used;
    fmlx_json_doc_init(doc);
    if (fmlx_json_parse(body, body_len, 0, doc) != FMLX_JSON_OK) return 0;
    const FmlxJsonValue *root = fmlx_json_root(doc);
    const FmlxJsonValue *v = fmlx_json_obj_get(root, "model");
    const char *p = NULL;
    size_t n = 0;
    if (!v || !fmlx_json_as_str(v, &p, &n) || n == 0 || n >= cap) return 0;
    memcpy(out, p, n);
    out[n] = '\0';
    return 1;
}

/* POST /v1/models/load: carica e MANTIENE residente il modello (è la cache che fa risparmiare
 * il load alla richiesta successiva). */
FmlxServeSt fmlx_serve_body_model_load(FmlxServeBuf *buf, const char *body, size_t body_len,
                                   const FmlxServeOpts *o, int ka, size_t *out_len) {
    FmlxJsonDoc doc;
    char name[FMLX_ENGINE_NAME_MAX] = "";
    char err[512] = "";
    FmlxEngineModel *m;
    FmlxJsonW w;
    size_t blen = 0;

    if (!model_field(body, body_len, name, sizeof name, &doc, NULL)) {
        fmlx_json_doc_free(&doc);
        return fmlx_serve_engine_fail(buf, FMLX_API_INVALID_REQUEST, "model_not_found",
                           "model is required and must be a non-empty string", 0, out_len);
    }
    m = fmlx_engine_model(o ? o->cache_root : NULL, name, err, sizeof err);
    fmlx_json_doc_free(&doc);
    if (!m) {
        char msg[600];
        snprintf(msg, sizeof msg, "the requested model could not be loaded: %s", err);
        return fmlx_serve_engine_fail(buf, FMLX_API_MODEL_NOT_FOUND, NULL, msg, 0, out_len);
    }
    fmlx_jsonw_init(&w, buf->resp + FMLX_SERVE_RESERVE, buf->resp_cap - FMLX_SERVE_RESERVE, buf->resp_cap - FMLX_SERVE_RESERVE);
    if (fmlx_jsonw_obj_begin(&w) || fmlx_jsonw_key(&w, "id") || fmlx_jsonw_cstr(&w, name) ||
        fmlx_jsonw_key(&w, "object") || fmlx_jsonw_cstr(&w, "model") ||
        fmlx_jsonw_key(&w, "state") || fmlx_jsonw_cstr(&w, "loaded") ||
        fmlx_jsonw_obj_end(&w) || fmlx_jsonw_finish(&w, &blen)) {
        fmlx_jsonw_free(&w);
        return fmlx_serve_refuse_simple(buf, FMLX_API_INTERNAL, 0, NULL,
                             "the load response does not fit this server's buffer", out_len);
    }
    fmlx_jsonw_free(&w);
    return fmlx_serve_finish_ok(buf, 200, "application/json", blen, ka, out_len);
}

/* POST /v1/models/unload: libera il modello residente (RAM/KV). */
FmlxServeSt fmlx_serve_body_model_unload(FmlxServeBuf *buf, const char *body, size_t body_len,
                                     int ka, size_t *out_len) {
    FmlxJsonDoc doc;
    char name[FMLX_ENGINE_NAME_MAX] = "";
    int was;
    FmlxJsonW w;
    size_t blen = 0;

    if (!model_field(body, body_len, name, sizeof name, &doc, NULL)) {
        fmlx_json_doc_free(&doc);
        return fmlx_serve_engine_fail(buf, FMLX_API_INVALID_REQUEST, "model_not_found",
                           "model is required and must be a non-empty string", 0, out_len);
    }
    was = fmlx_engine_unload(name);
    fmlx_json_doc_free(&doc);
    fmlx_jsonw_init(&w, buf->resp + FMLX_SERVE_RESERVE, buf->resp_cap - FMLX_SERVE_RESERVE, buf->resp_cap - FMLX_SERVE_RESERVE);
    if (fmlx_jsonw_obj_begin(&w) || fmlx_jsonw_key(&w, "id") || fmlx_jsonw_cstr(&w, name) ||
        fmlx_jsonw_key(&w, "object") || fmlx_jsonw_cstr(&w, "model") ||
        fmlx_jsonw_key(&w, "state") ||
        fmlx_jsonw_cstr(&w, was ? "unloaded" : "not_loaded") || fmlx_jsonw_obj_end(&w) ||
        fmlx_jsonw_finish(&w, &blen)) {
        fmlx_jsonw_free(&w);
        return fmlx_serve_refuse_simple(buf, FMLX_API_INTERNAL, 0, NULL,
                             "the unload response does not fit this server's buffer", out_len);
    }
    fmlx_jsonw_free(&w);
    return fmlx_serve_finish_ok(buf, 200, "application/json", blen, ka, out_len);
}

