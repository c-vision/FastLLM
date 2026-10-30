/* fastllm_eval.c — banco di valutazione di qualita'/fedelta' per un server OpenAI-compatibile (FastLLM serve, mlx_lm.server).
 *
 * Sostituisce il vecchio banco in Python (tools/eval): stessa idea, stessi gate, un solo programma C. Asse ORTOGONALE alle
 * prestazioni (tools/bench_models.sh). Deterministico: nessun giudice LLM. Parla col motore SOLO via HTTP (curl di sistema).
 *
 *   gate "local":  id_parity    due run greedy dello stesso prompt -> testo identico
 *                  needle       un fatto sepolto in un contesto di rumore va recuperato
 *                  ppl          bits/token dei token greedy (/v1/completions + logprobs); banda opzionale
 *                  parity       testo greedy identico a un server di riferimento (--ref-url), altrimenti saltato
 *                  token_parity due run greedy -> stessa lista di token (logprobs.tokens)
 *   suite "halogen": factualita' con verificatore locale (le parole del fatto devono stare nella risposta)
 *
 * Uso: fastllm_eval [--suite local|halogen] [--backend http|mock|mock-vary] [--base-url URL] [--model M] [--reps N]
 *                   [--ref-url URL] [--tolerance X] [--band LOW,HIGH] [--min-recall X] [--report FILE.json]
 *                   [--save-golden FILE] [--golden FILE] [--golden-tol X]
 * Esce 0 se tutti i gate passano, 1 se uno fallisce, 2 per errori d'uso o di rete.
 *
 * Compilazione (dalla radice del repo):
 *   cc -O2 -std=gnu11 -Iinclude tools/fastllm_eval.c src/json/fmlx_json.c src/json/fmlx_jsonw.c -lm -o build-release/fastllm_eval */
#include "fmlx/fmlx_json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int mock, vary, n;
    char base[512], model[128], key[256];
} Client;

typedef struct {
    char text[8192];
    double lp[512];
    int n_lp;
    char tok[512][48];
    int n_tok;
} Reply;

/* ---- JSON in uscita ------------------------------------------------------------------------------------------------ */

static void jesc(char *out, size_t cap, const char *s) {   /* stringa JSON con virgolette */
    size_t n = fmlx_json_esc_into(s, strlen(s), out, cap);
    if (n == 0 && cap) out[0] = '\0';
}

/* ---- trasporto ----------------------------------------------------------------------------------------------------- */

static char *http_post(const Client *c, const char *path, const char *body) {
    char tmpl[] = "/tmp/fastllm_eval_XXXXXX", cmd[2048];
    int fd = mkstemp(tmpl);
    FILE *p;
    char *out = NULL;
    size_t len = 0, cap = 0;
    if (fd < 0) return NULL;
    if (write(fd, body, strlen(body)) < 0) { close(fd); unlink(tmpl); return NULL; }
    close(fd);
    if (strchr(c->base, '\'') || strchr(c->key, '\'')) { unlink(tmpl); return NULL; }
    snprintf(cmd, sizeof cmd, "curl -sS -m 1200 -H 'Content-Type: application/json' %s%s%s --data-binary @%s '%s%s' 2>/dev/null",
             c->key[0] ? "-H 'Authorization: Bearer " : "", c->key, c->key[0] ? "'" : "", tmpl, c->base, path);
    p = popen(cmd, "r");
    if (p) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, p)) > 0) {
            if (len + n + 1 > cap) { cap = (len + n + 1) * 2; out = (char *)realloc(out, cap); }
            memcpy(out + len, buf, n);
            len += n;
        }
        pclose(p);
        if (out) out[len] = '\0';
    }
    unlink(tmpl);
    return out;
}

/* ---- client: chat, completions, finto -------------------------------------------------------------------------------- */

static int mock_text(Client *c, const char *prompt, char *out, size_t cap) {
    const char *m = strstr(prompt, "access code is ");
    if (m) {
        size_t k = 0;
        m += strlen("access code is ");
        while (m[k] && (m[k] == '-' || (m[k] >= 'A' && m[k] <= 'Z') || (m[k] >= '0' && m[k] <= '9')) && k + 1 < cap) { out[k] = m[k]; k++; }
        out[k] = '\0';
    } else {
        snprintf(out, cap, "MOCK|%.40s", prompt);
    }
    if (c->vary) { size_t l = strlen(out); snprintf(out + l, cap - l, " #%d", ++c->n); }
    return 0;
}

static int client_chat(Client *c, const char *prompt, int max_tokens, Reply *r) {
    memset(r, 0, sizeof *r);
    if (c->mock) return mock_text(c, prompt, r->text, sizeof r->text);
    {
        static char body[65536], p[60000], m[300];
        char *raw;
        FmlxJsonDoc doc;
        const FmlxJsonValue *ch, *msg, *txt;
        const char *s;
        size_t sl;
        if (strlen(prompt) > 30000) return -1;
        jesc(p, sizeof p, prompt);
        jesc(m, sizeof m, c->model);
        snprintf(body, sizeof body, "{\"model\":%s,\"messages\":[{\"role\":\"user\",\"content\":%s}],\"temperature\":0,\"max_tokens\":%d,\"stream\":false}", m, p, max_tokens);
        raw = http_post(c, "/v1/chat/completions", body);
        if (!raw) return -1;
        fmlx_json_doc_init(&doc);
        if (fmlx_json_parse(raw, strlen(raw), 0, &doc) != FMLX_JSON_OK) { free(raw); return -1; }
        ch = fmlx_json_arr_at(fmlx_json_obj_get(fmlx_json_root(&doc), "choices"), 0);
        msg = ch ? fmlx_json_obj_get(ch, "message") : NULL;
        txt = msg ? fmlx_json_obj_get(msg, "content") : NULL;
        if (!txt || !fmlx_json_as_str(txt, &s, &sl)) { fmlx_json_doc_free(&doc); free(raw); return -1; }
        snprintf(r->text, sizeof r->text, "%.*s", (int)(sl < sizeof r->text - 1 ? sl : sizeof r->text - 1), s);
        fmlx_json_doc_free(&doc);
        free(raw);
    }
    return 0;
}

static int client_complete(Client *c, const char *prompt, int max_tokens, Reply *r) {
    memset(r, 0, sizeof *r);
    if (c->mock) {
        char *w;
        mock_text(c, prompt, r->text, sizeof r->text);
        {   /* token = parole del testo */
            char tmp[8192];
            snprintf(tmp, sizeof tmp, "%s", r->text);
            for (w = strtok(tmp, " "); w && r->n_tok < 512; w = strtok(NULL, " ")) {
                snprintf(r->tok[r->n_tok], sizeof r->tok[0], "%s", w);
                r->lp[r->n_lp++] = -0.2;
                r->n_tok++;
            }
        }
        return 0;
    }
    {
        static char body[65536], p[60000], m[300];
        char *raw;
        FmlxJsonDoc doc;
        const FmlxJsonValue *ch, *lp, *a, *t;
        const char *s;
        size_t sl;
        if (strlen(prompt) > 30000) return -1;
        jesc(p, sizeof p, prompt);
        jesc(m, sizeof m, c->model);
        snprintf(body, sizeof body, "{\"model\":%s,\"prompt\":%s,\"max_tokens\":%d,\"temperature\":0,\"stream\":false,\"logprobs\":1}", m, p, max_tokens);
        raw = http_post(c, "/v1/completions", body);
        if (!raw) return -1;
        fmlx_json_doc_init(&doc);
        if (fmlx_json_parse(raw, strlen(raw), 0, &doc) != FMLX_JSON_OK) { free(raw); return -1; }
        ch = fmlx_json_arr_at(fmlx_json_obj_get(fmlx_json_root(&doc), "choices"), 0);
        if (!ch) { fmlx_json_doc_free(&doc); free(raw); return -1; }
        t = fmlx_json_obj_get(ch, "text");
        if (t && fmlx_json_as_str(t, &s, &sl))
            snprintf(r->text, sizeof r->text, "%.*s", (int)(sl < sizeof r->text - 1 ? sl : sizeof r->text - 1), s);
        lp = fmlx_json_obj_get(ch, "logprobs");
        if (lp && !fmlx_json_is_null(lp)) {
            a = fmlx_json_obj_get(lp, "token_logprobs");
            for (size_t i = 0; a && i < fmlx_json_arr_n(a) && r->n_lp < 512; i++) {
                double d;
                if (fmlx_json_as_f64(fmlx_json_arr_at(a, i), &d)) r->lp[r->n_lp++] = d;
            }
            a = fmlx_json_obj_get(lp, "tokens");
            for (size_t i = 0; a && i < fmlx_json_arr_n(a) && r->n_tok < 512; i++)
                if (fmlx_json_as_str(fmlx_json_arr_at(a, i), &s, &sl)) {
                    snprintf(r->tok[r->n_tok], sizeof r->tok[0], "%.*s", (int)(sl < 47 ? sl : 47), s);
                    r->n_tok++;
                }
        }
        fmlx_json_doc_free(&doc);
        free(raw);
    }
    return 0;
}

/* ---- gate ----------------------------------------------------------------------------------------------------------- */

typedef struct {
    char gate[24], metric[32];
    double value;      /* bool = 0/1; NAN = nessun valore */
    int is_bool, pass;
    char note[200];
} Gate;

typedef struct {
    Client eng, ref;
    int have_ref, reps;
    double tol, min_recall, band_lo, band_hi;
    int have_band, have_min_recall, max_tokens;
} Cfg;

static const char *k_prompts[] = { "The capital of France is", "1, 2, 3,", "def add(a, b):" };

static void gate_id_parity(Cfg *g, Gate *o) {
    int all = 1;
    snprintf(o->gate, sizeof o->gate, "id_parity");
    snprintf(o->metric, sizeof o->metric, "id_parity");
    o->is_bool = 1;
    for (size_t i = 0; i < 3; i++) {
        Reply a, b;
        if (client_chat(&g->eng, k_prompts[i], 64, &a) != 0 || client_chat(&g->eng, k_prompts[i], 64, &b) != 0) { snprintf(o->note, sizeof o->note, "errore di rete"); all = 0; break; }
        if (strcmp(a.text, b.text) != 0) { all = 0; snprintf(o->note, sizeof o->note, "diverso su '%s'", k_prompts[i]); }
    }
    o->value = all;
    o->pass = all;
}

static void gate_needle(Cfg *g, Gate *o) {
    static char prompt[16384];
    const char *key = "PLUMBUS-7741";
    size_t off = 0;
    Reply r;
    snprintf(o->gate, sizeof o->gate, "needle");
    snprintf(o->metric, sizeof o->metric, "needle");
    o->is_bool = 1;
    for (int i = 0; i < 40; i++) {
        if (i == 20) off += (size_t)snprintf(prompt + off, sizeof prompt - off, "[20] The secret access code is %s.\n", key);
        else off += (size_t)snprintf(prompt + off, sizeof prompt - off,
                                     "[%d] The archive log entry records a routine system check with no anomalies. Operators reviewed the telemetry and documented the sequence for audit.\n", i);
    }
    snprintf(prompt + off, sizeof prompt - off, "\nWhat is the secret access code? Answer with only the code.");
    if (client_chat(&g->eng, prompt, 16, &r) != 0) { snprintf(o->note, sizeof o->note, "errore di rete"); o->pass = 0; return; }
    o->value = strstr(r.text, key) != NULL;
    o->pass = (int)o->value;
    snprintf(o->note, sizeof o->note, "risposta: %.120s", r.text);
}

static void gate_ppl(Cfg *g, Gate *o) {
    static const char *prompt = "The capital of France is Paris. The quick brown fox jumps over the lazy dog. Water boils at one hundred degrees Celsius at sea level.";
    Reply r;
    double sum = 0;
    snprintf(o->gate, sizeof o->gate, "ppl");
    snprintf(o->metric, sizeof o->metric, "ppl_bits_per_token");
    o->value = NAN;
    o->pass = 1;
    if (client_complete(&g->eng, prompt, 32, &r) != 0) { snprintf(o->note, sizeof o->note, "errore di rete"); o->pass = 0; return; }
    if (r.n_lp == 0) { snprintf(o->note, sizeof o->note, "nessuna logprob dal server (gate informativo)"); return; }
    for (int i = 0; i < r.n_lp; i++) sum += -r.lp[i] / log(2.0);
    o->value = sum / r.n_lp;
    if (g->have_band) o->pass = o->value >= g->band_lo && o->value <= g->band_hi;
    snprintf(o->note, sizeof o->note, "%d token", r.n_lp);
}

static int lcp(const char *a, const char *b) { int i = 0; while (a[i] && a[i] == b[i]) i++; return i; }

static void gate_parity(Cfg *g, Gate *o) {
    int all = 1;
    snprintf(o->gate, sizeof o->gate, "parity");
    snprintf(o->metric, sizeof o->metric, "parity");
    o->is_bool = 1;
    o->pass = 1;
    if (!g->have_ref) { o->value = NAN; snprintf(o->note, sizeof o->note, "reference non configurata (saltato)"); return; }
    for (size_t i = 0; i < 3; i++) {
        Reply a, b;
        int l, mn;
        if (client_chat(&g->eng, k_prompts[i], 64, &a) != 0 || client_chat(&g->ref, k_prompts[i], 64, &b) != 0) { all = 0; snprintf(o->note, sizeof o->note, "errore di rete"); break; }
        l = lcp(a.text, b.text);
        mn = (int)strlen(a.text) < (int)strlen(b.text) ? (int)strlen(a.text) : (int)strlen(b.text);
        if (!(strcmp(a.text, b.text) == 0 || (double)l / (mn > 0 ? mn : 1) >= g->tol)) all = 0;
    }
    o->value = all;
    o->pass = all;
}

static void gate_token_parity(Cfg *g, Gate *o) {
    int all = 1, have = 0;
    snprintf(o->gate, sizeof o->gate, "token_parity");
    snprintf(o->metric, sizeof o->metric, "token_parity");
    o->is_bool = 1;
    for (size_t i = 0; i < 2; i++) {
        Reply a, b;
        if (client_complete(&g->eng, k_prompts[i == 0 ? 0 : 2], 64, &a) != 0 || client_complete(&g->eng, k_prompts[i == 0 ? 0 : 2], 64, &b) != 0) { all = 0; snprintf(o->note, sizeof o->note, "errore di rete"); break; }
        if (a.n_tok == 0 && b.n_tok == 0) continue;
        have = 1;
        if (a.n_tok != b.n_tok) all = 0;
        else for (int k = 0; k < a.n_tok; k++) if (strcmp(a.tok[k], b.tok[k]) != 0) { all = 0; break; }
    }
    if (!have && all) { o->value = NAN; o->pass = 1; snprintf(o->note, sizeof o->note, "il server non espone i token (saltato)"); return; }
    o->value = all;
    o->pass = all;
}

/* verificatore locale: tutte le parole (>2 lettere) del fatto compaiono nella risposta, senza distinguere maiuscole */
static int words_in(const char *claim, const char *ctx) {
    char c2[8192], w[64];
    size_t k = 0;
    int any = 0;
    for (size_t i = 0; ctx[i] && i + 1 < sizeof c2; i++) c2[i] = (char)((ctx[i] >= 'A' && ctx[i] <= 'Z') ? ctx[i] + 32 : ctx[i]), c2[i + 1] = '\0';
    for (const char *p = claim;; p++) {
        char ch = *p;
        int alnum = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '\'' || (unsigned char)ch >= 0x80;
        if (alnum) { if (k < sizeof w - 1) w[k++] = (char)((ch >= 'A' && ch <= 'Z') ? ch + 32 : ch); }
        else {
            if (k > 2) { w[k] = '\0'; any = 1; if (!strstr(c2, w)) return 0; }
            k = 0;
            if (!ch) break;
        }
    }
    return any;
}

static void suite_halogen(Cfg *g, Gate *o) {
    static const struct { const char *q, *fact; } cases[] = {
        { "What is the capital of France? Answer with just the city.", "Paris" },
        { "How many days are in a leap year? Answer with just the number.", "366" },
        { "What gas do plants absorb from the atmosphere? Answer with just the name.", "carbon dioxide" },
    };
    int total = 0, ok = 0;
    snprintf(o->gate, sizeof o->gate, "halogen");
    snprintf(o->metric, sizeof o->metric, "halogen_recall");
    o->pass = 1;
    for (size_t i = 0; i < 3; i++) {
        Reply r;
        total++;
        if (client_chat(&g->eng, cases[i].q, 40, &r) != 0) { o->pass = 0; snprintf(o->note, sizeof o->note, "errore di rete"); return; }
        if (words_in(cases[i].fact, r.text)) ok++;
    }
    o->value = (double)ok / total;
    if (g->have_min_recall) o->pass = o->value >= g->min_recall;
    snprintf(o->note, sizeof o->note, "%d/%d fatti verificati", ok, total);
}

/* ---- golden (testo: una riga `gate valore`) ------------------------------------------------------------------------- */

static void save_golden(const char *path, const Gate *gs, int n) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < n; i++) fprintf(f, "%s %.6f\n", gs[i].gate, gs[i].value);
    fclose(f);
}

static int compare_golden(const char *path, const Gate *gs, int n, double tol) {
    FILE *f = fopen(path, "r");
    char name[32];
    double b;
    int ok = 1;
    if (!f) return -1;
    while (fscanf(f, "%31s %lf", name, &b) == 2)
        for (int i = 0; i < n; i++)
            if (!strcmp(gs[i].gate, name)) {
                int same = (isnan(gs[i].value) && isnan(b)) ||
                           (!isnan(gs[i].value) && !isnan(b) && fabs(gs[i].value - b) / fmax(1.0, fabs(b)) <= tol);
                if (!same) { printf("[eval] golden diverso: %s (golden %.4f, ora %.4f)\n", name, b, gs[i].value); ok = 0; }
            }
    fclose(f);
    return ok;
}

/* ---- main ----------------------------------------------------------------------------------------------------------- */

static void usage(void) {
    fprintf(stderr, "uso: fastllm_eval [--suite local|halogen] [--backend http|mock|mock-vary] [--base-url URL] [--model M] [--reps N]\n"
                    "                  [--ref-url URL] [--tolerance X] [--band LOW,HIGH] [--min-recall X] [--report FILE.json]\n"
                    "                  [--save-golden FILE] [--golden FILE] [--golden-tol X]\n");
}

int main(int argc, char **argv) {
    Cfg g;
    static Gate gs[64];
    int n = 0, overall = 1, halogen = 0, mock = 1;
    const char *report = NULL, *save = NULL, *golden = NULL;
    double gtol = 0.05;
    memset(&g, 0, sizeof g);
    snprintf(g.eng.base, sizeof g.eng.base, "http://127.0.0.1:8080");
    snprintf(g.eng.model, sizeof g.eng.model, "local");
    g.reps = 1;
    g.tol = 1.0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
#define OPT(name) (!strcmp(a, name) && v && (i++, 1))
        if (OPT("--suite")) halogen = !strcmp(v, "halogen");
        else if (OPT("--backend")) { mock = strncmp(v, "mock", 4) == 0; g.eng.vary = !strcmp(v, "mock-vary"); }
        else if (OPT("--base-url")) snprintf(g.eng.base, sizeof g.eng.base, "%s", v);
        else if (OPT("--model")) snprintf(g.eng.model, sizeof g.eng.model, "%s", v);
        else if (OPT("--reps")) g.reps = atoi(v) > 0 ? atoi(v) : 1;
        else if (OPT("--ref-url")) { g.have_ref = 1; snprintf(g.ref.base, sizeof g.ref.base, "%s", v); snprintf(g.ref.model, sizeof g.ref.model, "local"); }
        else if (OPT("--tolerance")) g.tol = atof(v);
        else if (OPT("--band")) g.have_band = sscanf(v, "%lf,%lf", &g.band_lo, &g.band_hi) == 2;
        else if (OPT("--min-recall")) { g.have_min_recall = 1; g.min_recall = atof(v); }
        else if (OPT("--report")) report = v;
        else if (OPT("--save-golden")) save = v;
        else if (OPT("--golden")) golden = v;
        else if (OPT("--golden-tol")) gtol = atof(v);
        else { usage(); return 2; }
#undef OPT
    }
    g.eng.mock = mock;
    if (g.reps > 12) g.reps = 12;
    for (int rep = 0; rep < g.reps; rep++) {
        int k0 = n;
        if (halogen) suite_halogen(&g, &gs[n++]);
        else {
            gate_id_parity(&g, &gs[n++]);
            gate_needle(&g, &gs[n++]);
            gate_ppl(&g, &gs[n++]);
            gate_parity(&g, &gs[n++]);
            gate_token_parity(&g, &gs[n++]);
        }
        for (int i = k0; i < n; i++) {
            printf("[eval] %-12s %s  (%s=%.4f)%s%s\n", gs[i].gate, gs[i].pass ? "PASS" : "FAIL", gs[i].metric,
                   isnan(gs[i].value) ? 0.0 : gs[i].value, gs[i].note[0] ? "  " : "", gs[i].note);
            overall = overall && gs[i].pass;
        }
    }
    if (golden) {
        int r = compare_golden(golden, gs, n, gtol);
        printf("[eval] golden %s\n", r < 0 ? "non leggibile" : (r ? "PASS" : "FAIL"));
        if (r <= 0) overall = 0;
    }
    if (save) { save_golden(save, gs, n); printf("[eval] golden salvato: %s\n", save); }
    if (report) {
        FILE *f = fopen(report, "w");
        if (f) {
            fprintf(f, "{\n  \"generated\": %ld,\n  \"suite\": \"%s\",\n  \"pass\": %s,\n  \"gates\": [", (long)time(NULL), halogen ? "halogen" : "local", overall ? "true" : "false");
            for (int i = 0; i < n; i++)
            {
                char val[40];
                if (isnan(gs[i].value)) snprintf(val, sizeof val, "null"); else snprintf(val, sizeof val, "%.6f", gs[i].value);
                fprintf(f, "%s\n    {\"gate\": \"%s\", \"pass\": %s, \"metric\": \"%s\", \"value\": %s}", i ? "," : "", gs[i].gate,
                        gs[i].pass ? "true" : "false", gs[i].metric, val);
            }
            fprintf(f, "\n  ]\n}\n");
            fclose(f);
        }
    }
    printf("[eval] overall: %s\n", overall ? "PASS" : "FAIL");
    return overall ? 0 : 1;
}
