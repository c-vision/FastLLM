/* fmlx_engine_gen.c — render del prompt, prefill/decode via l'ABI `llmengine`, e l'iteratore
 * di eventi che il layer ② consuma.
 *
 * DECODE INCREMENTALE. L'ABI è ids-only e `generate_ids(s, 1, eos, out)` è il modo previsto per
 * il server (`llm_mlx_session_generate.c`: "il server chiama generate(s, 1) in un ciclo"): qui
 * si genera UN token per chiamata dell'iteratore, così il testo esce appena prodotto (TTFT =
 * primo token) invece che a generazione finita. Dopo ogni token si decodifica l'INTERA sequenza
 * e si consegna solo la coda nuova tagliata su confini di codepoint: è l'unico modo di non far
 * passare per il filo byte UTF-8 spezzati a metà, che il writer JSON rifiuta.
 *
 * STOP SEQUENCES. Con l'emissione token-live una stop può attraversare più token: si tiene
 * indietro la coda che può essere il PREFISSO di una stop (holdback) e si tronca al primo match
 * completo, esattamente come farebbe il decode batch.
 */
#include "fmlx_engine_internal.h"

#include "llmjinja.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

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

/* ---- render del chat_template REALE (Jinja) ---- */

/* Costruisce un valore stringa dal `content` di un messaggio (stringa o array di blocchi). */
static llmjinja_val *jv_content(const FmlxJsonValue *content) {
    FmlxSb sb = {0};
    llmjinja_val *v;

    if (content_text(content, &sb, 0) != 0) {
        fmlx_sb_free(&sb);
        return NULL;
    }
    v = llmjinja_str(sb.p ? sb.p : "");
    fmlx_sb_free(&sb);
    return v;
}

/* Converte un nodo del DOM JSON negli stessi valori che il motore Jinja conosce. I REALI
 * diventano stringhe (il motore non ha un tipo double): per uno schema di tool un `default`
 * numerico reso come stringa è un difetto minore e dichiarato, non un crash. */
static llmjinja_val *jv_from_json(const FmlxJsonValue *v) {
    int64_t i64;
    double f64;
    int b;

    if (!v) return llmjinja_null();
    switch (v->type) {
    case FMLX_JSON_NULL: return llmjinja_null();
    case FMLX_JSON_BOOL:
        if (fmlx_json_as_bool(v, &b)) return llmjinja_bool(b);
        return llmjinja_bool(0);
    case FMLX_JSON_INT:
        if (fmlx_json_as_i64(v, &i64)) return llmjinja_long((long)i64);
        return llmjinja_long(0);
    case FMLX_JSON_REAL:
        if (fmlx_json_as_f64(v, &f64)) {
            char buf[40];
            snprintf(buf, sizeof buf, "%.17g", f64);
            return llmjinja_str(buf);
        }
        return llmjinja_str("0");
    case FMLX_JSON_STR: {
        const char *s = fmlx_json_str(v);
        size_t n = fmlx_json_str_len(v);
        char *tmp = malloc(n + 1);
        llmjinja_val *r;
        if (!tmp) return llmjinja_str("");
        memcpy(tmp, s, n);
        tmp[n] = '\0';
        r = llmjinja_str(tmp);
        free(tmp);
        return r;
    }
    case FMLX_JSON_ARR: {
        llmjinja_val *a = llmjinja_arr();
        for (size_t k = 0; k < fmlx_json_arr_n(v); k++)
            llmjinja_arr_push(a, jv_from_json(fmlx_json_arr_at(v, k)));
        return a;
    }
    case FMLX_JSON_OBJ: {
        llmjinja_val *o = llmjinja_obj();
        for (size_t k = 0; k < fmlx_json_obj_n(v); k++) {
            const FmlxJsonValue *key = fmlx_json_obj_key_at(v, k);
            const char *kp = fmlx_json_str(key);
            size_t kn = fmlx_json_str_len(key);
            char name[FMLX_API_MAX_NAME];
            if (!kp || kn >= sizeof name) continue;
            memcpy(name, kp, kn);
            name[kn] = '\0';
            llmjinja_obj_set(o, name, jv_from_json(fmlx_json_obj_value_at(v, k)));
        }
        return o;
    }
    case FMLX_JSON_KEY: break;
    }
    return llmjinja_null();
}

/* L'array `tools` per il template: lo stesso oggetto che il client ha mandato. Qwen e
 * Llama-3 lo serializzano con `tojson` e ne fanno le definizioni. */
static llmjinja_val *jv_tools(const FmlxApiChat *c) {
    llmjinja_val *a = llmjinja_arr();
    if (!a) return NULL;
    for (size_t i = 0; c->tools && i < fmlx_json_arr_n(c->tools); i++)
        llmjinja_arr_push(a, jv_from_json(fmlx_json_arr_at(c->tools, i)));
    return a;
}

/* Aggiunge a `b` un valore del DOM reso JSON compatto. */
static int sb_json(FmlxSb *b, const FmlxJsonValue *v) {
    FmlxJsonW w;
    size_t n = 0;
    int rc = -1;

    fmlx_jsonw_init(&w, NULL, 0, 1u << 20);
    if (fmlx_json_render(v, &w) == FMLX_JSON_OK && fmlx_jsonw_finish(&w, &n) == FMLX_JSON_OK)
        rc = fmlx_sb_add(b, w.buf ? w.buf : "null", n);
    fmlx_jsonw_free(&w);
    return rc;
}

/* Il preambolo dei tool per i template di FAMIGLIA (quelli senza Jinja). I modelli veri hanno
 * il loro `chat_template`, che i tool li rende da solo: questo è il ripiego, e lo dice. */
static int append_tool_preamble(FmlxSb *b, const FmlxApiChat *c, FmlxTplKind tpl) {
    if (tpl == FMLX_TPL_LLAMA3) {
        if (fmlx_sb_cstr(b, "You have access to the following functions. To call a function, "
                            "respond with ONLY a JSON object of the form "
                            "{\"name\": <function name>, \"arguments\": <arguments>} and "
                            "nothing else. Do not use variables.\n\n") != 0)
            return -1;
        for (size_t i = 0; i < fmlx_json_arr_n(c->tools); i++) {
            if (sb_json(b, fmlx_json_arr_at(c->tools, i)) != 0 || fmlx_sb_cstr(b, "\n\n") != 0)
                return -1;
        }
        return 0;
    }
    if (fmlx_sb_cstr(b, "# Tools\n\nYou may call one or more functions to assist with the user "
                        "query. You are provided with function signatures within <tools></tools> "
                        "XML tags:\n<tools>\n") != 0)
        return -1;
    for (size_t i = 0; i < fmlx_json_arr_n(c->tools); i++) {
        if (sb_json(b, fmlx_json_arr_at(c->tools, i)) != 0 || fmlx_sb_cstr(b, "\n") != 0)
            return -1;
    }
    if (fmlx_sb_cstr(b, "</tools>\n\nFor each function call, return a json object with function "
                        "name and arguments within <tool_call></tool_call> XML tags:\n"
                        "<tool_call>\n{\"name\": <function-name>, \"arguments\": "
                        "<args-json-object>}\n</tool_call>\n") != 0)
        return -1;
    return 0;
}

/* L'istruzione di `response_format`. NON è grammatica: è un vincolo di prompt e una
 * post-validazione, e il limite va detto (l'ABI non espone un grammar/guided decoder). */
static int append_rf(FmlxSb *b, const FmlxApiChat *c) {
    if (!c->response_format) return 0;
    if (c->rf_json_object &&
        fmlx_sb_cstr(b, "You must respond with a single valid JSON object and nothing else: no "
                        "prose, no markdown fences.") != 0)
        return -1;
    if (c->rf_json_schema) {
        const FmlxJsonValue *js = fmlx_json_obj_get(c->response_format, "json_schema");
        const FmlxJsonValue *sch = js ? fmlx_json_obj_get(js, "schema") : NULL;
        if (fmlx_sb_cstr(b, "You must respond with a single valid JSON object and nothing else: "
                            "no prose, no markdown fences. It must conform to this JSON "
                            "Schema:\n") != 0)
            return -1;
        if (sch && sb_json(b, sch) != 0) return -1;
    }
    return 0;
}

/* Le istruzioni tool/response_format più il testo di sistema, in QUEST'ORDINE: prima il
 * sistema del client, poi i vincoli. `include_tools` è 0 sul percorso Jinja (le definizioni
 * viaggiano nella variabile `tools`). */
static int build_system(FmlxSb *out, const FmlxApiChat *c, const char *sys, size_t sys_n,
                        int include_tools, int mode, const FmlxSlice *named, FmlxTplKind tpl,
                        char *err, size_t errcap) {
    if (sys_n && fmlx_sb_add(out, sys, sys_n) != 0) goto oom;
    if (include_tools && mode != FMLX_API_TOOL_NONE && c->tools &&
        fmlx_json_arr_n(c->tools)) {
        if (out->len && fmlx_sb_cstr(out, "\n\n") != 0) goto oom;
        if (append_tool_preamble(out, c, tpl) != 0) goto oom;
    }
    if (mode == FMLX_API_TOOL_REQUIRED) {
        if (out->len && fmlx_sb_cstr(out, "\n\n") != 0) goto oom;
        if (fmlx_sb_cstr(out, "You MUST call one of the available functions.") != 0) goto oom;
    } else if (mode == FMLX_API_TOOL_NAMED) {
        if (out->len && fmlx_sb_cstr(out, "\n\n") != 0) goto oom;
        if (fmlx_sb_cstr(out, "You MUST call the function ") != 0 ||
            fmlx_sb_add(out, named->p, named->n) != 0 || fmlx_sb_cstr(out, ".") != 0)
            goto oom;
    }
    if (c->response_format) {
        if (out->len && fmlx_sb_cstr(out, "\n\n") != 0) goto oom;
        if (append_rf(out, c) != 0) goto oom;
    }
    return 0;
oom:
    snprintf(err, errcap, "out of memory building the system prompt");
    return -1;
}

/* `messages` per il template: il system (fuso da `join_system`) diventa il PRIMO messaggio, i
 * messaggi di sistema originali si saltano (altrimenti il template li vedrebbe due volte). */
static llmjinja_val *jv_messages(const FmlxApiChat *c, const char *sys, size_t sys_n) {
    llmjinja_val *arr = llmjinja_arr();

    if (!arr) return NULL;
    if (sys_n) {
        llmjinja_val *m = llmjinja_obj();
        char *s = malloc(sys_n + 1);
        if (s) {
            memcpy(s, sys, sys_n);
            s[sys_n] = '\0';
        }
        llmjinja_obj_set(m, "role", llmjinja_str("system"));
        llmjinja_obj_set(m, "content", llmjinja_str(s ? s : ""));
        free(s);
        llmjinja_arr_push(arr, m);
    }
    for (size_t i = 0; c->messages && i < fmlx_json_arr_n(c->messages); i++) {
        const FmlxJsonValue *msg = fmlx_json_arr_at(c->messages, i);
        const char *role;
        size_t rn;
        llmjinja_val *m, *cv;
        char *r;

        role_of(msg, &role, &rn);
        if (!role || is_system_role(role, rn)) continue;
        m = llmjinja_obj();
        r = malloc(rn + 1);
        if (r) {
            memcpy(r, role, rn);
            r[rn] = '\0';
        }
        llmjinja_obj_set(m, "role", llmjinja_str(r ? r : ""));
        free(r);
        cv = jv_content(fmlx_json_obj_get(msg, "content"));
        llmjinja_obj_set(m, "content", cv ? cv : llmjinja_str(""));
        /* Un turno assistant con `tool_calls` (la risposta a un giro precedente) va reso al
         * template, o la conversazione multi-step perde la chiamata. La forma è quella
         * OpenAI: `function.arguments` resta la STRINGA che il client ha rimandato. */
        {
            const FmlxJsonValue *tcs = fmlx_json_obj_get(msg, "tool_calls");
            if (tcs && tcs->type == FMLX_JSON_ARR && fmlx_json_arr_n(tcs)) {
                llmjinja_val *tca = llmjinja_arr();
                for (size_t k = 0; k < fmlx_json_arr_n(tcs); k++)
                    llmjinja_arr_push(tca, jv_from_json(fmlx_json_arr_at(tcs, k)));
                llmjinja_obj_set(m, "tool_calls", tca);
            }
        }
        llmjinja_arr_push(arr, m);
    }
    return arr;
}

/* `strftime_now(fmt)` di contesto (J_FN): la template Llama-3 la usa per le date. */
static llmjinja_val *jfn_strftime_now(int n, llmjinja_val **args) {
    char out[128];
    const char *fmt = "%d %b %Y";
    time_t t = time(NULL);
    struct tm tmv;

    localtime_r(&t, &tmv);
    if (n >= 1 && args && args[0] && llmjinja_is_str(args[0])) fmt = llmjinja_as_str(args[0]);
    if (!strftime(out, sizeof out, fmt, &tmv)) out[0] = '\0';
    return llmjinja_str(out);
}

/* Render con il template reale del modello. Ritorna 0/-1 (con `err`). Su -1 il chiamante può
 * ripiegare sul template di famiglia: un costrutto non supportato non deve far fallire la chat. */
static int render_jinja(const FmlxEngineModel *m, const FmlxApiChat *c, FmlxSb *b, char *err,
                        size_t errcap) {
    char errbuf[256] = "";
    char *sysmem = malloc(1u << 20);
    FmlxSlice sys = {NULL, 0};
    FmlxSlice named = {NULL, 0};
    int mode;
    FmlxSb aug = {0};
    llmjinja_val *vars;
    char *out;

    if (!sysmem) {
        snprintf(err, errcap, "out of memory building the system prompt");
        return -1;
    }
    if (fmlx_api_join_system(c, sysmem, 1u << 20, &sys) != FMLX_API_OK) {
        free(sysmem);
        snprintf(err, errcap, "the system prompt does not fit the render buffer");
        return -1;
    }
    mode = fmlx_api_tool_mode(c, &named);
    /* Sul percorso Jinja le definizioni dei tool viaggiano nella variabile `tools` (i
     * template di Qwen/Llama le rendono da sole): il preambolo testuale NON si aggiunge. */
    if (build_system(&aug, c, sys.p, sys.n, 0, mode, &named, m->tpl, err, errcap) != 0) {
        fmlx_sb_free(&aug);
        free(sysmem);
        return -1;
    }
    vars = llmjinja_obj();
    llmjinja_obj_set(vars, "messages", jv_messages(c, aug.p, aug.len));
    llmjinja_obj_set(vars, "add_generation_prompt", llmjinja_bool(1));
    /* THINKING: di default `false` (disattivato) — i template Qwen pre-riempiono <think></think>.
     * `--think` lo riporta a true. I template che non usano la variabile la ignorano. */
    llmjinja_obj_set(vars, "enable_thinking", llmjinja_bool(fmlx_engine_think_enabled() ? 1 : 0));
    if (mode != FMLX_API_TOOL_NONE && c->tools && fmlx_json_arr_n(c->tools))
        llmjinja_obj_set(vars, "tools", jv_tools(c));
    if (m->bos[0]) llmjinja_obj_set(vars, "bos_token", llmjinja_str(m->bos));
    llmjinja_obj_set(vars, "strftime_now", llmjinja_fn_val((llmjinja_fn)jfn_strftime_now));
    out = llmjinja_render(m->chat_tpl, vars, errbuf, sizeof errbuf);
    llmjinja_val_free(vars);
    fmlx_sb_free(&aug);
    free(sysmem);
    if (!out) {
        if (getenv("FMLX_TEMPLATE_DEBUG"))
            fprintf(stderr, "[template] Jinja render FALLITO: %s (ripiego di famiglia)\n",
                    errbuf[0] ? errbuf : "unknown");
        snprintf(err, errcap, "chat template render failed: %s", errbuf[0] ? errbuf : "unknown");
        return -1;
    }
    if (getenv("FMLX_TEMPLATE_DEBUG"))
        fprintf(stderr, "[template] Jinja render ok (%zu byte)\n", strlen(out));
    if (fmlx_sb_cstr(b, out) != 0) {
        free(out);
        snprintf(err, errcap, "out of memory building the prompt");
        return -1;
    }
    free(out);
    return 0;
}
/* ---- render del prompt ---- */

int fmlx_engine_render_chat(const FmlxEngineModel *m, const FmlxApiChat *c, FmlxSb *b, char *err,
                            size_t errcap) {
    char *sysmem = NULL;
    FmlxSlice sys = {NULL, 0};
    FmlxSlice named = {NULL, 0};
    FmlxSb aug = {0};
    int mode;

    if (!m || !c || !b) return -1;
    /* Se il modello porta il suo `chat_template`, si rende QUELLO. Un fallimento (costrutto
     * Jinja non supportato) non è fatale: si ripiega sul template di famiglia, che è meglio di
     * un 500 e viene dichiarato nel log del server. */
    if (m->chat_tpl) {
        if (render_jinja(m, c, b, err, errcap) == 0) return 0;
        if (err && errcap) err[0] = '\0';
    }
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
    mode = fmlx_api_tool_mode(c, &named);
    /* Percorso di FAMIGLIA: qui i tool NON viaggiano in una variabile, quindi il preambolo
     * testuale si aggiunge al sistema. */
    if (build_system(&aug, c, sys.p, sys.n, 1, mode, &named, m->tpl, err, errcap) != 0) {
        free(sysmem);
        return -1;
    }
    sys.p = aug.p;
    sys.n = (uint32_t)aug.len;

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
    } else if (m->tpl == FMLX_TPL_DS4) {
        /* DeepSeek V4 (ds4/GGUF singolo): BOS, system, i turni `<｜User｜>`/`<｜Assistant｜>`, opener
         * assistant. I delimitatori usano la barra verticale e l'underscore FULLWIDTH (U+FF5C/U+2581),
         * che sono i token letterali del vocabolario DS4. */
        if (fmlx_sb_cstr(b, "<｜begin▁of▁sentence｜>") != 0) goto oom;
        if (sys.p && sys.n) {
            if (fmlx_sb_add(b, sys.p, sys.n) != 0 || fmlx_sb_cstr(b, "\n") != 0) goto oom;
        }
        for (size_t i = 0; c->messages && i < fmlx_json_arr_n(c->messages); i++) {
            const FmlxJsonValue *msg = fmlx_json_arr_at(c->messages, i);
            const char *role;
            size_t rn;
            role_of(msg, &role, &rn);
            if (!role || is_system_role(role, rn)) continue;
            if (rn == 4 && strncmp(role, "user", 4) == 0) {
                if (fmlx_sb_cstr(b, "<｜User｜>") != 0 ||
                    content_text(fmlx_json_obj_get(msg, "content"), b, 1) != 0)
                    goto oom;
            } else if (rn == 9 && strncmp(role, "assistant", 9) == 0) {
                if (fmlx_sb_cstr(b, "<｜Assistant｜>") != 0 ||
                    content_text(fmlx_json_obj_get(msg, "content"), b, 1) != 0)
                    goto oom;
            }
        }
        if (fmlx_sb_cstr(b, "<｜Assistant｜>") != 0) goto oom;
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
    fmlx_sb_free(&aug);
    return 0;
oom:
    free(sysmem);
    fmlx_sb_free(&aug);
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

/* ---- stop sequences ---- */

/* Ritorna la lunghezza del più lungo SUFFISSO di `s[0..len)` che è un prefisso PROPRIO di una
 * stop. È la coda che non si può ancora emettere, perché un token successivo potrebbe
 * completare la stop. */
static size_t stop_holdback(const FmlxApiChat *c, const char *s, size_t len) {
    size_t hb = 0;

    if (!c) return 0;
    for (uint32_t i = 0; i < c->n_stop; i++) {
        const char *t = c->stop[i].p;
        uint32_t tn = c->stop[i].n;
        if (!t || tn < 2) continue;
        uint32_t maxp = tn - 1;
        if ((size_t)maxp > len) maxp = (uint32_t)len;
        for (uint32_t p = maxp; p > 0; p--) {
            if ((size_t)p <= len && memcmp(s + len - p, t, p) == 0) {
                if (p > hb) hb = p;
                break;
            }
        }
    }
    return hb;
}

static int is_secondary_eos(const FmlxEngineModel *m, int32_t id) {
    for (int i = 0; i < m->n_eos; i++)
        if (m->eos_list[i] == id) return 1;
    return 0;
}

static int utf8_is_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

/* ---- THINK SPLIT: ragionamento (THINK) vs risposta (TEXT) ----
 *
 * I modelli reasoning aprono il blocco NEL PROMPT (il chat template scrive `<think>` / ` thinking`)
 * e lo chiudono NELL'OUTPUT (`</think>` / ` response`). Qui si separa il flusso: i byte del
 * ragionamento escono come eventi THINK, quelli della risposta come TEXT, e il marcatore di
 * chiusura NON viene consegnato. Sono marker ASCII, quindi il taglio non rompe l'UTF-8. */
static const char *const THINK_OPEN[] = {"<think>", " thinking"};
static const char *const THINK_CLOSE[] = {"</think>", " response"};
#define THINK_NOPEN ((int)(sizeof THINK_OPEN / sizeof THINK_OPEN[0]))
#define THINK_NCLOSE ((int)(sizeof THINK_CLOSE / sizeof THINK_CLOSE[0]))
#define THINK_MARKER_MAX 9 /* la lunghezza del marker più lungo */

static const char *find_bytes(const char *hay, size_t hn, const char *needle, size_t nn) {
    if (nn == 0 || hn < nn) return NULL;
    for (size_t i = 0; i + nn <= hn; i++)
        if (hay[i] == needle[0] && memcmp(hay + i, needle, nn) == 0) return hay + i;
    return NULL;
}

static size_t last_find(const char *hay, const char *needle) {
    size_t nn = strlen(needle), last = (size_t)-1;
    for (const char *q = hay; (q = strstr(q, needle)) != NULL; q += nn) last = (size_t)(q - hay);
    return last;
}

/* 1 se il prompt renderizzato termina DENTRO un blocco di ragionamento (l'apertura più recente
 * non è seguita da una chiusura): è il caso di Qwen/DeepSeek con thinking attivo. */
static int prompt_opens_think(const char *p) {
    size_t o = (size_t)-1, c = (size_t)-1;
    if (!p) return 0;
    for (int i = 0; i < THINK_NOPEN; i++) {
        size_t x = last_find(p, THINK_OPEN[i]);
        if (x != (size_t)-1 && (o == (size_t)-1 || x > o)) o = x;
    }
    for (int i = 0; i < THINK_NCLOSE; i++) {
        size_t x = last_find(p, THINK_CLOSE[i]);
        if (x != (size_t)-1 && (c == (size_t)-1 || x > c)) c = x;
    }
    return o != (size_t)-1 && (c == (size_t)-1 || o > c);
}

/* Lunghezza della coda di `s[0..len)` che è un PREFISSO PROPRIO del prossimo marker atteso:
 * quei byte non si possono ancora consegnare (un token successivo può completare il marker). */
static size_t think_holdback(const char *s, size_t len, int in_think) {
    const char *const *set = in_think ? THINK_CLOSE : THINK_OPEN;
    int n = in_think ? THINK_NCLOSE : THINK_NOPEN;
    size_t best = 0;
    for (int i = 0; i < n; i++) {
        size_t ml = strlen(set[i]);
        size_t maxp = ml - 1;
        if (maxp > len) maxp = len;
        for (size_t p = maxp; p > 0; p--) {
            if (memcmp(s + len - p, set[i], p) == 0) {
                if (p > best) best = p;
                break;
            }
        }
    }
    return best;
}

/* Ricalcola il testo dell'intera sequenza, decide la stop e consegna la coda nuova (tagliata a
 * un confine di codepoint) in `g->text`. Ritorna 0/-1. */
static int text_update(FmlxEngineGen *g, char *err, size_t errcap) {
    int need = llmtoken_decode(g->m->tok, g->ids, g->n_gen, NULL, 0);
    char *cur;
    size_t limit, hb;
    const char *which = NULL;
    uint32_t which_n = 0;

    if (need < 0) {
        snprintf(err, errcap, "the tokenizer could not decode the output");
        return -1;
    }
    cur = malloc((size_t)need + 1);
    if (!cur) {
        snprintf(err, errcap, "out of memory decoding the output");
        return -1;
    }
    if (need && llmtoken_decode(g->m->tok, g->ids, g->n_gen, cur, need + 1) != need) {
        free(cur);
        snprintf(err, errcap, "the tokenizer produced an inconsistent decode");
        return -1;
    }
    cur[need] = '\0';
    free(g->full);
    g->full = cur;
    g->full_len = (size_t)need;
    /* Confine in byte di QUESTO token dentro `full`: il decode di un prefisso è un prefisso
     * del decode intero (byte-level BPE; metaspace toglie lo spazio solo in testa), quindi
     * `full_len` appena calcolato è la fine del token `n_gen-1`. */
    if (g->want_logprobs && g->tok_end) g->tok_end[g->n_gen - 1] = (uint32_t)g->full_len;

    /* stop completa: la PRIMA posizione di match vince */
    limit = g->full_len;
    if (g->c) {
        for (uint32_t i = 0; i < g->c->n_stop; i++) {
            const char *s = g->c->stop[i].p;
            uint32_t sn = g->c->stop[i].n;
            if (!s || sn == 0 || (size_t)sn > g->full_len) continue;
            for (size_t k = 0; k + sn <= g->full_len; k++) {
                if (memcmp(g->full + k, s, sn) == 0) {
                    if (k < limit) {
                        limit = k;
                        which = s;
                        which_n = sn;
                    }
                    break;
                }
            }
        }
        if (limit < g->full_len) g->ended = 1;
    }

    /* Senza una stop completa, si tiene indietro un possibile prefisso di stop. */
    if (limit == g->full_len) {
        hb = stop_holdback(g->c, g->full, limit);
        if (hb <= limit) limit -= hb;
    }

    /* Think split: si tiene indietro anche un possibile PREFISSO del prossimo marker (`</think>`
     * o l'apertura), così un marker spezzato fra due token non diventa testo. */
    if (g->want_think && !g->ended) {
        hb = think_holdback(g->full, limit, g->in_think);
        if (hb > limit - g->emitted) hb = limit - g->emitted;
        limit -= hb;
    }

    /* confine di codepoint: mai consegnare un UTF-8 a metà */
    while (limit > g->emitted && limit < g->full_len &&
           utf8_is_cont((unsigned char)g->full[limit]))
        limit--;

    if (limit > g->emitted) {
        /* In modalità logprobs/think il testo esce da `full` (offset dei token o scanner dei
         * marker), non dal buffer a chunk: si aggiorna solo il confine sicuro. */
        if (!g->want_logprobs && !g->want_think) {
            size_t cnt = limit - g->emitted;
            char *nb = malloc(cnt);
            if (!nb) {
                snprintf(err, errcap, "out of memory buffering the decoded text");
                return -1;
            }
            memcpy(nb, g->full + g->emitted, cnt);
            free(g->text);
            g->text = nb;
            g->text_len = cnt;
            g->pos = 0;
        }
        g->emitted = limit;
    }

    if (g->ended && limit < g->full_len) {
        /* stop completa: registra quale, per il `stop_sequence` del DONE */
        if (which)
            snprintf(g->stop_seq, sizeof g->stop_seq, "%.*s", (int)(which_n < FMLX_API_MAX_NAME - 1
                                                                        ? which_n
                                                                        : FMLX_API_MAX_NAME - 1),
                     which);
        g->finish = FMLX_API_FINISH_STOPSEQ;
    }
    return 0;
}

/* Un passo di decode: esattamente UN token. Con `want_logprobs` si usa l'estensione ABI v2
 * `generate_ids_probs`, che cattura la logprob del token PRIMA di avanzare (esatta anche con
 * sampling: la distribuzione è quella del modello alla posizione di partenza). */
static int advance(FmlxEngineGen *g, char *err, size_t errcap) {
    int got;
    int32_t id = 0;

    if (g->n_gen >= g->max_new) {
        g->finish = FMLX_API_FINISH_LENGTH;
        g->ended = 1;
        return 0;
    }
    if (g->want_logprobs) {
        float lp = 0.0f;
        /* TOP-K (ABI v3): prima del passo, la distribuzione pending fotografata ha lo stesso
         * pending che `generate_ids_probs` usa per la logprob del token emesso. */
        if (g->want_top_logprobs) {
            int32_t *ti = g->top_ids + (size_t)g->n_gen * (size_t)g->top_k;
            float *tl = g->top_lp + (size_t)g->n_gen * (size_t)g->top_k;
            int rc = g->m->eng->token_topk(g->session, g->top_k, ti, tl);
            for (int j = (rc < 0 ? 0 : rc); j < g->top_k; j++) ti[j] = -1;
        }
        got = g->m->eng->generate_ids_probs(g->session, 1, g->m->eos, &id, &lp);
        if (got > 0) g->logprobs[g->n_gen] = lp;
    } else {
        got = g->m->eng->generate_ids(g->session, 1, g->m->eos, &id);
    }
    if (got < 0) {
        snprintf(err, errcap, "the engine failed while generating");
        return -1;
    }
    if (got == 0) { /* l'engine ha incontrato l'eos primario e non l'ha emesso */
        g->finish = FMLX_API_FINISH_STOP;
        g->ended = 1;
        return 0;
    }
    if (is_secondary_eos(g->m, id)) { /* eos della lista non gestito dall'ABI */
        g->finish = FMLX_API_FINISH_STOP;
        g->ended = 1;
        return 0;
    }
    g->ids[g->n_gen++] = id;
    return text_update(g, err, errcap);
}

/* ---- avvio della generazione ---- */

/* Intero d'ambiente con minimo: un valore assente/illeggibile/fuori range ricade sul default
 * (una politica di memoria non deve poter essere azzerata da un typo). */
static int env_int_at_least(const char *name, int def, int min) {
    const char *v = getenv(name);
    char *end;
    long n;

    if (!v || !*v) return def;
    n = strtol(v, &end, 10);
    if (end == v || *end || n < min || n > 1000000) return def;
    return (int)n;
}

/* FNV-1a sui byte degli id: il NOME del prefisso salvato. Contenuto (non puntatore) di modo che
 * richieste con lo STESSO prompt sostituiscano la stessa voce invece di accumularne una per
 * richiesta; il match vero resta comunque content-addressed (radix sull'array di token). */
static uint64_t fnv1a_ids(const int32_t *ids, int n) {
    uint64_t h = 1469598103934665603ull;
    const unsigned char *p = (const unsigned char *)ids;

    for (size_t i = 0; i < (size_t)n * sizeof *ids; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static FmlxEngineGen *start(const FmlxEngineModel *m, const int32_t *ids, int n_prompt,
                            int max_tokens, const LlmEngineLlmSampling *sp, const FmlxApiChat *c,
                            int want_logprobs, int want_top_logprobs, int think_open, char *err,
                            size_t errcap) {
    LlmEngineLlmSession s;
    FmlxEngineGen *g;
    int can_prefix;
    int reused = 0;
    int can_lp;
    int can_top;

    if (n_prompt <= 0) {
        snprintf(err, errcap, "empty prompt");
        return NULL;
    }
    if (max_tokens < 1) max_tokens = 1;
    if (max_tokens > FMLX_ENGINE_MAX_NEW) max_tokens = FMLX_ENGINE_MAX_NEW;

    /* LOGPROBS: onorabili solo se il runner espone l'estensione ABI v2. Chiedere una cosa che
     * il motore non sa dare è un errore esplicito, mai un campo nullo che finge. Le top-k
     * esigono `logprobs` (la logprob del token scelto accompagna sempre le alternative) e il
     * metodo ABI v3 `token_topk`. */
    if (want_top_logprobs > 0) want_logprobs = 1;
    can_lp = want_logprobs && (m->eng->caps & LLMMODELIO_CAP_LOGPROBS) &&
             m->eng->generate_ids_probs;
    if (want_logprobs && !can_lp) {
        snprintf(err, errcap, "logprobs were requested but this engine does not expose them");
        return NULL;
    }
    if (want_top_logprobs > (int)FMLX_API_MAX_TOP_K) want_top_logprobs = (int)FMLX_API_MAX_TOP_K;
    can_top = want_top_logprobs > 0 && can_lp && (m->eng->caps & LLMMODELIO_CAP_LOGPROBS) &&
              m->eng->token_topk;
    if (want_top_logprobs > 0 && !can_top) {
        snprintf(err, errcap,
                 "top_logprobs were requested but this engine does not expose the top-k "
                 "distribution");
        return NULL;
    }

    s = m->eng->create_session(m->model);
    if (!s) {
        snprintf(err, errcap, "the engine could not create a session");
        return NULL;
    }
    if (sp && m->eng->set_sampling) m->eng->set_sampling(s, sp);

    /* Prefill. Con CAP_PREFIX si usa il percorso AUTOMATICO (`append_ids_prefix`): il runner cerca
     * il prefisso salvato che meglio combacia coi token d'ingresso e pre-filla SOLO la coda; se
     * non c'è match utile fa l'append normale. Il match non combacia mai per costruzione della
     * ricerca (token-per-token); -1 è un errore vero e abortisce, come nel server C di GMBasic. */
    can_prefix = (m->eng->caps & LLMMODELIO_CAP_PREFIX) && m->eng->append_ids_prefix;
    if (can_prefix) {
        int rc = m->eng->append_ids_prefix(m->model, s, ids, n_prompt,
                                           env_int_at_least("FMLX_PREFIX_MIN_MATCH",
                                                            FMLX_PREFIX_MIN_MATCH, 1));
        if (rc < 0) {
            m->eng->destroy_session(s);
            snprintf(err, errcap, "the engine failed to ingest the prompt");
            return NULL;
        }
        reused = rc > 0;
    } else if (!m->eng->append_ids || m->eng->append_ids(s, ids, n_prompt) != 0) {
        m->eng->destroy_session(s);
        snprintf(err, errcap, "the engine failed to ingest the prompt");
        return NULL;
    }

    /* Salva il PREFISSO DEL PROMPT (non i token generati) per la richiesta futura. Salvare
     * prompt+uscita sarebbe riusabile solo se il prossimo prompt la CONTIENE (conversazione che
     * cresce); salvando il solo prompt, due richieste con lo stesso system prompt si riusano
     * anche quando la domanda cambia. L'auto-prefix richiede `n_ids >= token_len`, quindi un
     * prefisso salvato più lungo del prompt successivo non verrebbe comunque riusato. */
    if (can_prefix && m->eng->save_prefix &&
        n_prompt >= env_int_at_least("FMLX_PREFIX_MIN_SAVE_LEN", FMLX_PREFIX_MIN_SAVE_LEN, 1)) {
        char key[48];
        snprintf(key, sizeof key, "fmlx-%016llx", (unsigned long long)fnv1a_ids(ids, n_prompt));
        (void)m->eng->save_prefix(m->model, key, s);
    }
    if (getenv("FMLX_PREFIX_DEBUG"))
        fprintf(stderr, "[prefix] %s prompt=%d\n",
                reused ? "HIT (riuso KV)" : "miss (prefill)", n_prompt);

    g = calloc(1, sizeof *g);
    if (!g) {
        m->eng->destroy_session(s);
        snprintf(err, errcap, "out of memory for the generation state");
        return NULL;
    }
    g->ids = malloc((size_t)max_tokens * sizeof *g->ids);
    if (!g->ids) {
        m->eng->destroy_session(s);
        free(g);
        snprintf(err, errcap, "out of memory for %d output tokens", max_tokens);
        return NULL;
    }
    if (can_lp) {
        g->logprobs = malloc((size_t)max_tokens * sizeof *g->logprobs);
        g->tok_end = malloc((size_t)max_tokens * sizeof *g->tok_end);
        if (!g->logprobs || !g->tok_end) {
            m->eng->destroy_session(s);
            free(g->ids);
            free(g->logprobs);
            free(g->tok_end);
            free(g);
            snprintf(err, errcap, "out of memory for %d token logprobs", max_tokens);
            return NULL;
        }
    }
    if (can_top) {
        g->top_k = want_top_logprobs;
        g->top_ids = malloc((size_t)max_tokens * (size_t)g->top_k * sizeof *g->top_ids);
        g->top_lp = malloc((size_t)max_tokens * (size_t)g->top_k * sizeof *g->top_lp);
        if (!g->top_ids || !g->top_lp) {
            m->eng->destroy_session(s);
            free(g->ids);
            free(g->logprobs);
            free(g->tok_end);
            free(g->top_ids);
            free(g->top_lp);
            free(g);
            snprintf(err, errcap, "out of memory for %d top_logprobs", max_tokens);
            return NULL;
        }
        g->want_top_logprobs = 1;
    }
    g->m = m;
    g->session = s;
    g->c = c;
    g->max_new = max_tokens;
    g->prompt_tokens = (uint64_t)n_prompt;
    g->prefix_hit = reused;
    g->finish = FMLX_API_FINISH_LENGTH;
    g->want_logprobs = can_lp;
    /* Think split: attivo solo se il prompt ha aperto il blocco, e non in modalità logprobs
     * (che emette un token per evento e ha bisogno del testo intero del token). */
    g->want_think = (think_open && !can_lp) ? 1 : 0;
    g->in_think = g->want_think;
    g->skip_ws = 0;
    g->think_out = 0;
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
    int think_open;
    LlmEngineLlmSampling sp;
    FmlxEngineGen *g;

    if (!m || !c) return NULL;
    if (fmlx_engine_render_chat(m, c, &prompt, err, errcap) != 0) {
        fmlx_sb_free(&prompt);
        return NULL;
    }
    think_open = prompt_opens_think(prompt.p);
    ids = tokenize(m->tok, prompt.p ? prompt.p : "", &n, err, errcap);
    fmlx_sb_free(&prompt);
    if (!ids) return NULL;
    /* Default = parametro `maxtokens` del registro; `--max-tokens` (esplicito) vince. Nessuna env. */
    {
        int def = FMLX_ENGINE_MAX_NEW;
        if (fmlx_engine_opt_get_int("maxtokens", &def) != 0 || def < 1) def = FMLX_ENGINE_MAX_NEW;
        max_tokens = c->has_max_tokens ? (int)c->max_tokens : def;
    }
    sampling_of(c, &sp);
    g = start(m, ids, n, max_tokens, &sp, c, c->want_logprobs, c->want_top_logprobs, think_open,
              err, errcap);
    free(ids);
    if (g && fmlx_api_tool_mode(c, NULL) != FMLX_API_TOOL_NONE) {
        /* Qwen/ChatML marca i tool con `<tool_call>`: lì il JSON nudo non si riconosce (una
         * risposta JSON della stessa famiglia non deve diventare una chiamata). Le altre
         * famiglie (Llama-3 nativa, Hermes, generico) possono o marcare o emettere l'oggetto
         * nudo, quindi lì il riconoscimento nudo è attivo. */
        g->tools = fmlx_toolstream_new(m->tpl != FMLX_TPL_CHATML);
        /* Con i tool attivi il testo passa dal riconoscitore: il think split interferirebbe,
         * quindi si disattiva (il ragionamento resterebbe col marcatore). */
        g->want_think = 0;
    }
    return g;
}

FmlxEngineGen *fmlx_engine_prompt(FmlxEngineModel *m, const char *prompt, int max_tokens,
                                  float temperature, float top_p, int top_k, float presence,
                                  float frequency, int64_t seed, const FmlxApiChat *c, char *err,
                                  size_t errcap) {
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
    g = start(m, ids, n, max_tokens, &sp, c, c ? c->want_logprobs : 0,
              c ? c->want_top_logprobs : 0, prompt_opens_think(prompt), err, errcap);
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

int fmlx_engine_embed_dim(FmlxEngineModel *m) {
    if (!m || !(m->eng->caps & LLMMODELIO_CAP_EMBED) || !m->eng->emb_dim) return -1;
    return m->eng->emb_dim(m->model);
}

int fmlx_engine_embed(FmlxEngineModel *m, const char *text, float *out, int cap, int *out_tokens) {
    int dim, rc = -1, ntok = 0;

    if (!m || !text || !out) return -1;
    if (!(m->eng->caps & LLMMODELIO_CAP_EMBED) || !m->eng->emb_dim) return -1;
    dim = m->eng->emb_dim(m->model);
    if (dim <= 0 || cap < dim) return -1;

    if (m->eng->embed_text) {
        /* Motori con tokenizer proprio (llama.cpp): testo in ingresso. */
        rc = m->eng->embed_text(m->model, text, out, cap);
    } else if (m->eng->embed_ids) {
        /* Runner IDS-only (MLX): tokenizza qui e passa gli id. */
        int n = llmtoken_encode(m->tok, text, NULL, 0);
        int32_t *ids;
        if (n <= 0) return -1;
        ids = malloc((size_t)n * sizeof *ids);
        if (!ids) return -1;
        if (llmtoken_encode(m->tok, text, ids, n) != n) {
            free(ids);
            return -1;
        }
        rc = m->eng->embed_ids(m->model, ids, n, out, cap);
        ntok = n;
        free(ids);
    }
    if (rc <= 0) return -1;

    /* L2-normalizzazione (il mean-pool grezzo del runner non è normalizzato). */
    double s = 0.0;
    for (int i = 0; i < dim; i++) s += (double)out[i] * out[i];
    if (s > 0.0) {
        double inv = 1.0 / sqrt(s);
        for (int i = 0; i < dim; i++) out[i] = (float)((double)out[i] * inv);
    }
    if (out_tokens) *out_tokens = ntok;
    return dim;
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

/* Modalità logprobs: consegna UN token per evento leggendo da `full` con i confini `tok_end`.
 * Un token può finire a metà di una codepoint UTF-8 (BPE byte-level): in quel caso il pezzo
 * non è serializzabile da solo, quindi si UNISCE col token successivo in un solo evento (la
 * concatenazione resta il testo esatto). Ritorna 1 se un evento è pronto, 0 se serve ancora
 * avanzare (nessun token completo entro `emitted`). */
static int emit_logprob(FmlxEngineGen *g, FmlxApiEvent *ev) {
    while (g->tok_emit < g->n_gen) {
        int j = g->tok_emit;
        size_t end = g->tok_end[j];
        if (end > g->emitted) return 0; /* il token non è ancora interamente al sicuro */
        while (end < g->full_len && ((unsigned char)g->full[end] & 0xC0) == 0x80) {
            j++; /* il token finisce a metà codepoint: unisci col prossimo */
            if (j >= g->n_gen) return 0;
            end = g->tok_end[j];
            if (end > g->emitted) return 0;
        }
        size_t start = g->tok_byte;
        ev->ev = FMLX_API_EV_TEXT;
        ev->text = g->full + start;
        ev->n = (uint32_t)(end - start);
        ev->has_logprob = 1;
        ev->logprob = g->logprobs[g->tok_emit];
        /* TOP-K (ABI v3): le alternative della STESSA posizione del token emesso. Si decodifica
         * qui (il layer ① possiede il tokenizer) e si copia inline nell'evento. Una voce che non
         * entra nel buffer si omette — mai un token mozzato. */
        ev->n_top = 0;
        if (g->want_top_logprobs) {
            const int32_t *ti = g->top_ids + (size_t)g->tok_emit * (size_t)g->top_k;
            const float *tl = g->top_lp + (size_t)g->tok_emit * (size_t)g->top_k;
            int nt = 0;
            for (int t = 0; t < g->top_k; t++) {
                if (ti[t] < 0) continue;
                int need = llmtoken_decode_ex(g->m->tok, &ti[t], 1, ev->top[nt].token,
                                              (int)sizeof ev->top[nt].token, 0);
                if (need < 0 || (size_t)need >= sizeof ev->top[nt].token) continue;
                ev->top[nt].n = (uint32_t)need;
                ev->top[nt].logprob = tl[t];
                nt++;
            }
            ev->n_top = nt;
        }
        g->tok_byte = end;
        g->tok_emit = j + 1;
        return 1;
    }
    return 0;
}

/* Un evento dal flusso THINK-split: chunk omogeneo (ragionamento o risposta), con il marker di
 * chiusura/apertura consumato (mai consegnato). Ritorna 1 se un evento è pronto, 0 se serve
 * avanzare. La coda di `full` è già limitata a un confine sicuro da `text_update`. */
static int emit_think(FmlxEngineGen *g, FmlxApiEvent *ev) {
    while (g->think_out < g->emitted) {
        /* Dopo `</think>` il template mette `\n\n`: è formattazione, non risposta. Si salta lo
         * spazio bianco in testa alla risposta (solo quello immediatamente dopo il marker). */
        if (g->skip_ws) {
            unsigned char c = (unsigned char)g->full[g->think_out];
            if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
                g->think_out++;
                continue;
            }
            g->skip_ws = 0;
        }
        const char *const *set = g->in_think ? THINK_CLOSE : THINK_OPEN;
        int nset = g->in_think ? THINK_NCLOSE : THINK_NOPEN;
        size_t mpos = (size_t)-1, mlen = 0;
        for (int i = 0; i < nset; i++) {
            size_t ml = strlen(set[i]);
            const char *q = find_bytes(g->full + g->think_out, g->emitted - g->think_out, set[i], ml);
            if (q) {
                size_t pos = (size_t)(q - g->full);
                if (pos < mpos) {
                    mpos = pos;
                    mlen = ml;
                }
            }
        }
        if (mpos == g->think_out) { /* il marker è esattamente qui: si consuma e si cambia stato */
            g->think_out += mlen;
            g->in_think = !g->in_think;
            if (!g->in_think) g->skip_ws = 1; /* `</think>` consumato: salta lo spazio del template */
            continue;
        }
        size_t upto = (mpos == (size_t)-1) ? g->emitted : mpos;
        if (upto > g->think_out) {
            size_t n = upto - g->think_out;
            if (n > FMLX_ENGINE_CHUNK) n = FMLX_ENGINE_CHUNK;
            while (n < upto - g->think_out && ((unsigned char)g->full[g->think_out + n] & 0xC0) == 0x80)
                n--; /* mai spezzare una codepoint */
            ev->ev = g->in_think ? FMLX_API_EV_THINK : FMLX_API_EV_TEXT;
            ev->text = g->full + g->think_out;
            ev->n = (uint32_t)n;
            g->think_out += n;
            return 1;
        }
        break;
    }
    return 0;
}

FmlxApiGenSt fmlx_engine_next(void *ctx, FmlxApiEvent *ev, FmlxApiErrCtx *err) {
    FmlxEngineGen *g = ctx;

    if (!g || !ev) return FMLX_API_GEN_ERR;
    if (g->err) {
        if (err) {
            err->err = FMLX_API_INTERNAL;
            err->message = "the engine failed while generating";
        }
        return FMLX_API_GEN_ERR;
    }
    for (;;) {
        if (g->tools) {
            FmlxToolOut o;
            if (fmlx_toolstream_pop(g->tools, &o)) {
                if (o.kind == FMLX_TS_TEXT) {
                    ev->ev = FMLX_API_EV_TEXT;
                    ev->text = o.text;
                    ev->n = (uint32_t)o.n;
                } else if (o.kind == FMLX_TS_TOOL_NAME) {
                    ev->ev = FMLX_API_EV_TOOL_NAME;
                    ev->tool_name = o.name;
                    ev->tool_id = NULL; /* l'adapter assegna `call_<i>` */
                } else {
                    ev->ev = FMLX_API_EV_TOOL_ARGS;
                    ev->text = o.text;
                    ev->n = (uint32_t)o.n;
                }
                return FMLX_API_GEN_MORE;
            }
        }
        if (g->want_logprobs) {
            /* Un token per evento, con la sua logprob. `tools` è mutuamente escluso con
             * `logprobs` a monte, quindi non c'è da passare dal riconoscitore. */
            if (emit_logprob(g, ev)) return FMLX_API_GEN_MORE;
        } else if (g->want_think) {
            /* Ragionamento → THINK, risposta → TEXT, marker consumato (non esce). */
            if (emit_think(g, ev)) return FMLX_API_GEN_MORE;
        } else if (g->pos < g->text_len) {
            if (g->tools) {
                /* Tutto il delta decodificato passa dal riconoscitore: lui decide testo vs
                 * chiamata, e trattiene la coda che può ancora essere un marcatore. */
                fmlx_toolstream_feed(g->tools, g->text + g->pos, g->text_len - g->pos);
                g->pos = g->text_len;
                continue;
            }
            size_t end = g->pos;
            while (end < g->text_len && end - g->pos < FMLX_ENGINE_CHUNK)
                end = utf8_step(g->text, end, g->text_len);
            ev->ev = FMLX_API_EV_TEXT;
            ev->text = g->text + g->pos;
            ev->n = (uint32_t)(end - g->pos);
            g->pos = end;
            return FMLX_API_GEN_MORE;
        }
        if (g->ended) {
            if (g->tools && !g->tools_flushed) {
                g->tools_flushed = 1;
                fmlx_toolstream_finish(g->tools);
                continue; /* la coda trattenuta può ancora produrre testo o chiamate */
            }
            break;
        }
        {
            char e[256] = "";
            if (advance(g, e, sizeof e) != 0) {
                g->err = 1;
                if (err) {
                    err->err = FMLX_API_INTERNAL;
                    err->message = "the engine failed while generating";
                }
                return FMLX_API_GEN_ERR;
            }
        }
    }
    if (!g->done) {
        g->done = 1;
        ev->ev = FMLX_API_EV_DONE;
        ev->finish = g->finish;
        /* Se il modello ha emesso chiamate e non c'è una causa più forte (eos/lunghezza/stop),
         * il finish è `tool_calls`: è ciò che il client si aspetta per eseguirle. */
        if (g->tools && fmlx_toolstream_ntools(g->tools) > 0 &&
            ev->finish == FMLX_API_FINISH_STOP)
            ev->finish = FMLX_API_FINISH_TOOL;
        ev->prompt_tokens = g->prompt_tokens;
        ev->completion_tokens = (uint64_t)g->n_gen;
        ev->cached_tokens = 0;
        ev->stop_sequence =
            g->finish == FMLX_API_FINISH_STOPSEQ && g->stop_seq[0] ? g->stop_seq : NULL;
        return FMLX_API_GEN_MORE;
    }
    return FMLX_API_GEN_END;
}

int fmlx_engine_gen_prefix_hit(const FmlxEngineGen *g) { return g ? g->prefix_hit : 0; }

int fmlx_engine_gen_kv_stats(const FmlxEngineGen *g, int *out_used, int *out_cap) {
    if (!g || !g->session || !g->m || !g->m->eng || !g->m->eng->kv_stats) return -1;
    return g->m->eng->kv_stats(g->session, out_used, out_cap);
}

void fmlx_engine_gen_free(FmlxEngineGen *g) {
    if (!g) return;
    if (g->session && g->m && g->m->eng && g->m->eng->destroy_session)
        g->m->eng->destroy_session(g->session);
    free(g->ids);
    free(g->full);
    free(g->text);
    free(g->logprobs);
    free(g->tok_end);
    free(g->top_ids);
    free(g->top_lp);
    if (g->tools) fmlx_toolstream_free(g->tools);
    free(g);
}
