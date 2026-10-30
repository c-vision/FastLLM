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
#include <pthread.h>
#include "llm_grammar.h"
#include "llmengine_mlx.h" /* llm_ai_mlx_set_prefill_yield / session_set_prefill_chunk (chunked prefill + mixed batch) */

/* CHUNKED PREFILL + MIXED BATCH: stream in decode gestiti dal coordinatore e flag "START in corso sul coordinatore". */
static int g_active_streams;
static int g_in_coord_start;
static void gram_jump(FmlxEngineGen *g, char *err, size_t errcap);

/* MLX: device/stream di default PER-THREAD. Ogni thread di lavoro che usa il motore deve
 * registrarlo (idempotente), altrimenti la prima op MLX aborta con "no Stream(gpu, 0)". */
extern int llm_ai_mlx_thread_init(void);

/* ---- ANTI-INJECTION dei token di controllo ----
 * Il testo non fidato (messaggi, tool, argomenti, risultati, nomi) entra nel prompt solo tramite `sb_add_untrusted`/`untrusted_dup`: i marcatori di
 * controllo vengono racchiusi fra guardie (U+E000/U+E001) e `tokenize_chat` li codifica come testo ORDINARIO. Il template resta libero di
 * emettere i propri token di controllo. t_guard e' per-thread (il render puo' girare su piu' thread I/O). */
static __thread llmtoken_handle t_guard;

static int sb_add_untrusted(FmlxSb *out, const char *p, size_t n) {
    if (!t_guard || n == 0) return fmlx_sb_add(out, p, n);
    size_t i = 0;
    while (i < n) {   /* i NUL non possono stare in una stringa C: si saltano (nessun testo valido li contiene) */
        size_t j = i;
        while (j < n && p[j] != 0) j++;
        if (j > i) {
            char *tmp = malloc(j - i + 1), *g;
            int rc;
            if (!tmp) return -1;
            memcpy(tmp, p + i, j - i);
            tmp[j - i] = 0;
            g = llmtoken_guard_text(t_guard, tmp);
            free(tmp);
            if (!g) return -1;
            rc = fmlx_sb_add(out, g, strlen(g));
            free(g);
            if (rc != 0) return -1;
        }
        i = j + 1;
    }
    return 0;
}

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

/* ---- IMMAGINI (input multimodale): descrittore registrato dal server (vedi fmlx_engine.h).
 * Il motore MLX non ha vision: il server collega un VLM (lane llama.cpp/mtmd) e inietta la
 * descrizione testuale dell'immagine nel prompt. Assente -> i blocchi immagine sono ignorati. */
static FmlxImageDescriber g_img_desc = NULL;
static void *g_img_ctx = NULL;
void fmlx_engine_set_image_describer(FmlxImageDescriber fn, void *ctx) {
    g_img_desc = fn;
    g_img_ctx = ctx;
}

/* URL di un blocco immagine: OpenAI `{type:image_url,image_url:{url}}` (data URL o http);
 * Anthropic `{type:image,source:{type:base64,media_type,data}}` -> ricostruito in
 * `data:<mime>;base64,<data>`. Ritorna una stringa mallocata (chiamante free) o NULL. */
static char *image_url_dup(const FmlxJsonValue *b) {
    const FmlxJsonValue *ty = fmlx_json_obj_get(b, "type");
    const char *tp = NULL;
    size_t tn = 0;
    if (!ty || !fmlx_json_as_str(ty, &tp, &tn)) return NULL;
    if (tn == 9 && !memcmp(tp, "image_url", 9)) {                 /* OpenAI */
        const FmlxJsonValue *iu = fmlx_json_obj_get(b, "image_url");
        const FmlxJsonValue *u = iu ? fmlx_json_obj_get(iu, "url") : NULL;
        const char *p = NULL;
        size_t n = 0;
        if (!u || !fmlx_json_as_str(u, &p, &n)) return NULL;
        char *s = (char *)malloc(n + 1);
        if (!s) return NULL;
        memcpy(s, p, n); s[n] = '\0';
        return s;
    }
    if ((tn == 5 && !memcmp(tp, "image", 5)) || (tn == 8 && !memcmp(tp, "document", 8))) { /* Anthropic */
        const FmlxJsonValue *src = fmlx_json_obj_get(b, "source");
        const FmlxJsonValue *st = src ? fmlx_json_obj_get(src, "type") : NULL;
        const FmlxJsonValue *data = src ? fmlx_json_obj_get(src, "data") : NULL;
        const FmlxJsonValue *media = src ? fmlx_json_obj_get(src, "media_type") : NULL;
        const char *sp = NULL, *dp = NULL, *mp = "image/png";
        size_t sn = 0, dn = 0, mn = 9;
        if (!st || !fmlx_json_as_str(st, &sp, &sn) || sn != 6 || memcmp(sp, "base64", 6)) return NULL;
        if (!data || !fmlx_json_as_str(data, &dp, &dn)) return NULL;
        if (media) (void)fmlx_json_as_str(media, &mp, &mn);
        char *s = (char *)malloc(mn + dn + 32);
        if (!s) return NULL;
        int h = snprintf(s, mn + dn + 32, "data:%.*s;base64,", (int)mn, mp);
        memcpy(s + h, dp, dn); s[h + dn] = '\0';
        return s;
    }
    return NULL;
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
        return sb_add_untrusted(out, p, n);
    }
    if (content->type != FMLX_JSON_ARR) return 0;
    for (size_t i = 0; i < fmlx_json_arr_n(content); i++) {
        const FmlxJsonValue *b = fmlx_json_arr_at(content, i);
        if (!b) continue;
        if (b->type == FMLX_JSON_STR) {
            const char *p = fmlx_json_str(b);
            size_t n = fmlx_json_str_len(b);
            if (p && sb_add_untrusted(out, p, n) != 0) return -1;
        } else if (b->type == FMLX_JSON_OBJ) {
            /* blocco testo (Anthropic e OpenAI a blocchi): si prende `text`. Un blocco IMMAGINE
             * viene descritto da un VLM (descrittore registrato dal server) e iniettato come testo. */
            const FmlxJsonValue *t = fmlx_json_obj_get(b, "text");
            if (t && t->type == FMLX_JSON_STR) {
                const char *p = fmlx_json_str(t);
                size_t n = fmlx_json_str_len(t);
                if (p && sb_add_untrusted(out, p, n) != 0) return -1;
            } else if (g_img_desc) {
                char *u = image_url_dup(b);
                if (u) {
                    char desc[16384];
                    if (g_img_desc(g_img_ctx, u, strlen(u), desc, sizeof desc) == 0 && desc[0])
                        if (fmlx_sb_add(out, "\n", 1) != 0 ||
                            sb_add_untrusted(out, desc, strlen(desc)) != 0) { free(u); return -1; }
                    free(u);
                }
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
        if (t_guard) {   /* stringa non fidata: marcatori di controllo protetti */
            char *gd = llmtoken_guard_text(t_guard, tmp);
            if (gd) { free(tmp); tmp = gd; }
        }
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
        rc = sb_add_untrusted(b, w.buf ? w.buf : "null", n);
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
    if (sys_n && sb_add_untrusted(out, sys, sys_n) != 0) goto oom;
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
            sb_add_untrusted(out, named->p, named->n) != 0 || fmlx_sb_cstr(out, ".") != 0)
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
                        size_t errcap, int gen_prompt) {
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
    llmjinja_obj_set(vars, "add_generation_prompt", llmjinja_bool(gen_prompt ? 1 : 0));
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

static int render_chat_impl(const FmlxEngineModel *m, const FmlxApiChat *c, FmlxSb *b, char *err, size_t errcap) {
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
        if (render_jinja(m, c, b, err, errcap, 1) == 0) return 0;
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
                sb_add_untrusted(b, role, rn) != 0 ||
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
            if (fmlx_sb_cstr(b, "<|im_start|>") != 0 || sb_add_untrusted(b, role, rn) != 0 ||
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
            if (sb_add_untrusted(b, role, rn) != 0 || fmlx_sb_cstr(b, ": ") != 0 ||
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

int fmlx_engine_render_chat(const FmlxEngineModel *m, const FmlxApiChat *c, FmlxSb *b, char *err, size_t errcap) {
    int rc;

    if (!m) return -1;
    t_guard = m->tok;   /* attiva la protezione dei marcatori di controllo per tutto il render (per-thread) */
    rc = render_chat_impl(m, c, b, err, errcap);
    t_guard = NULL;
    return rc;
}

/* ---- tokenizzazione ---- */

static int32_t *tokenize_g(llmtoken_handle tok, const char *text, int guarded, int *out_n, char *err, size_t errcap) {
    int n = guarded ? llmtoken_encode_guarded(tok, text, NULL, 0) : llmtoken_encode(tok, text, NULL, 0);
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
    if ((guarded ? llmtoken_encode_guarded(tok, text, ids, n) : llmtoken_encode(tok, text, ids, n)) != n) {
        free(ids);
        snprintf(err, errcap, "the tokenizer produced an inconsistent token count");
        return NULL;
    }
    *out_n = n;
    return ids;
}

/* Testo grezzo (completions, candidati interni): i token speciali scritti nel testo restano riconosciuti, e' un prompt "raw" dell'utente. */
static int32_t *tokenize(llmtoken_handle tok, const char *text, int *out_n, char *err, size_t errcap) {
    return tokenize_g(tok, text, 0, out_n, err, errcap);
}
/* Prompt di chat renderizzato: gli span non fidati protetti da guardie sono testo ORDINARIO. */
static int32_t *tokenize_chat(llmtoken_handle tok, const char *text, int *out_n, char *err, size_t errcap) {
    return tokenize_g(tok, text, 1, out_n, err, errcap);
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
    char *cur;
    size_t limit, hb;
    const char *which = NULL;
    uint32_t which_n = 0;
    const size_t prev_len = g->full_len;

    /* DECODE INCREMENTALE (C14): si decodificano solo i token NUOVI e si accodano a `full` (il decode di una sequenza e'
     * la concatenazione dei decode dei singoli token; solo il PRIMO ha lo strip dello spazio iniziale del metaspace).
     * Verificato su tokenizer byte-level e metaspace (gemma4) contro il decode dell'intera sequenza. Prima: decode di
     * TUTTA la sequenza a ogni token = O(n^2) su uscite lunghe. */
    for (int i = g->dec_n; i < g->n_gen; i++) {
        char buf[512];
        int32_t id = g->ids[i];
        int l = llmtoken_decode_ex(g->m->tok, &id, 1, buf, sizeof buf, i == 0 ? 1 : 0);
        if (l < 0 || l >= (int)sizeof buf) {
            snprintf(err, errcap, "the tokenizer could not decode the output");
            return -1;
        }
        if (g->full_len + (size_t)l + 1 > g->full_cap) {
            size_t nc = (g->full_cap ? g->full_cap * 2 : 256);
            while (nc < g->full_len + (size_t)l + 1) nc *= 2;
            cur = realloc(g->full, nc);
            if (!cur) {
                snprintf(err, errcap, "out of memory decoding the output");
                return -1;
            }
            g->full = cur;
            g->full_cap = nc;
        }
        memcpy(g->full + g->full_len, buf, (size_t)l);
        g->full_len += (size_t)l;
        g->full[g->full_len] = '\0';
    }
    g->dec_n = g->n_gen;
    if (!g->full) { /* nessun token: full vuoto ma valido */
        g->full = malloc(1);
        if (!g->full) {
            snprintf(err, errcap, "out of memory decoding the output");
            return -1;
        }
        g->full[0] = '\0';
        g->full_cap = 1;
    }
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
            for (size_t k = prev_len + 1 > sn ? prev_len + 1 - sn : 0; k + sn <= g->full_len; k++) { /* solo le posizioni nuove */
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

/* La generazione è finita PER LUNGHEZZA? 1 = sì (imposta `finish`/`ended`), 0 = serve un token. */
static int adv_no_token(FmlxEngineGen *g) {
    if (g->n_gen >= g->max_new) {
        g->finish = FMLX_API_FINISH_LENGTH;
        g->ended = 1;
        return 1;
    }
    return 0;
}

/* La parte POST-generazione di un passo: applica UN token già generato. `got` = >0 token valido in
 * `id`; 0 = l'engine ha incontrato l'eos primario e non l'ha emesso; <0 = errore. Separata dal
 * `generate_*` perché il lotto continuo (M3) genera il token con `generate_batch` per TUTTE le
 * sessioni e poi lo applica a ciascuna QUI, senza rifare il passo. */
static int gen_apply_token(FmlxEngineGen *g, int got, int32_t id, char *err, size_t errcap) {
    if (getenv("GEN_IDS") && got > 0) fprintf(stderr, "[genid] %d\n", id);
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

/* Un passo di decode: esattamente UN token. Con `want_logprobs` si usa l'estensione ABI v2
 * `generate_ids_probs`, che cattura la logprob del token PRIMA di avanzare (esatta anche con
 * sampling: la distribuzione è quella del modello alla posizione di partenza). */
static int advance(FmlxEngineGen *g, char *err, size_t errcap) {
    int got;
    int32_t id = 0;

    if (adv_no_token(g)) return 0;
    if (g->gram) { /* token dalla bitmask della grammatica (ABI v6), greedy */
        LlmGrammar *gr = (LlmGrammar *)g->gram;
        if (llm_grammar_fill_mask(gr, g->gmask) <= 0) {
            snprintf(err, errcap, "the grammar has no valid continuation");
            return -1;
        }
        int rc = g->m->eng->generate_id_masked(g->session, g->gmask, g->gwords, &id);
        if (rc != 0) {
            snprintf(err, errcap, "the engine failed while generating");
            return -1;
        }
        (void)llm_grammar_accept(gr, id);
        {
            int rcg = gen_apply_token(g, id == g->m->eos ? 0 : 1, id, err, errcap);
            if (rcg == 0 && !g->ended) gram_jump(g, err, errcap);
            return rcg;
        }
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
    return gen_apply_token(g, got, id, err, errcap);
}

/* ---- GRAMMATICA (response_format) ----
 * Tabella id -> byte del vocabolario, costruita una volta per modello (decode di ogni id SENZA strip dello spazio
 * iniziale). I token speciali (tipo CONTROL del tokenizer, oppure <|...|>, <s>, </s>, <unk>, <pad>) hanno lunghezza 0:
 * la grammatica non li ammette mai tranne l'EOS a documento completo. */
static struct { const FmlxEngineModel *m; int n; uint8_t **tok; int *len; } g_gv;
static pthread_mutex_t g_gv_mu = PTHREAD_MUTEX_INITIALIZER;

static int gram_special_text(const char *s, int l) {
    if (l >= 4 && s[0] == '<' && s[1] == '|' && s[l - 2] == '|' && s[l - 1] == '>') return 1;
    if ((l == 3 && !memcmp(s, "<s>", 3)) || (l == 4 && !memcmp(s, "</s>", 4)) || (l == 5 && !memcmp(s, "<unk>", 5)) ||
        (l == 5 && !memcmp(s, "<pad>", 5)))
        return 1;
    return 0;
}

static int gram_vocab(const FmlxEngineModel *m) {
    size_t n = 0;

    pthread_mutex_lock(&g_gv_mu);
    if (g_gv.m == m && g_gv.n > 0) { pthread_mutex_unlock(&g_gv_mu); return g_gv.n; }
    if (!m->tok || !llmtoken_export_tokens(m->tok, &n) || n == 0) { pthread_mutex_unlock(&g_gv_mu); return 0; }
    size_t nt = 0;
    const int32_t *types = llmtoken_export_token_types(m->tok, &nt);
    uint8_t **tok = calloc(n, sizeof *tok);
    int *len = calloc(n, sizeof *len);
    if (!tok || !len) { free(tok); free(len); pthread_mutex_unlock(&g_gv_mu); return 0; }
    char buf[512];
    for (size_t i = 0; i < n; i++) {
        int32_t id = (int32_t)i;
        int l = llmtoken_decode_ex(m->tok, &id, 1, buf, sizeof buf, 0);
        if (l <= 0 || l >= (int)sizeof buf) continue;
        if (types && i < nt && types[i] == 3) continue;   /* CONTROL */
        if (gram_special_text(buf, l)) continue;
        tok[i] = malloc((size_t)l);
        if (!tok[i]) continue;
        memcpy(tok[i], buf, (size_t)l);
        len[i] = l;
    }
    if (g_gv.tok) { for (int i = 0; i < g_gv.n; i++) free(g_gv.tok[i]); free(g_gv.tok); free(g_gv.len); }
    g_gv.m = m; g_gv.n = (int)n; g_gv.tok = tok; g_gv.len = len;
    pthread_mutex_unlock(&g_gv_mu);
    return (int)n;
}

/* TOOL-CALLING OBBLIGATO (`tool_choice` = required o una funzione specifica): lo schema delle chiamate ammesse
 * {anyOf:[{name:const, <argkey>:<parametri>}...]} nel formato della famiglia del modello (ChatML/Qwen: avvolto in
 * <tool_call>\n..\n</tool_call> con "arguments"; Llama-3: oggetto nudo con "parameters"). Ritorna 1 e riempie `sch`/pre/post. */
static int gram_tool_schema(const FmlxApiChat *c, const FmlxEngineModel *m, FmlxSb *sch, const char **pre,
                            const char **post) {
    FmlxSlice named = { 0 };
    int mode = fmlx_api_tool_mode(c, &named);
    const char *argkey;
    int on = 1;

    if (fmlx_engine_opt_get_int("grammar_tools", &on) == 0 && !on) return 0;   /* A/B: 0 = tool_choice obbligato SENZA grammatica */

    if (!c->tools || !fmlx_json_arr_n(c->tools) || (mode != FMLX_API_TOOL_REQUIRED && mode != FMLX_API_TOOL_NAMED)) return 0;
    {   /* formato di chiamata dedotto dal template della famiglia (il classificatore GENERIC non basta: Llama-3.2 non cita <|begin_of_text|>) */
        const int has_tc = (m->chat_tpl && strstr(m->chat_tpl, "<tool_call>")) ? 1 : 0;
        const int has_params = (m->chat_tpl && strstr(m->chat_tpl, "\"parameters\"")) ? 1 : 0;
        if (m->tpl == FMLX_TPL_CHATML || has_tc) { *pre = "<tool_call>\n"; *post = "\n</tool_call>"; argkey = "arguments"; }
        else if (m->tpl == FMLX_TPL_LLAMA3 || has_params) { *pre = NULL; *post = NULL; argkey = "parameters"; }
        else return 0;
    }
    {   /* due passate sui tool ammessi: i nomi (enum) e i parametri (anyOf), nello STESSO ordine: lo schema usa
         * l'estensione `x-dependent-arguments` (il ramo di `arguments` e' scelto dal nome gia' emesso) */
        const FmlxJsonValue *sel[64];
        size_t nsel = 0;

        for (size_t i = 0; i < fmlx_json_arr_n(c->tools) && nsel < 64; i++) {
            const FmlxJsonValue *t = fmlx_json_arr_at(c->tools, i);
            const FmlxJsonValue *fn = t ? fmlx_json_obj_get(t, "function") : NULL;
            const FmlxJsonValue *src = fn ? fn : t;
            const FmlxJsonValue *nm = src ? fmlx_json_obj_get(src, "name") : NULL;
            const char *s = nm ? fmlx_json_str(nm) : NULL;
            size_t sl = nm ? fmlx_json_str_len(nm) : 0;
            int safe = s && sl > 0;

            for (size_t k = 0; safe && k < sl; k++) {
                char ch = s[k];
                if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.'))
                    safe = 0;
            }
            if (!safe) continue;
            if (mode == FMLX_API_TOOL_NAMED && (named.n != sl || memcmp(named.p, s, sl) != 0)) continue;
            sel[nsel++] = src;
        }
        if (nsel == 0) return 0;
        if (fmlx_sb_cstr(sch, "{\"type\":\"object\",\"x-dependent-arguments\":true,\"properties\":{\"name\":{\"enum\":[") != 0) return 0;
        for (size_t i = 0; i < nsel; i++) {
            const FmlxJsonValue *nm = fmlx_json_obj_get(sel[i], "name");
            if ((i && fmlx_sb_cstr(sch, ",") != 0) || fmlx_sb_cstr(sch, "\"") != 0 ||
                fmlx_sb_add(sch, fmlx_json_str(nm), fmlx_json_str_len(nm)) != 0 || fmlx_sb_cstr(sch, "\"") != 0)
                return 0;
        }
        if (fmlx_sb_cstr(sch, "]},\"") != 0 || fmlx_sb_cstr(sch, argkey) != 0 || fmlx_sb_cstr(sch, "\":{\"anyOf\":[") != 0) return 0;
        for (size_t i = 0; i < nsel; i++) {
            const FmlxJsonValue *ps = fmlx_json_obj_get(sel[i], "parameters");
            if (!ps) ps = fmlx_json_obj_get(sel[i], "input_schema");
            if (i && fmlx_sb_cstr(sch, ",") != 0) return 0;
            if (ps) { if (sb_json(sch, ps) != 0) return 0; }
            else if (fmlx_sb_cstr(sch, "{\"type\":\"object\"}") != 0) return 0;
        }
        if (fmlx_sb_cstr(sch, "]}},\"required\":[\"name\",\"") != 0 || fmlx_sb_cstr(sch, argkey) != 0 || fmlx_sb_cstr(sch, "\"]}") != 0) return 0;
    }
    return 1;
}

static int gram_wanted(const FmlxApiChat *c) {
    FmlxSlice n = { 0 };
    int mode;
    int on = 1;

    if (!c) return 0;
    if (c->response_format && (c->rf_json_object || c->rf_json_schema)) return 1;
    if (fmlx_engine_opt_get_int("grammar_tools", &on) == 0 && !on) return 0;
    mode = fmlx_api_tool_mode(c, &n);
    return c->tools && fmlx_json_arr_n(c->tools) && (mode == FMLX_API_TOOL_REQUIRED || mode == FMLX_API_TOOL_NAMED);
}

/* Crea la grammatica per la richiesta (NULL = nessun vincolo o non supportato: resta l'istruzione nel prompt). */
static void gram_setup(FmlxEngineGen *g, const FmlxApiChat *c, const FmlxEngineModel *m) {
    LlmGrammar *gr = NULL;
    char err[200] = "";
    FmlxSb tsch = { 0 };
    const char *tpre = NULL, *tpost = NULL;
    int tool = 0;

    if (!c || !m->eng->generate_id_masked || g->want_logprobs || g->want_think) return;
    if (!c->response_format || !(c->rf_json_object || c->rf_json_schema)) {
        tool = gram_tool_schema(c, m, &tsch, &tpre, &tpost);
        if (!tool) { free(tsch.p); return; }
    } else if (c->tools && fmlx_json_arr_n(c->tools)) return;   /* tools + response_format: non vincolare */
    if (tool) {
        gr = llm_grammar_new_schema_wrapped(tsch.p, tsch.len, tpre, tpost, err, sizeof err);
        free(tsch.p);
        if (!gr) { fprintf(stderr, "fastllm: schema dei tool non compilabile in grammatica (%s): chiamata non vincolata\n", err); return; }
    } else if (c->rf_json_schema) {
        const FmlxJsonValue *js = fmlx_json_obj_get(c->response_format, "json_schema");
        const FmlxJsonValue *sch = js ? fmlx_json_obj_get(js, "schema") : NULL;
        FmlxSb sb = { 0 };
        if (!sch || sb_json(&sb, sch) != 0) { free(sb.p); return; }
        gr = llm_grammar_new_schema(sb.p, sb.len, err, sizeof err);
        free(sb.p);
        if (!gr) { fprintf(stderr, "fastllm: json_schema non compilabile in grammatica (%s): solo istruzione nel prompt\n", err); return; }
    } else {
        gr = llm_grammar_new_json_object();
        if (!gr) return;
    }
    int nv = gram_vocab(m);
    if (nv <= 0) { llm_grammar_free(gr); return; }
    pthread_mutex_lock(&g_gv_mu);
    int eos[FMLX_ENGINE_EOS_MAX + 1], ne = 0;
    eos[ne++] = m->eos;
    for (int i = 0; i < m->n_eos && ne < FMLX_ENGINE_EOS_MAX; i++) if (m->eos_list[i] != m->eos) eos[ne++] = m->eos_list[i];
    int rc = llm_grammar_set_vocab(gr, g_gv.n, (const uint8_t *const *)g_gv.tok, g_gv.len, eos, ne);
    pthread_mutex_unlock(&g_gv_mu);
    g->gmask = rc == 0 ? malloc((size_t)gr->n_words * sizeof *g->gmask) : NULL;
    if (!g->gmask) { llm_grammar_free(gr); return; }
    g->gwords = gr->n_words;
    g->gram = gr;
}

/* JUMP-FORWARD (opzione `grammar_jump_forward`, default 0): quando la grammatica impone un tratto di testo (chiavi
 * dello schema, separatori), lo si accoda con UN append invece di un decode per token. Si accettano solo i token che
 * cadono interamente nel tratto forzato E la cui tokenizzazione in contesto coincide (encode(testo+forzato) deve
 * cominciare esattamente con gli id gia' generati); altrimenti nessun salto. Assume JSON compatto (nessuna whitespace
 * nel tratto saltato). I token saltati non hanno logprob. */
static void gram_jump(FmlxEngineGen *g, char *err, size_t errcap) {
    LlmGrammar *gr = (LlmGrammar *)g->gram;
    uint8_t forced[256];
    int on = 0;

    if (fmlx_engine_opt_get_int("grammar_jump_forward", &on) != 0 || !on) return;
    if (!g->m->eng->append_ids || !g->full) return;
    int fl = llm_grammar_forced(gr, forced, (int)sizeof forced);
    if (fl < 4) return;
    size_t base = g->full_len;
    char *cand = malloc(base + (size_t)fl + 1);
    if (!cand) return;
    memcpy(cand, g->full, base);
    memcpy(cand + base, forced, (size_t)fl);
    cand[base + (size_t)fl] = 0;
    int n = 0;
    char e2[64];
    int32_t *ids = tokenize(g->m->tok, cand, &n, e2, sizeof e2);
    free(cand);
    if (!ids) return;
    int ok = n > g->n_gen && memcmp(ids, g->ids, (size_t)g->n_gen * sizeof *ids) == 0;
    int j = 0, used = 0;
    if (ok) {
        for (int i = g->n_gen; i < n; i++) {
            int l = (i < g_gv.n) ? g_gv.len[ids[i]] : 0;
            if (l <= 0 || used + l > fl || memcmp(forced + used, g_gv.tok[ids[i]], (size_t)l) != 0) break;
            used += l; j++;
        }
        if (g->n_gen + j >= g->max_new) j = g->max_new - g->n_gen - 1;
    }
    if (ok && j > 0 && g->m->eng->append_ids(g->session, ids + g->n_gen, j) == 0) {
        /* `text_update` SOSTITUISCE il testo in attesa di consegna: quello del token appena emesso va conservato e
         * anteposto al nuovo, altrimenti il consumer perde i byte (es. la `{` iniziale). */
        char *old = g->text;
        size_t old_n = g->text_len > g->pos ? g->text_len - g->pos : 0;
        size_t old_pos = g->pos;
        g->text = NULL; g->text_len = 0; g->pos = 0;
        for (int i = 0; i < j; i++) {
            int32_t id = ids[g->n_gen];
            if (llm_grammar_accept(gr, id) != 0) break;   /* non dovrebbe accadere: stato coerente fino qui */
            g->ids[g->n_gen++] = id;
        }
        (void)text_update(g, err, errcap);
        if (old) {
            if (old_n) {
                char *nb = malloc(old_n + g->text_len);
                if (nb) {
                    memcpy(nb, old + old_pos, old_n);
                    if (g->text_len) memcpy(nb + old_n, g->text + g->pos, g->text_len - g->pos);
                    free(g->text);
                    g->text = nb; g->text_len = old_n + (g->text_len - g->pos); g->pos = 0;
                }
            }
            free(old);
        }
    }
    free(ids);
}

/* ---- avvio della generazione ---- */

/* Intero d'ambiente con minimo: un valore assente/illeggibile/fuori range ricade sul default
 * (una politica di memoria non deve poter essere azzerata da un typo). */
/* Legacy: alcune soglie arrivavano da variabili d'ambiente. Ora sono parametri del REGISTRO
 * motore (niente env): `prefix_min_match` / `prefix_min_save_len`. */
static int opt_int_at_least(const char *name, int def, int min) {
    int v = 0;
    if (fmlx_engine_opt_get_int(name, &v) != 0) return def;
    if (v < min || v > 1000000) return def;
    return v;
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

/* Confine STABILE del prompt, per i modelli il cui stato di prefisso vale solo a lunghezza esatta (stato ricorrente: GDN/KDA/SSM).
 * Il prompt finisce con l'intestazione di generazione ("<|im_start|>assistant\n<think>..."), che nel turno successivo viene sostituita
 * dalla risposta vera: salvare lo stato alla fine del prompt non combacerebbe mai. Il confine e' il prefisso comune fra il prompt e lo
 * stesso prompt reso SENZA intestazione di generazione: li' comincia il turno successivo. 0 = non determinabile (nessun template Jinja,
 * render fallito): il chiamante ripiega sul comportamento storico. */
static int stable_prefix_len(const FmlxEngineModel *m, const FmlxApiChat *c, const int32_t *ids, int n_prompt) {
    FmlxSb b = {0};
    char err[160] = "";
    int32_t *t;
    int n = 0, i, lim, rc;

    if (!m || !c || !m->chat_tpl || !ids || n_prompt <= 0) return 0;
    t_guard = m->tok;
    rc = render_jinja(m, c, &b, err, sizeof err, 0);
    t_guard = NULL;
    if (rc != 0) {
        fmlx_sb_free(&b);
        return 0;
    }
    t = tokenize_chat(m->tok, b.p ? b.p : "", &n, err, sizeof err);
    fmlx_sb_free(&b);
    if (!t) return 0;
    lim = n < n_prompt ? n : n_prompt;
    for (i = 0; i < lim && t[i] == ids[i]; i++) {
    }
    free(t);
    return i;
}

static FmlxEngineGen *start(const FmlxEngineModel *m, const int32_t *ids, int n_prompt,
                            int max_tokens, const LlmEngineLlmSampling *sp, const FmlxApiChat *c,
                            int want_logprobs, int want_top_logprobs, int think_open, char *err,
                            size_t errcap) {
    LlmEngineLlmSession s;
    FmlxEngineGen *g;
    int can_prefix;
    int reused = 0;
    int exact_kv = 0, stable = 0, split = 0;
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
    /* Mixed batch: se altri stream stanno decodificando, il prefill di questo prompt procede a chunk PICCOLI (opzione
     * `serve_prefill_chunk`, default 512) e fra un chunk e l'altro il coordinatore fa avanzare i loro decode (yield). Senza
     * stream attivi resta il chunk di default del motore (throughput massimo). Solo motore MLX (handle = LlmMlxSession). */
    if (g_in_coord_start && m->eng == llmengine_llm_engine_mlx() && __atomic_load_n(&g_active_streams, __ATOMIC_RELAXED) > 0) {
        int pc = 512;
        if (fmlx_engine_opt_get_int("serve_prefill_chunk", &pc) != 0 || pc < 1) pc = 512;
        (void)llm_ai_mlx_session_set_prefill_chunk((llm_ai_mlx_session_handle)s, pc);
    }

    /* Prefill. Con CAP_PREFIX si usa il percorso AUTOMATICO (`append_ids_prefix`): il runner cerca
     * il prefisso salvato che meglio combacia coi token d'ingresso e pre-filla SOLO la coda; se
     * non c'è match utile fa l'append normale. Il match non combacia mai per costruzione della
     * ricerca (token-per-token); -1 è un errore vero e abortisce, come nel server C di GMBasic. */
    can_prefix = (m->eng->caps & LLMMODELIO_CAP_PREFIX) && m->eng->append_ids_prefix;
    /* Modelli con stato ricorrente: il prefisso si salva al confine stabile del prompt (vedi stable_prefix_len), non alla fine. */
    if (can_prefix && m->eng == llmengine_llm_engine_mlx() &&
        llm_ai_mlx_model_prefix_exact((llm_ai_mlx_model_handle)m->model)) {
        const int min_save = opt_int_at_least("prefix_min_save_len", FMLX_PREFIX_MIN_SAVE_LEN, 1);
        exact_kv = 1;
        stable = stable_prefix_len(m, c, ids, n_prompt);
        if (stable >= min_save && stable < n_prompt && m->eng->save_prefix && m->eng->append_ids) split = stable;
    }
    if (can_prefix && split > 0) {
        char key[48];
        int rc = m->eng->append_ids_prefix(m->model, s, ids, split,
                                           opt_int_at_least("prefix_min_match", FMLX_PREFIX_MIN_MATCH, 1));
        if (rc >= 0) {
            snprintf(key, sizeof key, "fmlx-%016llx", (unsigned long long)fnv1a_ids(ids, split));
            (void)m->eng->save_prefix(m->model, key, s);
            if (m->eng->append_ids(s, ids + split, n_prompt - split) != 0) rc = -1;
        }
        if (rc < 0) {
            m->eng->destroy_session(s);
            snprintf(err, errcap, "the engine failed to ingest the prompt");
            return NULL;
        }
        reused = rc > 0;
    } else if (can_prefix) {
        /* GRAMMATICA: la decodifica a maschera parte da `pending_h`, che un riuso di prefisso con coda vuota (prompt identico
         * a uno salvato) NON ripristina. Si ingerisce quindi il prompt meno l'ultimo token via prefisso e l'ultimo con un
         * append, cosi' pending_h e' sempre calcolato. */
        const int gram_req = gram_wanted(c) &&
                             m->eng->generate_id_masked && m->eng->append_ids && n_prompt >= 2;
        int rc = m->eng->append_ids_prefix(m->model, s, ids, gram_req ? n_prompt - 1 : n_prompt,
                                           opt_int_at_least("prefix_min_match",
                                                            FMLX_PREFIX_MIN_MATCH, 1));
        if (rc >= 0 && gram_req && m->eng->append_ids(s, ids + n_prompt - 1, 1) != 0) rc = -1;
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
    if (can_prefix && m->eng->save_prefix && split == 0 &&
        n_prompt >= opt_int_at_least("prefix_min_save_len", FMLX_PREFIX_MIN_SAVE_LEN, 1)) {
        char key[48];
        snprintf(key, sizeof key, "fmlx-%016llx", (unsigned long long)fnv1a_ids(ids, n_prompt));
        (void)m->eng->save_prefix(m->model, key, s);
    }
    if (getenv("FMLX_PREFIX_DEBUG"))
        fprintf(stderr, "[prefix] %s prompt=%d%s stable=%d split=%d\n",
                reused ? "HIT (riuso KV)" : "miss (prefill)", n_prompt, exact_kv ? " (stato ricorrente)" : "", stable, split);

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
    /* Think split: attivo se il prompt apre il blocco OPPURE se il client ha chiesto reasoning
     * (`reasoning_effort` OpenAI / `thinking` Anthropic). Nel secondo caso è il MODELLO a emettere
     * `<think>` (i template Qwen con enable_thinking=true non lo pre-aprono): si parte FUORI dal
     * blocco (`in_think=0`) e lo scanner rileva l'apertura. Mai in modalità logprobs. */
    int reasoning_req = (c && c->has_think && c->think) ? 1 : 0;
    g->want_think = ((think_open || reasoning_req) && !can_lp) ? 1 : 0;
    g->in_think = (think_open && !can_lp) ? 1 : 0;
    g->skip_ws = 0;
    g->think_out = 0;
    gram_setup(g, c, m);
    return g;
}

/* ============================================================================
 * M3 — LOTTO CONTINUO (dynamic batching) a livello ENGINE. Opt-in (`fmlx_engine_batch_request`).
 *
 * Problema: il responder gira su un thread I/O, ma le chiamate al modello devono stare su UN solo
 * thread (single-owner, §8). Soluzione: un COORDINATORE con un thread dedicato che possiede il
 * motore; i thread I/O sottopongono un lavoro e ATTENDONO. Default OFF -> path diretto di prima.
 *  - START (crea sessione + prefill) e FREE (distrugge) sono SERIALI, in ordine;
 *  - STEP: TUTTI gli `advance()` in attesa vengono serviti con UNA `generate_batch` (un token per
 *    sessione) = continuous batching. Logprob/top-k e modelli diversi dal coordinatore restano
 *    seriali (sempre sul thread del coordinatore, quindi single-owner resta vero).
 * ============================================================================ */

#define FMLX_BATCH_MAX 64

typedef struct BJob {
    int kind; /* 0 START, 1 STEP, 2 FREE, 3 KV */
    int done, err, handled;
    char errmsg[160];
    FmlxEngineGen *g; /* STEP/FREE/KV */
    const FmlxEngineModel *m;
    const int32_t *ids;
    int n_prompt, max_tokens, want_logprobs, want_top_logprobs, think_open;
    LlmEngineLlmSampling sp;
    const FmlxApiChat *c;
    FmlxEngineGen *out;
    int used, cap;
    FmlxBatchFn fn; /* kind 4 = CALL generico sul thread del motore (es. load) */
    void *arg, *ptr;
    struct BJob *next;
} BJob;

static struct {
    int on, request, max;
    const FmlxEngineModel *m;
    pthread_t th;
    int running;
    pthread_mutex_t mu;
    pthread_cond_t cv_work, cv_done;
    BJob *ser_head, *ser_tail, *st_head, *st_tail;
} g_bc;
static pthread_mutex_t g_bc_init = PTHREAD_MUTEX_INITIALIZER;

static void *bc_main(void *arg);
static void bc_submit(BJob *j, int step);

static int batch_on(void) { return g_bc.on; }

/* Crea il thread del coordinatore. Indipendente dal modello: il LOAD avverrà SU questo thread
 * (via `fmlx_engine_batch_call`), così possiede device/stream MLX. Idempotente. */
static int bc_start_thread(int max) {
    pthread_mutex_lock(&g_bc_init);
    if (g_bc.on) { pthread_mutex_unlock(&g_bc_init); return 0; }
    if (max < 2) max = 2;
    if (max > FMLX_BATCH_MAX) max = FMLX_BATCH_MAX;
    g_bc.ser_head = g_bc.ser_tail = g_bc.st_head = g_bc.st_tail = NULL;
    g_bc.on = 1; g_bc.max = max; g_bc.running = 1;
    pthread_mutex_init(&g_bc.mu, NULL);
    pthread_cond_init(&g_bc.cv_work, NULL);
    pthread_cond_init(&g_bc.cv_done, NULL);
    {
        /* Pila GRANDE: il LOAD del modello e il render usano molta pila. */
        pthread_attr_t at;
        int rc;
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 48u << 20);
        rc = pthread_create(&g_bc.th, &at, bc_main, NULL);
        pthread_attr_destroy(&at);
        if (rc != 0) {
            g_bc.on = 0;
            pthread_mutex_unlock(&g_bc_init);
            return -1;
        }
    }
    pthread_mutex_unlock(&g_bc_init);
    return 0;
}

int fmlx_engine_batch_enable(FmlxEngineModel *m, int max) { (void)m; return bc_start_thread(max); }

int fmlx_engine_batch_active(void) { return g_bc.on; }

void fmlx_engine_batch_request(int on) {
    if (on) (void)bc_start_thread(g_bc.max ? g_bc.max : 8);
    else fmlx_engine_batch_disable();
}

/* Esegue `fn(arg)` sul thread del coordinatore (dove vive il motore). Ritorna il risultato. */
void *fmlx_engine_batch_call(FmlxBatchFn fn, void *arg) {
    BJob j;
    memset(&j, 0, sizeof j);
    j.kind = 4;
    j.fn = fn;
    j.arg = arg;
    bc_submit(&j, 0);
    return j.ptr;
}

void fmlx_engine_batch_disable(void) {
    pthread_mutex_lock(&g_bc_init);
    if (!g_bc.on) { pthread_mutex_unlock(&g_bc_init); return; }
    pthread_mutex_lock(&g_bc.mu);
    g_bc.running = 0;
    pthread_cond_broadcast(&g_bc.cv_work);
    pthread_cond_broadcast(&g_bc.cv_done);
    pthread_mutex_unlock(&g_bc.mu);
    pthread_join(g_bc.th, NULL);
    g_bc.on = 0;
    pthread_mutex_unlock(&g_bc_init);
}

/* Sottopone un lavoro e attende che il coordinatore l'abbia eseguito. `j` vive nello stack del
 * chiamante fino al ritorno: il coordinatore scrive i risultati nei SUOI campi. */
static void bc_submit(BJob *j, int step) {
    pthread_mutex_lock(&g_bc.mu);
    j->done = 0;
    j->next = NULL;
    if (step) {
        if (g_bc.st_tail) g_bc.st_tail->next = j; else g_bc.st_head = j;
        g_bc.st_tail = j;
    } else {
        if (g_bc.ser_tail) g_bc.ser_tail->next = j; else g_bc.ser_head = j;
        g_bc.ser_tail = j;
    }
    pthread_cond_signal(&g_bc.cv_work);
    while (!j->done) pthread_cond_wait(&g_bc.cv_done, &g_bc.mu);
    pthread_mutex_unlock(&g_bc.mu);
}

static void bc_finish(BJob *j) {
    pthread_mutex_lock(&g_bc.mu);
    j->done = 1;
    pthread_cond_broadcast(&g_bc.cv_done);
    pthread_mutex_unlock(&g_bc.mu);
}

static void bc_run_steps(BJob *head);

/* Chiamata dal prefill (stesso thread del coordinatore) fra un chunk e l'altro: serve i STEP in coda (un token per stream
 * attivo). Non tocca i job seriali (START/FREE): niente START annidati. */
static void bc_yield(void *arg) {
    BJob *st;

    (void)arg;
    pthread_mutex_lock(&g_bc.mu);
    st = g_bc.st_head;
    g_bc.st_head = g_bc.st_tail = NULL;
    pthread_mutex_unlock(&g_bc.mu);
    if (st) bc_run_steps(st);
}

static void bc_run_serial(BJob *j) {
    switch (j->kind) {
    case 0: /* START */
        g_in_coord_start = 1;
        llm_ai_mlx_set_prefill_yield(bc_yield, NULL);   /* i decode degli altri stream avanzano fra i chunk del prefill */
        j->out = start(j->m, j->ids, j->n_prompt, j->max_tokens, &j->sp, j->c, j->want_logprobs,
                       j->want_top_logprobs, j->think_open, j->errmsg, sizeof j->errmsg);
        llm_ai_mlx_set_prefill_yield(NULL, NULL);
        g_in_coord_start = 0;
        j->err = j->out ? 0 : 1;
        if (j->out) { j->out->batch = 1; __atomic_add_fetch(&g_active_streams, 1, __ATOMIC_RELAXED); }
        break;
    case 2: /* FREE */
        if (j->g && j->g->session && j->g->m && j->g->m->eng && j->g->m->eng->destroy_session) {
            j->g->m->eng->destroy_session(j->g->session);
            __atomic_sub_fetch(&g_active_streams, 1, __ATOMIC_RELAXED);
        }
        if (j->g) j->g->session = NULL;
        break;
    case 3: /* KV */
        if (j->g && j->g->session && j->g->m && j->g->m->eng && j->g->m->eng->kv_stats)
            j->err = j->g->m->eng->kv_stats(j->g->session, &j->used, &j->cap);
        else
            j->err = -1;
        break;
    case 4: /* CALL generico (load del modello, ecc.) sul thread del motore */
        j->ptr = j->fn ? j->fn(j->arg) : NULL;
        break;
    }
    bc_finish(j);
}

/* UN passo per TUTTE le gen in attesa: `generate_batch` per le batchabili, `advance` per le altre.
 * Sempre sul thread del coordinatore -> il motore ha UN solo proprietario. */
static void bc_run_steps(BJob *head) {
    LlmEngineLlmSession sess[FMLX_BATCH_MAX];
    int32_t out[FMLX_BATCH_MAX];
    BJob *map[FMLX_BATCH_MAX];
    const FmlxEngineModel *base = NULL;
    int n = 0, rc = 0, i;
    BJob *j;
    char e[160];

    /* Modello base del lotto: la prima gen batchabile (stesso `m` + `generate_batch`). */
    for (j = head; j; j = j->next) {
        if (j->g->want_logprobs || j->g->gram) continue;
        if (j->g->m && j->g->m->eng && j->g->m->eng->generate_batch) { base = j->g->m; break; }
    }
    for (j = head; j; j = j->next) {
        if (j->g->want_logprobs || j->g->gram || !base || j->g->m != base) continue; /* non batchabile: seriale sotto */
        if (adv_no_token(j->g)) { j->handled = 1; continue; }   /* lunghezza: nessun token */
        if (n >= g_bc.max) continue;                            /* oltre il lotto: seriale sotto */
        sess[n] = j->g->session;
        out[n] = -1; /* sentinella: se l'engine SALTA la sessione (pending == eos) resta -1 */
        map[n] = j;
        j->handled = 1;
        n++;
    }
    if (n > 0) {
        rc = base->eng->generate_batch(sess, n, 1, base->eos, out);
        for (i = 0; i < n; i++) {
            e[0] = '\0';
            if (rc != 0) {
                map[i]->err = 1;
                snprintf(map[i]->errmsg, sizeof map[i]->errmsg, "the engine failed while generating");
            } else if (out[i] == -1) {
                /* la sessione non ha prodotto: pending == eos -> l'engine l'ha saltata. */
                (void)gen_apply_token(map[i]->g, 0, 0, e, sizeof e);
            } else if (gen_apply_token(map[i]->g, 1, out[i], e, sizeof e) != 0) {
                map[i]->err = 1;
                snprintf(map[i]->errmsg, sizeof map[i]->errmsg, "%s", e);
            }
        }
    }
    for (j = head; j; j = j->next) { /* le slave (logprob/altro modello/oltre cap) */
        if (j->handled) continue;
        e[0] = '\0';
        if (advance(j->g, e, sizeof e) != 0) {
            j->err = 1;
            snprintf(j->errmsg, sizeof j->errmsg, "%s", e);
        }
        j->handled = 1;
    }
    for (j = head; j; j = j->next) bc_finish(j);
}

static void *bc_main(void *arg) {
    (void)arg;
    llm_ai_mlx_thread_init(); /* questo thread possiede il motore: registra device/stream MLX */
    for (;;) {
        BJob *ser, *st, *j;
        pthread_mutex_lock(&g_bc.mu);
        while (g_bc.running && !g_bc.ser_head && !g_bc.st_head)
            pthread_cond_wait(&g_bc.cv_work, &g_bc.mu);
        if (!g_bc.ser_head && !g_bc.st_head) { pthread_mutex_unlock(&g_bc.mu); break; }
        ser = g_bc.ser_head; g_bc.ser_head = g_bc.ser_tail = NULL;
        st = g_bc.st_head; g_bc.st_head = g_bc.st_tail = NULL;
        pthread_mutex_unlock(&g_bc.mu);
        for (j = ser; j; j = j->next) bc_run_serial(j);
        if (st) bc_run_steps(st);
    }
    return NULL;
}

/* Wrapper: sottopongono al coordinatore e traducono l'esito per i chiamanti di prima. */
static FmlxEngineGen *batch_start(const FmlxEngineModel *m, const int32_t *ids, int n_prompt,
                                  int max_tokens, const LlmEngineLlmSampling *sp,
                                  const FmlxApiChat *c, int wl, int wtl, int think_open, char *err,
                                  size_t errcap) {
    BJob j;
    memset(&j, 0, sizeof j);
    j.kind = 0; j.m = m; j.ids = ids; j.n_prompt = n_prompt; j.max_tokens = max_tokens;
    if (sp) j.sp = *sp;
    j.c = c; j.want_logprobs = wl; j.want_top_logprobs = wtl; j.think_open = think_open;
    bc_submit(&j, 0);
    if (j.err || !j.out) {
        snprintf(err, errcap, "%s", j.errmsg[0] ? j.errmsg : "batch start failed");
        return NULL;
    }
    return j.out;
}

static int batch_advance(FmlxEngineGen *g, char *err, size_t errcap) {
    BJob j;
    memset(&j, 0, sizeof j);
    j.kind = 1; j.g = g;
    bc_submit(&j, 1);
    if (j.err) {
        snprintf(err, errcap, "%s", j.errmsg[0] ? j.errmsg : "the engine failed while generating");
        return -1;
    }
    return 0;
}

static void batch_free_gen(FmlxEngineGen *g) {
    BJob j;
    memset(&j, 0, sizeof j);
    j.kind = 2; j.g = g;
    bc_submit(&j, 0);
}

static int batch_kv(const FmlxEngineGen *g, int *used, int *cap) {
    BJob j;
    memset(&j, 0, sizeof j);
    j.kind = 3; j.g = (FmlxEngineGen *)g;
    bc_submit(&j, 0);
    if (j.err == 0) { *used = j.used; *cap = j.cap; return 0; }
    return -1;
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
    ids = tokenize_chat(m->tok, prompt.p ? prompt.p : "", &n, err, errcap);
    fmlx_sb_free(&prompt);
    if (!ids) return NULL;
    /* Default = parametro `maxtokens` del registro; `--max-tokens` (esplicito) vince. Nessuna env. */
    {
        int def = FMLX_ENGINE_MAX_NEW;
        if (fmlx_engine_opt_get_int("maxtokens", &def) != 0 || def < 1) def = FMLX_ENGINE_MAX_NEW;
        max_tokens = c->has_max_tokens ? (int)c->max_tokens : def;
    }
    sampling_of(c, &sp);
    if (batch_on())
        g = batch_start(m, ids, n, max_tokens, &sp, c, c->want_logprobs, c->want_top_logprobs,
                        think_open, err, errcap);
    else
        g = start(m, ids, n, max_tokens, &sp, c, c->want_logprobs, c->want_top_logprobs,
                  think_open, err, errcap);
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
    if (batch_on())
        g = batch_start(m, ids, n, max_tokens, &sp, c, c ? c->want_logprobs : 0,
                        c ? c->want_top_logprobs : 0, prompt_opens_think(prompt), err, errcap);
    else
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
    n = llmtoken_encode_guarded(m->tok, prompt.p ? prompt.p : "", NULL, 0);
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
            int rc = g->batch ? batch_advance(g, e, sizeof e) : advance(g, e, sizeof e);
            if (rc != 0) {
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
    if (g->batch) return batch_kv(g, out_used, out_cap);
    return g->m->eng->kv_stats(g->session, out_used, out_cap);
}

void fmlx_engine_gen_free(FmlxEngineGen *g) {
    if (!g) return;
    if (g->batch)
        batch_free_gen(g); /* la distruzione della sessione avviene sul thread del motore */
    else if (g->session && g->m && g->m->eng && g->m->eng->destroy_session)
        g->m->eng->destroy_session(g->session);
    free(g->ids);
    free(g->full);
    free(g->text);
    free(g->logprobs);
    free(g->tok_end);
    free(g->top_ids);
    free(g->top_lp);
    if (g->gram) llm_grammar_free((LlmGrammar *)g->gram);
    free(g->gmask);
    if (g->tools) fmlx_toolstream_free(g->tools);
    free(g);
}
