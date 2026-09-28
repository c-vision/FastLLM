/* fmlx_tools.c — implementazione di `fmlx_tools.h`. PURO: dipende solo da fmlx_json e dalla
 * libc, quindi si prova senza motore, senza rete e senza MLX.
 *
 * IL PUNTO DELICATO È IL CONFINE DEI BLOCCHI. Un marcatore può essere spezzato fra due token
 * (`<tool_` + `call>`), quindi lo stream NON emette il testo che potrebbe essere un prefisso
 * di marcatore: lo trattiene. Le due funzioni che fanno questo sono `holdback()` (quanta coda
 * tenere) e lo scanner `scan_balanced()` (quanto misura un oggetto JSON, rispettando stringhe
 * e escape). Tutto il resto è contabilità.
 */
#include "fmlx/fmlx_tools.h"

#include "fmlx/fmlx_json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- byte buffer ---- */

typedef struct {
    char *p;
    size_t len, cap;
} Buf;

static int buf_add(Buf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n + 1) cap *= 2;
        char *np = realloc(b->p, cap);
        if (!np) return -1;
        b->p = np;
        b->cap = cap;
    }
    if (n) memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
    return 0;
}

static void buf_free(Buf *b) {
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

/* ---- chiamate ---- */

void fmlx_tools_init(FmlxToolCalls *tc) {
    if (tc) memset(tc, 0, sizeof *tc);
}

void fmlx_tools_free(FmlxToolCalls *tc) {
    if (!tc) return;
    for (int i = 0; i < tc->n; i++) free(tc->call[i].args);
    memset(tc, 0, sizeof *tc);
}

static char *dup_bytes(const char *s, size_t n) {
    char *p = malloc(n + 1);
    if (!p) return NULL;
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/* Renderizza un valore del DOM nella SUA forma JSON (compatto), malloc. */
static char *render_value(const FmlxJsonValue *v, size_t *out_len) {
    FmlxJsonW w;
    size_t n = 0;
    char *p;

    fmlx_jsonw_init(&w, NULL, 0, 1u << 20);
    if (fmlx_json_render(v, &w) != FMLX_JSON_OK || fmlx_jsonw_finish(&w, &n) != FMLX_JSON_OK) {
        fmlx_jsonw_free(&w);
        return NULL;
    }
    p = dup_bytes(w.buf ? w.buf : "{}", n);
    fmlx_jsonw_free(&w);
    if (p && out_len) *out_len = n;
    return p;
}

/* Aggiunge una chiamata. `args` è il valore JSON degli argomenti (NULL -> "{}"). Se il nome
 * è vuoto o troppo lungo, NON si aggiunge. Ritorna 1 se aggiunta. */
static int add_call(FmlxToolCalls *out, const char *name, size_t name_n, const FmlxJsonValue *args) {
    FmlxToolCall *c;
    size_t alen = 0;

    if (!out || out->n >= (int)FMLX_TOOL_MAX_CALLS) return 0;
    if (!name || name_n == 0 || name_n >= FMLX_TOOL_MAX_NAME) return 0;
    c = &out->call[out->n];
    memset(c, 0, sizeof *c);
    memcpy(c->name, name, name_n);
    c->name[name_n] = '\0';
    if (!args) {
        c->args = dup_bytes("{}", 2);
        alen = 2;
    } else if (args->type == FMLX_JSON_STR) {
        const char *s = fmlx_json_str(args);
        size_t n = fmlx_json_str_len(args);
        c->args = dup_bytes(s ? s : "{}", s ? n : 2);
        alen = s ? n : 2;
    } else {
        c->args = render_value(args, &alen);
    }
    if (!c->args) return 0;
    c->args_len = alen;
    out->n++;
    return 1;
}

/* ---- scanner JSON ---- */

static int skip_ws(const char *s, size_t n, size_t *i) {
    size_t k = *i;
    while (k < n && (s[k] == ' ' || s[k] == '\t' || s[k] == '\n' || s[k] == '\r')) k++;
    *i = k;
    return k < n;
}

/* Dato un valore JSON che INIZIA a `s[0]`, ne ritorna la lunghezza in `*len`. 0 se incompleto
 * o non valido sintatticamente. Rispetta stringhe ed escape, e la profondità di oggetti/array. */
static int scan_balanced(const char *s, size_t n, size_t *len) {
    size_t i = 0;
    int depth = 0, instr = 0, esc = 0;

    if (n == 0) return 0;
    if (s[0] != '{' && s[0] != '[') {
        if (s[0] == '"') {
            for (i = 0; i < n; i++) {
                if (esc) { esc = 0; continue; }
                if (s[i] == '\\') { esc = 1; continue; }
                if (s[i] == '"') { *len = i + 1; return 1; }
            }
            return 0;
        }
        /* scalare: fino a un delimitatore */
        for (i = 0; i < n; i++) {
            char c = s[i];
            if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\t' || c == '\n' ||
                c == '\r') {
                *len = i;
                return i > 0;
            }
        }
        *len = n;
        return n > 0;
    }
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (instr) {
            if (esc) esc = 0;
            else if (c == '\\') esc = 1;
            else if (c == '"') instr = 0;
            continue;
        }
        if (c == '"') instr = 1;
        else if (c == '{' || c == '[') depth++;
        else if (c == '}' || c == ']') {
            depth--;
            if (depth == 0) { *len = i + 1; return 1; }
            if (depth < 0) return 0;
        }
    }
    return 0;
}

static void trim_span(const char **p, size_t *n) {
    const char *s = *p;
    size_t len = *n;
    while (len && (unsigned char)s[0] <= ' ') { s++; len--; }
    while (len && (unsigned char)s[len - 1] <= ' ') len--;
    *p = s;
    *n = len;
}

/* Costruisce un oggetto JSON `{k:v,...}` dagli argomenti di una forma XML
 * `<parameter=KEY>VALUE</parameter>`. I valori restano stringhe: senza schema non c'è modo di
 * sapere se `42` è un numero o un id, e indovinare sarebbe peggio che essere conservativi. */
static char *params_xml_to_json(const char *s, size_t n, size_t *out_len) {
    FmlxJsonW w;
    size_t nout = 0;
    char *res = NULL;

    fmlx_jsonw_init(&w, NULL, 0, 1u << 20);
    if (fmlx_jsonw_obj_begin(&w) != FMLX_JSON_OK) goto done;
    for (size_t i = 0; i < n;) {
        size_t j = i;
        while (j + 11 <= n && memcmp(s + j, "<parameter=", 11) != 0) j++;
        if (j + 11 > n) break;
        j += 11;
        size_t ke = j;
        while (ke < n && s[ke] != '>') ke++;
        if (ke >= n) break;
        size_t ve = ke + 1;
        while (ve + 12 <= n && memcmp(s + ve, "</parameter>", 12) != 0) ve++;
        if (ve + 12 > n) break;
        const char *vp = s + ke + 1;
        size_t vn = ve - (ke + 1);
        trim_span(&vp, &vn);
        if (fmlx_jsonw_keyn(&w, s + j, ke - j) != FMLX_JSON_OK) goto done;
        if (fmlx_jsonw_str(&w, vp, vn) != FMLX_JSON_OK) goto done;
        i = ve + 12;
    }
    if (fmlx_jsonw_obj_end(&w) != FMLX_JSON_OK) goto done;
    if (fmlx_jsonw_finish(&w, &nout) != FMLX_JSON_OK) goto done;
    res = dup_bytes(w.buf, nout);
    if (res && out_len) *out_len = nout;
done:
    fmlx_jsonw_free(&w);
    return res;
}

static void parse_one_object(const char *s, size_t n, FmlxToolCalls *out) {
    FmlxJsonDoc d;
    const FmlxJsonValue *root, *name, *args;

    fmlx_json_doc_init(&d);
    if (fmlx_json_parse(s, n, 0, &d) != FMLX_JSON_OK) {
        fmlx_json_doc_free(&d);
        return;
    }
    root = fmlx_json_root(&d);
    if (!root || root->type != FMLX_JSON_OBJ) {
        fmlx_json_doc_free(&d);
        return;
    }
    name = fmlx_json_obj_get(root, "name");
    args = fmlx_json_obj_get(root, "arguments");
    if (!args) args = fmlx_json_obj_get(root, "parameters");
    if (name && name->type == FMLX_JSON_STR) {
        const char *np = fmlx_json_str(name);
        size_t nn = fmlx_json_str_len(name);
        add_call(out, np, nn, args);
    }
    fmlx_json_doc_free(&d);
}

/* Corpo del blocco `<function=NAME>…</function>`: o un oggetto JSON di argomenti (forma
 * llama) o una lista `<parameter=K>V</parameter>` (forma XML). */
static void parse_infunc_body(const char *name, const char *s, size_t n, FmlxToolCalls *out) {
    size_t i = 0;
    size_t nn = strlen(name);

    if (!skip_ws(s, n, &i)) {
        add_call(out, name, nn, NULL);
        return;
    }
    if (s[i] == '{') {
        size_t L;
        if (scan_balanced(s + i, n - i, &L)) {
            FmlxJsonDoc d;
            fmlx_json_doc_init(&d);
            if (fmlx_json_parse(s + i, L, 0, &d) == FMLX_JSON_OK) {
                add_call(out, name, nn, fmlx_json_root(&d));
                fmlx_json_doc_free(&d);
                return;
            }
            fmlx_json_doc_free(&d);
        }
    }
    /* `<parameter=…>` o corpo non JSON: si conserva come oggetto di stringhe. */
    {
        char *args = params_xml_to_json(s, n, NULL);
        FmlxJsonDoc d;
        fmlx_json_doc_init(&d);
        if (args && fmlx_json_parse(args, strlen(args), 0, &d) == FMLX_JSON_OK)
            add_call(out, name, nn, fmlx_json_root(&d));
        else
            add_call(out, name, nn, NULL);
        fmlx_json_doc_free(&d);
        free(args);
    }
}

int fmlx_tools_parse_body(const char *s, size_t n, FmlxToolCalls *out) {
    size_t i = 0;
    const char *p = s;
    size_t pn = n;

    if (!out) return 0;
    fmlx_tools_init(out);
    if (!s) return 0;
    trim_span(&p, &pn);
    while (i < pn) {
        size_t L;
        skip_ws(p, pn, &i);
        if (i >= pn) break;
        if (p[i] == '{') {
            if (!scan_balanced(p + i, pn - i, &L)) break;
            parse_one_object(p + i, L, out);
            i += L;
            continue;
        }
        if (pn - i >= 10 && !memcmp(p + i, "<function=", 10)) {
            size_t ne = i + 10;
            while (ne < pn && p[ne] != '>') ne++;
            if (ne >= pn) break;
            char name[FMLX_TOOL_MAX_NAME];
            size_t nl = ne - (i + 10);
            if (nl >= sizeof name) nl = sizeof name - 1;
            memcpy(name, p + i + 10, nl);
            name[nl] = '\0';
            {
                const char *np = name;
                size_t nn = strlen(name);
                trim_span(&np, &nn);
                memmove(name, np, nn);
                name[nn] = '\0';
            }
            size_t body_start = ne + 1;
            size_t fe = body_start;
            while (fe + 11 <= pn && memcmp(p + fe, "</function>", 11) != 0) fe++;
            if (fe + 11 <= pn) {
                parse_infunc_body(name, p + body_start, fe - body_start, out);
                i = fe + 11;
            } else {
                parse_infunc_body(name, p + body_start, pn - body_start, out);
                i = pn;
            }
            continue;
        }
        break;
    }
    return out->n;
}

/* ---- stream ---- */

typedef enum {
    TS_TEXT = 0,
    TS_INTAG,
    TS_INFUNC_NAME,
    TS_INFUNC_BODY,
    TS_INJSON
} TsMode;

typedef struct {
    FmlxToolOutKind kind;
    char *bytes; /* TEXT/ARGS */
    size_t n;
    char name[FMLX_TOOL_MAX_NAME]; /* TOOL_NAME */
    int ti;
} TsItem;

struct FmlxToolStream {
    int allow_bare;
    TsMode mode;
    Buf buf;
    char funcname[FMLX_TOOL_MAX_NAME];
    int ntools;
    TsItem *q;
    size_t qn, qcap, qhead;
};

typedef struct {
    const char *s;
    size_t l;
} Marker;

static const Marker START[] = {{"<tool_call>", 11}, {"<|python_tag|>", 13}, {"<function=", 10}};
#define NSTART ((int)(sizeof START / sizeof START[0]))

static int find_bytes(const char *s, size_t n, const char *m, size_t ml) {
    if (ml > n) return -1;
    for (size_t i = 0; i + ml <= n; i++)
        if (!memcmp(s + i, m, ml)) return (int)i;
    return -1;
}

FmlxToolStream *fmlx_toolstream_new(int allow_bare) {
    FmlxToolStream *t = calloc(1, sizeof *t);
    if (t) t->allow_bare = allow_bare ? 1 : 0;
    return t;
}

void fmlx_toolstream_free(FmlxToolStream *t) {
    if (!t) return;
    for (size_t i = t->qhead; i < t->qn; i++) free(t->q[i].bytes);
    free(t->q);
    buf_free(&t->buf);
    free(t);
}

int fmlx_toolstream_ntools(const FmlxToolStream *t) { return t ? t->ntools : 0; }

static int q_push(FmlxToolStream *t, const TsItem *it) {
    if (t->qn == t->qcap) {
        size_t cap = t->qcap ? t->qcap * 2 : 8;
        TsItem *nq = realloc(t->q, cap * sizeof *nq);
        if (!nq) return -1;
        t->q = nq;
        t->qcap = cap;
    }
    t->q[t->qn++] = *it;
    return 0;
}

/* Accoda TEXT spezzato su confine di codepoint e sotto il tetto del frame SSE. */
static void emit_text(FmlxToolStream *t, const char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        size_t end = i;
        while (end < n && end - i < 512) {
            unsigned char c = (unsigned char)s[end];
            size_t step = 1;
            if (c >= 0xF0) step = 4;
            else if (c >= 0xE0) step = 3;
            else if (c >= 0xC0) step = 2;
            if (end + step > n || end - i + step > 512) break;
            end += step;
        }
        if (end == i) end = n; /* byte malformato: non bloccare l'emissione */
        TsItem it;
        memset(&it, 0, sizeof it);
        it.kind = FMLX_TS_TEXT;
        it.n = end - i;
        it.bytes = dup_bytes(s + i, it.n);
        if (it.bytes && q_push(t, &it) != 0) free(it.bytes);
        i = end;
    }
}

static void emit_call(FmlxToolStream *t, const FmlxToolCall *c) {
    TsItem it;
    int idx = t->ntools++;

    memset(&it, 0, sizeof it);
    it.kind = FMLX_TS_TOOL_NAME;
    it.ti = idx;
    snprintf(it.name, sizeof it.name, "%s", c->name);
    if (q_push(t, &it) != 0) return;

    size_t i = 0;
    while (i < c->args_len) {
        size_t end = i;
        while (end < c->args_len && end - i < 1024) end++;
        TsItem a;
        memset(&a, 0, sizeof a);
        a.kind = FMLX_TS_TOOL_ARGS;
        a.ti = idx;
        a.n = end - i;
        a.bytes = dup_bytes(c->args + i, a.n);
        if (a.bytes && q_push(t, &a) != 0) free(a.bytes);
        i = end;
    }
}

static void emit_calls(FmlxToolStream *t, const char *body, size_t n) {
    FmlxToolCalls tc;
    fmlx_tools_init(&tc);
    if (fmlx_tools_parse_body(body, n, &tc) > 0)
        for (int i = 0; i < tc.n; i++) emit_call(t, &tc.call[i]);
    fmlx_tools_free(&tc);
}

static void buf_consume(Buf *b, size_t k) {
    if (k >= b->len) {
        b->len = 0;
        if (b->p) b->p[0] = '\0';
        return;
    }
    memmove(b->p, b->p + k, b->len - k);
    b->len -= k;
    b->p[b->len] = '\0';
}

/* Quanta coda di `buf` può essere il PREFISSO di un marcatore di apertura. */
static size_t holdback(const Buf *b) {
    size_t hb = 0;
    for (int m = 0; m < NSTART; m++) {
        size_t ml = START[m].l;
        size_t maxp = ml - 1;
        if (maxp > b->len) maxp = b->len;
        for (size_t p = maxp; p > 0; p--) {
            if (memcmp(b->p + b->len - p, START[m].s, p) == 0) {
                if (p > hb) hb = p;
                break;
            }
        }
    }
    return hb;
}

static void process(FmlxToolStream *t) {
    Buf *b = &t->buf;

    for (;;) {
        size_t len = b->len;
        const char *d = b->p ? b->p : "";
        if (t->mode == TS_TEXT) {
            int k = -1, w = -1;
            for (int m = 0; m < NSTART; m++) {
                int pos = find_bytes(d, len, START[m].s, START[m].l);
                if (pos >= 0 && (k < 0 || pos < k)) { k = pos; w = m; }
            }
            if (k < 0) {
                if (t->allow_bare) {
                    const char *lb = memchr(d, '{', len);
                    if (lb) {
                        size_t j = (size_t)(lb - d);
                        size_t L = 0;
                        emit_text(t, d, j);
                        buf_consume(b, j);
                        if (scan_balanced(b->p, b->len, &L)) {
                            FmlxToolCalls tc;
                            fmlx_tools_init(&tc);
                            if (fmlx_tools_parse_body(b->p, L, &tc) > 0) {
                                for (int x = 0; x < tc.n; x++) emit_call(t, &tc.call[x]);
                            } else {
                                emit_text(t, b->p, L);
                            }
                            fmlx_tools_free(&tc);
                            buf_consume(b, L);
                            continue;
                        }
                        return; /* oggetto incompleto: si attende il resto */
                    }
                }
                size_t hb = holdback(b);
                size_t emit = len > hb ? len - hb : 0;
                emit_text(t, d, emit);
                buf_consume(b, emit);
                return;
            }
            emit_text(t, d, (size_t)k);
            buf_consume(b, (size_t)k);
            const Marker *mk = &START[w];
            buf_consume(b, mk->l);
            if (w == 0) t->mode = TS_INTAG;
            else if (w == 1) t->mode = TS_INJSON;
            else { t->mode = TS_INFUNC_NAME; t->funcname[0] = '\0'; }
            continue;
        }
        if (t->mode == TS_INTAG) {
            int e = find_bytes(d, len, "</tool_call>", 12);
            if (e < 0) return;
            emit_calls(t, d, (size_t)e);
            buf_consume(b, (size_t)e + 12);
            t->mode = TS_TEXT;
            continue;
        }
        if (t->mode == TS_INFUNC_NAME) {
            int e = find_bytes(d, len, ">", 1);
            if (e < 0) return;
            size_t nn = (size_t)e;
            if (nn >= sizeof t->funcname) nn = sizeof t->funcname - 1;
            memcpy(t->funcname, d, nn);
            t->funcname[nn] = '\0';
            buf_consume(b, (size_t)e + 1);
            t->mode = TS_INFUNC_BODY;
            continue;
        }
        if (t->mode == TS_INFUNC_BODY) {
            int e = find_bytes(d, len, "</function>", 11);
            if (e >= 0) {
                FmlxToolCalls tc;
                fmlx_tools_init(&tc);
                parse_infunc_body(t->funcname, d, (size_t)e, &tc);
                for (int i = 0; i < tc.n; i++) emit_call(t, &tc.call[i]);
                fmlx_tools_free(&tc);
                buf_consume(b, (size_t)e + 11);
                t->mode = TS_TEXT;
                continue;
            }
            return;
        }
        if (t->mode == TS_INJSON) {
            size_t i = 0;
            skip_ws(d, len, &i);
            if (i >= len) { buf_consume(b, i); return; }
            if (d[i] != '{') {
                emit_text(t, d, i + 1);
                buf_consume(b, i + 1);
                t->mode = TS_TEXT;
                continue;
            }
            size_t L = 0;
            if (!scan_balanced(d + i, len - i, &L)) { buf_consume(b, i); return; }
            emit_calls(t, d + i, L);
            buf_consume(b, i + L);
            t->mode = TS_TEXT;
            continue;
        }
        return;
    }
}

void fmlx_toolstream_feed(FmlxToolStream *t, const char *s, size_t n) {
    if (!t || !s || n == 0) return;
    if (buf_add(&t->buf, s, n) != 0) return;
    process(t);
}

void fmlx_toolstream_finish(FmlxToolStream *t) {
    if (!t) return;
    Buf *b = &t->buf;
    const char *d = b->p ? b->p : "";
    switch (t->mode) {
    case TS_TEXT:
        emit_text(t, d, b->len);
        break;
    case TS_INTAG:
    case TS_INJSON: {
        size_t i = 0;
        skip_ws(d, b->len, &i);
        FmlxToolCalls tc;
        fmlx_tools_init(&tc);
        if (i < b->len && fmlx_tools_parse_body(d + i, b->len - i, &tc) == 0 && b->len > 0)
            emit_text(t, d, b->len); /* non era un tool: non si perde l'output */
        for (int k = 0; k < tc.n; k++) emit_call(t, &tc.call[k]);
        fmlx_tools_free(&tc);
        break;
    }
    case TS_INFUNC_NAME:
        emit_text(t, "<function=", 10);
        emit_text(t, t->funcname, strlen(t->funcname));
        emit_text(t, d, b->len);
        break;
    case TS_INFUNC_BODY: {
        FmlxToolCalls tc;
        fmlx_tools_init(&tc);
        parse_infunc_body(t->funcname, d, b->len, &tc);
        if (tc.n == 0 && b->len > 0) emit_text(t, d, b->len);
        for (int k = 0; k < tc.n; k++) emit_call(t, &tc.call[k]);
        fmlx_tools_free(&tc);
        break;
    }
    }
    b->len = 0;
    if (b->p) b->p[0] = '\0';
}

int fmlx_toolstream_pop(FmlxToolStream *t, FmlxToolOut *out) {
    if (!t || !out) return 0;
    if (t->qhead >= t->qn) return 0;
    /* libera ciò che è già stato consumato */
    for (size_t i = 0; i < t->qhead; i++) free(t->q[i].bytes);
    if (t->qhead > 0) {
        memmove(t->q, t->q + t->qhead, (t->qn - t->qhead) * sizeof *t->q);
        t->qn -= t->qhead;
        t->qhead = 0;
    }
    memset(out, 0, sizeof *out);
    out->kind = t->q[0].kind;
    out->text = t->q[0].bytes;
    out->n = t->q[0].n;
    out->name = t->q[0].kind == FMLX_TS_TOOL_NAME ? t->q[0].name : NULL;
    out->tool_index = t->q[0].ti;
    t->qhead = 1;
    return 1;
}

/* ---- parse completo ---- */

int fmlx_tools_parse(const char *s, size_t n, FmlxToolCalls *out) {
    FmlxToolStream *t;
    FmlxToolOut o;
    int cur = -1;

    if (!out) return 0;
    fmlx_tools_init(out);
    if (!s || n == 0) return 0;
    t = fmlx_toolstream_new(1);
    if (!t) return 0;
    fmlx_toolstream_feed(t, s, n);
    fmlx_toolstream_finish(t);
    while (fmlx_toolstream_pop(t, &o)) {
        if (o.kind == FMLX_TS_TOOL_NAME) {
            if (out->n >= (int)FMLX_TOOL_MAX_CALLS) break;
            cur = out->n++;
            memset(&out->call[cur], 0, sizeof out->call[cur]);
            snprintf(out->call[cur].name, sizeof out->call[cur].name, "%s", o.name ? o.name : "");
        } else if (o.kind == FMLX_TS_TOOL_ARGS && cur >= 0) {
            FmlxToolCall *c = &out->call[cur];
            char *np = realloc(c->args, c->args_len + o.n + 1);
            if (!np) break;
            c->args = np;
            memcpy(c->args + c->args_len, o.text, o.n);
            c->args_len += o.n;
            c->args[c->args_len] = '\0';
        }
    }
    for (int i = 0; i < out->n; i++)
        if (!out->call[i].args) out->call[i].args = dup_bytes("{}", 2);
    fmlx_toolstream_free(t);
    return out->n;
}
