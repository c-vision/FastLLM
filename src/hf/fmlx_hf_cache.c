/* fmlx_hf_cache.c — percorsi e cache locale di HF Hub, SENZA rete (vedi fmlx_hf.h).
 *
 * Ogni regola di qui è misurata da una fonte citata, non ricordata:
 * Le righe citate qui sotto sono della copia `~/ai/env-mflux` di huggingface_hub 0.36.2;
 * su questa macchina ne esiste ANCHE un'altra `~/ai/env-vllmlx` (stesso numero, altro file)
 * con le STESSE regole a righe diverse: tabella in LICENSES.md.
 *  - radice del cache: `huggingface_hub/constants.py:166-189` (HF_HUB_CACHE → legacy
 *    HUGGINGFACE_HUB_CACHE → HF_HOME/hub → XDG_CACHE_HOME/huggingface/hub → ~/.cache/...,
 *    con expanduser PRIMA di expandvars);
 *  - offline: `constants.py:202` + `ENV_VARS_TRUE_VALUES` a `constants.py:11` = {1,ON,YES,TRUE};
 *  - endpoint e URL: `constants.py:68-69` (ENDPOINT con rstrip("/") e
 *    "{endpoint}/{repo_id}/resolve/{revision}/{filename}");
 *  - nome di directory: `constants.py:118` REPO_ID_SEPARATOR = "--", e un id SENZA namespace
 *    resta un solo segmento (`utils/_cache_manager.py:480` mostra models--roberta-base).
 */
#include "fmlx/fmlx_hf.h"
#include "fmlx_hf_internal.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_msg[FMLX_HF_PATH_MAX + 256];

const char *fmlx_hf_last_error(void) { return g_msg[0] ? g_msg : NULL; }

FmlxHfStatus fmlx_hf_fail(FmlxHfStatus s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(g_msg, sizeof g_msg, fmt, ap);
    va_end(ap);
    return s;
}

const char *fmlx_hf_status_str(FmlxHfStatus s) {
    switch (s) {
        case FMLX_HF_OK: return "ok";
        case FMLX_HF_ERR_BUF_SMALL: return "buffer troppo piccolo";
        case FMLX_HF_ERR_NO_HOME: return "nessuna HOME/HF_HOME/HF_HUB_CACHE nell'ambiente";
        case FMLX_HF_ERR_REPO_ID: return "repo id malformato";
        case FMLX_HF_ERR_NAME: return "path del file nel repo inaccettabile";
        case FMLX_HF_ERR_REVISION: return "revisione inaccettabile";
        case FMLX_HF_ERR_REF_MISSING: return "refs/<branch> assente";
        case FMLX_HF_ERR_REF_CONTENT: return "refs/<branch> non contiene un commit a 40 hex";
        case FMLX_HF_ERR_NO_REPO: return "il repo non è nel cache";
        case FMLX_HF_ERR_SNAPSHOT_MISSING: return "lo snapshot della revisione non è nel cache";
        case FMLX_HF_ERR_NOT_CACHED: return "file non scaricato (il remote potrebbe averlo)";
        case FMLX_HF_ERR_NOT_ON_REMOTE: return "marker .no_exist: il remote non ha questo file";
        case FMLX_HF_ERR_LINK_FORM: return "symlink fuori dalla forma ../../blobs/<nome>";
        case FMLX_HF_ERR_BLOB_MISSING: return "symlink pendente: il blob puntato non esiste";
        case FMLX_HF_ERR_IO: return "errore di I/O";
        case FMLX_HF_ERR_HASH_MISMATCH: return "il contenuto non corrisponde al nome del blob";
        case FMLX_HF_ERR_NOT_VERIFIABLE: return "entry non verificabile dal nome";
        case FMLX_HF_ERR_NOT_A_FILE: return "la voce e' una directory, non un file";
        case FMLX_HF_ERR_OFFLINE: return "offline: il file non e' nel cache e la rete e' vietata";
        case FMLX_HF_ERR_HTTP: return "risposta HTTP non valida";
        case FMLX_HF_ERR_LOCKED: return "blob bloccato da un altro processo";
        case FMLX_HF_ERR_NO_ETAG: return "il server non ha fornito un etag";
        case FMLX_HF_ERR_SIZE: return "lunghezza ricevuta diversa da quella dichiarata";
        case FMLX_HF_ERR_BAD_ETAG: return "l'etag del server non e' un nome di file accettabile";
        case FMLX_HF_ERR_NO_SIZE: return "il server non ha dichiarato una lunghezza";
        case FMLX_HF_ERR_STALL: return "lo stream si e' fermato: nessun byte per il tempo limite";
        case FMLX_HF_ERR_TOO_MANY: return "la risposta chiede piu' di quanto questo limite tiene";
        case FMLX_HF_ERR_NO_CACHE: return "la radice del cache non esiste";
        case FMLX_HF_ERR_NO_MATCH: return "nessun file del repo corrisponde al filtro --file";
    }
    return "codice sconosciuto";
}

FmlxHfStatus fmlx_hf_pjoin(char *out, size_t n, const char *a, const char *b) {
    int k = snprintf(out, n, "%s/%s", a, b);
    if (k < 0 || (size_t)k >= n) {
        out[0] = '\0';
        return fmlx_hf_fail(FMLX_HF_ERR_BUF_SMALL, "path '%s/%s' non sta in %zu byte", a, b, n);
    }
    return FMLX_HF_OK;
}

/* ---- creare directory ----
 * Stanno qui e non nella lane download perché non c'entrano nulla con la rete: sono la STESSA
 * famiglia di regole di `fmlx_hf_pjoin`, e la prova è chi le chiama — `fmlx_hf_fetch.c` (che le ha
 * viste nascere), il `pull` della CLI (che costruisce il `--dest`) e i test che si preparano un
 * cache finto. Stare nella lane "nessuna rete" è ciò che permette a un test di catalogo di creare
 * directory senza linkarci dentro libcurl. */
int fmlx_hf_mkdir_p(const char *path) {
    char buf[FMLX_HF_PATH_MAX];
    if (!path || !*path) return -1;
    snprintf(buf, sizeof buf, "%s", path);
    for (char *c = buf + 1; *c; c++) {
        if (*c != '/') continue;
        *c = '\0';
        if (mkdir(buf, 0777) != 0 && errno != EEXIST) return -1;
        *c = '/';
    }
    return mkdir(buf, 0777) == 0 || errno == EEXIST ? 0 : -1;
}

/* SOLO il genitore. Il nome dice esattamente ciò che fa: `fmlx_hf_mkdir_p(dirname(path))`. Chi deve
 * scrivere IN una directory (blobs/, il repo stesso) deve chiamare fmlx_hf_mkdir_p su QUELLA
 * directory: confondere i due è il bug che ha reso impossibile il primo download. */
int fmlx_hf_ensure_parent(const char *path) {
    char dir[FMLX_HF_PATH_MAX];
    const char *slash = strrchr(path, '/');
    if (!slash) return 0;
    size_t n = (size_t)(slash - path);
    if (n == 0 || n >= sizeof dir) return -1;
    memcpy(dir, path, n);
    dir[n] = '\0';
    return fmlx_hf_mkdir_p(dir);
}

/* ---- validazione ---- */

/* Non-static da quando l'audit deve rispondere alla STESSA domanda: `resolve_ref` accetta una
 * revisione come commit SOLO se sono 40 esadecimali minuscoli, quindi un verificatore che ammettesse
 * anche il maiuscolo guarderebbe directory che il `pull` non raggiungerebbe mai — e il `doctor`
 * direbbe «a posto» su uno snapshot che la lane download non sa caricare. */
int fmlx_hf_is_hex_lower(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    return 1;
}

/* Segmento di path: rifiuta vuoto, "." e "..", i caratteri di controllo e il backslash.
 * Tutto il resto passa — spazi e UTF-8 compresi — PERCHE' il mondo reale li usa: nel cache
 * misurato c'è `.gitattributes` (punto iniziale legittimo) e su Hub esistono nomi di repo
 * con spazi. Essere più stretti qui non è sicurezza: è un modello che non si scarica.
 * `for_repo_id` aggiunge i soli vincoli che servono a NOI, dove il segmento diventa parte
 * di un nome di directory con separatore "--": niente "." iniziale/finale (collisione con
 * la convenzione dei file di lock e con `.no_exist`) e niente spazio iniziale/finale. */
static int segment_ok(const char *s, size_t n, int for_repo_id) {
    if (n == 0) return 0;
    if (n == 1 && s[0] == '.') return 0;
    if (n == 2 && s[0] == '.' && s[1] == '.') return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7f || c == '\\') return 0;
        if (!for_repo_id) continue;
        if (i == 0 && (c == '.' || c == ' ')) return 0;
        if (i + 1 == n && (c == '.' || c == ' ')) return 0;
    }
    return 1;
}

/* UN SOLO segmento, e alla nostra stregua: la stessa regola vale per il nome letto da disco
 * (un symlink dello snapshot) e per quello dato dal server (l'etag, che DIVENTA il nome del
 * blob). Sono due fonti ostili diverse, e una sola regola da non duplicare: un etag con dentro
 * un "/" o un ".." non deve mai scegliere un path nel nostro cache. */
int fmlx_hf_name_is_safe(const char *name) {
    if (!name) return 0;
    size_t n = strlen(name);
    if (n == 0 || n >= FMLX_HF_BLOB_NAME_MAX) return 0;
    if (strchr(name, '/')) return 0;
    return segment_ok(name, n, 1);
}

FmlxHfHashKind fmlx_hf_blob_hash_kind(const char *blob_name) {
    if (!blob_name || !*blob_name) return FMLX_HF_HASH_NONE;
    size_t n = strlen(blob_name);
    if (n == 40 && fmlx_hf_is_hex_lower(blob_name, n)) return FMLX_HF_HASH_GIT_BLOB;
    if (n == 64 && fmlx_hf_is_hex_lower(blob_name, n)) return FMLX_HF_HASH_SHA256;
    return FMLX_HF_HASH_NONE;
}

/* ---- ambiente ---- */

/* expanduser di Apple/POSIX in miniatura: solo "~" e "~/<rest>", nessun "~utente"
 * (l'ambiente di HF non lo usa mai per queste variabili). */
static int expand_tilde(const char *in, char *out, size_t out_len) {
    if (in[0] != '~') return snprintf(out, out_len, "%s", in) < (int)out_len ? 0 : -1;
    const char *home = getenv("HOME");
    if (!home) home = "";
    if (in[1] == '\0') return snprintf(out, out_len, "%s", home) < (int)out_len ? 0 : -1;
    if (in[1] != '/') return snprintf(out, out_len, "%s", in) < (int)out_len ? 0 : -1;
    return snprintf(out, out_len, "%s%s", home, in + 1) < (int)out_len ? 0 : -1;
}

/* expandvars: solo "$NAME" e "${NAME}", che è tutto ciò che compare nell'uso reale di queste
 * variabili. Un `$` che non apre una variabile resta letterale. */
static int expand_vars(const char *in, char *out, size_t out_len) {
    size_t o = 0;
    for (size_t i = 0; in[i];) {
        if (in[i] != '$') {
            if (o + 1 >= out_len) return -1;
            out[o++] = in[i++];
            continue;
        }
        size_t j = i + 1;
        int braced = 0;
        if (in[j] == '{') { braced = 1; j++; }
        size_t start = j;
        while (in[j] && (isalnum((unsigned char)in[j]) || in[j] == '_')) j++;
        size_t name_len = j - start;
        if (braced && in[j] == '}') j++;
        if (name_len == 0) { /* "$" nudo o "${}" : letterale */
            if (o + 1 >= out_len) return -1;
            out[o++] = in[i++];
            continue;
        }
        char name[128];
        if (name_len >= sizeof name) return -1;
        memcpy(name, in + start, name_len);
        name[name_len] = '\0';
        const char *val = getenv(name);
        if (!val) val = "";
        size_t vl = strlen(val);
        if (o + vl >= out_len) return -1;
        memcpy(out + o, val, vl);
        o += vl;
        i = j;
    }
    out[o] = '\0';
    return 0;
}

/* Stesso ordine di Python: expanduser PRIMA di expandvars (constants.py:168-189: expanduser dopo expandvars, entrambi su HF_HOME e HF_HUB_CACHE). */
static int hf_expand(const char *in, char *out, size_t out_len) {
    char tmp[FMLX_HF_PATH_MAX];
    if (expand_tilde(in, tmp, sizeof tmp) != 0) return -1;
    return expand_vars(tmp, out, out_len);
}

static void rtrim_slash(char *s) {
    size_t n = strlen(s);
    while (n > 1 && s[n - 1] == '/') s[--n] = '\0';
}

FmlxHfStatus fmlx_hf_cache_root(char *out, size_t out_len) {
    const char *v;
    char hf_home[FMLX_HF_PATH_MAX];

    v = getenv("HF_HUB_CACHE");
    if (v && *v) {
        if (hf_expand(v, out, out_len) != 0)
            return fmlx_hf_fail(FMLX_HF_ERR_BUF_SMALL, "HF_HUB_CACHE espansa non sta in %zu byte", out_len);
        rtrim_slash(out);
        return FMLX_HF_OK;
    }
    /* Il legacy ha la precedenza su HF_HOME: a constants.py:189 HUGGINGFACE_HUB_CACHE è il
     * DEFAULT di HF_HUB_CACHE, quindi viene consultato prima, non dopo. */
    v = getenv("HUGGINGFACE_HUB_CACHE");
    if (v && *v) {
        if (hf_expand(v, out, out_len) != 0)
            return fmlx_hf_fail(FMLX_HF_ERR_BUF_SMALL, "HUGGINGFACE_HUB_CACHE espansa non sta in %zu byte", out_len);
        rtrim_slash(out);
        return FMLX_HF_OK;
    }
    if (getenv("HF_HOME") && *getenv("HF_HOME")) {
        if (hf_expand(getenv("HF_HOME"), hf_home, sizeof hf_home) != 0)
            return fmlx_hf_fail(FMLX_HF_ERR_BUF_SMALL, "HF_HOME espansa non sta in %d byte", FMLX_HF_PATH_MAX);
        rtrim_slash(hf_home); /* come os.path.join: "/h/" + "hub" = "/h/hub", non "/h//hub" */
    } else {
        const char *xdg = getenv("XDG_CACHE_HOME");
        const char *home = getenv("HOME");
        if (xdg && *xdg) {
            if (hf_expand(xdg, hf_home, sizeof hf_home) != 0)
                return fmlx_hf_fail(FMLX_HF_ERR_BUF_SMALL, "XDG_CACHE_HOME espansa non sta in %d byte", FMLX_HF_PATH_MAX);
        } else {
            if (!home || !*home) return fmlx_hf_fail(FMLX_HF_ERR_NO_HOME, "nessuna HOME da cui derivare il cache");
            if (snprintf(hf_home, sizeof hf_home, "%s/.cache", home) >= (int)sizeof hf_home)
                return fmlx_hf_fail(FMLX_HF_ERR_BUF_SMALL, "HOME troppo lunga");
        }
        rtrim_slash(hf_home); /* vale anche per XDG_CACHE_HOME con slash finale */
        size_t n = strlen(hf_home);
        if (n + strlen("/huggingface") >= sizeof hf_home)
            return fmlx_hf_fail(FMLX_HF_ERR_BUF_SMALL, "HF_HOME derivata troppo lunga");
        memcpy(hf_home + n, "/huggingface", strlen("/huggingface") + 1);
    }
    if (fmlx_hf_pjoin(out, out_len, hf_home, "hub") != FMLX_HF_OK)
        return FMLX_HF_ERR_BUF_SMALL; /* il messaggio lo ha messo pjoin */
    rtrim_slash(out);
    return FMLX_HF_OK;
}

static int truthy(const char *v) {
    if (!v) return 0;
    char t[8];
    size_t i = 0;
    for (; v[i] && i < sizeof t - 1; i++) t[i] = (char)toupper((unsigned char)v[i]);
    t[i] = '\0';
    return strcmp(t, "1") == 0 || strcmp(t, "ON") == 0 || strcmp(t, "YES") == 0 || strcmp(t, "TRUE") == 0;
}

int fmlx_hf_offline(void) {
    /* constants.py:202: HF_HUB_OFFLINE oppure TRANSFORMERS_OFFLINE, valori {1,ON,YES,TRUE} */
    return truthy(getenv("HF_HUB_OFFLINE")) || truthy(getenv("TRANSFORMERS_OFFLINE"));
}

FmlxHfStatus fmlx_hf_endpoint(char *out, size_t out_len) {
    const char *v = getenv("HF_ENDPOINT");
    if (!v || !*v) v = FMLX_HF_ENDPOINT_DEFAULT;
    if (snprintf(out, out_len, "%s", v) >= (int)out_len)
        return fmlx_hf_fail(FMLX_HF_ERR_BUF_SMALL, "endpoint non sta in %zu byte", out_len);
    rtrim_slash(out);
    return FMLX_HF_OK;
}

/* ---- percorsi ---- */

FmlxHfStatus fmlx_hf_repo_dirname(const char *repo_id, char *out, size_t out_len) {
    if (!repo_id || !*repo_id) return fmlx_hf_fail(FMLX_HF_ERR_REPO_ID, "repo id vuoto");
    const char *slash = strchr(repo_id, '/');
    size_t n = strlen(repo_id);
    if (slash && strchr(slash + 1, '/'))
        return fmlx_hf_fail(FMLX_HF_ERR_REPO_ID, "repo id '%s': piu di un '/'", repo_id);
    if (slash) {
        size_t a = (size_t)(slash - repo_id), b = n - a - 1;
        if (!segment_ok(repo_id, a, 1) || !segment_ok(slash + 1, b, 1))
            return fmlx_hf_fail(FMLX_HF_ERR_REPO_ID, "repo id '%s': un segmento e' vuoto o vietato", repo_id);
    } else {
        /* Id senza namespace: legittimo e misurato (models--roberta-base). */
        if (!segment_ok(repo_id, n, 1))
            return fmlx_hf_fail(FMLX_HF_ERR_REPO_ID, "repo id '%s': vietato", repo_id);
    }
    int k;
    if (slash) k = snprintf(out, out_len, "models--%.*s--%s", (int)(slash - repo_id), repo_id, slash + 1);
    else k = snprintf(out, out_len, "models--%s", repo_id);
    if (k < 0 || (size_t)k >= out_len) {
        out[0] = '\0';
        return fmlx_hf_fail(FMLX_HF_ERR_BUF_SMALL, "nome di directory non sta in %zu byte", out_len);
    }
    return FMLX_HF_OK;
}

FmlxHfStatus fmlx_hf_repo_dir(const char *cache_root, const char *repo_id, char *out, size_t out_len) {
    char dir[FMLX_HF_PATH_MAX];
    FmlxHfStatus s = fmlx_hf_repo_dirname(repo_id, dir, sizeof dir);
    if (s != FMLX_HF_OK) return s;
    /* Ambiguità ereditata, non nostra: "a--b/c" e "a/b--c" danno LO STESSO nome di directory,
     * perche' HF separa con "--" e non sfugge nulla (constants.py:118). Non possiamo risolverla
     * qui, quindi la dichiariamo: chi ha bisogno di un mapping iniettivo deve passare l'id. */
    return fmlx_hf_pjoin(out, out_len, cache_root, dir);
}

FmlxHfStatus fmlx_hf_check_relpath(const char *relpath) {
    if (!relpath || !*relpath) return fmlx_hf_fail(FMLX_HF_ERR_NAME, "path del file vuoto");
    if (relpath[0] == '/') return fmlx_hf_fail(FMLX_HF_ERR_NAME, "path assoluto: '%s'", relpath);
    size_t start = 0;
    for (;;) {
        const char *slash = strchr(relpath + start, '/');
        size_t n = slash ? (size_t)(slash - (relpath + start)) : strlen(relpath + start);
        if (!segment_ok(relpath + start, n, 0))
            return fmlx_hf_fail(FMLX_HF_ERR_NAME, "segmento '%.*s' di '%s' vietato", (int)n, relpath + start, relpath);
        if (!slash) break;
        start += n + 1;
        if (relpath[start] == '\0')
            return fmlx_hf_fail(FMLX_HF_ERR_NAME, "'%s' finisce con '/': non e' un file", relpath);
    }
    return FMLX_HF_OK;
}

static int read_ref_file(const char *path, char *sha_out, size_t sha_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char buf[128];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    if (n == 0) return -1;
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ' || buf[n - 1] == '\t')) buf[--n] = '\0';
    if (n != 40 || !fmlx_hf_is_hex_lower(buf, n)) return -1;
    if (sha_len < 41) return -1;
    memcpy(sha_out, buf, 41);
    return 0;
}

FmlxHfStatus fmlx_hf_resolve_ref(const char *repo_dir, const char *revision, char *sha_out, size_t sha_len) {
    if (sha_len < 41) return fmlx_hf_fail(FMLX_HF_ERR_BUF_SMALL, "serve spazio per 40 hex + NUL");
    if (!revision || !*revision) return fmlx_hf_fail(FMLX_HF_ERR_REVISION, "revisione vuota");
    size_t rn = strlen(revision);
    if (rn == 40 && fmlx_hf_is_hex_lower(revision, rn)) {
        memcpy(sha_out, revision, 41);
        return FMLX_HF_OK;
    }
    if (!segment_ok(revision, rn, 1))
        return fmlx_hf_fail(FMLX_HF_ERR_REVISION, "revisione '%s' inaccettabile", revision);
    char ref[FMLX_HF_PATH_MAX];
    if (fmlx_hf_pjoin(ref, sizeof ref, repo_dir, "refs") != FMLX_HF_OK) return FMLX_HF_ERR_BUF_SMALL;
    FmlxHfStatus s = fmlx_hf_pjoin(ref, sizeof ref, ref, revision);
    if (s != FMLX_HF_OK) return s;
    if (read_ref_file(ref, sha_out, sha_len) != 0) {
        if (access(ref, R_OK) != 0)
            return fmlx_hf_fail(FMLX_HF_ERR_REF_MISSING, "nessun ref: %s", ref);
        return fmlx_hf_fail(FMLX_HF_ERR_REF_CONTENT, "ref non a 40 hex: %s", ref);
    }
    return FMLX_HF_OK;
}

FmlxHfStatus fmlx_hf_url(const char *repo_id, const char *revision, const char *relpath, int use_resolve,
                         char *out, size_t out_len) {
    char endpoint[FMLX_HF_PATH_MAX];
    FmlxHfStatus s = fmlx_hf_endpoint(endpoint, sizeof endpoint);
    if (s != FMLX_HF_OK) return s;
    return fmlx_hf_url_with(endpoint, repo_id, revision, relpath, use_resolve, out, out_len);
}

/* `quote(s, safe)` di Python applicato agli stessi argomenti che usa huggingface_hub
 * (file_download.py:278-280: `quote(revision, safe="")` e `quote(filename)` con il default
 * `safe="/"`). Unreserved = [A-Za-z0-9._~-] più i caratteri di `safe`; tutto il resto — UTF-8
 * compreso, byte per byte — diventa %XX. Non è un dettaglio estetico: "model_é.bin" chiesto crudo
 * non è la stessa richiesta che fa huggingface_hub, e due client sullo stesso cache devono
 * chiedere la STESSA cosa al server. Non è statica: anche il manifesto dei file (piano §12.1)
 * codifica la revisione, e due copie di `quote()` divergerebbero proprio sul ramo che contiene
 * una `/` o una `%`. */
int fmlx_hf_pct_encode(const char *s, const char *safe, char *out, size_t out_len) {
    static const char keep[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._~-";
    size_t o = 0;
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        int plain = strchr(keep, (char)*c) != NULL || strchr(safe, (char)*c) != NULL;
        if (plain) {
            if (o + 1 >= out_len) return -1;
            out[o++] = (char)*c;
            continue;
        }
        if (o + 3 >= out_len) return -1;
        o += (size_t)snprintf(out + o, out_len - o, "%%%02X", *c);
    }
    out[o] = '\0';
    return 0;
}

/* Il template, in un posto solo. `fmlx_hf_url` lo chiama con l'endpoint dell'ambiente; chi ha
 * un endpoint esplicito (FmlxHfFetchOpts.endpoint) passa di qui, cosi' NON esiste una seconda
 * copia del formato da far divergere. */
FmlxHfStatus fmlx_hf_url_with(const char *endpoint, const char *repo_id, const char *revision,
                              const char *relpath, int use_resolve, char *out, size_t out_len) {
    char ep[FMLX_HF_PATH_MAX];
    FmlxHfStatus s;
    if (!endpoint || !*endpoint) return fmlx_hf_fail(FMLX_HF_ERR_REVISION, "endpoint vuoto");
    if (snprintf(ep, sizeof ep, "%s", endpoint) >= (int)sizeof ep)
        return fmlx_hf_fail(FMLX_HF_ERR_BUF_SMALL, "endpoint non sta in %d byte", FMLX_HF_PATH_MAX);
    rtrim_slash(ep); /* un endpoint dato con lo slash finale non deve produrre "//org/repo" */
    if (!repo_id || !*repo_id) return fmlx_hf_fail(FMLX_HF_ERR_REPO_ID, "repo id vuoto");
    if (use_resolve) {
        s = fmlx_hf_check_relpath(relpath);
        if (s != FMLX_HF_OK) return s;
        if (strchr(repo_id, '/')) {
            /* id con namespace: com'è nel template di constants.py:69 */
        } else if (!segment_ok(repo_id, strlen(repo_id), 1)) {
            return fmlx_hf_fail(FMLX_HF_ERR_REPO_ID, "repo id '%s' vietato", repo_id);
        }
    } else {
        if (!relpath || !*relpath) return fmlx_hf_fail(FMLX_HF_ERR_NAME, "path vuoto");
    }
    if (!revision || !*revision) return fmlx_hf_fail(FMLX_HF_ERR_REVISION, "revisione vuota");
    char rev[FMLX_HF_PATH_MAX * 3], rel[FMLX_HF_PATH_MAX * 3];
    if (fmlx_hf_pct_encode(revision, "", rev, sizeof rev) != 0)
        return fmlx_hf_fail(FMLX_HF_ERR_BUF_SMALL, "revisione codificata non sta in buffer");
    if (use_resolve && fmlx_hf_pct_encode(relpath, "/", rel, sizeof rel) != 0)
        return fmlx_hf_fail(FMLX_HF_ERR_BUF_SMALL, "path codificato non sta in buffer");
    int k;
    if (use_resolve)
        k = snprintf(out, out_len, "%s/%s/resolve/%s/%s", ep, repo_id, rev, rel);
    else
        k = snprintf(out, out_len, "%s/api/models/%s/revision/%s", ep, repo_id, rev);
    if (k < 0 || (size_t)k >= out_len) {
        out[0] = '\0';
        return fmlx_hf_fail(FMLX_HF_ERR_BUF_SMALL, "URL non sta in %zu byte", out_len);
    }
    return FMLX_HF_OK;
}

/* ---- locate ---- */

static FmlxHfStatus check_no_exist(const char *repo_dir, const char *sha, const char *relpath,
                                   FmlxHfStatus if_absent) {
    char p[FMLX_HF_PATH_MAX];
    if (fmlx_hf_pjoin(p, sizeof p, repo_dir, ".no_exist") != FMLX_HF_OK) return FMLX_HF_ERR_BUF_SMALL;
    if (fmlx_hf_pjoin(p, sizeof p, p, sha) != FMLX_HF_OK) return FMLX_HF_ERR_BUF_SMALL;
    char path[FMLX_HF_PATH_MAX];
    if (fmlx_hf_pjoin(path, sizeof path, p, relpath) != FMLX_HF_OK) return FMLX_HF_ERR_BUF_SMALL;
    struct stat st;
    if (stat(path, &st) == 0)
        return fmlx_hf_fail(FMLX_HF_ERR_NOT_ON_REMOTE, "marker presente: %s", path);
    return if_absent;
}

FmlxHfStatus fmlx_hf_locate(const char *cache_root, const char *repo_id, const char *revision,
                            const char *relpath, FmlxHfRef *out) {
    char root[FMLX_HF_PATH_MAX];
    memset(out, 0, sizeof *out); /* anche SU FALLIMENTO: un chiamante non deve mai leggere un residuo */
    if (!cache_root) {
        FmlxHfStatus s = fmlx_hf_cache_root(root, sizeof root);
        if (s != FMLX_HF_OK) return s;
        cache_root = root;
    }
    FmlxHfStatus s = fmlx_hf_check_relpath(relpath);
    if (s != FMLX_HF_OK) return s;

    char repo[FMLX_HF_PATH_MAX];
    s = fmlx_hf_repo_dir(cache_root, repo_id, repo, sizeof repo);
    if (s != FMLX_HF_OK) return s;
    struct stat st;
    if (stat(repo, &st) != 0) {
        if (errno == ENOENT) return fmlx_hf_fail(FMLX_HF_ERR_NO_REPO, "nessun repo nel cache: %s", repo);
        return fmlx_hf_fail(FMLX_HF_ERR_IO, "stat(%s): %s", repo, strerror(errno));
    }

    char sha[64];
    s = fmlx_hf_resolve_ref(repo, revision, sha, sizeof sha);
    if (s != FMLX_HF_OK) return s;

    char snap[FMLX_HF_PATH_MAX];
    if (fmlx_hf_pjoin(snap, sizeof snap, repo, "snapshots") != FMLX_HF_OK) return FMLX_HF_ERR_BUF_SMALL;
    if (fmlx_hf_pjoin(snap, sizeof snap, snap, sha) != FMLX_HF_OK) return FMLX_HF_ERR_BUF_SMALL;
    if (stat(snap, &st) != 0) {
        if (errno == ENOENT) {
            FmlxHfStatus m = check_no_exist(repo, sha, relpath, FMLX_HF_ERR_SNAPSHOT_MISSING);
            if (m != FMLX_HF_ERR_SNAPSHOT_MISSING) return m; /* il marker vince: non scaricare */
            return fmlx_hf_fail(m, "snapshot %s assente nel cache", snap);
        }
        return fmlx_hf_fail(FMLX_HF_ERR_IO, "stat(%s): %s", snap, strerror(errno));
    }

    char entry[FMLX_HF_PATH_MAX];
    if (fmlx_hf_pjoin(entry, sizeof entry, snap, relpath) != FMLX_HF_OK) return FMLX_HF_ERR_BUF_SMALL;
    struct stat lst;
    if (lstat(entry, &lst) != 0) {
        if (errno == ENOENT) {
            FmlxHfStatus m = check_no_exist(repo, sha, relpath, FMLX_HF_ERR_NOT_CACHED);
            if (m != FMLX_HF_ERR_NOT_CACHED) return m;
            return fmlx_hf_fail(m, "entry assente: %s", entry);
        }
        return fmlx_hf_fail(FMLX_HF_ERR_IO, "lstat(%s): %s", entry, strerror(errno));
    }

    memset(out, 0, sizeof *out);

    if (S_ISREG(lst.st_mode)) {
        /* Entry come file regolare: esiste, misurato — chi ha scaricato con un altro tool
         * (Rapid-MLX) scrive i byte direttamente nello snapshot senza symlink. Usabile, ma
         * il nome non promette un hash: la verifica dira' NOT_VERIFIABLE, non "ok". */
        snprintf(out->path, sizeof out->path, "%s", entry);
        out->is_blob_link = 0;
        out->hash_kind = FMLX_HF_HASH_NONE;
        out->size = (int64_t)lst.st_size;
        return FMLX_HF_OK;
    }
    if (S_ISDIR(lst.st_mode))
        return fmlx_hf_fail(FMLX_HF_ERR_NOT_A_FILE,
                            "%s e' una directory: passare il file dentro di essa", entry);
    if (!S_ISLNK(lst.st_mode))
        return fmlx_hf_fail(FMLX_HF_ERR_LINK_FORM, "%s non e' ne' file ne' symlink", entry);

    char link[FMLX_HF_PATH_MAX];
    ssize_t ll = readlink(entry, link, sizeof link - 1);
    if (ll < 0) return fmlx_hf_fail(FMLX_HF_ERR_IO, "readlink(%s): %s", entry, strerror(errno));
    link[ll] = '\0';
    if (link[0] == '/')
        return fmlx_hf_fail(FMLX_HF_ERR_LINK_FORM, "symlink assoluto, non relativo: %s -> %s", entry, link);

    /* Forma esatta: (../)×(2 + sottocartelle del path) + "blobs/" + un solo segmento.
     * Confrontata con la regola, non "risolta e basta": un symlink che punta altrove
     * (es. ../../../../etc/passwd, o un blob fuori dal repo) e' un rifiuto, non un path. */
    size_t depth = 2;
    for (const char *c = relpath; *c; c++)
        if (*c == '/') depth++;
    size_t want = depth * 3; /* "../" per ogni livello */
    if ((size_t)ll < want + strlen("blobs/") || memcmp(link, "../../", 6) != 0)
        return fmlx_hf_fail(FMLX_HF_ERR_LINK_FORM, "prefisso non ../../: %s -> %s", entry, link);
    size_t i = 0;
    for (size_t lvl = 0; lvl < depth; lvl++) {
        if (memcmp(link + i, "../", 3) != 0)
            return fmlx_hf_fail(FMLX_HF_ERR_LINK_FORM, "profondita' %zu attesa, link '%s' (%s)", depth, link, entry);
        i += 3;
    }
    if (memcmp(link + i, "blobs/", 6) != 0)
        return fmlx_hf_fail(FMLX_HF_ERR_LINK_FORM, "non punta a blobs/: %s -> %s", entry, link);
    i += 6;
    const char *name = link + i; /* `link` e' NUL-terminato qui: strlen(name) == ll - i */
    if (!fmlx_hf_name_is_safe(name))
        return fmlx_hf_fail(FMLX_HF_ERR_LINK_FORM, "nome di blob non un solo segmento: '%s' (%s)", name, entry);

    char blob[FMLX_HF_PATH_MAX];
    if (fmlx_hf_pjoin(blob, sizeof blob, repo, "blobs") != FMLX_HF_OK) return FMLX_HF_ERR_BUF_SMALL;
    char nb[FMLX_HF_BLOB_NAME_MAX];
    snprintf(nb, sizeof nb, "%s", name); /* sta: name_is_safe ha gia' controllato la lunghezza */
    if (fmlx_hf_pjoin(blob, sizeof blob, blob, nb) != FMLX_HF_OK) return FMLX_HF_ERR_BUF_SMALL;
    struct stat bst;
    if (stat(blob, &bst) != 0) {
        if (errno == ENOENT) return fmlx_hf_fail(FMLX_HF_ERR_BLOB_MISSING, "blob assente: %s", blob);
        return fmlx_hf_fail(FMLX_HF_ERR_IO, "stat(%s): %s", blob, strerror(errno));
    }
    if (!S_ISREG(bst.st_mode)) return fmlx_hf_fail(FMLX_HF_ERR_BLOB_MISSING, "blob non regolare: %s", blob);

    snprintf(out->path, sizeof out->path, "%s", blob);
    snprintf(out->blob_name, sizeof out->blob_name, "%s", nb);
    out->hash_kind = fmlx_hf_blob_hash_kind(out->blob_name);
    out->is_blob_link = 1;
    out->size = (int64_t)bst.st_size;
    return FMLX_HF_OK;
}

FmlxHfStatus fmlx_hf_verify(const FmlxHfRef *ref, int64_t *bytes_hashed) {
    if (ref->hash_kind == FMLX_HF_HASH_NONE)
        return fmlx_hf_fail(FMLX_HF_ERR_NOT_VERIFIABLE, "il nome '%s' non promette un hash",
                            ref->blob_name[0] ? ref->blob_name : ref->path);
    int fd = open(ref->path, O_RDONLY);
    if (fd < 0) return fmlx_hf_fail(FMLX_HF_ERR_IO, "open(%s): %s", ref->path, strerror(errno));
    char hex[FMLX_HF_HEX_MAX];
    int64_t got = 0;
    int rc = fmlx_hf_hash_fd(fd, ref->hash_kind, ref->size, hex, &got);
    close(fd);
    if (rc != 0) return fmlx_hf_fail(FMLX_HF_ERR_IO, "hash di %s: %s", ref->path, strerror(errno));
    if (bytes_hashed) *bytes_hashed = got;
    if (strcmp(hex, ref->blob_name) != 0)
        return fmlx_hf_fail(FMLX_HF_ERR_HASH_MISMATCH, "%s: nome=%s calcolato=%s (%lld byte)", ref->path,
                            ref->blob_name, hex, (long long)got);
    return FMLX_HF_OK;
}
