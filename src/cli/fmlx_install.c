/* fmlx_install.c — installazione e aggiornamento: `fastllm version`, `fastllm doctor --install`, `fastllm upgrade`
 * e il controllo di compatibilita' fra questo binario e il motore LLMEnv installato.
 *
 * FastLLM non contiene il motore: lo carica da `lib/llmenv` (rpath relativo al binario). Il motore ha il suo file
 * `LLMENV_VERSION` (version, variant, min_macos, nax) scritto dal pacchetto di LLMEnv. Qui si legge dalla cartella
 * della libreria REALMENTE caricata (dladdr), non da un percorso ipotizzato. Un binario costruito contro LLMEnv X.Y
 * accetta solo un motore X.Y.*: le librerie sono collegate per simbolo, e una versione diversa non fallisce con un
 * messaggio ma con un crash. Un motore senza LLMENV_VERSION (build di sviluppo) non e' controllato.
 */
#include "fmlx/fmlx_cli.h"
#include "llmdesc.h"

#include <dlfcn.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef FASTLLM_VERSION
#define FASTLLM_VERSION "dev"
#endif
#ifndef FASTLLM_LLMENV_VERSION
#define FASTLLM_LLMENV_VERSION "0.0.0"
#endif
#define FASTLLM_REPO "c-vision/FastLLM"

extern int llm_ai_mlx_opt_set(const char *name, const char *value);   /* simbolo del motore: serve solo per dladdr */

typedef struct {
    char version[32], variant[16], min_macos[16];
    int nax;
} EngineVer;

static int is_file(const char *p) { struct stat st; return p && stat(p, &st) == 0 && S_ISREG(st.st_mode); }

/* cartella della libreria del motore caricata (percorso reale). 0 = non trovata */
static int engine_dir(char *out, size_t cap) {
    Dl_info info;
    char real[PATH_MAX], *slash;
    if (!dladdr((void *)&llm_ai_mlx_opt_set, &info) || !info.dli_fname) return 0;
    if (!realpath(info.dli_fname, real)) snprintf(real, sizeof real, "%s", info.dli_fname);
    slash = strrchr(real, '/');
    if (!slash) return 0;
    *slash = 0;
    snprintf(out, cap, "%s", real);
    return 1;
}

static int read_engine_ver(const char *dir, EngineVer *v) {
    char path[PATH_MAX + 32], line[256];
    FILE *f;
    memset(v, 0, sizeof *v);
    snprintf(path, sizeof path, "%s/LLMENV_VERSION", dir);
    f = fopen(path, "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='), *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (!eq) continue;
        *eq++ = 0;
        if (!strcmp(line, "version")) snprintf(v->version, sizeof v->version, "%s", eq);
        else if (!strcmp(line, "variant")) snprintf(v->variant, sizeof v->variant, "%s", eq);
        else if (!strcmp(line, "min_macos")) snprintf(v->min_macos, sizeof v->min_macos, "%s", eq);
        else if (!strcmp(line, "nax")) v->nax = atoi(eq);
    }
    fclose(f);
    return v->version[0] != 0;
}

/* "X.Y" di una versione "X.Y.Z" */
static void minor_of(const char *v, char *out, size_t cap) {
    const char *d1 = strchr(v, '.'), *d2 = d1 ? strchr(d1 + 1, '.') : NULL;
    size_t n = d2 ? (size_t)(d2 - v) : strlen(v);
    if (n >= cap) n = cap - 1;
    memcpy(out, v, n);
    out[n] = 0;
}

/* confronto numerico "a.b.c" >= "x.y.z" */
static int ver_ge(const char *a, const char *b) {
    int p[3] = {0, 0, 0}, q[3] = {0, 0, 0}, i;
    sscanf(a, "%d.%d.%d", &p[0], &p[1], &p[2]);
    sscanf(b, "%d.%d.%d", &q[0], &q[1], &q[2]);
    for (i = 0; i < 3; i++) {
        if (p[i] != q[i]) return p[i] > q[i];
    }
    return 1;
}

static void macos_version(char *out, size_t cap) {
    FILE *p = popen("sw_vers -productVersion 2>/dev/null", "r");
    out[0] = 0;
    if (p) {
        if (fgets(out, (int)cap, p)) { char *nl = strchr(out, '\n'); if (nl) *nl = 0; }
        pclose(p);
    }
}

/* percorso reale del binario e modo in cui e' stato installato */
static void self_path(char *out, size_t cap) {
    char raw[PATH_MAX]; uint32_t n = sizeof raw;
    out[0] = 0;
    if (_NSGetExecutablePath(raw, &n) == 0 && !realpath(raw, out)) snprintf(out, cap, "%s", raw);
}
static const char *install_method(const char *path) {
    if (strstr(path, "/Cellar/")) return "homebrew";
    if (strstr(path, "/.local/")) return "install.sh";
    if (!strncmp(path, "/usr/local/bin/", 15)) return "pkg";
    return "other (build or manual copy)";
}

/* ---- compatibilita' all'avvio ---- */

int fmlx_install_gate(FILE *err) {
    char dir[PATH_MAX];
    EngineVer ev;
    char want[16], have[16];
    if (!engine_dir(dir, sizeof dir) || !read_engine_ver(dir, &ev)) return 1;   /* build di sviluppo: nessun controllo */
    minor_of(FASTLLM_LLMENV_VERSION, want, sizeof want);
    minor_of(ev.version, have, sizeof have);
    if (!strcmp(want, have)) return 1;
    fprintf(err,
            "fastllm: this build needs the LLMEnv engine %s.x, but %s has version %s.\n"
            "  Fix it with:  fastllm upgrade        (or: brew upgrade llmenv)\n",
            want, dir, ev.version);
    return 0;
}

/* ---- version ---- */

FmlxCliStatus fmlx_version_exec(const FmlxCliArgs *a, FILE *out, FILE *err) {
    char dir[PATH_MAX];
    EngineVer ev;
    (void)a; (void)err;
    fprintf(out, "fastllm %s\n", FASTLLM_VERSION);
    if (engine_dir(dir, sizeof dir)) {
        if (read_engine_ver(dir, &ev))
            fprintf(out, "engine LLMEnv %s (%s, macOS %s or later) in %s\n", ev.version, ev.variant, ev.min_macos, dir);
        else
            fprintf(out, "engine LLMEnv (development build, no LLMENV_VERSION) in %s\n", dir);
    }
    return FMLX_CLI_OK;
}

/* ---- doctor --install ---- */

static int report(FILE *out, int *problems, const char *level, const char *fmt, const char *a1, const char *a2) {
    fprintf(out, "  [%-4s] ", level);
    fprintf(out, fmt, a1 ? a1 : "", a2 ? a2 : "");
    fputc('\n', out);
    if (!strcmp(level, "FAIL")) (*problems)++;
    return 0;
}

FmlxCliStatus fmlx_doctor_install_exec(const FmlxCliArgs *a, FILE *out, FILE *err) {
    char self[PATH_MAX], dir[PATH_MAX], osv[32], tmp[PATH_MAX + 64], real[PATH_MAX], abi[PATH_MAX];
    EngineVer ev;
    int problems = 0, have_ver;
    const char *home = getenv("HOME");
    const char *cands[5];
    size_t i, nc = 0;
    char c_local[PATH_MAX], c_env[PATH_MAX];
    (void)a; (void)err;

    self_path(self, sizeof self);
    macos_version(osv, sizeof osv);
    fprintf(out, "fastllm %s  (%s)\n", FASTLLM_VERSION, install_method(self));
    fprintf(out, "  binary:  %s\n  macOS:   %s\n", self, osv);

    if (!engine_dir(dir, sizeof dir)) {
        report(out, &problems, "FAIL", "the LLMEnv engine library was not found", NULL, NULL);
        return FMLX_CLI_FAILED;
    }
    have_ver = read_engine_ver(dir, &ev);
    fprintf(out, "  engine:  %s\n", dir);
    if (!have_ver) {
        report(out, &problems, "WARN", "no LLMENV_VERSION in the engine folder (development build): compatibility is not checked", NULL, NULL);
    } else {
        char want[16], have[16];
        minor_of(FASTLLM_LLMENV_VERSION, want, sizeof want);
        minor_of(ev.version, have, sizeof have);
        if (!strcmp(want, have)) {
            snprintf(tmp, sizeof tmp, "LLMEnv %s (variant %s) matches what this fastllm needs (%s.x)", ev.version, ev.variant, want);
            report(out, &problems, "ok", "%s", tmp, NULL);
        }
        else {
            snprintf(tmp, sizeof tmp, "LLMEnv %s found, this fastllm needs %s.x: run `fastllm upgrade`", ev.version, want);
            report(out, &problems, "FAIL", "%s", tmp, NULL);
        }
        if (osv[0] && ev.min_macos[0] && !ver_ge(osv, ev.min_macos)) {
            snprintf(tmp, sizeof tmp, "the engine needs macOS %s or later, this is %s", ev.min_macos, osv);
            report(out, &problems, "FAIL", "%s", tmp, NULL);
        }
        if (osv[0] && !ev.nax && ver_ge(osv, "26.2"))
            report(out, &problems, "WARN", "this macOS can use the faster NAX engine variant: curl -fsSL https://github.com/c-vision/LLMEnv/releases/latest/download/install-llmenv.sh | bash -s -- --variant nax --force", NULL, NULL);
    }

    snprintf(tmp, sizeof tmp, "%s/mlx.metallib", dir);
    {
        char exe_dir[PATH_MAX], *sl;
        snprintf(exe_dir, sizeof exe_dir, "%s", self);
        sl = strrchr(exe_dir, '/');
        if (sl) *sl = 0;
        snprintf(real, sizeof real, "%s/mlx.metallib", exe_dir);   /* build di sviluppo: accanto al binario */
    }
    if (is_file(tmp) || is_file(real)) report(out, &problems, "ok", "mlx.metallib present", NULL, NULL);
    else report(out, &problems, "FAIL", "mlx.metallib is missing next to the engine library: the Metal kernels cannot load", NULL, NULL);

    if (llmdesc_abi_dir(abi, sizeof abi)) {
        snprintf(tmp, sizeof tmp, "model descriptors found: %s", abi);
        report(out, &problems, "ok", "%s", tmp, NULL);
    } else report(out, &problems, "WARN", "no ABI folder with model descriptors was found (fastllm gmbarch cannot create one without it)", NULL, NULL);

    /* altre copie di LLMEnv sulla macchina: non e' un errore, ma una fonte di confusione */
    c_local[0] = c_env[0] = 0;
    if (home) snprintf(c_local, sizeof c_local, "%s/.local/lib/llmenv", home);
    if (getenv("LLMENV_HOME")) snprintf(c_env, sizeof c_env, "%s", getenv("LLMENV_HOME"));
    cands[nc++] = "/opt/homebrew/opt/llmenv/lib/llmenv";
    cands[nc++] = "/usr/local/lib/llmenv";
    if (c_local[0]) cands[nc++] = c_local;
    if (c_env[0]) cands[nc++] = c_env;
    for (i = 0; i < nc; i++) {
        EngineVer o;
        if (!realpath(cands[i], real) || !strcmp(real, dir)) continue;
        if (!read_engine_ver(real, &o)) continue;
        snprintf(tmp, sizeof tmp, "another LLMEnv %s is installed at %s (not the one in use)", o.version, cands[i]);
        report(out, &problems, "WARN", "%s", tmp, NULL);
    }

    fprintf(out, "%s\n", problems ? "problems found" : "installation OK");
    return problems ? FMLX_CLI_FAILED : FMLX_CLI_OK;
}

/* ---- upgrade ---- */

static int latest_tag(char *out, size_t cap) {
    char line[4096];
    FILE *p = popen("curl -fsSL --retry 2 https://api.github.com/repos/" FASTLLM_REPO "/releases/latest 2>/dev/null", "r");
    out[0] = 0;
    if (!p) return 0;
    while (fgets(line, sizeof line, p)) {
        char *t = strstr(line, "\"tag_name\"");
        if (t && (t = strchr(t + 10, ':')) && (t = strchr(t, '"'))) {
            char *e = strchr(++t, '"');
            if (e) { *e = 0; if (*t == 'v') t++; snprintf(out, cap, "%s", t); break; }
        }
    }
    pclose(p);
    return out[0] != 0;
}

FmlxCliStatus fmlx_upgrade_exec(const FmlxCliArgs *a, FILE *out, FILE *err) {
    char self[PATH_MAX], latest[32];
    int check_only = a ? a->upgrade_check : 0;
    self_path(self, sizeof self);
    if (!strcmp(install_method(self), "homebrew")) {
        fprintf(out, "fastllm was installed with Homebrew: upgrade it with  brew upgrade fastllm\n");
        return FMLX_CLI_OK;
    }
    if (!latest_tag(latest, sizeof latest)) {
        fprintf(err, "fastllm upgrade: cannot read the latest release of %s (network?)\n", FASTLLM_REPO);
        return FMLX_CLI_FAILED;
    }
    fprintf(out, "installed: %s   latest: %s\n", FASTLLM_VERSION, latest);
    if (check_only) return FMLX_CLI_OK;
    if (!strcmp(latest, FASTLLM_VERSION) && fmlx_install_gate(err)) {
        fprintf(out, "fastllm is up to date.\n");
        return FMLX_CLI_OK;
    }
    /* stesso script della prima installazione: e' idempotente (non rifa' cio' che e' gia' a posto) */
    fprintf(out, "running the installer...\n");
    if (system("curl -fsSL https://github.com/" FASTLLM_REPO "/releases/latest/download/install.sh | bash") != 0) {
        fprintf(err, "fastllm upgrade: the installer failed\n");
        return FMLX_CLI_FAILED;
    }
    return FMLX_CLI_OK;
}
