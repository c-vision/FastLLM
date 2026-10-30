/* fmlx_gmbarch.c — descrittori `.gmbarch` delle architetture (cartella ABI di LLMEnv).
 *
 *   fastllm gmbarch <url Hugging Face | org/nome | cartella modello>
 *
 * crea `<model_type>.gmbarch` se manca (per un URL scarica SOLO config.json e l'indice dei tensori, pochi kB; per un repo
 * con un solo model.safetensors, l'header). Lo stesso controllo parte da solo dopo `fastllm pull` e `fastllm add`.
 * Il descrittore e' per ARCHITETTURA: se esiste (anche curato a mano) non si tocca mai. */
#include "fmlx/fmlx_cli.h"
#include "llmdesc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int is_dir(const char *p) { struct stat st; return p && *p && stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static int is_file(const char *p) { struct stat st; return p && *p && stat(p, &st) == 0 && S_ISREG(st.st_mode); }

void fmlx_gmbarch_ensure_dir(const char *dir, FILE *out) {
    char path[4096] = "", err[256] = "";
    int r;
    char cfg[4200];
    if (!out) out = stdout;
    snprintf(cfg, sizeof cfg, "%s/config.json", dir);
    if (!is_file(cfg)) return;   /* non e' un modello HF (es. GGUF): niente da fare */
    r = llmdesc_ensure_for_dir(dir, NULL, path, sizeof path, err, sizeof err);
    if (r == 1) fprintf(out, "[abi] creato il descrittore %s\n", path);
    else if (r == 0) fprintf(out, "[abi] descrittore gia' presente: %s\n", path);
    else if (strstr(err, "ABI folder")) fprintf(out, "[abi] %s\n", err);   /* LLMEnv non trovato: lo si dice */
    /* altri errori (es. non e' un modello di linguaggio): nessun rumore */
}

/* "https://huggingface.co/org/nome/tree/main" | "org/nome" -> "org/nome" (0 = non valido) */
static int parse_repo(const char *in, char *out, size_t cap) {
    const char *p = in;
    char *slash2;
    size_t n;
    if (!strncmp(p, "https://huggingface.co/", 23)) p += 23;
    else if (!strncmp(p, "http://huggingface.co/", 22)) p += 22;
    else if (!strncmp(p, "huggingface.co/", 15)) p += 15;
    snprintf(out, cap, "%s", p);
    slash2 = strchr(out, '/');
    if (!slash2) return 0;
    slash2 = strchr(slash2 + 1, '/');
    if (slash2) *slash2 = '\0';
    n = strlen(out);
    while (n > 0 && out[n - 1] == '/') out[--n] = '\0';
    for (const char *q = out; *q; q++)
        if (!isalnum((unsigned char)*q) && *q != '/' && *q != '.' && *q != '_' && *q != '-') return 0;
    return n > 3;
}

static int curl_get(const char *repo, const char *file, const char *range, const char *dest) {
    char cmd[2048], auth[600] = "";
    const char *tok = getenv("HF_TOKEN");
    if (tok && *tok && strlen(tok) < 400 && !strpbrk(tok, "'\"\\ \n")) snprintf(auth, sizeof auth, "-H 'Authorization: Bearer %s' ", tok);
    snprintf(cmd, sizeof cmd, "curl -fsSL %s%s%s'https://huggingface.co/%s/resolve/main/%s' -o '%s' 2>/dev/null", auth,
             range ? "-r " : "", range ? range : "", repo, file, dest);
    if (range) {   /* curl -r vuole "0-7" seguito da spazio */
        snprintf(cmd, sizeof cmd, "curl -fsSL %s-r %s 'https://huggingface.co/%s/resolve/main/%s' -o '%s' 2>/dev/null", auth,
                 range, repo, file, dest);
    }
    return system(cmd) == 0 && is_file(dest);
}

FmlxCliStatus fmlx_gmbarch_exec(const FmlxCliArgs *a, FILE *out, FILE *err) {
    char path[4096] = "", e[256] = "", repo[300];
    int r;

    if (!out) out = stdout;
    if (!err) err = stderr;
    if (!a->repo[0]) {
        fprintf(err, "fastllm gmbarch <url Hugging Face | org/nome | cartella modello>\n");
        return FMLX_CLI_USAGE;
    }
    if (is_dir(a->repo)) {
        r = llmdesc_ensure_for_dir(a->repo, NULL, path, sizeof path, e, sizeof e);
    } else {
        char tmpl[] = "/tmp/fastllm_gmbarch_XXXXXX", f[600], cmd[800];
        if (!parse_repo(a->repo, repo, sizeof repo)) {
            fprintf(err, "fastllm gmbarch: '%s' non e' una cartella ne' un repo Hugging Face (org/nome o URL)\n", a->repo);
            return FMLX_CLI_USAGE;
        }
        if (!mkdtemp(tmpl)) { fprintf(err, "fastllm gmbarch: cartella temporanea non creabile\n"); return FMLX_CLI_FAILED; }
        snprintf(f, sizeof f, "%s/config.json", tmpl);
        if (!curl_get(repo, "config.json", NULL, f)) {
            fprintf(err, "fastllm gmbarch: config.json non raggiungibile in %s (repo privato? imposta HF_TOKEN)\n", repo);
            snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmpl);
            if (system(cmd) != 0) { }
            return FMLX_CLI_FAILED;
        }
        snprintf(f, sizeof f, "%s/model.safetensors.index.json", tmpl);
        if (!curl_get(repo, "model.safetensors.index.json", NULL, f)) {
            /* repo con un solo model.safetensors: servono solo i primi byte (8 di lunghezza + header) */
            unsigned char len8[8];
            FILE *h;
            snprintf(f, sizeof f, "%s/len.bin", tmpl);
            if (curl_get(repo, "model.safetensors", "0-7", f) && (h = fopen(f, "rb")) != NULL) {
                unsigned long long hl = 0;
                if (fread(len8, 1, 8, h) == 8) {
                    char range[64];
                    for (int i = 7; i >= 0; i--) hl = hl * 256 + len8[i];
                    fclose(h);
                    snprintf(range, sizeof range, "0-%llu", hl + 7);
                    snprintf(f, sizeof f, "%s/model.safetensors", tmpl);
                    if (hl == 0 || hl > 64ull * 1024 * 1024 || !curl_get(repo, "model.safetensors", range, f)) { /* senza nomi: errore sotto */ }
                } else fclose(h);
            }
        }
        r = llmdesc_ensure_for_dir(tmpl, NULL, path, sizeof path, e, sizeof e);
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmpl);
        if (system(cmd) != 0) { }
    }
    if (r < 0) {
        fprintf(err, "fastllm gmbarch: %s\n", e);
        return FMLX_CLI_FAILED;
    }
    fprintf(out, "%s: %s\n", r ? "creato" : "gia' presente", path);
    return FMLX_CLI_OK;
}
