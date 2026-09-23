/* fmlx_api.c — il pezzo del layer ② che NON è dialetto: catalogo errori, rotta, richiesta
 * normalizzata, accumulatore, ponte `arguments` ↔ `input`.
 *
 * Qui stanno le decisioni che valgono per entrambi i formati su filo. Spostarle in uno dei
 * due adapter è il modo in cui i due smettono di dire la stessa cosa sullo stesso evento.
 */
#include "fmlx/fmlx_api.h"

#include <stdio.h>
#include <string.h>

/* ---- Catalogo ---------------------------------------------------------------- */

typedef struct {
    int status;
    const char *code;   /* il nome del piano §11.5: è il campo `code`, non il `type` */
    const char *oai;    /* type OpenAI  */
    const char *ant;    /* type Anthropic */
} ErrRow;

/* `code` = i nomi esatti di piano:515-517. Il `type` è il lessico del dialetto: due
 * colonne perché i due API non usano le stesse parole, e il piano fissa la FORMA del corpo
 * (piano:518) non l'elenco delle stringhe. Le tre che il piano non nomina (`not_found`
 * sulla rotta, 409, 502) sono scelte qui e solo qui. */
static const ErrRow ERR[] = {
    /* status code              OpenAI                     Anthropic */
    {200, "ok",                  NULL,                       NULL},
    {400, "invalid_request",     "invalid_request_error",   "invalid_request_error"},
    {401, "unauthorized",        "authentication_error",    "authentication_error"},
    {404, "model_not_found",     "model_not_found",         "not_found_error"},
    {404, "unknown_route",       "not_found_error",         "not_found_error"},
    {400, "context_length_exceeded", "context_length_exceeded", "invalid_request_error"},
    {429, "resource_exhausted",  "rate_limit_exceeded",     "rate_limit_error"},
    {409, "memory_guard_refused", "memory_guard_refused",   "overloaded_error"},
    {502, "upstream_hub_error",  "upstream_hub_error",      "api_error"},
    {500, "internal",            "internal_server_error",   "api_error"},
    /* 501 NON è nel piano §11.5 ma il piano:494 lo esige: "`count_tokens`... se non può
     * essere esatto, si ritorna un errore `not_supported`, non un numero falso". Un numero
     * stimato avrebbe il suo codice, e il codice è questo. */
    {501, "not_supported",       "not_supported_error",     "api_error"}
};

#define ERR_N ((int)(sizeof ERR / sizeof ERR[0]))

static const ErrRow *row(FmlxApiErr e) {
    if ((int)e < 0 || (int)e >= ERR_N) return &ERR[FMLX_API_INTERNAL];
    return &ERR[(int)e];
}

const char *fmlx_api_err_str(FmlxApiErr e) { return row(e)->code; }
int fmlx_api_http_status(FmlxApiErr e) { return row(e)->status; }
const char *fmlx_api_openai_type(FmlxApiErr e) { return row(e)->oai; }
const char *fmlx_api_anthropic_type(FmlxApiErr e) { return row(e)->ant; }

FmlxApiErr fmlx_api_render_error_openai(const FmlxApiErrCtx *e, FmlxJsonW *w) {
    if (!e || !w) return FMLX_API_INTERNAL;
    FmlxApiErr bad = FMLX_API_OK;
    if (fmlx_jsonw_obj_begin(w)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "error")) bad = FMLX_API_INTERNAL;
    if (fmlx_jsonw_obj_begin(w)) bad = FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "message")) bad = FMLX_API_INTERNAL;
    if (fmlx_jsonw_cstr(w, e->message ? e->message : fmlx_api_err_str(e->err))) bad = FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "type")) bad = FMLX_API_INTERNAL;
    if (fmlx_jsonw_cstr(w, fmlx_api_openai_type(e->err))) bad = FMLX_API_INTERNAL;
    if (e->code) {
        if (fmlx_jsonw_key(w, "code")) bad = FMLX_API_INTERNAL;
        if (fmlx_jsonw_cstr(w, e->code)) bad = FMLX_API_INTERNAL;
    }
    if (e->param) {
        if (fmlx_jsonw_key(w, "param")) bad = FMLX_API_INTERNAL;
        if (fmlx_jsonw_cstr(w, e->param)) bad = FMLX_API_INTERNAL;
    }
    if (e->err == FMLX_API_CONTEXT_LENGTH) {
        /* piano:516: "con tokens_requested/tokens_available" — i numeri, non un messaggio
         * che dice "troppo lungo". Un client con i numeri cambia richiesta; con il
         * messaggio cambia modello a caso. */
        if (fmlx_jsonw_key(w, "tokens_requested")) bad = FMLX_API_INTERNAL;
        if (fmlx_jsonw_u64(w, e->tok_requested)) bad = FMLX_API_INTERNAL;
        if (fmlx_jsonw_key(w, "tokens_available")) bad = FMLX_API_INTERNAL;
        if (fmlx_jsonw_u64(w, e->tok_available)) bad = FMLX_API_INTERNAL;
    }
    if (e->err == FMLX_API_MEMORY_GUARD) {
        /* piano:517: "deve dire i byte e il suggerimento --stream-cache-experts". */
        if (fmlx_jsonw_key(w, "required_bytes")) bad = FMLX_API_INTERNAL;
        if (fmlx_jsonw_u64(w, e->need_bytes)) bad = FMLX_API_INTERNAL;
        if (fmlx_jsonw_key(w, "hint")) bad = FMLX_API_INTERNAL;
        if (fmlx_jsonw_cstr(w, "run with --stream-cache-experts")) bad = FMLX_API_INTERNAL;
    }
    if (fmlx_jsonw_obj_end(w)) bad = FMLX_API_INTERNAL;
    if (fmlx_jsonw_obj_end(w)) bad = FMLX_API_INTERNAL;
    return bad;
}

FmlxApiErr fmlx_api_render_error_anthropic(const FmlxApiErrCtx *e, FmlxJsonW *w) {
    if (!e || !w) return FMLX_API_INTERNAL;
    FmlxApiErr bad = FMLX_API_OK;
    if (fmlx_jsonw_obj_begin(w)) return FMLX_API_INTERNAL;
    /* `type` in RADICE e poi `error{type,message}`: è la forma di piano:518, e lo `type`
     * esterno vale "questo messaggio è un errore", quello interno dice quale. */
    if (fmlx_jsonw_key(w, "type")) bad = FMLX_API_INTERNAL;
    if (fmlx_jsonw_cstr(w, "error")) bad = FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "error")) bad = FMLX_API_INTERNAL;
    if (fmlx_jsonw_obj_begin(w)) bad = FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "type")) bad = FMLX_API_INTERNAL;
    if (fmlx_jsonw_cstr(w, fmlx_api_anthropic_type(e->err))) bad = FMLX_API_INTERNAL;
    if (fmlx_jsonw_key(w, "message")) bad = FMLX_API_INTERNAL;
    if (fmlx_jsonw_cstr(w, e->message ? e->message : fmlx_api_err_str(e->err))) bad = FMLX_API_INTERNAL;
    if (e->err == FMLX_API_CONTEXT_LENGTH) {
        if (fmlx_jsonw_key(w, "tokens_requested")) bad = FMLX_API_INTERNAL;
        if (fmlx_jsonw_u64(w, e->tok_requested)) bad = FMLX_API_INTERNAL;
        if (fmlx_jsonw_key(w, "tokens_available")) bad = FMLX_API_INTERNAL;
        if (fmlx_jsonw_u64(w, e->tok_available)) bad = FMLX_API_INTERNAL;
    }
    if (e->err == FMLX_API_MEMORY_GUARD) {
        if (fmlx_jsonw_key(w, "required_bytes")) bad = FMLX_API_INTERNAL;
        if (fmlx_jsonw_u64(w, e->need_bytes)) bad = FMLX_API_INTERNAL;
        if (fmlx_jsonw_key(w, "hint")) bad = FMLX_API_INTERNAL;
        if (fmlx_jsonw_cstr(w, "run with --stream-cache-experts")) bad = FMLX_API_INTERNAL;
    }
    if (fmlx_jsonw_obj_end(w)) bad = FMLX_API_INTERNAL;
    if (fmlx_jsonw_obj_end(w)) bad = FMLX_API_INTERNAL;
    return bad;
}

/* Lo spazio riservato davanti al corpo: la testata la scrive il writer HTTP da offset 0 e
 * il corpo va poi spostato dopo di lei. 512 byte coprono status-line + Content-Type +
 * Content-Length/TE + Retry-After + CRLFCRLF con margine; se non bastassero, l'errore è
 * BUF_SMALL e NON una risposta con i byte del corpo dentro l'header. */
#define HEAD_RESERVE 512u

FmlxApiErr fmlx_api_error_response(const FmlxApiErrCtx *e, int anthropic, char *mem, size_t cap,
                                   size_t *out_len) {
    if (!e || !mem || !out_len || cap < HEAD_RESERVE + 32u) return FMLX_API_INTERNAL;
    *out_len = 0;

    FmlxJsonW jw;
    fmlx_jsonw_init(&jw, mem + HEAD_RESERVE, cap - HEAD_RESERVE, cap - HEAD_RESERVE);
    FmlxApiErr bad = anthropic ? fmlx_api_render_error_anthropic(e, &jw)
                               : fmlx_api_render_error_openai(e, &jw);
    size_t blen = 0;
    if (!bad) bad = fmlx_jsonw_finish(&jw, &blen) ? FMLX_API_INTERNAL : FMLX_API_OK;
    fmlx_jsonw_free(&jw);
    if (bad) return bad;

    FmlxHttpRes w;
    fmlx_http_res_init(&w, mem, cap);
    char extra[64];
    const char *xh = NULL;
    if (e->err == FMLX_API_RESOURCE_EXHAUSTED && e->retry_after > 0) {
        /* Retry-After è un header, non un campo del corpo: chi si sveglia tra 30 secondi
         * deve lo svegliarsi tra 30 secondi, non "leggere il consiglio nel JSON". */
        snprintf(extra, sizeof extra, "Retry-After: %d", e->retry_after);
        xh = extra;
    }
    int st = fmlx_api_http_status(e->err);
    if (fmlx_http_write_head(&w, st, "application/json", blen, xh) != FMLX_HTTP_OK) {
        return FMLX_API_INTERNAL;
    }
    if (w.len > HEAD_RESERVE) return FMLX_API_INTERNAL;
    memmove(mem + w.len, mem + HEAD_RESERVE, blen);
    w.len += blen;
    *out_len = w.len;
    return FMLX_API_OK;
}

/* ---- Finish ------------------------------------------------------------------ */

const char *fmlx_api_finish_openai(FmlxApiFinish f) {
    switch (f) {
    case FMLX_API_FINISH_STOP: return "stop";
    case FMLX_API_FINISH_LENGTH: return "length";
    case FMLX_API_FINISH_TOOL: return "tool_calls";
    case FMLX_API_FINISH_STOPSEQ: return "stop";
    }
    return "stop";
}

const char *fmlx_api_finish_anthropic(FmlxApiFinish f) {
    switch (f) {
    case FMLX_API_FINISH_STOP: return "end_turn";
    case FMLX_API_FINISH_LENGTH: return "max_tokens";
    case FMLX_API_FINISH_TOOL: return "tool_use";
    case FMLX_API_FINISH_STOPSEQ: return "stop_sequence";
    }
    return "end_turn";
}

/* `finish_reason:"stop"` su OpenAI copre DUE cause (fine turno e stop sequence) che in
 * Anthropic sono `end_turn` e `stop_sequence`: la mappa è molti-a-uno in uscita, e quindi
 * `from_openai` non può invertirla — "stop" torna sempre STOP, e chi ha una stop sequence
 * deve portarsi dietro `stop_sequence`, non dedurla dal finish. */
int fmlx_api_finish_from_openai(const char *s, FmlxApiFinish *out) {
    if (!s || !out) return 0;
    if (!strcmp(s, "stop")) *out = FMLX_API_FINISH_STOP;
    else if (!strcmp(s, "length")) *out = FMLX_API_FINISH_LENGTH;
    else if (!strcmp(s, "tool_calls")) *out = FMLX_API_FINISH_TOOL;
    else if (!strcmp(s, "function_call")) return 0; /* deprecato: non è un nostro passato */
    else return 0;
    return 1;
}

/* ---- Richiesta --------------------------------------------------------------- */

void fmlx_api_chat_init(FmlxApiChat *c) {
    if (!c) return;
    memset(c, 0, sizeof *c);
}

int fmlx_api_ignore(FmlxApiChat *c, FmlxSlice key) {
    if (!c) return 0;
    if (c->n_ignored >= FMLX_API_MAX_IGNORED) {
        c->overflow = 1; /* NON "il ventiquattresimo campo non conta" */
        return 0;
    }
    if (key.n >= FMLX_API_MAX_NAME) {
        c->overflow = 1;
        return 0;
    }
    c->ignored[c->n_ignored++] = key;
    return 1;
}

int fmlx_api_ignore_copy(FmlxApiChat *c, const char *s) {
    if (!c || !s) return 0;
    size_t n = strlen(s);
    if (c->n_ignored >= FMLX_API_MAX_IGNORED || n >= FMLX_API_MAX_NAME) {
        c->overflow = 1;
        return 0;
    }
    char *pool = c->ignored_pool[c->n_ignored];
    memcpy(pool, s, n + 1);
    c->ignored[c->n_ignored++] = (FmlxSlice){pool, (uint32_t)n};
    return 1;
}

int fmlx_api_warn(FmlxApiChat *c, const char *msg) {
    if (!c || !msg) return 0;
    if (c->n_warn >= FMLX_API_MAX_WARN) {
        c->overflow = 1;
        return 0;
    }
    c->warn[c->n_warn++] = msg;
    return 1;
}

/* ---- Il testo di sistema ------------------------------------------------------ */

#define SYS_MAX_PIECES 16u

/* Raccoglie le fette di testo di un nodo che è una stringa, una lista di stringhe o una
 * lista di blocchi `{type:"text",...}`. Ritorna 1 e scrive in `*np` il numero di pezzi (che
 * possono anche essere ZERO: un array vuoto è una forma legale), 0 se il nodo ha una forma
 * che testo non è o se i pezzi sono più di `SYS_MAX_PIECES`. */
static int collect_text(const FmlxJsonValue *node, FmlxSlice *piec, size_t cap_n, size_t *np) {
    if (!node) return 1;
    if (node->type == FMLX_JSON_STR) {
        const char *p = fmlx_json_str(node);
        size_t n = fmlx_json_str_len(node);
        if (!p) return 0;
        if (*np >= cap_n) return 0;
        if (n > 0xffffffffu) return 0;
        piec[(*np)++] = (FmlxSlice){p, (uint32_t)n};
        return 1;
    }
    if (node->type != FMLX_JSON_ARR) return 0;
    for (size_t i = 0; i < fmlx_json_arr_n(node); i++) {
        const FmlxJsonValue *b = fmlx_json_arr_at(node, i);
        if (*np >= cap_n) return 0;
        if (b && b->type == FMLX_JSON_STR) {
            if (!collect_text(b, piec, cap_n, np)) return 0;
        } else if (!collect_text(fmlx_json_obj_get(b, "text"), piec, cap_n, np)) {
            return 0;
        }
    }
    return 1;
}

FmlxApiErr fmlx_api_join_system(const FmlxApiChat *c, char *mem, size_t cap, FmlxSlice *out) {
    if (!c || !out) return FMLX_API_INTERNAL;
    out->p = NULL;
    out->n = 0;

    /* Il campo `system` (Anthropic) viene PRIMA dei messaggi di ruolo: dove il client
     * dichiara il sistema con un campo, quella è la sua dichiarazione. */
    const FmlxJsonValue *src[1 + FMLX_API_MAX_SYSTEM];
    size_t nsrc = 0;
    if (c->system) src[nsrc++] = c->system;
    for (uint32_t i = 0; i < c->n_sys_msgs; i++)
        if (c->sys_msgs[i]) src[nsrc++] = c->sys_msgs[i];
    if (nsrc == 0) return FMLX_API_OK; /* nessun sistema: fetta vuota, non stringa vuota */

    FmlxSlice piec[SYS_MAX_PIECES];
    size_t np = 0;
    size_t total = 0;
    for (size_t i = 0; i < nsrc; i++) {
        size_t before = np;
        if (!collect_text(src[i], piec, SYS_MAX_PIECES, &np)) return FMLX_API_INTERNAL;
        for (size_t k = before; k < np; k++) total += piec[k].n;
    }
    if (np == 0) return FMLX_API_OK;

    /* Un pezzo solo e nessuna giunzione da fare: la fetta resta nel DOM, la memoria del
     * chiamante non viene toccata. È il caso del 99% delle richieste e copiarlo sarebbe il
     * costo che nessuno misura. */
    if (np == 1) {
        *out = piec[0];
        return FMLX_API_OK;
    }
    if (total + 2 * (np - 1) > 0xffffffffu) return FMLX_API_INTERNAL;
    if (!mem || total + 2 * (np - 1) > cap) return FMLX_API_INTERNAL;

    size_t o = 0;
    for (size_t i = 0; i < np; i++) {
        if (i) {
            memcpy(mem + o, "\n\n", 2);
            o += 2;
        }
        if (piec[i].n) memcpy(mem + o, piec[i].p, piec[i].n);
        o += piec[i].n;
    }
    out->p = mem;
    out->n = (uint32_t)o;
    return FMLX_API_OK;
}

/* ---- Accumulatore ------------------------------------------------------------ */

void fmlx_api_acc_init(FmlxApiAcc *a, char *text, size_t text_cap, char *think, size_t think_cap,
                       char *args, size_t args_cap_each) {
    if (!a) return;
    memset(a, 0, sizeof *a);
    a->text = text;
    a->text_cap = text ? text_cap : 0;
    a->think = think;
    a->think_cap = think ? think_cap : 0;
    for (uint32_t i = 0; i < FMLX_API_MAX_TOOLCALLS; i++) {
        a->tool[i].args = args ? args + (size_t)i * args_cap_each : NULL;
        a->tool[i].args_cap = args ? args_cap_each : 0;
    }
}

/* Aggiunge byte in coda. Se non entrano TUTTI, non ne entra NESSUNO: un token tagliato a
 * metà è un carattere spezzato in UTF-8, e la risposta "quasi completa" è quella che un
 * client manda a valle senza accorgersi. */
static int append_buf(char *dst, size_t cap, size_t *len, const char *src, size_t n) {
    if (!dst || !len) return n == 0;
    if (*len + n > cap) return 0;
    if (n) memcpy(dst + *len, src, n);
    *len += n;
    return 1;
}

/* Gli id e i nomi dei tool restano C string: arrivano dal motore (già testuali) e finiscono
 * in un campo JSON. Copiarli con la loro lunghezza sarebbe un secondo modo di
 * rappresentarli, e `copy_cstr` ritorna 0 se NON entrano: un nome tagliato a metà è un tool
 * che il client non può eseguire, quindi è overflow, non truncation. */
static int copy_cstr(const char *src, char *dst, size_t cap) {
    size_t n = src ? strlen(src) : 0;
    if (n + 1 > cap) return 0;
    if (n) memcpy(dst, src, n);
    dst[n] = '\0';
    return 1;
}

FmlxApiErr fmlx_api_acc_push(FmlxApiAcc *a, const FmlxApiEvent *ev) {
    if (!a || !ev) return FMLX_API_INTERNAL;
    if (a->overflow) return FMLX_API_INTERNAL; /* già non inviabile: lo stato non migliora */

    int ok = 1;
    switch (ev->ev) {
    case FMLX_API_EV_TEXT: ok = append_buf(a->text, a->text_cap, &a->text_len, ev->text, ev->n); break;
    case FMLX_API_EV_THINK:
        /* piano:508: "se il modello non ha thinking: campo assente, mai stringa vuota".
         * Un evento THINK da 0 byte NON apre il blocco: resta assente. */
        if (ev->n) ok = append_buf(a->think, a->think_cap, &a->think_len, ev->text, ev->n);
        break;
    case FMLX_API_EV_TOOL_NAME: {
        /* `ntool` è int (i loop degli adapter contano blocchi, non byte), i limiti sono
         * unsigned: il cast è sul COSTANTE, non sul contatore, o un confronto firm-to-
         * unsigned nasconderebbe un `-1` invece di scoprirlo. */
        if (a->ntool >= (int)FMLX_API_MAX_TOOLCALLS) { ok = 0; break; }
        int i = a->ntool;
        a->tool[i].args_len = 0;
        a->tool[i].closed = 0;
        ok = copy_cstr(ev->tool_name, a->tool[i].name, FMLX_API_MAX_NAME);
        if (ok) ok = copy_cstr(ev->tool_id, a->tool[i].id, FMLX_API_MAX_NAME);
        if (ok) a->ntool++;
        break;
    }
    case FMLX_API_EV_TOOL_ARGS: {
        /* "buffer dentro ②, flush a blocco chiuso" (piano:506). Un args frame senza un tool
         * aperto è il motore che viola il confine: errore, non "lo scarto". */
        if (a->ntool == 0) { ok = 0; break; }
        int i = a->ntool - 1;
        if (a->tool[i].closed) { ok = 0; break; }
        ok = append_buf(a->tool[i].args, a->tool[i].args_cap, &a->tool[i].args_len, ev->text, ev->n);
        break;
    }
    case FMLX_API_EV_DONE:
        a->finish = ev->finish;
        a->have_finish = 1;
        a->prompt_tokens = ev->prompt_tokens;
        a->completion_tokens = ev->completion_tokens;
        a->cached_tokens = ev->cached_tokens;
        ok = copy_cstr(ev->stop_sequence, a->stop_seq, sizeof a->stop_seq);
        /* Un DONE senza stop_sequence è normale: `copy_cstr(NULL,...)` scrive "" e non
         * pone overflow. Chiudere qui il/gli ultimi tool: da ora gli argomenti sono
         * definitivi e `acc_tool_ok` può giudicarli. */
        for (int i = 0; i < a->ntool; i++) a->tool[i].closed = 1;
        break;
    }
    if (!ok) {
        a->overflow = 1;
        return FMLX_API_INTERNAL;
    }
    return FMLX_API_OK;
}

int fmlx_api_acc_tool_ok(const FmlxApiAcc *a, int i) {
    if (!a || i < 0 || i >= a->ntool || a->overflow) return 0;
    const char *s = a->tool[i].args;
    size_t n = a->tool[i].args_len;
    if (!s) return 0;
    if (n == 0) return 1; /* nessun argomento = `{}`, che è un oggetto valido */
    FmlxJsonDoc d;
    fmlx_json_doc_init(&d);
    FmlxJsonStatus st = fmlx_json_parse(s, n, 0, &d);
    int good = 0;
    if (st == FMLX_JSON_OK) {
        const FmlxJsonValue *r = fmlx_json_root(&d);
        /* `input` Anthropic È un oggetto (piano:505). `"42"` è JSON valido e non è un
         * oggetto: emetterlo significherebbe mandare un blocco che il client non può
         * passare alla funzione che ha chiesto. */
        good = r && r->type == FMLX_JSON_OBJ;
    }
    fmlx_json_doc_free(&d);
    return good;
}

FmlxApiErr fmlx_api_tool_input(FmlxJsonW *w, const char *args, size_t len) {
    if (!w) return FMLX_API_INTERNAL;
    if (len == 0) {
        if (fmlx_jsonw_obj_begin(w) || fmlx_jsonw_obj_end(w)) return FMLX_API_INTERNAL;
        return FMLX_API_OK;
    }
    /* Secondo parse, deliberato: `acc_tool_ok` ha già giudicato la stringa e il DOM di lì
     * non sopravvive alla chiamata. Tenere il DOM sarebbe un campo in più nell'accumulatore
     * per ogni tool (che è memoria), rifare il parse costa CPU su poche centinaia di byte.
     * Chi cambia una delle due funzioni deve cambiare anche l'altra: sono un contratto. */
    FmlxJsonDoc d;
    fmlx_json_doc_init(&d);
    if (fmlx_json_parse(args, len, 0, &d) != FMLX_JSON_OK) {
        fmlx_json_doc_free(&d);
        return FMLX_API_INTERNAL;
    }
    FmlxApiErr bad = fmlx_json_render(fmlx_json_root(&d), w) ? FMLX_API_INTERNAL : FMLX_API_OK;
    fmlx_json_doc_free(&d);
    return bad;
}

/* ---- Rotta ------------------------------------------------------------------- */

/* Confronto del path con la query ignorata: `/v1/models?x=1` È la lista dei modelli. Il
 * target è già stato validato dal codec HTTP (niente spazi, niente CTRL), qui si guarda
 * solo se è la rotta che conosciamo. */
static int path_eq(FmlxSlice t, const char *lit) {
    size_t n = strlen(lit);
    if (t.n < n) return 0;
    if (memcmp(t.p, lit, n) != 0) return 0;
    return t.n == n || t.p[n] == '?';
}

static int method_is(FmlxSlice m, const char *lit) {
    size_t n = strlen(lit);
    return m.n == n && memcmp(m.p, lit, n) == 0;
}

FmlxApiErr fmlx_api_route(FmlxSlice method, FmlxSlice target, FmlxApiAction *act) {
    if (!act) return FMLX_API_INTERNAL;
    *act = FMLX_API_ACT_NONE;
    int get = method_is(method, "GET"), post = method_is(method, "POST");

    struct { const char *path; int is_post; FmlxApiAction act; } T[] = {
        {"/v1/models", 0, FMLX_API_ACT_MODELS},
        {"/v1/models/status", 0, FMLX_API_ACT_MODELS_STATUS},
        {"/health", 0, FMLX_API_ACT_HEALTH},
        {"/metrics", 0, FMLX_API_ACT_METRICS},
        {"/v1/models/load", 1, FMLX_API_ACT_LOAD},
        {"/v1/models/unload", 1, FMLX_API_ACT_UNLOAD},
        {"/v1/chat/completions", 1, FMLX_API_ACT_CHAT},
        {"/v1/completions", 1, FMLX_API_ACT_COMPLETIONS},
        {"/v1/responses", 1, FMLX_API_ACT_RESPONSES},
        {"/v1/messages/count_tokens", 1, FMLX_API_ACT_COUNT_TOKENS},
        {"/v1/messages", 1, FMLX_API_ACT_MESSAGES},
        {"/v1/embeddings", 1, FMLX_API_ACT_EMBEDDINGS},
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        if (!path_eq(target, T[i].path)) continue;
        /* L'ordine del table conta: `/v1/messages/count_tokens` viene PRIMA di
         * `/v1/messages`, perché `path_eq` tollera il suffisso `?` e non il `/`. */
        if (T[i].is_post ? !post : !get) return FMLX_API_INVALID_REQUEST;
        *act = T[i].act;
        return FMLX_API_OK;
    }
    return FMLX_API_NOT_FOUND;
}

int fmlx_api_is_anthropic(FmlxApiAction act) {
    return act == FMLX_API_ACT_MESSAGES || act == FMLX_API_ACT_COUNT_TOKENS;
}

/* ---- Condiviso --------------------------------------------------------------- */

int fmlx_api_streq(FmlxSlice s, const char *lit) {
    size_t n = strlen(lit);
    return s.n == n && memcmp(s.p, lit, n) == 0;
}

FmlxHttpStatus fmlx_api_sse_head(FmlxHttpRes *w) {
    if (!w) return FMLX_HTTP_ERR_BAD_ARG;
    /* Chunked: non esiste un Content-Length per uno stream che finisce quando il motore
     * dice stop. `Connection: close` non c'è: la chiusura della connessione NON è il
     * segnale di fine stream, `data: [DONE]` lo è. */
    w->chunked = 1;
    return fmlx_http_write_head(w, 200, "text/event-stream", 0,
                                "Cache-Control: no-cache\r\nX-Accel-Buffering: no");
}

FmlxApiErr fmlx_api_write_ignored(FmlxJsonW *w, const FmlxApiChat *c) {
    if (!w || !c) return FMLX_API_INTERNAL;
    if (c->n_ignored == 0) return FMLX_API_OK; /* assente, non vuoto: vedi header */
    if (fmlx_jsonw_key(w, "ignored_params")) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_arr_begin(w)) return FMLX_API_INTERNAL;
    for (uint32_t i = 0; i < c->n_ignored; i++)
        if (fmlx_jsonw_str(w, c->ignored[i].p, c->ignored[i].n)) return FMLX_API_INTERNAL;
    if (fmlx_jsonw_arr_end(w)) return FMLX_API_INTERNAL;
    return FMLX_API_OK;
}
