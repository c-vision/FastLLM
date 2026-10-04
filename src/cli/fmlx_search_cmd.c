/* fmlx_search_cmd.c — `fastllm search [<path>]`: trova le cartelle che contengono modelli LLM.
 *
 * Senza <path> cerca su TUTTI i dischi accessibili:
 *   - i percorsi di `fastllm paths` (config),
 *   - $HOME,
 *   - ogni punto di mount di `getmntinfo` (interni ed esterni).
 *
 * OTTIMIZZAZIONE (l'obiettivo dichiarato):
 *   - una sola lettura di ogni directory (`readdir`) e NESSUN `stat`: il tipo lo dà `d_type`;
 *   - PODA: appena una directory è riconosciuta come modello non ci si scende dentro
 *     (i pesi/snapshot di un modello non contengono altri modelli);
 *   - skip-list di nomi notoriamente estranei (System, .git, node_modules, build, ...);
 *   - tetto di profondità (--max-depth) e tetto di risultati.
 *
 * Una directory è un modello se contiene `config.json` (MLX/HF) o almeno un file `*.gguf`,
 * oppure se il suo nome ha la forma del repo-cache HF (`models--org--nome`). */
#include "fmlx/fmlx_cli.h"
#include "fmlx/fmlx_hf.h"
#include "fmlx/fmlx_json.h"
#include "fmlx/fmlx_paths.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp */
#include <sys/mount.h>
#include <sys/stat.h> /* stat: dimensione dei soli file candidati */
#include <time.h>

#define SR_SEARCH_MAX_DEPTH 7
#define SR_RESULT_MAX 4096
#define SR_SKIP_MAX 40

typedef struct { char path[FMLX_PATHS_DIR_MAX]; char kind[8]; } SrHit;
typedef struct {
    SrHit *v;
    int n, cap;
    const char *skip[SR_SKIP_MAX];
    int nskip;
    int maxdepth;
    char (*roots)[FMLX_PATHS_DIR_MAX]; /* le radici di ricerca (per il tree e per --roots) */
    int nroots;
} SrCtx;

static int sr_is_gguf(const char *n) { size_t l = strlen(n); return l > 5 && strcmp(n + l - 5, ".gguf") == 0; }
static int sr_is_hf_repo(const char *n) { return strncmp(n, "models--", 8) == 0; }

static int sr_skip(const SrCtx *c, const char *n) {
    int i;
    for (i = 0; i < c->nskip; i++)
        if (strcmp(c->skip[i], n) == 0) return 1;
    return 0;
}

static void sr_add(SrCtx *c, const char *path, const char *kind) {
    int i;
    if (c->n >= c->cap) return;
    for (i = 0; i < c->n; i++) if (strcmp(c->v[i].path, path) == 0) return; /* dedup */
    snprintf(c->v[c->n].path, FMLX_PATHS_DIR_MAX, "%s", path);
    snprintf(c->v[c->n].kind, sizeof c->v[c->n].kind, "%s", kind);
    c->n++;
}

/* Sotto 1 MB non e' un modello ma un artefatto (vocab, probe, fixture "tiny"). Il `stat` si fa
 * SOLO sui candidati (nome con estensione di pesi), non su ogni file: il costo e' trascurabile. */
#define SR_MIN_BYTES ((long long)1 << 20)
static int sr_file_big(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && (long long)st.st_size >= SR_MIN_BYTES;
}
static int sr_is_vocab(const char *name) { return strncmp(name, "ggml-vocab-", 11) == 0; }
/* Pesi veri (NON `.gguf`, che ha una via sua con il controllo vocabolario). */
static int sr_is_weight_name(const char *name) {
    static const char *const ext[] = {".safetensors", ".bin", ".npz", ".pth", ".ckpt", NULL};
    size_t l = strlen(name);
    int i;
    for (i = 0; ext[i]; i++) { size_t e = strlen(ext[i]); if (l > e && strcasecmp(name + l - e, ext[i]) == 0) return 1; }
    return 0;
}
static int sr_dir_has_weights(const char *dir) {
    DIR *d = opendir(dir);
    struct dirent *e;
    int hit = 0;
    if (!d) return 0;
    while ((e = readdir(d))) {
        char full[FMLX_PATHS_DIR_MAX];
        if (e->d_name[0] == '.' || e->d_type == DT_DIR) continue;
        if (!sr_is_weight_name(e->d_name)) continue;
        if (snprintf(full, sizeof full, "%s/%s", dir, e->d_name) < (int)sizeof full && sr_file_big(full)) { hit = 1; break; }
    }
    closedir(d);
    return hit;
}
/* Un .gguf valido come modello: non vocab e abbastanza grande. */
static int sr_gguf_ok(const char *path, const char *name) { return !sr_is_vocab(name) && sr_file_big(path); }

/* La dir-modello a partire dal PERCORSO DI UN FILE trovato (config.json o *.gguf): il suo parent.
 * Per un blob dentro un repo-cache HF (`.../models--org--nome/snapshots/<sha>/...`) risale al repo
 * (`kind` = "hf"). Per config.json si richiede anche un file di pesi (altrimenti e' un falso
 * positivo: qualunque progetto ha un config.json). */
static void sr_add_from_file(SrCtx *c, const char *file, const char *kind, int require_weights) {
    char dir[FMLX_PATHS_DIR_MAX];
    char *sl, *snap;

    snprintf(dir, sizeof dir, "%s", file);
    sl = strrchr(dir, '/');
    if (!sl) return;
    *sl = 0;
    snap = strstr(dir, "/snapshots/");
    if (snap) {
        char *repo = dir;
        while (repo > dir && strncmp(repo, "/models--", 9) != 0) repo--;
        if (strncmp(repo, "/models--", 9) == 0) { *repo = 0; sr_add(c, dir, "hf"); return; }
    }
    if (require_weights) {
        if (!sr_dir_has_weights(dir)) return;         /* config.json da solo: non basta */
        sr_add(c, dir, kind);                          /* dir MLX/HF = il modello */
    } else {
        const char *base = strrchr(file, '/'); base = base ? base + 1 : file;
        if (!sr_gguf_ok(file, base)) return;           /* gguf: scarta vocab e < 1 MB */
        sr_add(c, file, "gguf");                        /* il FILE gguf = il modello */
    }
}

/* Ricerca via SPOTLIGHT (`mdfind`): istantanea sui volumi indicizzati. `onlyin` limita a un ramo. */
static void sr_mdfind(SrCtx *c, const char *onlyin) {
    static const struct { const char *q; const char *kind; int reqw; } q[2] = {
        { "-name config.json", "mlx", 1 },        /* config.json da solo e' troppo debole: servono i pesi */
        { "kMDItemFSName == '*.gguf'", "gguf", 0 },
    };
    int k;
    for (k = 0; k < 2; k++) {
        char cmd[1200], line[FMLX_PATHS_DIR_MAX];
        FILE *p;
        if (onlyin && *onlyin) snprintf(cmd, sizeof cmd, "mdfind -onlyin '%s' %s 2>/dev/null", onlyin, q[k].q);
        else snprintf(cmd, sizeof cmd, "mdfind %s 2>/dev/null", q[k].q);
        p = popen(cmd, "r");
        if (!p) continue;
        while (fgets(line, sizeof line, p)) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            if (line[0] == '/') sr_add_from_file(c, line, q[k].kind, q[k].reqw);
        }
        pclose(p);
    }
}

/* Ricorsione a UNA passata `readdir`: legge la dir, decide se è un modello (config.json / *.gguf /
 * forma repo-cache HF), e solo se NON lo è scende — raccogliendo i sottodirectory nella stessa
 * passata. Niente stat (usa d_type). */
static void sr_scan(SrCtx *c, const char *dir, int depth) {
    DIR *d;
    struct dirent *e;
    int has_cfg = 0, has_weight = 0, added = 0, nsub = 0, capsub = 0, i;
    char (*subs)[FMLX_PATHS_DIR_MAX] = NULL;

    if (depth > c->maxdepth || c->n >= c->cap) return;
    d = opendir(dir);
    if (!d) return;
    { const char *base = strrchr(dir, '/'); base = base ? base + 1 : dir;
      if (sr_is_hf_repo(base)) { sr_add(c, dir, "hf"); closedir(d); return; } }

    while ((e = readdir(d))) {
        const char *n = e->d_name;
        if (n[0] == '.') continue;                      /* nascondi . .. e i file nascosti */
        if (e->d_type == DT_DIR) {
            if (sr_skip(c, n)) continue;
            if (nsub >= capsub) {
                capsub = capsub ? capsub * 2 : 16;
                { char(*ns)[FMLX_PATHS_DIR_MAX] = realloc(subs, (size_t)capsub * FMLX_PATHS_DIR_MAX);
                  if (!ns) { free(subs); closedir(d); return; } subs = ns; }
            }
            snprintf(subs[nsub++], FMLX_PATHS_DIR_MAX, "%s/%s", dir, n);
        } else if (strcmp(n, "config.json") == 0) {
            has_cfg = 1;
        } else if (sr_is_gguf(n)) {
            /* Un .gguf e' un MODELLO (il FILE), non la cartella che lo contiene: cosi' una dir
             * con dieci gguf da' dieci modelli, non una voce "gguf". */
            char full[FMLX_PATHS_DIR_MAX];
            if (snprintf(full, sizeof full, "%s/%s", dir, n) < (int)sizeof full && sr_gguf_ok(full, n)) { sr_add(c, full, "gguf"); added = 1; }
        } else if (sr_is_weight_name(n)) {
            char full[FMLX_PATHS_DIR_MAX];
            if (snprintf(full, sizeof full, "%s/%s", dir, n) < (int)sizeof full && sr_file_big(full)) has_weight = 1;
        }
    }
    closedir(d);
    if (has_cfg && has_weight) { sr_add(c, dir, "mlx"); added = 1; }  /* dir MLX/HF = il modello */
    if (added) { free(subs); return; }                               /* PODA: modelli trovati qui */
    for (i = 0; i < nsub; i++) sr_scan(c, subs[i], depth + 1);
    free(subs);
}

/* I roots del WALK quando non si dà <path>: i percorsi di config + $HOME + OGNI volume montato
 * (`getmntinfo`). Di default si cammina TUTTO il disco (nessun tetto di profondita'): completo ma
 * lento su dischi grandi; --max-depth limita. I duplicati (mount che coincidono) si scartano. */
static int sr_roots(char (*roots)[FMLX_PATHS_DIR_MAX], int max) {
    int n = 0, i, k, np = fmlx_paths_count();
    char p[FMLX_PATHS_DIR_MAX];
    struct statfs *mi = NULL;
    int nm;

    for (i = 0; i < np && n < max; i++)
        if (fmlx_paths_get(i, p, sizeof p) == 0 && p[0]) snprintf(roots[n++], FMLX_PATHS_DIR_MAX, "%s", p);
    { const char *h = getenv("HOME"); if (h && *h && n < max) snprintf(roots[n++], FMLX_PATHS_DIR_MAX, "%s", h); }
    nm = getmntinfo(&mi, MNT_NOWAIT);
    for (i = 0; i < nm && n < max; i++) {
        const char *mp = mi[i].f_mntonname;
        int dup = 0;
        if (!mp || !*mp) continue;
        for (k = 0; k < n; k++) if (strcmp(roots[k], mp) == 0) { dup = 1; break; }
        if (!dup) snprintf(roots[n++], FMLX_PATHS_DIR_MAX, "%s", mp);
    }
    return n;
}

/* ---- presentazione ---- */

#define SRT_CT 128 /* lunghezza di un componente di percorso nel tree */

static const char *sr_base(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }
/* Nome mostrato: repo-cache HF `models--org--nome` -> `org/nome`; altrimenti la basename. */
static void sr_name(const char *path, char *out, size_t cap) {
    const char *b = sr_base(path);
    if (strncmp(b, "models--", 8) == 0) {
        char tmp[FMLX_PATHS_DIR_MAX];
        char *dd;
        snprintf(tmp, sizeof tmp, "%s", b + 8);
        dd = strstr(tmp, "--");
        if (dd) { *dd = '/'; if (dd[2]) memmove(dd + 1, dd + 2, strlen(dd + 2) + 1); }
        snprintf(out, cap, "%s", tmp);
    } else snprintf(out, cap, "%s", b);
}
static int sr_cmp_name(const void *a, const void *b) {
    const SrHit *x = (const SrHit *)a, *y = (const SrHit *)b;
    char nx[FMLX_PATHS_DIR_MAX], ny[FMLX_PATHS_DIR_MAX];
    sr_name(x->path, nx, sizeof nx); sr_name(y->path, ny, sizeof ny);
    return strcasecmp(nx, ny);
}
static int sr_split(const char *rel, char parts[][SRT_CT], int max) {
    int n = 0; const char *p = rel;
    while (*p && n < max) {
        const char *s = p; int l;
        while (*p && *p != '/') p++;
        l = (int)(p - s);
        if (l > 0 && l < SRT_CT) { memcpy(parts[n], s, (size_t)l); parts[n][l] = 0; n++; }
        if (*p == '/') p++;
    }
    return n;
}
static int sr_prefix_eq(char a[][SRT_CT], char b[][SRT_CT], int d) {
    int k; for (k = 0; k < d; k++) if (strcmp(a[k], b[k]) != 0) return 0; return 1;
}

/* DEFAULT: lista NUMERATA, ALFABETICA. Il NOME su una riga, il PERCORSO sotto (stile tree),
 * senza ellipsis: il percorso puo' essere lungo quanto serve. */
static void sr_emit_list(FILE *out, SrCtx *c) {
    int i;
    if (c->n == 0) { fprintf(out, "nessun modello trovato\n"); return; }
    qsort(c->v, c->n, sizeof *c->v, sr_cmp_name);
    fprintf(out, "%d modelli (ordinati per nome):\n", c->n);
    for (i = 0; i < c->n; i++) {
        char name[FMLX_PATHS_DIR_MAX];
        sr_name(c->v[i].path, name, sizeof name);
        fprintf(out, "%4d. [%-4s] %s\n", i + 1, c->v[i].kind, name);
        fprintf(out, "       └─ %s\n", c->v[i].path);   /* percorso sotto, niente ellipsis */
    }
}

/* --roots: TREE delle cartelle RADICE, coi modelli annidati sotto ciascuna. */
static void sr_emit_tree(FILE *out, SrCtx *c) {
    int i, r, d;
    int n0 = c->n > 0 ? c->n : 1;
    int *assign = malloc((size_t)n0 * sizeof(int));
    char (*rel)[FMLX_PATHS_DIR_MAX] = malloc((size_t)n0 * FMLX_PATHS_DIR_MAX);
    int *idx = malloc((size_t)n0 * sizeof(int));
    if (!assign || !rel || !idx) { free(assign); free(rel); free(idx); fprintf(out, "memoria esaurita\n"); return; }
    for (i = 0; i < c->n; i++) {
        int best = -1, bestlen = -1;
        for (r = 0; r < c->nroots; r++) {
            size_t rl = strlen(c->roots[r]);
            if (strncmp(c->v[i].path, c->roots[r], rl) == 0 && (c->v[i].path[rl] == '/' || c->v[i].path[rl] == 0) && (int)rl > bestlen) { best = r; bestlen = (int)rl; }
        }
        assign[i] = best;
    }
    fprintf(out, "%d modelli sotto %d cartelle radice:\n", c->n, c->nroots);
    for (r = 0; r <= c->nroots; r++) {
        int nr = 0, k, laststack[64], nprev = 0;
        char prev[64][SRT_CT];
        for (i = 0; i < c->n; i++) if (assign[i] == (r < c->nroots ? r : -1)) idx[nr++] = i;
        if (nr == 0) continue;
        for (i = 0; i < nr; i++) {
            const char *base = c->v[idx[i]].path;
            if (r < c->nroots) { size_t rl = strlen(c->roots[r]); base = (base[rl] == '/') ? base + rl + 1 : base; }
            snprintf(rel[i], FMLX_PATHS_DIR_MAX, "%s", base);
        }
        for (i = 0; i < nr; i++)
            for (k = i + 1; k < nr; k++)
                if (strcmp(rel[i], rel[k]) > 0) {
                    char t[FMLX_PATHS_DIR_MAX];
                    memcpy(t, rel[i], FMLX_PATHS_DIR_MAX); memcpy(rel[i], rel[k], FMLX_PATHS_DIR_MAX); memcpy(rel[k], t, FMLX_PATHS_DIR_MAX);
                }
        fprintf(out, "\n%s  (%d)\n", r < c->nroots ? c->roots[r] : "(altrove)", nr);
        memset(laststack, 0, sizeof laststack);
        for (i = 0; i < nr; i++) {
            char parts[64][SRT_CT];
            int np = sr_split(rel[i], parts, 64), common = 0, dd;
            while (common < nprev && common < np && strcmp(prev[common], parts[common]) == 0) common++;
            for (dd = common; dd < np; dd++) {
                int a, last = (i + 1 >= nr);
                /* "last" = il percorso SUCCESSIVO non e' un fratello a questo livello:
                 * deve condividere il PADRE (prime `dd` componenti) e avere una componente a `dd`. */
                if (!last) { char nxt[64][SRT_CT]; int nn = sr_split(rel[i + 1], nxt, 64); last = !(nn > dd && sr_prefix_eq(parts, nxt, dd)); }
                for (a = 0; a < dd; a++) fputs(laststack[a] ? "   " : "│  ", out);
                fprintf(out, "%s%s\n", last ? "└── " : "├── ", parts[dd]);
                laststack[dd] = last;
            }
            for (k = 0; k < np; k++) snprintf(prev[k], SRT_CT, "%s", parts[k]);
            nprev = np;
        }
    }
    (void)d;
    free(assign); free(rel); free(idx);
}

static FmlxCliStatus sr_emit(FILE *out, FILE *err, SrCtx *c, int json, int roots, const char *given) {
    int i;
    if (json) {
        FmlxJsonW w; size_t blen = 0; int ok = 0;
        fmlx_jsonw_init(&w, NULL, 0, 1u << 22);
        do {
            if (fmlx_jsonw_obj_begin(&w) != FMLX_JSON_OK) break;
            if (fmlx_jsonw_key(&w, "generated") != FMLX_JSON_OK || fmlx_jsonw_i64(&w, (int64_t)time(NULL)) != FMLX_JSON_OK) break;
            if (fmlx_jsonw_key(&w, "from") != FMLX_JSON_OK || fmlx_jsonw_cstr(&w, given ? given : "(tutti i dischi)") != FMLX_JSON_OK) break;
            if (fmlx_jsonw_key(&w, "models") != FMLX_JSON_OK || fmlx_jsonw_arr_begin(&w) != FMLX_JSON_OK) break;
            for (i = 0; i < c->n; i++) {
                if (fmlx_jsonw_obj_begin(&w) != FMLX_JSON_OK ||
                    fmlx_jsonw_key(&w, "path") != FMLX_JSON_OK || fmlx_jsonw_cstr(&w, c->v[i].path) != FMLX_JSON_OK ||
                    fmlx_jsonw_key(&w, "kind") != FMLX_JSON_OK || fmlx_jsonw_cstr(&w, c->v[i].kind) != FMLX_JSON_OK ||
                    fmlx_jsonw_obj_end(&w) != FMLX_JSON_OK) break;
            }
            if (fmlx_jsonw_arr_end(&w) != FMLX_JSON_OK || fmlx_jsonw_obj_end(&w) != FMLX_JSON_OK || fmlx_jsonw_finish(&w, &blen) != FMLX_JSON_OK) break;
            ok = 1;
        } while (0);
        if (!ok) { fmlx_jsonw_free(&w); fprintf(err, "fastllm search: JSON troppo grande\n"); return FMLX_CLI_FAILED; }
        fwrite(w.buf ? w.buf : "{}", 1, blen, out); fputc('\n', out); fmlx_jsonw_free(&w);
    } else if (roots) {
        sr_emit_tree(out, c);
    } else {
        sr_emit_list(out, c);
    }
    return FMLX_CLI_OK;
}

FmlxCliStatus fmlx_search_exec(const FmlxCliArgs *a, FILE *out, FILE *err) {
    char(*roots)[FMLX_PATHS_DIR_MAX];
    SrCtx c;
    int nr, i;

    if (!out) out = stdout;
    if (!err) err = stderr;
    roots = malloc((size_t)FMLX_PATHS_MAX * FMLX_PATHS_DIR_MAX);
    c.v = malloc((size_t)SR_RESULT_MAX * sizeof *c.v);
    if (!roots || !c.v) { free(roots); free(c.v); fprintf(err, "fastllm search: memoria esaurita\n"); return FMLX_CLI_FAILED; }
    c.n = 0; c.cap = SR_RESULT_MAX;
    /* --max-depth limita; senza, il tetto pratico e' alto (64): si cammina tutto il disco. */
    c.maxdepth = (a && a->search_max_depth > 0) ? a->search_max_depth : 64;
    { static const char *const sk[] = {"System", "private", "dev", "cores", "proc", "sys", "usr", "bin", "sbin",
                                "var", "tmp", "opt", "Library", "Applications", ".git", "node_modules",
                                "build", ".build", "_deps", ".Trash", ".Spotlight-V100", ".fseventsd",
                                ".DocumentRevisions-V100", ".TemporaryItems", ".vol"};
      int nsk = (int)(sizeof sk / sizeof sk[0]);
      for (c.nskip = 0; c.nskip < nsk && c.nskip < SR_SKIP_MAX; c.nskip++) c.skip[c.nskip] = sk[c.nskip]; }

    { const char *given = (a && a->repo[0]) ? a->repo : NULL;
      int walk_only = a && a->search_walk;
      if (given) { snprintf(roots[0], FMLX_PATHS_DIR_MAX, "%s", given); nr = 1; }
      else nr = sr_roots(roots, FMLX_PATHS_MAX);   /* config + $HOME + OGNI volume montato */
      c.roots = roots; c.nroots = nr;
      if (!walk_only) sr_mdfind(&c, given);        /* Spotlight: volumi indicizzati, istantaneo */
      for (i = 0; i < nr; i++) sr_scan(&c, roots[i], 0);
    }
    if (a && a->search_type[0]) {  /* --type: tiene solo il tipo richiesto (i duplicati restano) */
        int w = 0;
        for (i = 0; i < c.n; i++)
            if (!strcmp(c.v[i].kind, a->search_type)) { if (w != i) c.v[w] = c.v[i]; w++; }
        c.n = w;
    }
    { FmlxCliStatus s = sr_emit(out, err, &c, a && a->json, a && a->search_roots, a && a->repo[0] ? a->repo : NULL);
      free(c.v); free(roots); return s; }
}
