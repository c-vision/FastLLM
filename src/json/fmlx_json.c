/* fmlx_json.c — parser: RFC 8259 severo, DOM a postordine, zero allocazioni non dichiarate.
 *
 * Le quattro decisioni che sembrano pignolerie e invece sono il contratto:
 *  1) I NODI SONO IN POSTORDINE e i figli stanno in una tabella separata (`kids`). Un
 *     contenitore sa i suoi figli solo quando si chiude, quindi li appende in blocchi
 *     contigui in quel momento: `child`/`n` restano validi anche se il documento cresce.
 *     (Prima bozza: figli contigui in `nodes`. Falsa per `[[1,2],[3]]`: i due figli
 *     interni finiscono a indici 2 e 4, NON contigui. Misurato su un caso scritto da me.)
 *  2) Le chiavi di un oggetto SONO NODI (tipo FMLX_JSON_KEY) referenziati da `kids` a
 *     coppie [chiave, valore]. Costa un indice in più per membro, e `obj_n` divide per 2.
 *  3) Ogni nodo porta il puntatore al SUO documento. Sarebbe stato +8 byte per nodo "a
 *     gratis" rispetto a far passare `FmlxJsonDoc*` a ogni accessorio: ho scelto la forma
 *     che rende illeggibile un uso sbagliato (`fmlx_json_obj_get(v, "role")` non può
 *     chiedere il documento di un valore che arriva da un altro documento).
 *  4) Ogni rifiuto ha il SUO codice e la SUA offset: `err_off` è l'offset del byte
 *     colpevole, mai della fine del buffer. Un 400 che non dice dove non è un 400 utile.
 */
#include "fmlx/fmlx_json.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Il puntatore di parsing. `scr` è l'impasto dei figli: un contenitore APPENA APERTO non
 * sa quanti ne avrà, e i suoi figli vanno scritti DOPO quelli dei contenitori annidati
 * (che nel frattempo hanno già consumato slot). Tenere l'impasto separato e versarlo in
 * `d->kids` alla chiusura è ciò che rende `child`/`n` davvero un intervallo contiguo.
 * Vive solo per la durata della `parse`: il DOM resta di tre blocchi. */
typedef struct {
    const char *src;
    size_t len;
    size_t pos;
    unsigned depth;
    FmlxJsonDoc *d;
    uint32_t *scr;
    size_t nscr, cap_scr;
    uint32_t base[FMLX_JSON_MAX_DEPTH + 2]; /* impasto di partenza per ogni livello aperto */
} P;

const char *fmlx_json_status_str(FmlxJsonStatus s) {
    switch (s) {
    case FMLX_JSON_OK: return "ok";
    case FMLX_JSON_ERR_EMPTY: return "vuoto";
    case FMLX_JSON_ERR_SYNTAX: return "sintassi";
    case FMLX_JSON_ERR_TRAILING: return "trailing";
    case FMLX_JSON_ERR_TOO_DEEP: return "troppo profondo";
    case FMLX_JSON_ERR_TOO_BIG: return "troppo grande";
    case FMLX_JSON_ERR_NUMBER: return "numero";
    case FMLX_JSON_ERR_UTF8: return "utf-8";
    case FMLX_JSON_ERR_CONTROL: return "controllore nudo";
    case FMLX_JSON_ERR_NO_MEM: return "memoria";
    case FMLX_JSON_ERR_BAD_ARG: return "argomento";
    case FMLX_JSON_ERR_BUF_SMALL: return "buffer";
    }
    return "sconosciuto";
}

void fmlx_json_doc_init(FmlxJsonDoc *d) {
    if (!d) return;
    memset(d, 0, sizeof *d);
}

void fmlx_json_doc_free(FmlxJsonDoc *d) {
    if (!d) return;
    free(d->nodes);
    free(d->kids);
    free(d->arena);
    fmlx_json_doc_init(d);
}

size_t fmlx_json_node_bound(const char *src, size_t len) {
    (void)src;
    return len + 2; /* un nodo per byte è il peggior caso (`1` in `[1,1,1...]`); +2 di gioco */
}

static void fail(P *p, const char *what) {
    FmlxJsonDoc *d = p->d;
    d->err_off = p->pos < p->len ? p->pos : p->len;
    snprintf(d->err_msg, sizeof d->err_msg, "%s (offset %zu di %zu)", what, d->err_off, p->len);
}

static FmlxJsonValue *push_node(P *p, FmlxJsonStatus *err) {
    FmlxJsonDoc *d = p->d;
    if (d->nnodes >= d->cap_nodes) {
        *err = FMLX_JSON_ERR_TOO_BIG;
        fail(p, "troppi nodi per il limite dato");
        return NULL;
    }
    FmlxJsonValue *v = &d->nodes[d->nnodes];
    memset(v, 0, sizeof *v);
    v->d = d;
    d->nnodes++;
    return v;
}

static FmlxJsonStatus push_kid(P *p, uint32_t idx) {
    if (p->nscr >= p->cap_scr) {
        fail(p, "troppi figli per il limite dato");
        return FMLX_JSON_ERR_TOO_BIG;
    }
    p->scr[p->nscr++] = idx;
    return FMLX_JSON_OK;
}

static void skip_ws(P *p) {
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        p->pos++;
    }
}

static int hex4(const char *s, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        unsigned g;
        if (c >= '0' && c <= '9') g = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') g = (unsigned)(c - 'a') + 10u;
        else if (c >= 'A' && c <= 'F') g = (unsigned)(c - 'A') + 10u;
        else return -1;
        v = v * 16u + g;
    }
    *out = v;
    return 0;
}

/* Lunghezza di una sequenza UTF-8 ben formata che inizia a `s` con `n` byte disponibili,
 * o 0 se è invalida (sovralunga, surrogate, oltre U+10FFFF, troncata). Non è
 * "tollerante": un byte spazzatura deve diventare un errore con un offset, non un
 * replacement character che il client non può più confrontare con ciò che aveva mandato. */
static size_t utf8_len(const char *s, size_t n) {
    const unsigned char *b = (const unsigned char *)s;
    if (n == 0) return 0;
    if (b[0] < 0x80) return 1;
    if ((b[0] & 0xE0) == 0xC0) {
        if (n < 2 || (b[1] & 0xC0) != 0x80 || b[0] < 0xC2) return 0; /* 0xC0/0xC1 = sovrallungo */
        return 2;
    }
    if ((b[0] & 0xF0) == 0xE0) {
        if (n < 3 || (b[1] & 0xC0) != 0x80 || (b[2] & 0xC0) != 0x80) return 0;
        unsigned cp = ((unsigned)(b[0] & 0x0F) << 12) | ((unsigned)(b[1] & 0x3F) << 6) |
                      (unsigned)(b[2] & 0x3F);
        if (cp < 0x800) return 0;                   /* sovrallungo */
        if (cp >= 0xD800 && cp <= 0xDFFF) return 0; /* surrogate in UTF-8: vietato */
        return 3;
    }
    if ((b[0] & 0xF8) == 0xF0) {
        if (n < 4 || (b[1] & 0xC0) != 0x80 || (b[2] & 0xC0) != 0x80 || (b[3] & 0xC0) != 0x80)
            return 0;
        unsigned cp = ((unsigned)(b[0] & 0x07) << 18) | ((unsigned)(b[1] & 0x3F) << 12) |
                      ((unsigned)(b[2] & 0x3F) << 6) | (unsigned)(b[3] & 0x3F);
        if (cp < 0x10000 || cp > 0x10FFFF) return 0;
        return 4;
    }
    return 0;
}

static size_t utf8_put(char *dst, unsigned cp) {
    unsigned char *b = (unsigned char *)dst;
    if (cp < 0x80) { b[0] = (unsigned char)cp; return 1; }
    if (cp < 0x800) {
        b[0] = (unsigned char)(0xC0 | (cp >> 6));
        b[1] = (unsigned char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        b[0] = (unsigned char)(0xE0 | (cp >> 12));
        b[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (unsigned char)(0x80 | (cp & 0x3F));
        return 3;
    }
    b[0] = (unsigned char)(0xF0 | (cp >> 18));
    b[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
    b[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
    b[3] = (unsigned char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Una stringa JSON, valore o chiave. L'unescape avviene mentre si copia nell'arena:
 * `cap` (span della forma escapeata, delimitatori inclusi) è un tetto onesto perché ogni
 * escape ACCORCIA (`\n` 2->1, `\u00e9` 6->2) e ogni byte ASCII lascia uguale. A fine
 * decoding l'arena viene ritirata a `off + w`: lo spazio promesso e non usato non si
 * accumula stringa su stringa. */
static FmlxJsonStatus parse_string(P *p, FmlxJsonValue *out, int is_key) {
    FmlxJsonDoc *d = p->d;
    if (p->pos >= p->len || p->src[p->pos] != '"') {
        fail(p, "attesa \" che apre una stringa");
        return FMLX_JSON_ERR_SYNTAX;
    }
    size_t start = p->pos++;
    size_t scan = p->pos;
    while (scan < p->len) {
        char c = p->src[scan];
        if (c == '"') break;
        if (c == '\\') { scan += 2; continue; }
        scan++;
    }
    if (scan >= p->len) {
        p->pos = p->len;
        fail(p, "stringa non terminata");
        return FMLX_JSON_ERR_SYNTAX;
    }
    size_t cap = scan - start + 1;
    uint32_t off = 0;
    if (cap > d->cap_arena - d->narena) {
        fail(p, "arena delle stringhe esaurita");
        return FMLX_JSON_ERR_NO_MEM;
    }
    off = (uint32_t)d->narena;
    d->narena += cap;

    char *dst = d->arena + off;
    size_t w = 0;
    for (size_t r = start + 1; r < scan;) {
        unsigned char c = (unsigned char)p->src[r];
        if (c < 0x20) {
            p->pos = r;
            fail(p, "carattere di controllo nudo in una stringa");
            return FMLX_JSON_ERR_CONTROL;
        }
        if (c != '\\') {
            if (c < 0x80) { dst[w++] = (char)c; r++; continue; }
            size_t need = utf8_len(p->src + r, scan - r);
            if (need == 0) {
                p->pos = r;
                fail(p, "sequenza UTF-8 non valida in una stringa");
                return FMLX_JSON_ERR_UTF8;
            }
            memcpy(dst + w, p->src + r, need);
            w += need;
            r += need;
            continue;
        }
        if (r + 1 >= scan + 1) {
            p->pos = r;
            fail(p, "backslash a fine stringa");
            return FMLX_JSON_ERR_SYNTAX;
        }
        char esc = p->src[r + 1];
        r += 2;
        char one;
        switch (esc) {
        case '"': one = '"'; break;
        case '\\': one = '\\'; break;
        case '/': one = '/'; break;
        case 'b': one = '\b'; break;
        case 'f': one = '\f'; break;
        case 'n': one = '\n'; break;
        case 'r': one = '\r'; break;
        case 't': one = '\t'; break;
        case 'u': {
            if (r + 4 > p->len) {
                p->pos = r;
                fail(p, "\\u troncato");
                return FMLX_JSON_ERR_SYNTAX;
            }
            unsigned cp;
            if (hex4(p->src + r, &cp) != 0) {
                p->pos = r;
                fail(p, "\\u con cifra non esadecimale");
                return FMLX_JSON_ERR_SYNTAX;
            }
            size_t at = r - 2; /* dove è iniziato \u, per puntare l'indice dell'errore */
            r += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                if (r + 6 > p->len || p->src[r] != '\\' || p->src[r + 1] != 'u') {
                    p->pos = at;
                    fail(p, "surrogate alto non appaiato a un basso");
                    return FMLX_JSON_ERR_UTF8;
                }
                unsigned lo;
                if (hex4(p->src + r + 2, &lo) != 0 || lo < 0xDC00 || lo > 0xDFFF) {
                    p->pos = r;
                    fail(p, "seconda parte di una coppia di surrogate non valida");
                    return FMLX_JSON_ERR_UTF8;
                }
                r += 6;
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                p->pos = at;
                fail(p, "surrogate basso senza alto");
                return FMLX_JSON_ERR_UTF8;
            }
            w += utf8_put(dst + w, cp);
            continue;
        }
        default:
            p->pos = r - 2;
            fail(p, "escape non valida");
            return FMLX_JSON_ERR_SYNTAX;
        }
        dst[w++] = one;
    }

    out->type = is_key ? FMLX_JSON_KEY : FMLX_JSON_STR;
    out->str_off = off;
    out->str_len = (uint32_t)w;
    d->narena = off + w; /* ritiro lo spazio non usato (vedi sopra) */
    p->pos = scan + 1;
    return FMLX_JSON_OK;
}

/* Numero, grammatica RFC 8259: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][-+]?[0-9]+)?
 * Restituisce INT o REAL. Un intero fuori da int64 è FMLX_JSON_ERR_NUMBER, NON un double
 * approssimato: `{"id": 99999999999999999999}` deve diventare un 400, non un id falso. */
static FmlxJsonStatus parse_number(P *p, FmlxJsonValue *out) {
    size_t s = p->pos;
    const char *src = p->src;
    size_t len = p->len;
    size_t r = s;
    int neg = 0;
    if (r < len && src[r] == '-') { neg = 1; r++; }
    if (r >= len || src[r] < '0' || src[r] > '9') {
        p->pos = r;
        fail(p, "numero senza cifra dopo il segno");
        return FMLX_JSON_ERR_NUMBER;
    }
    if (src[r] == '0') {
        r++;
        if (r < len && src[r] >= '0' && src[r] <= '9') {
            p->pos = r;
            fail(p, "zero iniziale seguito da cifre: 01 non e' JSON");
            return FMLX_JSON_ERR_NUMBER;
        }
    } else {
        while (r < len && src[r] >= '0' && src[r] <= '9') r++;
    }
    int is_real = 0;
    if (r < len && src[r] == '.') {
        is_real = 1;
        r++;
        size_t d0 = r;
        while (r < len && src[r] >= '0' && src[r] <= '9') r++;
        if (r == d0) {
            p->pos = r;
            fail(p, "virgola senza cifre decimali");
            return FMLX_JSON_ERR_NUMBER;
        }
    }
    if (r < len && (src[r] == 'e' || src[r] == 'E')) {
        is_real = 1;
        r++;
        if (r < len && (src[r] == '+' || src[r] == '-')) r++;
        size_t e0 = r;
        while (r < len && src[r] >= '0' && src[r] <= '9') r++;
        if (r == e0) {
            p->pos = r;
            fail(p, "esponente senza cifre");
            return FMLX_JSON_ERR_NUMBER;
        }
    }
    p->pos = r;

    if (!is_real) {
        const char *dig = src + s + (neg ? 1 : 0);
        size_t ndig = r - s - (size_t)(neg ? 1 : 0);
        uint64_t mag = 0;
        for (size_t i = 0; i < ndig; i++) {
            if (mag > (UINT64_MAX - (uint64_t)(dig[i] - '0')) / 10ull) {
                fail(p, "intero fuori dalla portata di int64");
                return FMLX_JSON_ERR_NUMBER;
            }
            mag = mag * 10ull + (uint64_t)(dig[i] - '0');
        }
        if (!neg) {
            if (mag > (uint64_t)INT64_MAX) {
                fail(p, "intero oltre INT64_MAX");
                return FMLX_JSON_ERR_NUMBER;
            }
            out->type = FMLX_JSON_INT;
            out->v.i = (int64_t)mag;
            return FMLX_JSON_OK;
        }
        if (mag > (uint64_t)INT64_MAX + 1ull) {
            fail(p, "intero sotto INT64_MIN");
            return FMLX_JSON_ERR_NUMBER;
        }
        out->type = FMLX_JSON_INT;
        out->v.i = (mag == (uint64_t)INT64_MAX + 1ull) ? INT64_MIN : -(int64_t)mag;
        return FMLX_JSON_OK;
    }

    char buf[512];
    size_t n = r - s;
    if (n >= sizeof buf) {
        fail(p, "numero con troppe cifre");
        return FMLX_JSON_ERR_NUMBER;
    }
    memcpy(buf, src + s, n);
    buf[n] = '\0';
    errno = 0;
    char *end = NULL;
    double v = strtod(buf, &end);
    /* ERANGE copre DUE casi che non vanno confusi: overflow (il risultato è ±Inf, e JSON non
     * li ha -> si rifiuta) e la denormalizzazione (il risultato è un double ESATTO, il più
     * piccolo subnormale è `5e-324` -> si accetta). Rifiutare entrambi per via dell'errno
     * voleva dire rifiutare input legali che Python accetta. */
    if (end != buf + n || (errno == ERANGE && !isfinite(v))) {
        fail(p, "reale non rappresentabile in double");
        return FMLX_JSON_ERR_NUMBER;
    }
    out->type = FMLX_JSON_REAL;
    out->v.f = v;
    return FMLX_JSON_OK;
}

static FmlxJsonStatus parse_literal(P *p, FmlxJsonValue *out) {
    size_t r = p->pos;
    const char *src = p->src;
    size_t len = p->len;
    int b = 0;
    size_t w = 0;
    if (len - r >= 4 && !memcmp(src + r, "true", 4)) { b = 1; w = 4; }
    else if (len - r >= 5 && !memcmp(src + r, "false", 5)) { b = 0; w = 5; }
    else if (len - r >= 4 && !memcmp(src + r, "null", 4)) {
        out->type = FMLX_JSON_NULL;
        p->pos = r + 4;
        return FMLX_JSON_OK;
    } else {
        fail(p, "letterale true/false/null atteso");
        return FMLX_JSON_ERR_SYNTAX;
    }
    out->type = FMLX_JSON_BOOL;
    out->v.b = b;
    p->pos = r + w;
    return FMLX_JSON_OK;
}

static FmlxJsonStatus parse_value(P *p, uint32_t *out_idx);

/* Chiude con UN SOLO `push_node` per contenitore, su un'unica via d'uscita: la versione
 * con due rami (vuoto / non vuoto) era quella in cui, nella prima bozza, l'indice del
 * contenitore si perdeva. */
static FmlxJsonStatus parse_container(P *p, int want_obj, uint32_t *out_idx) {
    uint32_t count = 0;
    p->pos++; /* il { o [ */
    if (++p->depth > FMLX_JSON_MAX_DEPTH) {
        p->depth--;
        fail(p, "profondita' massima superata");
        return FMLX_JSON_ERR_TOO_DEEP;
    }
    uint32_t base = (uint32_t)p->nscr;
    p->base[p->depth - 1u] = base;
    skip_ws(p);
    if (p->pos < p->len && p->src[p->pos] == (want_obj ? '}' : ']')) {
        p->pos++;
        goto close;
    }
    for (;;) {
        skip_ws(p);
        if (want_obj) {
            FmlxJsonStatus e;
            FmlxJsonValue *k = push_node(p, &e);
            if (!k) return e;
            e = parse_string(p, k, 1);
            if (e != FMLX_JSON_OK) return e;
            if ((e = push_kid(p, (uint32_t)(k - p->d->nodes))) != FMLX_JSON_OK) return e;
            skip_ws(p);
            if (p->pos >= p->len || p->src[p->pos] != ':') {
                fail(p, "attesa : fra chiave e valore");
                return FMLX_JSON_ERR_SYNTAX;
            }
            p->pos++;
        }
        uint32_t vidx = 0;
        FmlxJsonStatus e = parse_value(p, &vidx);
        if (e != FMLX_JSON_OK) return e;
        if ((e = push_kid(p, vidx)) != FMLX_JSON_OK) return e;
        count++;
        skip_ws(p);
        if (p->pos >= p->len) {
            fail(p, "contenitore non chiuso");
            return FMLX_JSON_ERR_SYNTAX;
        }
        if (p->src[p->pos] == ',') { p->pos++; continue; }
        if (p->src[p->pos] == (want_obj ? '}' : ']')) { p->pos++; break; }
        fail(p, "attesa , o la chiusura del contenitore");
        return FMLX_JSON_ERR_SYNTAX;
    }
close:
    p->depth--;
    {
        FmlxJsonStatus e;
        FmlxJsonDoc *d = p->d;
        uint32_t base2 = p->base[p->depth]; /* quello salvato all'apertura di QUESTO livello */
        size_t take = p->nscr - base2;
        if (d->nkids + take > d->cap_kids) {
            fail(p, "troppi figli per il limite dato");
            return FMLX_JSON_ERR_TOO_BIG;
        }
        FmlxJsonValue *v = push_node(p, &e);
        if (!v) return e;
        v->type = want_obj ? FMLX_JSON_OBJ : FMLX_JSON_ARR;
        v->child = (uint32_t)d->nkids;
        memcpy(d->kids + d->nkids, p->scr + base2, take * sizeof *d->kids);
        d->nkids += take;
        p->nscr = base2; /* il contenitore genitore riprende da lì il proprio impasto */
        v->n = want_obj ? count * 2u : count;
        *out_idx = (uint32_t)(d->nnodes - 1);

        if (want_obj && count > 1) {
            /* Chiavi duplicate: confronto a coppie sui membri di QUESTO oggetto. O(n^2)
             * sul numero di membri, e il corpo della richiesta ha un tetto: è il costo di
             * sapere che `{"max_tokens":8,"max_tokens":4096}` non è una decisione da
             * prendere a caso. Qui si SEGNA e basta: `obj_get` prende la prima, sempre. */
            int dup = 0;
            for (uint32_t i = 0; i + 1 < v->n && !dup; i += 2) {
                const FmlxJsonValue *a = &p->d->nodes[p->d->kids[v->child + i]];
                for (uint32_t j = i + 2; j + 1 < v->n; j += 2) {
                    const FmlxJsonValue *b = &p->d->nodes[p->d->kids[v->child + j]];
                    if (a->str_len == b->str_len &&
                        !memcmp(p->d->arena + a->str_off, p->d->arena + b->str_off, a->str_len)) {
                        dup = 1;
                        break;
                    }
                }
            }
            if (dup) p->d->had_dup_keys = 1;
        }
        return FMLX_JSON_OK;
    }
}

static FmlxJsonStatus parse_value(P *p, uint32_t *out_idx) {
    skip_ws(p);
    if (p->pos >= p->len) {
        fail(p, "valore atteso, fine input");
        return FMLX_JSON_ERR_SYNTAX;
    }
    char c = p->src[p->pos];
    if (c == '{' || c == '[') return parse_container(p, c == '{', out_idx);

    FmlxJsonStatus e;
    FmlxJsonValue *v = push_node(p, &e);
    if (!v) return e;
    *out_idx = (uint32_t)(p->d->nnodes - 1);
    if (c == '"') return parse_string(p, v, 0);
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number(p, v);
    if (c == '+' || c == '.') {
        /* Sono numeri scritti male, non "un valore che non si è trovato": il cliente che
         * manda `max_tokens: +1` o `.5` riceve il codice che descrive il SUO errore. */
        p->pos++;
        fail(p, c == '+' ? "segno + non ammesso su un numero"
                         : "reale senza la parte intera: .5 non e' JSON");
        return FMLX_JSON_ERR_NUMBER;
    }
    return parse_literal(p, v);
}

/* Via d'uscita unica del parse: libera i tre blocchi MA lascia il messaggio e l'offset.
 * Senza questo, `doc_free` (che azzera il documento) cancellerebbe proprio la riga che
 * serve a chi legge il log: un 400 che dice "sintassi" e non dove. */
static FmlxJsonStatus bail(FmlxJsonDoc *out, FmlxJsonStatus e) {
    char keep[FMLX_JSON_ERR_MSG_MAX];
    size_t off = out->err_off;
    snprintf(keep, sizeof keep, "%s", out->err_msg);
    fmlx_json_doc_free(out);
    out->err_off = off;
    snprintf(out->err_msg, sizeof out->err_msg, "%s", keep);
    return e;
}

FmlxJsonStatus fmlx_json_parse(const char *src, size_t len, size_t max_nodes, FmlxJsonDoc *out) {
    if (!src || !out) return FMLX_JSON_ERR_BAD_ARG;
    fmlx_json_doc_free(out);
    if (len == 0) return FMLX_JSON_ERR_EMPTY;

    P p;
    memset(&p, 0, sizeof p);
    p.src = src;
    p.len = len;
    p.d = out;
    skip_ws(&p);
    if (p.pos == len) return FMLX_JSON_ERR_EMPTY; /* solo whitespace: non è un documento */

    size_t cap_nodes = max_nodes ? max_nodes : fmlx_json_node_bound(src, len);
    if (cap_nodes > (1u << 22)) cap_nodes = (1u << 22); /* tetto di processo, non di grammatica */
    out->nodes = calloc(cap_nodes, sizeof *out->nodes);
    out->kids = calloc(cap_nodes * 2u + 2u, sizeof *out->kids);
    out->arena = malloc(len + 1u);
    if (!out->nodes || !out->kids || !out->arena) {
        snprintf(out->err_msg, sizeof out->err_msg,
                 "allocazione rifiutata (%zu byte di input)", len);
        return bail(out, FMLX_JSON_ERR_NO_MEM);
    }
    out->cap_nodes = cap_nodes;
    out->cap_kids = cap_nodes * 2u + 2u;
    out->cap_arena = len + 1u;
    /* L'impasto ha lo stesso tetto della tabella dei figli: ogni voce che ci entra finisce
     * in `kids`, e un nodo è figlio di UN SOLO contenitore. */
    p.scr = malloc(out->cap_kids * sizeof *p.scr);
    p.cap_scr = p.scr ? out->cap_kids : 0;
    if (!p.scr) {
        snprintf(out->err_msg, sizeof out->err_msg,
                 "allocazione rifiutata (%zu byte di input)", len);
        return bail(out, FMLX_JSON_ERR_NO_MEM);
    }

    uint32_t root = 0;
    FmlxJsonStatus e = parse_value(&p, &root);
    if (e == FMLX_JSON_OK) {
        skip_ws(&p);
        if (p.pos != len) {
            fail(&p, "contenuto dopo il valore radice");
            e = FMLX_JSON_ERR_TRAILING;
        } else {
            out->root = root;
        }
    }
    free(p.scr);
    p.scr = NULL; /* non è DOM: a parse finita non deve restare nulla di appeso */
    return e == FMLX_JSON_OK ? FMLX_JSON_OK : bail(out, e);
}

/* ---- accessori ---- */

const FmlxJsonValue *fmlx_json_root(const FmlxJsonDoc *d) {
    if (!d || !d->nodes || d->nnodes == 0 || d->root >= d->nnodes) return NULL;
    return &d->nodes[d->root];
}

/* Una KEY si legge con gli stessi due accessorî di una STR: il nodo arriva da
 * `obj_key_at`, quindi il chiamante sa già cosa ha in mano. `as_str` NO, che è
 * l'accessorio che *valida*: una chiave non è mai un valore. */
const char *fmlx_json_str(const FmlxJsonValue *v) {
    if (!v || !v->d) return NULL;
    if (v->type != FMLX_JSON_STR && v->type != FMLX_JSON_KEY) return NULL;
    return v->d->arena + v->str_off;
}

size_t fmlx_json_str_len(const FmlxJsonValue *v) {
    if (!v) return 0;
    return (v->type == FMLX_JSON_STR || v->type == FMLX_JSON_KEY) ? v->str_len : 0;
}

int fmlx_json_is_null(const FmlxJsonValue *v) { return v && v->type == FMLX_JSON_NULL; }

int fmlx_json_as_bool(const FmlxJsonValue *v, int *out) {
    if (!v || v->type != FMLX_JSON_BOOL) return 0;
    if (out) *out = v->v.b;
    return 1;
}

int fmlx_json_as_i64(const FmlxJsonValue *v, int64_t *out) {
    if (!v || v->type != FMLX_JSON_INT) return 0;
    if (out) *out = v->v.i;
    return 1;
}

int fmlx_json_as_f64(const FmlxJsonValue *v, double *out) {
    if (!v) return 0;
    if (v->type == FMLX_JSON_REAL) { if (out) *out = v->v.f; return 1; }
    if (v->type == FMLX_JSON_INT) { if (out) *out = (double)v->v.i; return 1; }
    return 0;
}

int fmlx_json_as_str(const FmlxJsonValue *v, const char **p, size_t *len) {
    const char *s = fmlx_json_str(v);
    if (!s) return 0;
    if (p) *p = s;
    if (len) *len = v->str_len;
    return 1;
}

size_t fmlx_json_arr_n(const FmlxJsonValue *v) {
    return (v && v->type == FMLX_JSON_ARR) ? v->n : 0;
}

const FmlxJsonValue *fmlx_json_arr_at(const FmlxJsonValue *v, size_t i) {
    if (!v || v->type != FMLX_JSON_ARR || i >= v->n || !v->d) return NULL;
    return &v->d->nodes[v->d->kids[v->child + i]];
}

size_t fmlx_json_obj_n(const FmlxJsonValue *v) {
    return (v && v->type == FMLX_JSON_OBJ) ? v->n / 2u : 0;
}

const FmlxJsonValue *fmlx_json_obj_value_at(const FmlxJsonValue *obj, size_t i) {
    if (!obj || obj->type != FMLX_JSON_OBJ || i >= obj->n / 2u || !obj->d) return NULL;
    return &obj->d->nodes[obj->d->kids[obj->child + 2 * i + 1]];
}

const FmlxJsonValue *fmlx_json_obj_key_at(const FmlxJsonValue *obj, size_t i) {
    if (!obj || obj->type != FMLX_JSON_OBJ || i >= obj->n / 2u || !obj->d) return NULL;
    return &obj->d->nodes[obj->d->kids[obj->child + 2 * i]];
}

const FmlxJsonValue *fmlx_json_obj_get(const FmlxJsonValue *obj, const char *key) {
    if (!obj || obj->type != FMLX_JSON_OBJ || !key || !obj->d) return NULL;
    size_t klen = strlen(key);
    for (uint32_t i = 0; i + 1 < obj->n; i += 2) {
        const FmlxJsonValue *k = &obj->d->nodes[obj->d->kids[obj->child + i]];
        if (k->str_len == klen && !memcmp(obj->d->arena + k->str_off, key, klen))
            return &obj->d->nodes[obj->d->kids[obj->child + i + 1]];
    }
    return NULL;
}
