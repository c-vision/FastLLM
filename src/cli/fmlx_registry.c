/* fmlx_registry.c — registro LOCALE dei modelli di FastLLM (stile Ollama).
 *
 * PERCHÉ. Fino a oggi `fastllm models` era solo una VISTA sul cache Hugging Face: i modelli che
 * l'utente tiene in `~/ai/Models` (o un `.gguf` come ds4, o un VLM con il suo `mmproj`) non
 * comparivano, non si potevano "aggiungere per percorso", né scegliere per numero. Questo file
 * tiene il REGISTRO e i verbi che lo usano.
 *
 * FILE. `$FASTLLM_HOME/models.tsv` (default `~/.fastllm/models.tsv`), una riga per modello,
 * campi separati da TAB, `#` a inizio riga = commento. Formato volutamente MINIMO e leggibile:
 * niente dipendenza dal layer JSON per una cosa che si ispeziona a mano.
 *
 *   name<TAB>path<TAB>mmproj<TAB>modality<TAB>source<TAB>added
 *
 * - `path` può essere una DIR MLX, un FILE `.gguf` (ds4), o un modello con `mmproj` (VLM/OCR).
 * - `modality` ∈ {chat, vlm, ocr, audio}.
 * - `source` = "local" (aggiunto con `add`) o "hf:<repo>" (registrato da `pull`).
 * Risoluzione: la prima occorrenza del NOME vince; `#n`/`n` = indice 0-based nella lista.
 *
 * USO (verbi): `fastllm list` (numerata), `fastllm add <nome> <path> [--mmproj p] [--modality m]`,
 * `fastllm rm <nome|#n> [--purge]`, `fastllm show <nome|#n>`. `run`/`chat`/`serve` accettano
 * `<nome|#n>` e risolvono QUI (poi cadono su path/cache HF/$FMLX_MODELS_DIR come prima). */
#include "fmlx/fmlx_cli.h"
#include "fmlx/fmlx_hf.h"
#include "fmlx/fmlx_json.h"

#include "fmlx_cli_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define REG_NAME_MAX 128
#define REG_MOD_MAX 16
#define REG_SOURCE_MAX 256

typedef struct {
    char name[REG_NAME_MAX];
    char path[FMLX_HF_PATH_MAX];
    char mmproj[FMLX_HF_PATH_MAX];
    char modality[REG_MOD_MAX];
    char source[REG_SOURCE_MAX];
    char added[32];
} RegEntry;

/* Percorso del registro: $FASTLLM_HOME/models.tsv, default ~/.fastllm/models.tsv. */
static int reg_file(char *out, size_t cap, char *err, size_t errcap) {
    const char *home = getenv("FASTLLM_HOME");
    char dir[FMLX_HF_PATH_MAX];
    if (home && *home) {
        snprintf(dir, sizeof dir, "%s", home);
    } else {
        const char *h = getenv("HOME");
        if (!h || !*h) { snprintf(err, errcap, "HOME assente: impossibile localizzare il registro"); return -1; }
        snprintf(dir, sizeof dir, "%s/.fastllm", h);
    }
    mkdir(dir, 0755); /* idempotente */
    if (snprintf(out, cap, "%s/models.tsv", dir) >= (int)cap) {
        snprintf(err, errcap, "percorso del registro troppo lungo");
        return -1;
    }
    return 0;
}

static void reg_trim(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = '\0';
}

/* Copia il campo `idx` (0-based) di una riga TAB-separata in out. Ritorna 1 se esiste. */
static int field(const char *line, int idx, char *out, size_t cap) {
    const char *p = line;
    int i = 0;
    out[0] = '\0';
    while (p && *p) {
        const char *tab = strchr(p, '\t');
        size_t len = tab ? (size_t)(tab - p) : strlen(p);
        if (i == idx) {
            if (len >= cap) len = cap - 1;
            memcpy(out, p, len);
            out[len] = '\0';
            return 1;
        }
        if (!tab) break;
        p = tab + 1;
        i++;
    }
    return 0;
}

static size_t reg_load(RegEntry **out, char *err, size_t errcap) {
    char file[FMLX_HF_PATH_MAX];
    FILE *f;
    RegEntry *v = NULL;
    size_t n = 0, cap = 0;
    char line[FMLX_HF_PATH_MAX * 2];

    *out = NULL;
    if (reg_file(file, sizeof file, err, errcap) != 0) return 0;
    f = fopen(file, "r");
    if (!f) return 0; /* registro assente = nessun modello: non è un errore */
    while (fgets(line, sizeof line, f)) {
        RegEntry e;
        if (line[0] == '#' || line[0] == '\n') continue;
        memset(&e, 0, sizeof e);
        if (!field(line, 0, e.name, sizeof e.name) || !e.name[0]) continue;
        field(line, 1, e.path, sizeof e.path);
        field(line, 2, e.mmproj, sizeof e.mmproj);
        field(line, 3, e.modality, sizeof e.modality);
        field(line, 4, e.source, sizeof e.source);
        field(line, 5, e.added, sizeof e.added);
        reg_trim(e.path); reg_trim(e.mmproj); reg_trim(e.modality); reg_trim(e.source); reg_trim(e.added);
        if (n == cap) { cap = cap ? cap * 2 : 16; v = realloc(v, cap * sizeof *v); if (!v) { fclose(f); return 0; } }
        v[n++] = e;
    }
    fclose(f);
    *out = v;
    return n;
}

static int reg_save(const RegEntry *v, size_t n, char *err, size_t errcap) {
    char file[FMLX_HF_PATH_MAX];
    FILE *f;
    if (reg_file(file, sizeof file, err, errcap) != 0) return -1;
    f = fopen(file, "w");
    if (!f) { snprintf(err, errcap, "impossibile scrivere il registro: %s", file); return -1; }
    fprintf(f, "# FastLLM model registry — name\\tpath\\tmmproj\\tmodality\\tsource\\tadded\n");
    for (size_t i = 0; i < n; i++)
        fprintf(f, "%s\t%s\t%s\t%s\t%s\t%s\n", v[i].name, v[i].path, v[i].mmproj,
                v[i].modality[0] ? v[i].modality : "chat",
                v[i].source[0] ? v[i].source : "local", v[i].added);
    fclose(f);
    return 0;
}

/* `spec` = "#n" o "n" (indice) o un NOME. Scrive l'indice trovato in *idx (per n=0.. ), ritorna 1. */
static int reg_find(const RegEntry *v, size_t n, const char *spec, size_t *idx) {
    const char *num = spec;
    if (spec && spec[0] == '#') num = spec + 1;
    if (num && *num) {
        int all_digits = 1;
        for (const char *p = num; *p; p++) if (*p < '0' || *p > '9') { all_digits = 0; break; }
        if (all_digits) {
            long long i = strtoll(num, NULL, 10);
            if (i >= 0 && (size_t)i < n) { *idx = (size_t)i; return 1; }
            return 0;
        }
    }
    for (size_t i = 0; i < n; i++)
        if (strcmp(v[i].name, spec) == 0) { *idx = i; return 1; }
    return 0;
}

/* Risolve `spec` (#n/nome) e scrive il PATH del modello in `out_path` (e, se richiesti, mmproj e
 * modality). Ritorna 1 se risolto, 0 altrimenti. Usato dal dispatch di run/chat/serve. */
int fmlx_registry_resolve(const char *spec, char *out_path, size_t cap, char *out_mmproj,
                          size_t mcap, char *out_modality, size_t modcap) {
    RegEntry *v;
    char err[256];
    size_t n = reg_load(&v, err, sizeof err), idx;
    int ok = 0;
    if (n && reg_find(v, n, spec, &idx)) {
        snprintf(out_path, cap, "%s", v[idx].path);
        if (out_mmproj) snprintf(out_mmproj, mcap, "%s", v[idx].mmproj);
        if (out_modality) snprintf(out_modality, modcap, "%s", v[idx].modality[0] ? v[idx].modality : "chat");
        ok = 1;
    }
    free(v);
    return ok;
}

/* ---- verbi ---- */

FmlxCliStatus fmlx_registry_add_exec(const FmlxCliArgs *a, FILE *out, FILE *err) {
    RegEntry *v;
    char e[256];
    size_t n, cap = 0, i;
    const char *path = a->path[0] ? a->path : a->repo;

    if (!a->repo[0] || !path[0]) {
        fprintf(err ? err : stderr, "fastllm add: uso: add <nome> <path> [--mmproj <p>] [--modality chat|vlm|ocr|audio]\n");
        return FMLX_CLI_USAGE;
    }
    n = reg_load(&v, e, sizeof e);
    /* riuso lo spazio: una growable copia locale per l'append */
    for (i = 0; i < n; i++)
        if (strcmp(v[i].name, a->repo) == 0) {
            /* aggiorna in place (ri-aggiungere = aggiornare il percorso) */
            snprintf(v[i].path, sizeof v[i].path, "%s", path);
            snprintf(v[i].mmproj, sizeof v[i].mmproj, "%s", a->mmproj);
            snprintf(v[i].modality, sizeof v[i].modality, "%s", a->modality[0] ? a->modality : "chat");
            snprintf(v[i].source, sizeof v[i].source, "%s", "local");
            if (reg_save(v, n, e, sizeof e) != 0) { free(v); fprintf(err, "fastllm add: %s\n", e); return FMLX_CLI_FAILED; }
            free(v);
            fprintf(out ? out : stdout, "aggiornato: %s -> %s\n", a->repo, path);
            return FMLX_CLI_OK;
        }
    cap = n + 1;
    v = realloc(v, cap * sizeof *v);
    memset(&v[n], 0, sizeof v[n]);
    snprintf(v[n].name, sizeof v[n].name, "%s", a->repo);
    snprintf(v[n].path, sizeof v[n].path, "%s", path);
    snprintf(v[n].mmproj, sizeof v[n].mmproj, "%s", a->mmproj);
    snprintf(v[n].modality, sizeof v[n].modality, "%s", a->modality[0] ? a->modality : "chat");
    snprintf(v[n].source, sizeof v[n].source, "%s", "local");
    { time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm); strftime(v[n].added, sizeof v[n].added, "%Y-%m-%d", &tm); }
    if (reg_save(v, n + 1, e, sizeof e) != 0) { free(v); fprintf(err, "fastllm add: %s\n", e); return FMLX_CLI_FAILED; }
    free(v);
    fprintf(out ? out : stdout, "aggiunto: %s -> %s (%s)\n", a->repo, path, a->modality[0] ? a->modality : "chat");
    return FMLX_CLI_OK;
}

FmlxCliStatus fmlx_registry_rm_exec(const FmlxCliArgs *a, FILE *out, FILE *err) {
    RegEntry *v;
    char e[256];
    size_t n, idx;
    if (!a->repo[0]) { fprintf(err ? err : stderr, "fastllm rm: uso: rm <nome|#n> [--purge]\n"); return FMLX_CLI_USAGE; }
    n = reg_load(&v, e, sizeof e);
    if (!n || !reg_find(v, n, a->repo, &idx)) {
        free(v);
        fprintf(err ? err : stderr, "fastllm rm: '%s' non è nel registro\n", a->repo);
        return FMLX_CLI_FAILED;
    }
    if (a->purge) {
        struct stat st;
        if (stat(v[idx].path, &st) == 0 && S_ISDIR(st.st_mode)) {
            char cmd[FMLX_HF_PATH_MAX * 2 + 16];
            snprintf(cmd, sizeof cmd, "rm -rf -- '%s'", v[idx].path);
            if (system(cmd) != 0) fprintf(err ? err : stderr, "fastllm rm: --purge: rimozione fallita: %s\n", v[idx].path);
        } else if (stat(v[idx].path, &st) == 0) {
            unlink(v[idx].path);
        }
    }
    memmove(&v[idx], &v[idx + 1], (n - idx - 1) * sizeof *v);
    if (reg_save(v, n - 1, e, sizeof e) != 0) { free(v); fprintf(err, "fastllm rm: %s\n", e); return FMLX_CLI_FAILED; }
    free(v);
    fprintf(out ? out : stdout, "rimosso: %s%s\n", a->repo, a->purge ? " (file cancellati)" : "");
    return FMLX_CLI_OK;
}

FmlxCliStatus fmlx_registry_show_exec(const FmlxCliArgs *a, FILE *out, FILE *err) {
    RegEntry *v;
    char e[256];
    size_t n, idx;
    if (!a->repo[0]) { fprintf(err ? err : stderr, "fastllm show: uso: show <nome|#n>\n"); return FMLX_CLI_USAGE; }
    n = reg_load(&v, e, sizeof e);
    if (!n || !reg_find(v, n, a->repo, &idx)) {
        free(v);
        fprintf(err ? err : stderr, "fastllm show: '%s' non è nel registro\n", a->repo);
        return FMLX_CLI_FAILED;
    }
    fprintf(out ? out : stdout, "nome:     %s\ntipo:     %s\npercorso: %s\n",
            v[idx].name, v[idx].modality[0] ? v[idx].modality : "chat", v[idx].path);
    if (v[idx].mmproj[0]) fprintf(out, "mmproj:   %s\n", v[idx].mmproj);
    fprintf(out, "origine:  %s\naggiunto: %s\n", v[idx].source[0] ? v[idx].source : "local",
            v[idx].added[0] ? v[idx].added : "-");
    free(v);
    return FMLX_CLI_OK;
}

/* La lista: registro NUMERATO, poi i repo del cache HF non già registrati, numerati di seguito. */
FmlxCliStatus fmlx_registry_list_exec(const FmlxCliArgs *a, FILE *out, FILE *err) {
    RegEntry *v;
    char e[256];
    size_t n = reg_load(&v, e, sizeof e);
    FILE *o = out ? out : stdout;
    size_t num = 0;

    (void)err;
    if (a->json) {
        /* JSON: il registro, poi un campo per il cache HF. */
        fprintf(o, "{\"models\":[");
        for (size_t i = 0; i < n; i++) {
            fprintf(o, "%s{\"n\":%zu,\"name\":\"%s\",\"path\":\"%s\",\"mmproj\":\"%s\",\"modality\":\"%s\",\"source\":\"%s\"}",
                    i ? "," : "", i, v[i].name, v[i].path, v[i].mmproj,
                    v[i].modality[0] ? v[i].modality : "chat", v[i].source[0] ? v[i].source : "local");
        }
        fprintf(o, "]}\n");
        free(v);
        return FMLX_CLI_OK;
    }
    fprintf(o, "%-3s %-28s %-6s %-10s %s\n", "#", "NOME", "TIPO", "ORIGINE", "PERCORSO");
    for (size_t i = 0; i < n; i++, num++) {
        fprintf(o, "%-3zu %-28s %-6s %-10s %s\n", i, v[i].name,
                v[i].modality[0] ? v[i].modality : "chat",
                v[i].source[0] ? v[i].source : "local", v[i].path);
        if (v[i].mmproj[0]) fprintf(o, "    %-28s %-6s %-10s %s\n", "", "mmproj", "", v[i].mmproj);
    }
    /* cache HF (non già registrati) */
    {
        FmlxHfRepo *repos = calloc(FMLX_HF_CATALOG_REPOS_MAX, sizeof *repos);
        size_t nr = 0, ignored = 0;
        if (repos && fmlx_hf_catalog(a->pull.fetch.cache_root, repos, FMLX_HF_CATALOG_REPOS_MAX, &nr, &ignored) == FMLX_HF_OK) {
            for (size_t i = 0; i < nr; i++) {
                int already = 0;
                for (size_t k = 0; k < n; k++) if (strcmp(v[k].source, repos[i].repo_id) == 0) { already = 1; break; }
                if (already || repos[i].refs_only) continue;
                fprintf(o, "%-3zu %-28s %-6s %-10s %s\n", num++, repos[i].repo_id, "hf", "hf-cache", repos[i].repo_id);
            }
        }
        free(repos);
    }
    free(v);
    return FMLX_CLI_OK;
}
