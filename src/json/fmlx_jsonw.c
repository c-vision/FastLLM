/* fmlx_jsonw.c — serializzatore JSON: mai un output a metà, mai byte che non sono UTF-8.
 *
 * Le due regole che sembrano scomode e sono il punto:
 *  1) OLTRE IL LIMITE DI CAPIENZA OGNI CHIAMATA DIVENTA UN NO-OP e l'errore resta in `w`.
 *     Un writer che "fa finta di niente" e lascia andare il chiamante è come `snprintf`
 *     troncato usato per un `Content-Length`: la risposta parte tagliata e il client ci
 *     mette il tempo che ci mette a scoprirlo. `finish` NON riesce mai se è successo.
 *  2) UNA STRINGA CON UTF-8 INVALIDO È UN ERRORE, non un replacement character. Sembra
 *     duro per uno streaming SSE, dove il tokenizzatore può spezzare un codepoint fra due
 *     chunk: la conseguenza giusta è che il SERVER tiene da parte i byte incompleti
 *     (buffer diCodepoint), non che il serializzatore menta sul testo. Il test che lo
 *     verifica è in tests/json/test_jsonw.c (`\xed\xa0\x80` = surrogate U+D800 in UTF-8).
 *
 * La macchina degli stati è quella che rende impossibile scrivere `{"a"1}`: ogni valore
 * a un livello di oggetto DEVE essere preceduto da `key`, e una chiave non può arrivare
 * due volte. Uso sbagliato = FMLX_JSON_ERR_BAD_ARG, che è un bug di chi chiama e viene
 * fuori al primo test invece che in produzione con risposte malformate.
 */
#include "fmlx/fmlx_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSONW_LEVELS (sizeof ((FmlxJsonW *)0)->need_comma)

static int ensure(FmlxJsonW *w, size_t extra) {
    if (w->err) return 0;
    if (extra > w->max || w->len > w->max - extra) {
        w->err = FMLX_JSON_ERR_BUF_SMALL;
        return 0;
    }
    size_t need = w->len + extra + 1u; /* +1: sempre un terminatore disponibile */
    if (need <= w->cap) return 1;
    /* La memoria data dal chiamante NON si tocca: `realloc` su un array dello stack è un
     * crash con le sembianze di un caso raro. Se il chiamante ha dato un buffer, quello è
     * IL SUO tetto fisico e oltre c'è solo BUF_SMALL; cresce da sé solo il writer nato con
     * `mem == NULL`. */
    if (w->base) {
        w->err = FMLX_JSON_ERR_BUF_SMALL;
        return 0;
    }
    size_t cap = w->cap ? w->cap : 256u;
    while (cap < need && cap < w->max + 1u) {
        if (cap > (SIZE_MAX / 2u)) { cap = w->max + 1u; break; }
        cap *= 2u;
    }
    if (cap > w->max + 1u) cap = w->max + 1u;
    if (cap < need) {
        w->err = FMLX_JSON_ERR_BUF_SMALL;
        return 0;
    }
    char *nb = realloc(w->buf, cap);
    if (!nb) {
        w->err = FMLX_JSON_ERR_NO_MEM;
        return 0;
    }
    w->buf = nb;
    w->cap = cap;
    return 1;
}

static void put(FmlxJsonW *w, const char *s, size_t n) {
    if (!ensure(w, n)) return;
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = '\0';
}

static void putc1(FmlxJsonW *w, char c) { put(w, &c, 1); }

static size_t utf8_step(const char *s, size_t n) {
    const unsigned char *b = (const unsigned char *)s;
    if (n == 0) return 0;
    if (b[0] < 0x80) return 1;
    if ((b[0] & 0xE0) == 0xC0)
        return (n >= 2 && b[0] >= 0xC2 && (b[1] & 0xC0) == 0x80) ? 2 : 0;
    if ((b[0] & 0xF0) == 0xE0) {
        if (n < 3 || (b[1] & 0xC0) != 0x80 || (b[2] & 0xC0) != 0x80) return 0;
        unsigned cp = ((unsigned)(b[0] & 0x0F) << 12) | ((unsigned)(b[1] & 0x3F) << 6) |
                      (unsigned)(b[2] & 0x3F);
        return (cp >= 0x800 && !(cp >= 0xD800 && cp <= 0xDFFF)) ? 3 : 0;
    }
    if ((b[0] & 0xF8) == 0xF0) {
        if (n < 4 || (b[1] & 0xC0) != 0x80 || (b[2] & 0xC0) != 0x80 || (b[3] & 0xC0) != 0x80)
            return 0;
        unsigned cp = ((unsigned)(b[0] & 0x07) << 18) | ((unsigned)(b[1] & 0x3F) << 12) |
                      ((unsigned)(b[2] & 0x3F) << 6) | (unsigned)(b[3] & 0x3F);
        return (cp >= 0x10000 && cp <= 0x10FFFF) ? 4 : 0;
    }
    return 0;
}

int fmlx_json_utf8_ok(const char *s, size_t len) {
    if (!s) return 0;
    for (size_t i = 0; i < len;) {
        size_t k = utf8_step(s + i, len - i);
        if (k == 0) return 0;
        i += k;
    }
    return 1;
}

/* UN SOLO posto decide come si escapea un byte: `write_quoted` (il writer), `esc_len` e
 * `esc_into` (i due conti esportati) chiamano tutti `esc_char`. Avere la regola in due
 * punti è il modo classico di produrre un JSON troncato — `esc_len` calcola il cap, e se
 * il conteggio e la scrittura divergono di un byte l'output esce tagliato.
 * `\n \r \t \b \f` in forma corta, come tutti i producer JSON: serve per confrontare byte
 * con le trace degli altri client. Gli altri controlli restano `\u00xx`. */
static size_t esc_char(unsigned char c, char out[8]) {
    if (c == '"' || c == '\\') { out[0] = '\\'; out[1] = (char)c; return 2; }
    if (c < 0x20) {
        out[0] = '\\';
        switch (c) {
        case '\b': out[1] = 'b'; return 2;
        case '\f': out[1] = 'f'; return 2;
        case '\n': out[1] = 'n'; return 2;
        case '\r': out[1] = 'r'; return 2;
        case '\t': out[1] = 't'; return 2;
        default: return (size_t)snprintf(out, 7, "\\u%04x", c);
        }
    }
    out[0] = (char)c;
    return 1;
}

/* Una stringa fra virgolette, escapeata. Le chiavi passano di qui come i valori: la prima
 * versione di `fmlx_jsonw_key` scriveva la chiave "così com'è", e un `"` dentro una chiave
 * avrebbe prodotto JSON malformato che il writer dichiarava buono. */
static FmlxJsonStatus write_quoted(FmlxJsonW *w, const char *s, size_t len) {
    if (!fmlx_json_utf8_ok(s, len)) {
        w->err = FMLX_JSON_ERR_UTF8;
        return w->err;
    }
    putc1(w, '"');
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == '"' || c == '\\') {
            char esc[8];
            size_t k = esc_char(c, esc);
            put(w, esc, k);
            i++;
        } else {
            size_t k = utf8_step(s + i, len - i); /* l'input è stato validato sopra: k >= 1 */
            put(w, s + i, k);
            i += k;
        }
    }
    putc1(w, '"');
    return w->err;
}

size_t fmlx_json_esc_len(const char *s, size_t len) {
    size_t n = 2; /* le due " » */
    char scratch[8];
    for (size_t i = 0; i < len; i++) n += esc_char((unsigned char)s[i], scratch);
    return n;
}

/* Scrive la forma escapeata, delimitatori inclusi. Ritorna i byte NECESSARI: se `out_cap`
 * è minore nulla viene scritto (troncare a metà una stringa JSON è il bug, non l'output). */
size_t fmlx_json_esc_into(const char *s, size_t len, char *out, size_t out_cap) {
    size_t need = fmlx_json_esc_len(s, len);
    if (out_cap < need) return need;
    size_t w = 0;
    out[w++] = '"';
    for (size_t i = 0; i < len; i++) {
        /* In un temporaneo poi memcpy: scrivere `esc_char` direttamente su `out + w` vorrebbe
         * dire fidarsi che il terminatore cada dentro `out_cap`, e con `out_cap == need`
         * l'ultimo byte sarebbe fuori dal buffer. */
        char esc[8];
        size_t k = esc_char((unsigned char)s[i], esc);
        memcpy(out + w, esc, k);
        w += k;
    }
    out[w++] = '"';
    return w;
}

void fmlx_jsonw_init(FmlxJsonW *w, char *mem, size_t cap, size_t max) {
    if (!w) return;
    memset(w, 0, sizeof *w);
    w->base = mem;
    w->buf = mem;
    w->cap = mem ? cap : 0;
    w->max = max;
}

void fmlx_jsonw_free(FmlxJsonW *w) {
    if (!w) return;
    /* Solo ciò che il writer ha mallocato: `buf == base` vuol dire memoria del chiamante,
     * e liberarla sarebbe uno stack smash con le sembianze di un double free raro. */
    if (w->buf && w->buf != w->base) free(w->buf);
    memset(w, 0, sizeof *w);
}

static FmlxJsonStatus before_value(FmlxJsonW *w) {
    if (w->err) return w->err;
    if (w->depth == 0) {
        /* Il marcatore è un campo, non `need_comma[0]`: quel flag è ANCHE lo stato del
         * contenitore di livello 0, e `{}` lo lascia a 0 — così `finish` avrebbe dichiarato
         * "nessun valore" una risposta perfetta, e una SECONDA radice sarebbe passata. */
        if (w->root_done) {
            w->err = FMLX_JSON_ERR_BAD_ARG;
            return w->err;
        }
        w->root_done = 1;
        return FMLX_JSON_OK;
    }
    uint32_t lvl = w->depth - 1u;
    if (w->in_obj[lvl]) {
        if (!w->key_pending[lvl]) {
            w->err = FMLX_JSON_ERR_BAD_ARG; /* valore in un oggetto senza chiave */
            return w->err;
        }
        w->key_pending[lvl] = 0;
    } else {
        if (w->need_comma[lvl]) putc1(w, ',');
    }
    w->need_comma[lvl] = 1;
    return w->err;
}

static void emit_scalar(FmlxJsonW *w, const char *s, size_t n) {
    if (before_value(w) != FMLX_JSON_OK) return;
    put(w, s, n);
}

FmlxJsonStatus fmlx_jsonw_keyn(FmlxJsonW *w, const char *key, size_t len) {
    if (!w || (!key && len)) return FMLX_JSON_ERR_BAD_ARG;
    if (w->err) return w->err;
    if (w->depth == 0 || !w->in_obj[w->depth - 1u] || w->key_pending[w->depth - 1u]) {
        w->err = FMLX_JSON_ERR_BAD_ARG; /* chiave fuori da un oggetto, o due volte */
        return w->err;
    }
    uint32_t lvl = w->depth - 1u;
    if (w->need_comma[lvl]) putc1(w, ',');
    write_quoted(w, key, len); /* STESSA via dei valori: una chiave con un `"` non deve
                                * poter uscire come JSON malformato dichiarato buono */
    if (w->err) return w->err;
    putc1(w, ':');
    w->key_pending[lvl] = 1;
    return FMLX_JSON_OK;
}

FmlxJsonStatus fmlx_jsonw_key(FmlxJsonW *w, const char *key) {
    if (!key) return FMLX_JSON_ERR_BAD_ARG;
    return fmlx_jsonw_keyn(w, key, strlen(key));
}

static FmlxJsonStatus begin_container(FmlxJsonW *w, int obj) {
    if (!w) return FMLX_JSON_ERR_BAD_ARG;
    if (w->err) return w->err;
    if (w->depth >= JSONW_LEVELS) {
        w->err = FMLX_JSON_ERR_TOO_DEEP;
        return w->err;
    }
    if (before_value(w) != FMLX_JSON_OK) return w->err;
    if (w->err) return w->err;
    putc1(w, obj ? '{' : '[');
    w->in_obj[w->depth] = (uint8_t)obj;
    w->need_comma[w->depth] = 0;
    w->key_pending[w->depth] = 0;
    w->depth++;
    return FMLX_JSON_OK;
}

FmlxJsonStatus fmlx_jsonw_obj_begin(FmlxJsonW *w) { return begin_container(w, 1); }
FmlxJsonStatus fmlx_jsonw_arr_begin(FmlxJsonW *w) { return begin_container(w, 0); }

static FmlxJsonStatus end_container(FmlxJsonW *w, int obj) {
    if (!w) return FMLX_JSON_ERR_BAD_ARG;
    if (w->err) return w->err;
    if (w->depth == 0 || w->in_obj[w->depth - 1u] != (unsigned)obj) {
        w->err = FMLX_JSON_ERR_BAD_ARG; /* chiusura sbagliata o non annidata */
        return w->err;
    }
    if (w->key_pending[w->depth - 1u]) {
        w->err = FMLX_JSON_ERR_BAD_ARG; /* oggetto chiuso con una chiave senza valore */
        return w->err;
    }
    w->depth--;
    putc1(w, obj ? '}' : ']');
    return w->err;
}

FmlxJsonStatus fmlx_jsonw_obj_end(FmlxJsonW *w) { return end_container(w, 1); }
FmlxJsonStatus fmlx_jsonw_arr_end(FmlxJsonW *w) { return end_container(w, 0); }

FmlxJsonStatus fmlx_jsonw_null(FmlxJsonW *w) { emit_scalar(w, "null", 4); return w->err; }
FmlxJsonStatus fmlx_jsonw_bool(FmlxJsonW *w, int b) {
    if (b) emit_scalar(w, "true", 4); else emit_scalar(w, "false", 5);
    return w->err;
}

FmlxJsonStatus fmlx_jsonw_i64(FmlxJsonW *w, int64_t x) {
    char tmp[32];
    int n = snprintf(tmp, sizeof tmp, "%lld", (long long)x);
    emit_scalar(w, tmp, (size_t)(n > 0 ? n : 0));
    return w->err;
}

FmlxJsonStatus fmlx_jsonw_u64(FmlxJsonW *w, uint64_t x) {
    char tmp[32];
    int n = snprintf(tmp, sizeof tmp, "%llu", (unsigned long long)x);
    emit_scalar(w, tmp, (size_t)(n > 0 ? n : 0));
    return w->err;
}

/* Il più corto che torna indietro ESATTAMENTE lo stesso double: prima `%.15g`, e solo se
 * il round-trip non ricade sul valore originale `%.17g`. Serve perché `"temperature":0.7`
 * non diventi 0.69999999999999996 in una risposta. */
FmlxJsonStatus fmlx_jsonw_f64(FmlxJsonW *w, double x) {
    char tmp[40];
    int n = 0;
    /* Il PIÙ CORTO che rilegge sé stesso: si cresce di cifra a partire da 1. Il trucco
     * del "15 g, e se non basta 17 g" non è il più corto — per 1/3 salta sopra la forma
     * da 16 cifre e ne scrive 17. Costo: al più 17 strtod, su un double per risposta. */
    for (int prec = 1; prec <= 17; prec++) {
        n = snprintf(tmp, sizeof tmp, "%.*g", prec, x);
        if (n <= 0) break;
        if (strtod(tmp, NULL) == x) break; /* NaN non eguaglia mai sé stesso: esce a 17 */
    }
    if (n <= 0) { w->err = FMLX_JSON_ERR_SYNTAX; return w->err; }
    if (strstr(tmp, "nan") || strstr(tmp, "inf")) { /* JSON non ha NaN né Infinity */
        w->err = FMLX_JSON_ERR_NUMBER;
        return w->err;
    }
    /* Un REALE non deve mai uscire nella forma di un INTERO: `%g` stampiglia "-0" per -0.0
     * e "12345678" per quel valore, e rileggendo quelle forme il DOM dice INT — il writer
     * avrebbe CAMBIATO il tipo del valore. Se la forma più corta non ha né punto né
     * esponente si riemette con una cifra decimale: `%.1f` è sempre tonda (il valore ha già
     * round-trippato senza frazione) e non tocca i range che `%g` scrive in forma
     * esponenziale, quindi non può allungarsi oltre il buffer. */
    if (!strpbrk(tmp, ".eE")) {
        n = snprintf(tmp, sizeof tmp, "%.1f", x);
        if (n <= 0 || strtod(tmp, NULL) != x) {
            w->err = FMLX_JSON_ERR_NUMBER;
            return w->err;
        }
    }
    emit_scalar(w, tmp, (size_t)n);
    return w->err;
}

FmlxJsonStatus fmlx_jsonw_str(FmlxJsonW *w, const char *s, size_t len) {
    if (!w) return FMLX_JSON_ERR_BAD_ARG;
    if (w->err) return w->err;
    if (!s && len) { w->err = FMLX_JSON_ERR_BAD_ARG; return w->err; }
    if (before_value(w) != FMLX_JSON_OK) return w->err;
    if (w->err) return w->err;
    if (s && len) write_quoted(w, s, len);
    else put(w, "\"\"", 2);
    return w->err;
}

FmlxJsonStatus fmlx_jsonw_cstr(FmlxJsonW *w, const char *s) {
    if (!s) return fmlx_jsonw_null(w);
    return fmlx_jsonw_str(w, s, strlen(s));
}

/* Valore già serializzato: viene PARSATO prima di essere incorporato. È la guardia del
 * piano §11.4 — "se il JSON degli argomenti di un tool è troncato, `stop_reason =
 * max_tokens` e NESSUN `tool_use` emesso": un `.input` parziale non deve poter uscire. */
FmlxJsonStatus fmlx_jsonw_raw(FmlxJsonW *w, const char *json, size_t len) {
    if (!w || (!json && len)) return FMLX_JSON_ERR_BAD_ARG;
    if (w->err) return w->err;
    FmlxJsonDoc scratch;
    fmlx_json_doc_init(&scratch);
    FmlxJsonStatus e = fmlx_json_parse(json, len, 0, &scratch);
    fmlx_json_doc_free(&scratch);
    if (e != FMLX_JSON_OK) {
        /* Il writer ENTRA in errore, non lo rimanda soltanto: la promessa dell'intestazione
         * è che dopo un rifiuto ogni chiamata successiva sia un no-op e `finish` non riesca
         * mai. Lasciare `w->err` pulito voleva dire accettare il membro SEGUENTE al posto
         * di quello rifiutato — un `tool_use` con gli argomenti persi. */
        w->err = e;
        return e;
    }
    emit_scalar(w, json, len);
    return w->err;
}

FmlxJsonStatus fmlx_jsonw_finish(FmlxJsonW *w, size_t *out_len) {
    if (!w) return FMLX_JSON_ERR_BAD_ARG;
    if (w->err) return w->err;
    if (w->depth != 0) { w->err = FMLX_JSON_ERR_SYNTAX; return w->err; } /* contenitore aperto */
    if (!w->root_done) { w->err = FMLX_JSON_ERR_EMPTY; return w->err; }
    if (out_len) *out_len = w->len;
    return FMLX_JSON_OK;
}

/* DOM -> JSON. Serve al server per il punto del piano §11.4 che non ammette scorciatoie:
 * `tool_calls[].function.arguments` è una STRINGA per OpenAI e un OGGETTO per Anthropic,
 * e il passaggio dall'una all'altro deve passare da qui, non da un "togli le virgolette".
 * Ricorsione: la profondità è già limitata dal parser a FMLX_JSON_MAX_DEPTH, e il writer ha
 * lo stesso cap — quindi questo percorso non può incontrare `FMLX_JSON_ERR_TOO_DEEP` su un
 * documento che il parser ha accettato. Un documento più profondo NON arriva qui: il parser
 * l'ha già rifiutato con un errore, non con un output tagliato. */
FmlxJsonStatus fmlx_json_render(const FmlxJsonValue *v, FmlxJsonW *w) {
    if (!v || !w) return FMLX_JSON_ERR_BAD_ARG;
    if (w->err) return w->err;
    switch (v->type) {
    case FMLX_JSON_NULL: return fmlx_jsonw_null(w);
    case FMLX_JSON_BOOL: return fmlx_jsonw_bool(w, v->v.b);
    case FMLX_JSON_INT: return fmlx_jsonw_i64(w, v->v.i);
    case FMLX_JSON_REAL: return fmlx_jsonw_f64(w, v->v.f);
    case FMLX_JSON_STR: return fmlx_jsonw_str(w, v->d->arena + v->str_off, v->str_len);
    case FMLX_JSON_ARR: {
        FmlxJsonStatus e = fmlx_jsonw_arr_begin(w);
        if (e != FMLX_JSON_OK) return e;
        for (size_t i = 0; i < fmlx_json_arr_n(v); i++) {
            e = fmlx_json_render(fmlx_json_arr_at(v, i), w);
            if (e != FMLX_JSON_OK) return e;
        }
        return fmlx_jsonw_arr_end(w);
    }
    case FMLX_JSON_OBJ: {
        FmlxJsonStatus e = fmlx_jsonw_obj_begin(w);
        if (e != FMLX_JSON_OK) return e;
        for (size_t i = 0; i < fmlx_json_obj_n(v); i++) {
            const FmlxJsonValue *k = fmlx_json_obj_key_at(v, i);
            if (!k) return FMLX_JSON_ERR_BAD_ARG;
            e = fmlx_jsonw_keyn(w, k->d->arena + k->str_off, k->str_len);
            if (e != FMLX_JSON_OK) return e;
            e = fmlx_json_render(fmlx_json_obj_value_at(v, i), w);
            if (e != FMLX_JSON_OK) return e;
        }
        return fmlx_jsonw_obj_end(w);
    }
    case FMLX_JSON_KEY: break; /* una chiave non è un valore: chi ci arriva ha sbagliato indice */
    }
    w->err = FMLX_JSON_ERR_BAD_ARG;
    return w->err;
}
