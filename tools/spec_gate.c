/* spec_gate.c — Promotion Gate per il decode speculativo (metodo Gufo), generale. Sostituisce spec_gate.py.
 *
 * Per un modello+prompt verifica i gate di promozione di una modifica al decode:
 *   1. CORRETTEZZA: output BIT-IDENTICO tra le due configurazioni (parita' greedy);
 *   2. WORKLOAD: throughput (tok/s) e speedup;
 *   3. MEMORIA: maxrss (nessuna regressione oltre soglia).
 * Il "toggle" e' una coppia di opzioni di registro: n-gram (default) --off-opts spec_ngram=0 --on-opts spec_ngram=3;
 * MTP --off-opts LLM_MTP_AUTO=0 --on-opts "". Stampa un artefatto JSON; esce 0 se PASS (parita' + memoria), 1 altrimenti.
 *
 * Uso: spec_gate [--bin B] [--off-opts a=b,c=d] [--on-opts ...] [--repeat R] [--mem-tol 0.10] [--out f.json] <modello> "<prompt>" [max_tokens]
 * Compilazione: cc -O2 tools/spec_gate.c -o build-release/spec_gate */
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAXR 16

static void quote(char *dst, size_t cap, const char *s) {   /* 'testo' con ' -> '\'' */
    size_t k = 0;
    dst[k++] = '\'';
    for (; *s && k + 6 < cap; s++) {
        if (*s == '\'') { memcpy(dst + k, "'\\''", 4); k += 4; } else dst[k++] = *s;
    }
    dst[k++] = '\'';
    dst[k] = '\0';
}

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    long n;
    char *b;
    if (!f) return strdup("");
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (char *)malloc((size_t)n + 1);
    n = (long)fread(b, 1, (size_t)n, f);
    b[n] = '\0';
    fclose(f);
    return b;
}

static uint64_t fnv1a(const char *s) { uint64_t h = 1469598103934665603ULL; for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; } return h; }

/* una esecuzione: ritorna lo stdout (malloc), tok/s (-1 se assente), maxrss in byte (-1 se assente) */
static char *run_once(const char *bin, const char *model, const char *prompt, int n, const char *opts, double *tps, long long *rss) {
    char otmp[] = "/tmp/spec_gate_out_XXXXXX", etmp[] = "/tmp/spec_gate_err_XXXXXX", cmd[16384], qm[2048], qp[8192], qo[512], optargs[4096] = "", *o, *e, *tok, *save, *copy;
    close(mkstemp(otmp));
    close(mkstemp(etmp));
    quote(qm, sizeof qm, model);
    quote(qp, sizeof qp, prompt);
    copy = strdup(opts ? opts : "");
    for (tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ') tok++;
        if (!*tok) continue;
        quote(qo, sizeof qo, tok);
        snprintf(optargs + strlen(optargs), sizeof optargs - strlen(optargs), " --opt %s", qo);
    }
    free(copy);
    snprintf(cmd, sizeof cmd, "%s%s run --opt temp=0%s %s %s --max-tokens %d > %s 2> %s",
             access("/usr/bin/time", X_OK) == 0 ? "/usr/bin/time -l " : "", bin, optargs, qm, qp, n, otmp, etmp);
    if (system(cmd) < 0) { /* esito letto dall'output */ }
    o = slurp(otmp);
    e = slurp(etmp);
    *tps = -1;
    *rss = -1;
    {
        char *p = e;
        while (*p) {   /* primo numero seguito da "tok/s" */
            double v;
            int adv = 0;
            if ((*p >= '0' && *p <= '9') && sscanf(p, "%lf tok/s%n", &v, &adv) >= 1 && adv > 0) { *tps = v; break; }
            p++;
        }
        for (char *q = strstr(e, "maximum resident set size"); q; q = NULL) {
            char *b = q;
            while (b > e && b[-1] != '\n') b--;
            *rss = atoll(b);
        }
    }
    free(e);
    unlink(otmp);
    unlink(etmp);
    return o;
}

static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
static double med(double *v, int n) {
    double t[MAXR];
    int k = 0;
    for (int i = 0; i < n; i++) if (v[i] >= 0) t[k++] = v[i];
    if (!k) return -1;
    qsort(t, (size_t)k, sizeof t[0], cmpd);
    return t[k / 2];
}

int main(int argc, char **argv) {
    const char *bin = "build-release/fastllm", *off = "spec_ngram=0", *on = "spec_ngram=3", *out = NULL, *model = NULL, *prompt = NULL;
    int repeat = 1, max_tokens = 96, npos = 0;
    double mem_tol = 0.10, t0[MAXR], t1[MAXR], r0[MAXR], r1[MAXR], b, s, m0, m1, mem_delta = 0;
    char *o0[MAXR], *o1[MAXR];
    int parity = 1, mem_ok = 1, have_mem;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bin") && i + 1 < argc) bin = argv[++i];
        else if (!strcmp(argv[i], "--off-opts") && i + 1 < argc) off = argv[++i];
        else if (!strcmp(argv[i], "--on-opts") && i + 1 < argc) on = argv[++i];
        else if (!strcmp(argv[i], "--repeat") && i + 1 < argc) repeat = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mem-tol") && i + 1 < argc) mem_tol = atof(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (argv[i][0] == '-' && argv[i][1]) { fprintf(stderr, "opzione sconosciuta: %s\n", argv[i]); return 2; }
        else if (npos == 0) { model = argv[i]; npos++; }
        else if (npos == 1) { prompt = argv[i]; npos++; }
        else { max_tokens = atoi(argv[i]); npos++; }
    }
    if (!model || !prompt) {
        fprintf(stderr, "uso: spec_gate [--bin B] [--off-opts a=b,c=d] [--on-opts ...] [--repeat R] [--mem-tol 0.10] [--out f.json] <modello> \"<prompt>\" [max_tokens]\n");
        return 2;
    }
    if (repeat < 1) repeat = 1;
    if (repeat > MAXR) repeat = MAXR;
    for (int i = 0; i < repeat; i++) {
        long long r;
        o0[i] = run_once(bin, model, prompt, max_tokens, off, &t0[i], &r); r0[i] = (double)r;
        o1[i] = run_once(bin, model, prompt, max_tokens, on, &t1[i], &r); r1[i] = (double)r;
    }
    for (int i = 0; i < repeat; i++) if (strcmp(o0[i], o0[0]) != 0 || strcmp(o1[i], o0[0]) != 0) parity = 0;
    b = med(t0, repeat); s = med(t1, repeat); m0 = med(r0, repeat); m1 = med(r1, repeat);
    have_mem = m0 > 0 && m1 > 0;
    if (have_mem) { mem_delta = (m1 - m0) / m0; mem_ok = mem_delta <= mem_tol; }

    for (int pass = 0; pass < 2; pass++) {
        FILE *w = pass == 0 ? stdout : (out ? fopen(out, "w") : NULL);
        const char *base;
        if (!w) continue;
        base = strrchr(model, '/');
        fprintf(w, "{\n \"model\": \"%s\",\n \"prompt_hash\": \"%016llx\",\n \"max_tokens\": %d,\n \"repeat\": %d,\n \"off_opts\": \"%s\",\n \"on_opts\": \"%s\",\n",
                base ? base + 1 : model, (unsigned long long)fnv1a(prompt), max_tokens, repeat, off, on);
        fprintf(w, " \"off_tok_s\": %.2f,\n \"on_tok_s\": %.2f,\n \"speedup\": %.3f,\n", b, s, (b > 0 && s > 0) ? s / b : 0.0);
        fprintf(w, " \"off_maxrss_mb\": %.1f,\n \"on_maxrss_mb\": %.1f,\n \"mem_delta_pct\": %.2f,\n", m0 > 0 ? m0 / 1048576.0 : 0.0, m1 > 0 ? m1 / 1048576.0 : 0.0, mem_delta * 100.0);
        fprintf(w, " \"parity_bit_identical\": %s,\n \"output_hash\": \"%016llx\",\n \"gate\": \"%s\"\n}\n", parity ? "true" : "false",
                (unsigned long long)fnv1a(o0[0]), (parity && mem_ok) ? "PASS" : "FAIL");
        if (pass == 1) fclose(w);
    }
    return (parity && mem_ok) ? 0 : 1;
}
