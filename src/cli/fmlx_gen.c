/* fmlx_gen.c — il motore visto da un terminale: la cronologia di una conversazione e il giro che
 * trasforma una richiesta in token stampati mentre escono.
 *
 * Perché un file a parte e non dentro `fmlx_chat.c`: `run` e `chat` sono due FACCE dello stesso
 * giro — una volta sola perché la seconda non chieda al motore qualcosa di diverso dalla prima.
 * Se lo streaming vivesse in un file e la cronologia nell'altro, una correzione al primo (un byte
 * non ancora UTF-8 completo, un errore di decode) andrebbe replicata e prima o poi divergerebbe.
 *
 * Il confine con il motore è quello di `fmlx_engine.h`: qui NON si sa nulla di HTTP né di JSON di
 * protocollo. Si costruisce il DOM dei messaggi — perché il render del chat template lo legge da lì
 * e le slice del parser puntano nel documento, quindi il documento deve restare vivo per tutta la
 * generazione — e si consumano gli eventi neutri. La richiesta `FmlxApiChat` è la STESSA struct che
 * il server riempie parsando OpenAI/Anthropic: nessun campo nuovo, nessuna scorciatoia per la CLI.
 *
 * La temperatura è 0.0 di proposito: un terminale non è un campionatore da esplorare, e
 * riprodurre la stessa risposta a fronte della stessa conversazione è ciò che rende verificabile
 * «il contesto è mantenuto». Chi vuole varietà lo chiede al server, che ha `temperature` in
 * richiesta.
 */
#include "fmlx_cli_internal.h"

#include "engine/fmlx_engine.h"

#include "fmlx/fmlx_json.h"
#include <time.h>   /* clock_gettime: tok/s misurati sulla generazione */
#include <unistd.h> /* isatty: il dim del ragionamento solo su terminale */

#include <stdlib.h>
#include <string.h>

/* ---- cronologia ---- */

void fmlx_chat_hist_init(FmlxChatHist *h) {
    if (!h) return;
    h->v = NULL;
    h->n = h->cap = 0;
}

static char *dup_cstr(const char *s) {
    size_t n = s ? strlen(s) : 0;
    char *p = malloc(n + 1);
    if (!p) return NULL;
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

int fmlx_chat_hist_push(FmlxChatHist *h, const char *role, const char *content) {
    FmlxChatTurn *nv;
    char *r, *c;

    if (!h) return -1;
    r = dup_cstr(role ? role : "");
    c = dup_cstr(content ? content : "");
    if (!r || !c) {
        free(r);
        free(c);
        return -1;
    }
    if (h->n == h->cap) {
        size_t cap = h->cap ? h->cap * 2 : 8;
        nv = realloc(h->v, cap * sizeof *nv);
        if (!nv) {
            free(r);
            free(c);
            return -1;
        }
        h->v = nv;
        h->cap = cap;
    }
    h->v[h->n].role = r;
    h->v[h->n].content = c;
    h->n++;
    return 0;
}

void fmlx_chat_hist_clear(FmlxChatHist *h) {
    size_t i;
    if (!h) return;
    for (i = 0; i < h->n; i++) {
        free(h->v[i].role);
        free(h->v[i].content);
    }
    h->n = 0; /* la capacità resta: `/clear` non deve ri-pagare la crescita a ogni comando */
}

void fmlx_chat_hist_free(FmlxChatHist *h) {
    if (!h) return;
    fmlx_chat_hist_clear(h);
    free(h->v);
    h->v = NULL;
    h->cap = 0;
}

/* ---- stringa che cresce: il testo completo della risposta ---- */

typedef struct {
    char *p;
    size_t len, cap;
} Sb;

static int sb_add(Sb *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        char *p;
        while (cap < b->len + n + 1) cap *= 2;
        p = realloc(b->p, cap);
        if (!p) return -1;
        b->p = p;
        b->cap = cap;
    }
    if (n) memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
    return 0;
}

/* ---- il DOM dei messaggi ---- */

/* Costruisce `{"system":...,"messages":[{"role":...,"content":...},...]}` come TESTO e lo ri-parsea
 * nel documento `doc`: è l'unico modo di avere nodi `FmlxJsonValue` da passare al motore senza un
 * costruttore di DOM (che il parser, di proposito, non espone — la sua API è il parse). Il writer
 * fa l'escape e rifiuta ciò che non è UTF-8 valido: un byte storto da un terminale diventa un errore
 * dichiarato, non un prompt mutilato. */
static int build_chat_doc(const char *system, const FmlxChatHist *hist, FmlxJsonDoc *doc,
                          char *err, size_t errcap) {
    FmlxJsonW w;
    FmlxJsonStatus s;
    size_t n = 0;
    int rc = -1;

    fmlx_jsonw_init(&w, NULL, 0, 1u << 24);
    do {
        if (fmlx_jsonw_obj_begin(&w) != FMLX_JSON_OK) break;
        if (system && *system) {
            if (fmlx_jsonw_key(&w, "system") != FMLX_JSON_OK ||
                fmlx_jsonw_cstr(&w, system) != FMLX_JSON_OK)
                break;
        }
        if (fmlx_jsonw_key(&w, "messages") != FMLX_JSON_OK ||
            fmlx_jsonw_arr_begin(&w) != FMLX_JSON_OK)
            break;
        for (size_t i = 0; i < hist->n; i++) {
            if (fmlx_jsonw_obj_begin(&w) != FMLX_JSON_OK ||
                fmlx_jsonw_key(&w, "role") != FMLX_JSON_OK ||
                fmlx_jsonw_cstr(&w, hist->v[i].role) != FMLX_JSON_OK ||
                fmlx_jsonw_key(&w, "content") != FMLX_JSON_OK ||
                fmlx_jsonw_cstr(&w, hist->v[i].content) != FMLX_JSON_OK ||
                fmlx_jsonw_obj_end(&w) != FMLX_JSON_OK)
                break;
        }
        if (fmlx_jsonw_arr_end(&w) != FMLX_JSON_OK || fmlx_jsonw_obj_end(&w) != FMLX_JSON_OK)
            break;
        s = fmlx_jsonw_finish(&w, &n);
        if (s != FMLX_JSON_OK) {
            snprintf(err, errcap, "the conversation does not serialize: %s",
                     fmlx_json_status_str(s));
            break;
        }
        fmlx_json_doc_init(doc);
        if (fmlx_json_parse(w.buf, n, 0, doc) != FMLX_JSON_OK) {
            snprintf(err, errcap, "the conversation could not be re-parsed as JSON");
            break;
        }
        rc = 0;
    } while (0);
    fmlx_jsonw_free(&w);
    return rc;
}

/* ---- genera e stampa ---- */

FmlxCliStatus fmlx_cli_preload(const char *cache_root, const char *name, FILE *err) {
    char e[512] = "";
    if (!err) err = stderr;
    if (!name || !*name) return FMLX_CLI_USAGE;
    if (!fmlx_engine_model(cache_root, name, e, sizeof e)) {
        fprintf(err, "fastllm: %s\n", e);
        return FMLX_CLI_FAILED;
    }
    return FMLX_CLI_OK;
}

FmlxCliStatus fmlx_cli_generate_stream(const char *cache_root, const char *name, const char *system,
                                       const FmlxChatHist *hist, long long max_tokens, int show_think,
                                       FILE *out, FILE *err, char **out_text) {
    FmlxJsonDoc doc;
    const FmlxJsonValue *root;
    FmlxApiChat chat;
    FmlxEngineModel *model;
    FmlxEngineGen *g;
    char e[512] = "";
    Sb sb = {0};
    FmlxCliStatus st = FMLX_CLI_FAILED;

    if (!out) out = stdout;
    if (!err) err = stderr;
    if (out_text) *out_text = NULL;
    if (build_chat_doc(system, hist, &doc, e, sizeof e) != 0) {
        fprintf(err, "fastllm: %s\n", e);
        return FMLX_CLI_FAILED;
    }
    root = fmlx_json_root(&doc);
    fmlx_api_chat_init(&chat);
    chat.messages = fmlx_json_obj_get(root, "messages");
    chat.system = fmlx_json_obj_get(root, "system");
    chat.temperature = 0.0;
    chat.has_temperature = 1;
    if (max_tokens > 0) {
        chat.max_tokens = max_tokens;
        chat.has_max_tokens = 1;
    }

    model = fmlx_engine_model(cache_root, name, e, sizeof e);
    if (!model) {
        fprintf(err, "fastllm: %s\n", e);
        fmlx_json_doc_free(&doc);
        return FMLX_CLI_FAILED;
    }
    g = fmlx_engine_chat(model, &chat, e, sizeof e);
    if (!g) {
        fprintf(err, "fastllm: %s\n", e);
        fmlx_json_doc_free(&doc);
        return FMLX_CLI_FAILED;
    }

    struct timespec tmv0, tmv1;
    uint64_t ptok = 0, ctok = 0;
    int have_done = 0;
    clock_gettime(CLOCK_MONOTONIC, &tmv0);

    for (;;) {
        FmlxApiEvent ev;
        FmlxApiErrCtx ec;
        FmlxApiGenSt gs;
        memset(&ev, 0, sizeof ev);
        memset(&ec, 0, sizeof ec);
        gs = fmlx_engine_next(g, &ev, &ec);
        if (gs == FMLX_API_GEN_ERR) {
            fprintf(err, "fastllm: %s\n", ec.message ? ec.message : "the engine failed");
            break;
        }
        if (gs == FMLX_API_GEN_END) {
            st = FMLX_CLI_OK;
            break;
        }
        if (ev.ev == FMLX_API_EV_THINK) {
            /* Ragionamento del modello: di default NON si stampa e NON entra nella risposta
             * (la cronologia vede solo il testo finale). Con `--think` si mostra attenuato se
             * il terminale lo supporta (ANSI dim), altrimenti in chiaro. */
            if (show_think && ev.n) {
                int tty = isatty(fileno(out));
                if (tty) fputs("\033[2m", out);
                fwrite(ev.text, 1, ev.n, out);
                if (tty) fputs("\033[0m", out);
                fflush(out);
            }
            continue;
        }
        if (ev.ev == FMLX_API_EV_TEXT) {
            /* Token-live: si scrive e si scarica subito. Un `fwrite` che aspetta la fine della
             * risposta non è uno stream, è una risposta lenta. */
            if (ev.n) fwrite(ev.text, 1, ev.n, out);
            fflush(out);
            if (ev.n && sb_add(&sb, ev.text, ev.n) != 0) {
                fprintf(err, "fastllm: out of memory collecting the response\n");
                break;
            }
        }
        if (ev.ev == FMLX_API_EV_DONE) {
            /* Contatori di fine: alimentano la riga di statistica (tok/s, contesto). */
            ptok = ev.prompt_tokens;
            ctok = ev.completion_tokens;
            have_done = 1;
        }
    }
    fputc('\n', out);
    fflush(out);
    clock_gettime(CLOCK_MONOTONIC, &tmv1);

    /* Statistica per il terminale, su stderr: in una pipe l'output del modello resta pulito. */
    if (have_done) {
        double secs = (double)(tmv1.tv_sec - tmv0.tv_sec) +
                      (double)(tmv1.tv_nsec - tmv0.tv_nsec) / 1e9;
        fprintf(err, "[%llu tok · %.1f tok/s · contesto %llu tok]\n", (unsigned long long)ctok,
                secs > 0.0 ? (double)ctok / secs : 0.0, (unsigned long long)(ptok + ctok));
        fflush(err);
    }

    fmlx_engine_gen_free(g);
    fmlx_json_doc_free(&doc);
    if (st == FMLX_CLI_OK && out_text) {
        *out_text = sb.p; /* il buffer diventa proprietà del chiamante */
    } else {
        free(sb.p);
    }
    return st;
}
