/* mut_prove.c — prova che un'asserzione ROSSA lo sia davvero (mutation check). Sostituisce mut_prove.py.
 *
 * Un controllo che non e' mai stato visto fallire non prova niente. Questo programma fa il secondo passo del metodo usato sui casi
 * nuovi di `test_pull`/`test_cli`:
 *   1. copia il sorgente toccato in <build>/probe-<tag>/mut_<nome>.c (NON tocca l'originale);
 *   2. applica sostituzioni letterali, ciascuna deve combaciare ESATTAMENTE UNA VOLTA (altrimenti il mutante sarebbe costruito a
 *      mano e la prova falsa: si ferma);
 *   3. compila il mutante e lo mette PRIMA dell'archivio (libfmlx_cli.a): il .o definisce gia' i simboli di quel membro, che quindi
 *      non viene prelevato, e a girare e' il codice mutato;
 *   4. esegue il binario e riporta QUALI asserzioni sono diventate rosse. Un mutante che non ne cambia nessuna e' "VERDE-MUTO":
 *      il caso sotto accusa va riscritto (esce 1).
 *
 * Uso (dalla radice del repo):
 *   mut_prove --target test_pull --tag dedupe --src src/cli/fmlx_pull_progress.c --name m1 --sub 'VECCHIO' 'NUOVO' [--sub ...]
 *   mut_prove --target test_pull --tag dedupe --spec tests/mutants/x.json  (JSON: [{name, src, sub: [[vecchio, nuovo], ...]}, ...])
 *   mut_prove --target test_pull --tag dedupe --list                  (rilancia i mutanti salvati)
 * Opzioni: --build DIR (default build), --lib NOME (libreria CMake del sorgente, default fmlx_cli), --timeout S (default 60). Compilazione: cc -O2 tools/mut_prove.c -o build/mut_prove */
#include "fmlx/fmlx_json.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAXSUB 32
#define MAXMUT 64

typedef struct { char name[64], src[512]; char *old[MAXSUB], *neu[MAXSUB]; int nsub; } Mutant;

static void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
#include <stdarg.h>
static void die(const char *fmt, ...) {
    va_list ap;
    fputs("mut_prove: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(2);
}

static char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    char *b;
    long len;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (char *)malloc((size_t)len + 1);
    len = (long)fread(b, 1, (size_t)len, f);
    b[len] = '\0';
    fclose(f);
    if (n) *n = (size_t)len;
    return b;
}


static int count_occ(const char *h, const char *n) {
    int c = 0;
    size_t l = strlen(n);
    if (!l) return 0;
    for (const char *p = h; (p = strstr(p, n)) != NULL; p += l) c++;
    return c;
}

/* link.txt: tokenizza con le virgolette della shell; separa flag, oggetti, librerie (il mutante va fra oggetti e librerie) */
static int tokenize(char *line, char **tok, int cap) {
    int n = 0;
    char *p = line;
    while (*p && n < cap) {
        char *w;
        while (*p == ' ' || *p == '\n' || *p == '\t') p++;
        if (!*p) break;
        w = tok[n++] = p;
        for (char *r = p, *d = p;; r++) {
            if (*r == '\'' || *r == '"') { char q = *r++; while (*r && *r != q) *d++ = *r++; if (*r) continue; }
            else if (*r == '\\' && r[1]) { *d++ = *++r; continue; }
            else if (*r == ' ' || *r == '\n' || *r == '\t' || !*r) { p = *r ? r + 1 : r; *d = '\0'; break; }
            else *d++ = *r;
        }
        (void)w;
    }
    return n;
}

static int run_timeout(const char *exe, const char *work, int secs, const char *outfile) {
    pid_t pid = fork();
    int st = 0;
    if (pid < 0) return -1;
    if (pid == 0) {
        int fd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
        execl(exe, exe, work, (char *)NULL);
        _exit(127);
    }
    for (int i = 0; i < secs * 10; i++) {
        if (waitpid(pid, &st, WNOHANG) == pid) return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
        usleep(100000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);
    return -1;
}

int main(int argc, char **argv) {
    const char *target = NULL, *tag = NULL, *src = NULL, *name = NULL, *spec = NULL, *build = "build", *lib = "fmlx_cli";
    int list = 0, timeout = 60, nm = 0, rc_all = 0;
    static Mutant mut[MAXMUT];
    char outdir[512], specfile[600], linkp[600], exe[600], incs[8192];
    Mutant cur;

    memset(&cur, 0, sizeof cur);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--target") && i + 1 < argc) target = argv[++i];
        else if (!strcmp(argv[i], "--tag") && i + 1 < argc) tag = argv[++i];
        else if (!strcmp(argv[i], "--src") && i + 1 < argc) src = argv[++i];
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "--spec") && i + 1 < argc) spec = argv[++i];
        else if (!strcmp(argv[i], "--build") && i + 1 < argc) build = argv[++i];
        else if (!strcmp(argv[i], "--lib") && i + 1 < argc) lib = argv[++i];
        else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) timeout = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--list")) list = 1;
        else if (!strcmp(argv[i], "--sub") && i + 2 < argc && cur.nsub < MAXSUB) { cur.old[cur.nsub] = argv[i + 1]; cur.neu[cur.nsub] = argv[i + 2]; cur.nsub++; i += 2; }
        else die("argomento sconosciuto: %s", argv[i]);
    }
    if (!target || !tag) die("servono --target e --tag");
    snprintf(outdir, sizeof outdir, "%s/probe-%s", build, tag);
    snprintf(specfile, sizeof specfile, "%s/spec.json", outdir);
    { char cmd[700]; snprintf(cmd, sizeof cmd, "mkdir -p '%s'", outdir); if (system(cmd) != 0) die("cartella %s non creabile", outdir); }

    if (list || spec) {   /* file JSON: [{"name": ..., "src": ..., "sub": [["vecchio", "nuovo"], ...]}, ...] */
        const char *path = list ? specfile : spec;
        char *txt = slurp(path, NULL);
        FmlxJsonDoc doc;
        const FmlxJsonValue *root;
        if (!txt) die("nessun %s", path);
        fmlx_json_doc_init(&doc);
        if (fmlx_json_parse(txt, strlen(txt), 0, &doc) != FMLX_JSON_OK) die("%s non e' JSON valido", path);
        root = fmlx_json_root(&doc);
        for (size_t i = 0; i < fmlx_json_arr_n(root) && nm < MAXMUT; i++) {
            const FmlxJsonValue *e = fmlx_json_arr_at(root, i), *subs = fmlx_json_obj_get(e, "sub");
            const char *s;
            size_t sl;
            Mutant *m = &mut[nm];
            if (!fmlx_json_as_str(fmlx_json_obj_get(e, "name"), &s, &sl)) continue;
            snprintf(m->name, sizeof m->name, "%.*s", (int)sl, s);
            if (!fmlx_json_as_str(fmlx_json_obj_get(e, "src"), &s, &sl)) continue;
            snprintf(m->src, sizeof m->src, "%.*s", (int)sl, s);
            for (size_t k = 0; k < fmlx_json_arr_n(subs) && m->nsub < MAXSUB; k++) {
                const FmlxJsonValue *pair = fmlx_json_arr_at(subs, k);
                const char *a, *b;
                size_t la, lb;
                if (!fmlx_json_as_str(fmlx_json_arr_at(pair, 0), &a, &la) || !fmlx_json_as_str(fmlx_json_arr_at(pair, 1), &b, &lb)) continue;
                m->old[m->nsub] = strndup(a, la);
                m->neu[m->nsub] = strndup(b, lb);
                m->nsub++;
            }
            nm++;
        }
        if (spec && !list) { char cmd[1400]; snprintf(cmd, sizeof cmd, "cp '%s' '%s'", spec, specfile); if (system(cmd) != 0) { } }
    } else {
        if (!src || !name || !cur.nsub) die("servono --src --name e almeno una --sub (oppure --spec/--list)");
        snprintf(cur.name, sizeof cur.name, "%s", name);
        snprintf(cur.src, sizeof cur.src, "%s", src);
        mut[nm++] = cur;
    }

    snprintf(linkp, sizeof linkp, "%s/CMakeFiles/%s.dir/link.txt", build, target);
    {   /* include dirs del membro mutato: quelli veri della libreria CMake che lo contiene (flags.make) */
        char fm[700], *ft, *p;
        snprintf(fm, sizeof fm, "%s/CMakeFiles/%s.dir/flags.make", build, lib);
        ft = slurp(fm, NULL);
        if (!ft) die("manca %s (--lib NOME della libreria che contiene il sorgente)", fm);
        p = strstr(ft, "C_INCLUDES = ");
        if (!p) die("nessun C_INCLUDES in %s", fm);
        p += strlen("C_INCLUDES = ");
        snprintf(incs, sizeof incs, "-I include -I src %.*s", (int)strcspn(p, "\n"), p);
        free(ft);
    }
    {
        char *ltxt = slurp(linkp, NULL), *tok[512], flags[8192] = "", objs[8192] = "", libs[8192] = "";
        int nt;
        if (!ltxt) die("manca %s: configurare la build prima", linkp);
        nt = tokenize(ltxt, tok, 512);
        for (int i = 1; i < nt; i++) {   /* il primo token e' il compilatore */
            const char *t = tok[i];
            char piece[700];
            size_t l = strlen(t);
            if (!strcmp(t, "-o")) { i++; continue; }
            int takes_arg = i > 1 && (!strcmp(tok[i - 1], "-arch") || !strcmp(tok[i - 1], "-isysroot") || !strcmp(tok[i - 1], "-framework") ||
                                       !strcmp(tok[i - 1], "-rpath") || !strcmp(tok[i - 1], "-install_name") || !strcmp(tok[i - 1], "-Xlinker"));
            if (t[0] == '-' || t[0] == '/' || takes_arg) snprintf(piece, sizeof piece, " '%s'", t);
            else snprintf(piece, sizeof piece, " '%s/%s'", build, t);
            if (l > 2 && !strcmp(t + l - 2, ".o")) strncat(objs, piece, sizeof objs - strlen(objs) - 1);
            else if ((l > 2 && !strcmp(t + l - 2, ".a")) || (l > 4 && !strcmp(t + l - 4, ".tbd")) || !strncmp(t, "-l", 2)) strncat(libs, piece, sizeof libs - strlen(libs) - 1);
            else strncat(flags, piece, sizeof flags - strlen(flags) - 1);
        }
        if (!objs[0] || !libs[0]) die("riga di link non riconosciuta in %s", linkp);
        snprintf(exe, sizeof exe, "%s/test_%s_mut", build, !strncmp(target, "test_", 5) ? target + 5 : target);
        for (int mi = 0; mi < nm; mi++) {
            Mutant *m = &mut[mi];
            char mutc[700], muto[700], cmd[20000], thisexe[800], work[700], outf[800];
            char *text = slurp(m->src, NULL), *res;
            int rc;
            if (!text) die("sorgente non leggibile: %s", m->src);
            res = text;
            for (int s = 0; s < m->nsub; s++) {
                int c = count_occ(res, m->old[s]);
                char *p, *nres;
                size_t lo = strlen(m->old[s]);
                if (c != 1) die("la sostituzione per '%s' combacia %d volte in %s (serve 1): il mutante sarebbe ambiguo", m->name, c, m->src);
                p = strstr(res, m->old[s]);
                nres = (char *)malloc(strlen(res) + strlen(m->neu[s]) + 1);
                memcpy(nres, res, (size_t)(p - res));
                strcpy(nres + (p - res), m->neu[s]);
                strcpy(nres + (p - res) + strlen(m->neu[s]), p + lo);
                if (res != text) free(res);
                res = nres;
            }
            if (!strcmp(res, text)) die("nessuna sostituzione applicata per '%s'", m->name);
            snprintf(mutc, sizeof mutc, "%s/mut_%s.c", outdir, m->name);
            snprintf(muto, sizeof muto, "%s/mut_%s.o", outdir, m->name);
            { FILE *f = fopen(mutc, "w"); if (!f) die("scrittura %s", mutc); fputs(res, f); fclose(f); }
            char sdir[512];
            snprintf(sdir, sizeof sdir, "%s", m->src);
            { char *sl = strrchr(sdir, '/'); if (sl) *sl = '\0'; else strcpy(sdir, "."); }   /* gli header privati stanno accanto al sorgente */
            snprintf(cmd, sizeof cmd, "cc -g -std=gnu17 -Wall -Wextra -Werror %s -I '%s' -c '%s' -o '%s'", incs, sdir, mutc, muto);
            if (system(cmd) != 0) die("il mutante '%s' NON compila: -Werror ha detto qualcosa che il codice vero non dice", m->name);
            snprintf(thisexe, sizeof thisexe, "%s.%s", exe, m->name);
            snprintf(cmd, sizeof cmd, "cc%s%s '%s'%s -o '%s'", flags, objs, muto, libs, thisexe);
            if (system(cmd) != 0) die("link del mutante '%s' fallita", m->name);
            snprintf(work, sizeof work, "%s/w-%s", outdir, m->name);
            snprintf(cmd, sizeof cmd, "mkdir -p '%s'", work);
            if (system(cmd) != 0) { }
            snprintf(outf, sizeof outf, "%s/out-%s.txt", outdir, m->name);
            rc = run_timeout(thisexe, work, timeout, outf);
            {
                char *out = slurp(outf, NULL), *save2, *ln;
                int fired = 0;
                if (rc == -1) printf("== %s: BLOCCATO dopo %ds: il mutante impalla il test (pty piena, wait, rete)\n", m->name, timeout);
                else {
                    for (ln = out ? strtok_r(out, "\n", &save2) : NULL; ln; ln = strtok_r(NULL, "\n", &save2)) if (strstr(ln, "FALLIMENTO")) fired++;
                    printf("== %s: %s\n", m->name, fired ? "FALLISCE (prova valida)" : "VERDE-MUTO: nessuna asserzione e' cambiata");
                    free(out);
                    out = slurp(outf, NULL);
                    for (ln = out ? strtok_r(out, "\n", &save2) : NULL; ln; ln = strtok_r(NULL, "\n", &save2))
                        if (strstr(ln, "FALLIMENTO") || strstr(ln, "verifiche")) printf("   %s\n", ln);
                    if (!fired) rc_all = 1;
                }
                free(out);
            }
        }
    }
    return rc_all;
}
