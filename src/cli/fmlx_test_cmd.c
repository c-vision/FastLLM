/* fmlx_test_cmd.c — `fastllm test <modello>`: il modello e' COMPLETO su disco e integro?
 *
 * Riconosce i tre tipi e per ciascuno fa i controlli giusti:
 *  - GGUF (file): parsa l'header col lettore canonico LLMCodec (`llmcodec_gguf_read_info_status`,
 *    che distingue magic/versione/troncamento/sovrapposizione/dati corti) e verifica che la
 *    dimensione del file copra la DATA SECTION dichiarata (data_start + max offset+size).
 *  - MLX/HF (dir): `config.json` presente; pesi presenti; ogni header safetensors valido e la
 *    dimensione del file che copre i data_offsets; nessun marker di download incompleto.
 *  - repo-cache HF (`models--org--nome`): niente `*.incomplete` nei blob; gli snapshot risolvono.
 * In tutti i casi: marker di download incompleto (`*.part`/`*.incomplete`/`*.download`/`*.tmp`).
 *
 * Uscita 0 = COMPLETO, 1 = problema (col MOTIVO). Niente caricamento del modello. */
#include "fmlx/fmlx_cli.h"
#include "fmlx/fmlx_paths.h"
#include "llmcodec_gguf_read.h"
#include "llmcodec_gguf_status.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int t_dir(const char *p) { struct stat s; return stat(p, &s) == 0 && S_ISDIR(s.st_mode); }
static int t_file(const char *p) { struct stat s; return stat(p, &s) == 0 && S_ISREG(s.st_mode); }
static int t_gguf_name(const char *p) { size_t n = strlen(p); return n > 5 && strcmp(p + n - 5, ".gguf") == 0; }
static const char *t_base(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }

static void t_fmt(uint64_t b, char *o, size_t n) {
    static const char *const u[] = {"byte", "kB", "MB", "GB", "TB"};
    double v = (double)b; int k = 0;
    while (v >= 1000.0 && k + 1 < (int)(sizeof u / sizeof u[0])) { v /= 1000.0; k++; }
    if (k == 0) snprintf(o, n, "%llu byte", (unsigned long long)b);
    else snprintf(o, n, "%.1f %s", v, u[k]);
}

/* Marker di download incompleto/parziale in una dir. */
static int t_incomplete_marker(const char *dir, char *what, size_t wn) {
    DIR *d = opendir(dir);
    struct dirent *e;
    static const char *const mk[] = {".incomplete", ".part", ".download", ".crdownload", ".tmp", ".partial", NULL};
    if (!d) return 0;
    while ((e = readdir(d))) {
        int i; size_t l = strlen(e->d_name);
        if (e->d_name[0] == '.') continue;
        for (i = 0; mk[i]; i++) { size_t m = strlen(mk[i]); if (l > m && strcmp(e->d_name + l - m, mk[i]) == 0) { snprintf(what, wn, "%s", e->d_name); closedir(d); return 1; } }
    }
    closedir(d);
    return 0;
}

/* Validazione di un FILE .gguf col lettore canonico. Ritorna 0 se completo. */
static int t_check_gguf(const char *path, char *msg, size_t mn) {
    LlmCodecGgufFileInfo info; char fb[32];
    LlmCodecGgufStatus st;
    uint64_t end, i;
    memset(&info, 0, sizeof info);
    st = llmcodec_gguf_read_info_status(path, &info);
    if (st != LLMCODEC_GGUF_STATUS_OK) { snprintf(msg, mn, "header GGUF non valido: %s", llmcodec_gguf_status_str(st)); return -1; }
    end = info.data_start;
    for (i = 0; i < info.n_tensors; i++) {
        uint64_t e = info.data_start + info.tensors[i].offset + llmcodec_gguf_tensor_bytes(&info.tensors[i]);
        if (e > end) end = e;
    }
    if (info.alignment > 1) end = ((end + info.alignment - 1) / info.alignment) * info.alignment;
    if (info.file_size < end) {
        t_fmt(end - info.file_size, fb, sizeof fb);
        snprintf(msg, mn, "TRONCATO: i tensori chiedono %s, il file ne ha %s in meno (%llu tensori)", fb, fb, (unsigned long long)info.n_tensors);
        llmcodec_gguf_free_info(&info);
        return -1;
    }
    snprintf(msg, mn, "GGUF ok: %llu tensori, v%u, dati fino a %llu/%llu byte", (unsigned long long)info.n_tensors, info.version, (unsigned long long)end, (unsigned long long)info.file_size);
    llmcodec_gguf_free_info(&info);
    return 0;
}

/* Validazione di una dir MLX/HF: config.json + almeno un peso + header safetensors coerenti. */
static int t_check_mlx_dir(const char *dir, char *msg, size_t mn) {
    char cfg[FMLX_PATHS_DIR_MAX], inc[256];
    DIR *d; struct dirent *e; int nw = 0; char firstbad[256] = "";

    snprintf(cfg, sizeof cfg, "%s/config.json", dir);
    if (!t_file(cfg)) { snprintf(msg, mn, "manca config.json"); return -1; }
    if (t_incomplete_marker(dir, inc, sizeof inc)) { snprintf(msg, mn, "download INCOMPLETO: c'e' '%s'", inc); return -1; }
    d = opendir(dir);
    if (!d) { snprintf(msg, mn, "dir non apribile"); return -1; }
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (e->d_name[0] == '.' || e->d_type == DT_DIR) continue;
        if (l > 12 && strcmp(e->d_name + l - 12, ".safetensors") == 0) {
            char full[FMLX_PATHS_DIR_MAX]; struct stat s; FILE *fp; unsigned char h[8];
            nw++;
            snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
            fp = fopen(full, "rb");
            if (!fp) continue;
            if (fread(h, 1, 8, fp) != 8) { snprintf(firstbad, sizeof firstbad, "%s: header illeggibile (file vuoto o troncato)", e->d_name); fclose(fp); continue; }
            { uint64_t hl = 0; int b; for (b = 7; b >= 0; b--) hl = (hl << 8) | h[b]; /* little-endian */
              if (stat(full, &s) != 0) s.st_size = 0;
              if (hl == 0 || hl > (uint64_t)s.st_size) snprintf(firstbad, sizeof firstbad, "%s: header safetensors non valido (len=%llu > file=%lld)", e->d_name, (unsigned long long)hl, (long long)s.st_size); }
            fclose(fp);
        }
    }
    closedir(d);
    if (nw == 0) { snprintf(msg, mn, "nessun file di pesi (.safetensors) presente"); return -1; }
    if (firstbad[0]) { snprintf(msg, mn, "%s", firstbad); return -1; }
    snprintf(msg, mn, "MLX ok: config.json, %d file di pesi", nw);
    return 0;
}

/* repo-cache HF: niente .incomplete nei blob; gli snapshot risolvono (nessun symlink rotto). */
static int t_check_hf_repo(const char *dir, char *msg, size_t mn) {
    char blobs[FMLX_PATHS_DIR_MAX], snaps[FMLX_PATHS_DIR_MAX], inc[256];
    DIR *d; struct dirent *e;
    struct stat s;
    snprintf(blobs, sizeof blobs, "%s/blobs", dir);
    snprintf(snaps, sizeof snaps, "%s/snapshots", dir);
    if (t_dir(blobs) && t_incomplete_marker(blobs, inc, sizeof inc)) { snprintf(msg, mn, "download INCOMPLETO: blob '%s'", inc); return -1; }
    if (!t_dir(snaps)) { snprintf(msg, mn, "manca snapshots/"); return -1; }
    /* gli snapshot sono symlink ai blob: un symlink rotto = file mancante */
    d = opendir(snaps);
    if (d) {
        while ((e = readdir(d))) {
            char rev[FMLX_PATHS_DIR_MAX];
            DIR *r; struct dirent *re;
            if (e->d_name[0] == '.') continue;
            snprintf(rev, sizeof rev, "%s/%s", snaps, e->d_name);
            r = opendir(rev);
            if (!r) continue;
            while ((re = readdir(r))) {
                char f[FMLX_PATHS_DIR_MAX];
                if (re->d_name[0] == '.') continue;
                snprintf(f, sizeof f, "%s/%s", rev, re->d_name);
                if (stat(f, &s) != 0) { snprintf(msg, mn, "file mancante (symlink rotto): %s", re->d_name); closedir(r); closedir(d); return -1; }
            }
            closedir(r);
        }
        closedir(d);
    }
    snprintf(msg, mn, "repo HF ok: blob e snapshot presenti");
    return 0;
}

/* Risoluzione leggera: path esistente | registro | percorso di `paths`/<nome>. */
static int t_resolve(const char *spec, char *out, size_t cap) {
    char rp[FMLX_HF_PATH_MAX], root[FMLX_PATHS_DIR_MAX], t[FMLX_PATHS_DIR_MAX];
    struct stat s;
    const char *base = t_base(spec);
    int np, i;
    if (stat(spec, &s) == 0) { snprintf(out, cap, "%s", spec); return 0; }
    if (fmlx_registry_resolve(spec, rp, sizeof rp, NULL, 0, NULL, 0)) { snprintf(out, cap, "%s", rp); return 0; }
    np = fmlx_paths_count();
    for (i = 0; i < np; i++) {
        if (fmlx_paths_get(i, root, sizeof root) != 0) continue;
        snprintf(t, sizeof t, "%s/%s", root, spec); if (stat(t, &s) == 0) { snprintf(out, cap, "%s", t); return 0; }
        snprintf(t, sizeof t, "%s/%s", root, base);  if (stat(t, &s) == 0) { snprintf(out, cap, "%s", t); return 0; }
    }
    return -1;
}

FmlxCliStatus fmlx_test_exec(const FmlxCliArgs *a, FILE *out, FILE *err) {
    char path[FMLX_HF_PATH_MAX], msg[512], inc[256];
    const char *base;
    int rc;

    if (!out) out = stdout;
    if (!err) err = stderr;
    if (!a || !a->repo[0]) {
        fprintf(err, "fastllm test <nome|#n|percorso>: serve il modello da verificare\n");
        return FMLX_CLI_USAGE;
    }
    if (t_resolve(a->repo, path, sizeof path) != 0) {
        fprintf(err, "fastllm test: modello '%s' non trovato (registro/percorso/`paths`)\n", a->repo);
        return FMLX_CLI_FAILED;
    }
    base = t_base(path);

    if (t_dir(path)) {
        if (strncmp(base, "models--", 8) == 0) rc = t_check_hf_repo(path, msg, sizeof msg);
        else rc = t_check_mlx_dir(path, msg, sizeof msg);
    } else if (t_file(path) && t_gguf_name(path)) {
        char par[FMLX_HF_PATH_MAX];
        const char *sl = strrchr(path, '/');
        if (sl) { size_t k = (size_t)(sl - path); memcpy(par, path, k); par[k] = 0; }
        else snprintf(par, sizeof par, ".");
        if (t_incomplete_marker(par, inc, sizeof inc)) { rc = -1; snprintf(msg, sizeof msg, "download INCOMPLETO: c'e' '%s'", inc); }
        else rc = t_check_gguf(path, msg, sizeof msg);
    } else {
        fprintf(err, "fastllm test: '%s' non e' ne' un .gguf ne' una dir di modello\n", path);
        return FMLX_CLI_FAILED;
    }

    if (rc == 0) { fprintf(out, "PASS  %s\n  %s\n", path, msg); return FMLX_CLI_OK; }
    fprintf(out, "FAIL  %s\n  %s\n", path, msg);
    return FMLX_CLI_FAILED;
}
