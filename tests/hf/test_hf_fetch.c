/* test_hf_fetch.c — lane download SENZA RETE.
 *
 * Il transport è finto per scelta, non per pigrizia: HTTP è il mondo esterno e non entra nei test.
 * Tutto ciò che merita di rompersi — DOVE finiscono i byte, quando si rinomina, quando si rifiuta,
 * chi prende il lock — è qui, e qui è provato. Il trasporto reale (fmlx_hf_curl.c) non decide
 * nulla sul cache e infatti qui non compare.
 *
 * Le aspettative di hash vengono da UN'ALTRA fonte, come nel test della cache: i tre vettori sono
 * prodotti in questa sessione da `git hash-object` e `shasum -a 256` su questa macchina, non dal
 * nostro SHA1/SHA256. Se qualcuno rompe `fmlx_hf_hash_fd`, qui si vede.
 */
#include "fmlx/fmlx_fetch.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int checks, failures;

#define CK(cond, ...)                                                                                            \
    do {                                                                                                         \
        checks++;                                                                                                \
        if (!(cond)) {                                                                                           \
            failures++;                                                                                          \
            printf("  FALLIMENTO %s:%d: ", __FILE__, __LINE__);                                                  \
            printf(__VA_ARGS__);                                                                                 \
            printf("\n");                                                                                        \
        }                                                                                                        \
    } while (0)

#define CK_ST(call, want)                                                                                        \
    do {                                                                                                         \
        FmlxHfStatus got_ = (call);                                                                              \
        checks++;                                                                                                \
        if (got_ != (want)) {                                                                                    \
            failures++;                                                                                          \
            printf("  FALLIMENTO %s:%d: %s -> %s [%s], atteso %s\n", __FILE__, __LINE__, #call,                  \
                   fmlx_hf_status_str(got_),                                                                     \
                   fmlx_hf_last_error() ? fmlx_hf_last_error() : "-", fmlx_hf_status_str(want));                 \
        }                                                                                                        \
    } while (0)

/* ---- vettori da fonte INDIPENDENTE (git hash-object / shasum, misurati in questa sessione) ---- */
static const char W1[] = "hello fmlx\n"; /* 11 byte */
#define W1_GIT "b501f7a6dc5fc7e5a81fac68ce526a179b8f7dad"
#define W1_SHA256 "ba2a82d87d18475c1177c7a3b9449da14334cba8541ee0921c7f99a00d99521f"
static const char W2[] = "abc"; /* 3 byte */
#define W2_SHA256 "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
static const char W3[] = "0123456789"; /* 10 byte */
#define W3_GIT "ad471007bd7f5983d273b9584e5629230150fd54"
#define W3_LEN 10
/* Il git blob id del file VUOTO: un nome onesto che i byte sopra NON soddisfano mai. */
#define EMPTY_GIT "e69de29bb2d1d6434b8b29ae775ad8c2e48c5391"

#define COMMIT_OLD "0123456789abcdef0123456789abcdef01234567"
#define COMMIT_NEW "1111111111111111111111111111111111111111"

static const char *EP = "https://huggingface.co";
static const char *REPO = "org/repo";
/* Il nome di directory sotto .locks/ è il NOME DELLA CARTELLA del repo, non "<org>--<nome>":
 * misurato su file_download.py:1203 (`lock_path = os.path.join(locks_dir, repo_folder_name(...), f"{etag}.lock")`) e sul cache reale di questa
 * macchina. Qui ce ne sono DUE, entrambe con `.locks/` (misurato: 24 voci / 10 lock e 12 / 7):
 * `~/ai/hf-cache/hub`, perché `~/.zshrc:52` pone `HF_HOME=/Users/gaetano/ai/hf-cache`, e
 * `~/.cache/huggingface/hub`, che è il default quando `HF_HOME` non arriva (shell non
 * interattiva). Prima versione di questo test: 6
 * fallimenti dovuti a questa supposizione. */
static const char *DN = "models--org--repo";

static char BASE[FMLX_HF_PATH_MAX]; /* <build>/work */
static char ROOT[FMLX_HF_PATH_MAX]; /* cache root del caso corrente */
static char RD[FMLX_HF_PATH_MAX];   /* ROOT/models--org--repo */

/* ---- mini-fs ---- */

static int mkdir_p(const char *path) {
    char buf[FMLX_HF_PATH_MAX];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *c = buf + 1; *c; c++) {
        if (*c != '/') continue;
        *c = '\0';
        if (mkdir(buf, 0777) != 0 && errno != EEXIST) return -1;
        *c = '/';
    }
    return mkdir(buf, 0777) == 0 || errno == EEXIST ? 0 : -1;
}

static int mkdir_parent(const char *path) {
    char dir[FMLX_HF_PATH_MAX];
    const char *slash = strrchr(path, '/');
    if (!slash) return 0;
    size_t n = (size_t)(slash - path);
    if (n == 0 || n >= sizeof dir) return -1;
    memcpy(dir, path, n);
    dir[n] = '\0';
    return mkdir_p(dir);
}

/* Un caso per volta, da cache VUOTO: ereditare i byte del caso prima è il modo più rapido di
 * far passare un test che non prova nulla. Si cancella solo sotto BASE, e lo si verifica PRIMA. */
static int rm_under_base(const char *path, const struct stat *st, int flags, struct FTW *ftw) {
    (void)st;
    (void)flags;
    (void)ftw;
    return remove(path);
}
static void fresh(const char *name) {
    snprintf(ROOT, sizeof ROOT, "%s/fetch-%s", BASE, name);
    snprintf(RD, sizeof RD, "%s/models--org--repo", ROOT);
    CK(strstr(ROOT, BASE) == ROOT && strlen(BASE) > 8, "refuso: %s non è sotto %s", ROOT, BASE);
    if (nftw(ROOT, rm_under_base, 16, FTW_DEPTH | FTW_PHYS) != 0 && errno != ENOENT)
        printf("  NOTA: pulizia %s: %s\n", ROOT, strerror(errno));
    if (mkdir_p(ROOT) != 0) printf("  FIXTURE: mkdir %s: %s\n", ROOT, strerror(errno));
}

static void write_at(const char *path, const char *content, size_t len) {
    if (mkdir_parent(path) != 0) {
        printf("  FIXTURE: mkdir per %s: %s\n", path, strerror(errno));
        failures++;
        return;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("  FIXTURE: fopen %s: %s\n", path, strerror(errno));
        failures++;
        return;
    }
    if (len) fwrite(content, 1, len, f);
    fclose(f);
}
static void write_str(const char *path, const char *content) {
    write_at(path, content, strlen(content));
}

static int exists(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0;
}
static int is_regular(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0 && S_ISREG(st.st_mode);
}
static int is_link(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
}
static int64_t size_of(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (int64_t)st.st_size : -1;
}
static int read_str(const char *path, char *out, size_t n) {
    FILE *f = fopen(path, "rb");
    if (!f) { out[0] = '\0'; return -1; }
    size_t got = fread(out, 1, n - 1, f);
    out[got] = '\0';
    fclose(f);
    return (int)got;
}
static int same_bytes(const char *path, const char *want, size_t len) {
    char buf[4096];
    if (len > sizeof buf) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t got = fread(buf, 1, sizeof buf, f);
    fclose(f);
    return got == len && memcmp(buf, want, len) == 0;
}
static void link_target(const char *path, char *out, size_t n) {
    out[0] = '\0';
    ssize_t k = readlink(path, out, n - 1);
    if (k > 0) out[k] = '\0';
}
static int make_link(const char *to, const char *from) {
    if (mkdir_parent(from) != 0) return -1;
    return symlink(to, from);
}

/* I path del cache, in un posto solo per genere. */
static const char *pblob(char *out, size_t n, const char *etag) {
    snprintf(out, n, "%s/blobs/%s", RD, etag);
    return out;
}
static const char *pentry(char *out, size_t n, const char *sha, const char *rel) {
    snprintf(out, n, "%s/snapshots/%s/%s", RD, sha, rel);
    return out;
}
static const char *pref(char *out, size_t n, const char *revision) {
    snprintf(out, n, "%s/refs/%s", RD, revision);
    return out;
}
static const char *plock(char *out, size_t n, const char *etag) {
    snprintf(out, n, "%s/.locks/%s/%s.lock", ROOT, DN, etag);
    return out;
}
static const char *ptemp(char *out, size_t n, const char *etag) {
    snprintf(out, n, "%s/blobs/%s.incomplete", RD, etag);
    return out;
}

/* ---- il transport finto ---- */

typedef struct {
    /* ciò che "il server" risponde */
    FmlxHfMeta reply;
    FmlxHfStatus meta_status;
    FmlxHfStatus get_status;
    const char *body;
    int64_t body_len;
    int ignore_range; /* 1 = risponde 200 a una richiesta Range: il transport DEVE azzerare */
    int serve_less;   /* 1 = un byte in meno del dichiarato */
    /* Lo STREAM FERMO: `stall_times` risposte che scrivono `stall_bytes` byte e poi si fermano.
     * E' l'unico fallimento che il livello sopra DEVE ritentare (piano §12.5), quindi è l'unico
     * che va simulato: `get_status = ERR_STALL` senza byte non proverebbe il resume. */
    int stall_times;
    int64_t stall_bytes;
    /* le OSSERVAZIONI, non le promesse */
    int meta_calls, get_calls;
    int64_t got_from, got_limit, got_written;
    int64_t from_seq[8];  /* i `from` in ORDINE di chiamata: è la prova che si riparte dai byte
                           * già pagati e non da zero */
    int n_seq;
    int configure_calls;
    int saw_stall_timeout, saw_stall_retries;
    char meta_url[FMLX_HF_PATH_MAX * 2], get_url[FMLX_HF_PATH_MAX * 2];
    char meta_token[128], get_token[128];
    int get_token_was_null;
} Fake;

/* Il fake "corrente". Ogni caso chiama fake_init IMMEDIATAMENTE prima di scaricare, quindi
 * `CUR` è sempre quello che il caso ha appena preparato: è per questo che la tabella dei
 * callbacks è unica invece che una per caso. `ctx` resta disponibile per chi vuole passare
 * il fake a mano. */
static Fake *CUR;

static void fake_init(Fake *f, const char *commit, const char *etag, int64_t size, const char *body) {
    memset(f, 0, sizeof *f);
    CUR = f;
    snprintf(f->reply.commit, sizeof f->reply.commit, "%s", commit);
    if (etag) snprintf(f->reply.etag, sizeof f->reply.etag, "%s", etag);
    f->reply.size = size;
    f->reply.status = 200;
    f->body = body;
    f->body_len = body ? (int64_t)strlen(body) : 0;
}

static FmlxHfStatus fake_meta(void *ctx, const char *url, const char *token, FmlxHfMeta *out) {
    Fake *f = ctx ? ctx : CUR;
    if (!f) return FMLX_HF_ERR_IO; /* nessun fake preparato: il caso è scritto male, non il cache */
    f->meta_calls++;
    snprintf(f->meta_url, sizeof f->meta_url, "%s", url);
    if (token) snprintf(f->meta_token, sizeof f->meta_token, "%s", token);
    if (f->meta_status != FMLX_HF_OK) return f->meta_status;
    *out = f->reply;
    return FMLX_HF_OK;
}

/* Onora il contratto di fmlx_fetch.h: append su fd, `limit` come MASSIMO di byte nuovi, e un 200
 * a richiesta Range che riparte da zero (ftruncate) invece di raddoppiare il corpo. */
static FmlxHfStatus fake_get(void *ctx, const char *url, const char *token, int64_t from,
                             int64_t limit, int fd, int64_t *written) {
    Fake *f = ctx ? ctx : CUR;
    if (!f) return FMLX_HF_ERR_IO;
    f->get_calls++;
    f->got_from = from;
    f->got_limit = limit;
    if (f->n_seq < (int)(sizeof f->from_seq / sizeof f->from_seq[0])) f->from_seq[f->n_seq++] = from;
    snprintf(f->get_url, sizeof f->get_url, "%s", url);
    f->get_token_was_null = token == NULL;
    if (token) snprintf(f->get_token, sizeof f->get_token, "%s", token);
    if (f->get_status != FMLX_HF_OK) return f->get_status;

    int64_t start = from;
    if (from > 0 && f->ignore_range) {
        if (ftruncate(fd, 0) != 0) return FMLX_HF_ERR_IO;
        if (lseek(fd, 0, SEEK_SET) < 0) return FMLX_HF_ERR_IO;
        start = 0;
    }
    int64_t n = f->body_len - start;
    int stall_now = 0;
    if (n < 0) n = 0;
    if (f->serve_less && n > 1) n -= 1;
    if (limit >= 0 && n > limit) n = limit;
    if (f->stall_times > 0) {
        f->stall_times--;
        stall_now = 1;
        if (n > f->stall_bytes) n = f->stall_bytes;
    }
    int64_t off = 0;
    while (off < n) {
        ssize_t k = write(fd, f->body + start + off, (size_t)(n - off));
        if (k < 0) {
            if (errno == EINTR) continue;
            return FMLX_HF_ERR_IO;
        }
        off += k;
    }
    *written = off;
    f->got_written = off;
    if (stall_now) return FMLX_HF_ERR_STALL;
    return FMLX_HF_OK;
}

/* Il configure del finto serve a due cose: provare che le opzioni ARRIVANO al transport (altrimenti
 * `stall_timeout_s` sarebbe un campo decorativo) e registrare i valori visti. */
static void fake_configure(void *ctx, const FmlxHfFetchOpts *opts) {
    Fake *f = ctx ? ctx : CUR;
    if (!f) return;
    f->configure_calls++;
    f->saw_stall_timeout = opts ? opts->stall_timeout_s : -1;
    f->saw_stall_retries = opts ? opts->stall_retries : -1;
}

static const FmlxHfTransport FAKE_VT = {NULL, fake_meta, fake_get, fake_configure};

static void opts_for(FmlxHfFetchOpts *o) {
    memset(o, 0, sizeof *o);
    o->cache_root = ROOT;
    o->endpoint = EP;
}

/* ---- i casi ---- */

/* Colpo di fortuna: l'entry c'è e verifica; la rete NON deve essere toccata. */
static void case_hit_no_network(void) {
    fresh("hit");
    Fake f;
    FmlxHfFetchOpts o;
    FmlxHfRef out;
    char b[FMLX_HF_PATH_MAX], e[FMLX_HF_PATH_MAX], r[FMLX_HF_PATH_MAX], tgt[FMLX_HF_PATH_MAX * 2];

    pblob(b, sizeof b, W1_GIT);
    write_at(b, W1, sizeof W1 - 1);
    pentry(e, sizeof e, COMMIT_OLD, "model.bin");
    snprintf(tgt, sizeof tgt, "../../blobs/%s", W1_GIT);
    if (make_link(tgt, e) != 0) { printf("  FIXTURE: symlink %s\n", e); failures++; return; }
    pref(r, sizeof r, "main");
    write_str(r, COMMIT_OLD);

    fake_init(&f, COMMIT_NEW, W1_GIT, (int64_t)sizeof W1 - 1, W1);
    opts_for(&o);
    CK_ST(fmlx_hf_download_with(REPO, "main", "model.bin", &o, &FAKE_VT, &out), FMLX_HF_OK);
    CK(f.meta_calls == 0 && f.get_calls == 0, "colpo di fortuna: meta=%d get=%d, attendevo 0 e 0",
       f.meta_calls, f.get_calls);
    CK(out.is_blob_link == 1, "l'entry dovrebbe essere il symlink");
    CK(strcmp(out.blob_name, W1_GIT) == 0, "blob_name='%s'", out.blob_name);
    CK_ST(fmlx_hf_verify(&out, NULL), FMLX_HF_OK);
}

/* 404: il marker .no_exist va scritto — nel commit del server — e refs/ si allinea. */
static void case_404_marker(void) {
    fresh("notfound");
    Fake f;
    FmlxHfFetchOpts o;
    char p[FMLX_HF_PATH_MAX], r[FMLX_HF_PATH_MAX], cur[128];

    fake_init(&f, COMMIT_NEW, "", -1, NULL);
    f.reply.not_found = 1;
    f.reply.status = 404;
    opts_for(&o);
    CK_ST(fmlx_hf_download_with(REPO, "main", "missing.bin", &o, &FAKE_VT, NULL),
          FMLX_HF_ERR_NOT_ON_REMOTE);
    snprintf(p, sizeof p, "%s/.no_exist/%s/missing.bin", RD, COMMIT_NEW);
    CK(is_regular(p), "manca il marker %s", p);
    pref(r, sizeof r, "main");
    CK(read_str(r, cur, sizeof cur) > 0 && strcmp(cur, COMMIT_NEW) == 0,
       "refs/main='%s', attendevo %s", cur, COMMIT_NEW);
    CK(f.get_calls == 0, "un 404 non si scarica");

    /* e la seconda volta NON si chiede di nuovo: il marker risponde in casa */
    Fake g;
    FmlxHfFetchOpts g2;
    fake_init(&g, COMMIT_NEW, "", -1, NULL);
    opts_for(&g2);
    CK_ST(fmlx_hf_download_with(REPO, "main", "missing.bin", &g2, &FAKE_VT, NULL),
          FMLX_HF_ERR_NOT_ON_REMOTE);
    CK(g.meta_calls == 0, "marker ignorato: %d chiamate di rete per un file che non c'è", g.meta_calls);
}

/* Download nuovo, etag a 40 hex (git blob id): il percorso più comune, provato per intero. */
static void case_download_gitblob(void) {
    fresh("new");
    Fake f;
    FmlxHfFetchOpts o;
    FmlxHfRef out;
    char b[FMLX_HF_PATH_MAX], e[FMLX_HF_PATH_MAX], lk[FMLX_HF_PATH_MAX * 2], tgt[FMLX_HF_PATH_MAX * 2];

    fake_init(&f, COMMIT_NEW, W1_GIT, (int64_t)sizeof W1 - 1, W1);
    opts_for(&o);
    CK_ST(fmlx_hf_download_with(REPO, "main", "model.bin", &o, &FAKE_VT, &out), FMLX_HF_OK);
    pblob(b, sizeof b, W1_GIT);
    CK(same_bytes(b, W1, sizeof W1 - 1), "il blob non ha i byte giusti");
    CK(f.get_calls == 1 && f.meta_calls == 1, "meta=%d get=%d, attendevo 1 e 1", f.meta_calls,
       f.get_calls);
    CK(f.got_from == 0 && f.got_limit == (int64_t)sizeof W1 - 1, "from=%lld limit=%lld",
       (long long)f.got_from, (long long)f.got_limit);
    ptemp(tgt, sizeof tgt, W1_GIT);
    CK(!exists(tgt), "il temporaneo %s non è stato consumato", tgt);
    pentry(e, sizeof e, COMMIT_NEW, "model.bin");
    CK(is_link(e), "%s dovrebbe essere un symlink", e);
    link_target(e, tgt, sizeof tgt);
    CK(strcmp(tgt, "../../blobs/" W1_GIT) == 0, "symlink -> '%s'", tgt);
    CK_ST(fmlx_hf_verify(&out, NULL), FMLX_HF_OK);

    /* Il lock si rilascia CHIUDENDO il fd, non cancellando il file: nel cache reale il file resta
     * (lasciarlo è ciò che huggingface_hub fa) e deve essere di nuovo prendibile. */
    plock(lk, sizeof lk, W1_GIT);
    CK(is_regular(lk), "il file di lock %s non è rimasto dov'è", lk);
    int lfd = open(lk, O_RDWR);
    CK(lfd >= 0, "riaprire il lock: %s", strerror(errno));
    if (lfd >= 0) {
        CK(flock(lfd, LOCK_EX | LOCK_NB) == 0, "il lock è ancora trattenuto: non l'abbiamo rilasciato");
        close(lfd);
    }
}

/* Etag a 64 hex (LFS) e path profondo: `a/b/c/x` richiede 2+3 = 5 "../". */
static void case_download_sha256_deep(void) {
    fresh("deep");
    Fake f;
    FmlxHfFetchOpts o;
    FmlxHfRef out;
    char b[FMLX_HF_PATH_MAX], e[FMLX_HF_PATH_MAX], tgt[FMLX_HF_PATH_MAX * 2];

    fake_init(&f, COMMIT_NEW, W2_SHA256, 3, W2);
    opts_for(&o);
    CK_ST(fmlx_hf_download_with(REPO, "main", "a/b/c/weights.bin", &o, &FAKE_VT, &out), FMLX_HF_OK);
    pblob(b, sizeof b, W2_SHA256);
    CK(same_bytes(b, W2, 3), "blob sha256 con i byte sbagliati");
    pentry(e, sizeof e, COMMIT_NEW, "a/b/c/weights.bin");
    link_target(e, tgt, sizeof tgt);
    CK(strcmp(tgt, "../../../../../blobs/" W2_SHA256) == 0, "profondità sbagliata: '%s'", tgt);
    CK_ST(fmlx_hf_verify(&out, NULL), FMLX_HF_OK);

    /* Il SECONDO download non deve toccare la rete: è la prova che refs/ è stato scritto. */
    Fake g;
    FmlxHfFetchOpts g2;
    fake_init(&g, COMMIT_NEW, W2_SHA256, 3, W2);
    opts_for(&g2);
    CK_ST(fmlx_hf_download_with(REPO, "main", "a/b/c/weights.bin", &g2, &FAKE_VT, NULL), FMLX_HF_OK);
    CK(g.meta_calls == 0, "il secondo download rifà la rete: refs/main non è stato scritto");
}

/* Hash che non torna: il nome NON deve mai comparire nel cache. */
static void case_hash_mismatch(void) {
    fresh("badhash");
    Fake f;
    FmlxHfFetchOpts o;
    char b[FMLX_HF_PATH_MAX], e[FMLX_HF_PATH_MAX], t[FMLX_HF_PATH_MAX * 2];

    fake_init(&f, COMMIT_NEW, EMPTY_GIT, (int64_t)sizeof W1 - 1, W1);
    opts_for(&o);
    CK_ST(fmlx_hf_download_with(REPO, "main", "model.bin", &o, &FAKE_VT, NULL),
          FMLX_HF_ERR_HASH_MISMATCH);
    pblob(b, sizeof b, EMPTY_GIT);
    CK(!exists(b), "il blob falso ha preso il nome giusto: %s", b);
    ptemp(t, sizeof t, EMPTY_GIT);
    CK(!exists(t), "il temporaneo falso è rimasto in giro: %s", t);
    pentry(e, sizeof e, COMMIT_NEW, "model.bin");
    CK(!exists(e), "lo snapshot punta a byte che non verificano");
}

/* Corpo più corto del dichiarato: ERR_SIZE, e il .incomplete RESTA (lavoro già pagato). */
static void case_truncated_keeps_resume(void) {
    fresh("trunc");
    Fake f;
    FmlxHfFetchOpts o;
    char b[FMLX_HF_PATH_MAX], t[FMLX_HF_PATH_MAX * 2];

    fake_init(&f, COMMIT_NEW, W3_GIT, W3_LEN, W3);
    f.serve_less = 1;
    opts_for(&o);
    CK_ST(fmlx_hf_download_with(REPO, "main", "model.bin", &o, &FAKE_VT, NULL), FMLX_HF_ERR_SIZE);
    pblob(b, sizeof b, W3_GIT);
    ptemp(t, sizeof t, W3_GIT);
    CK(!exists(b), "un corpo troncato non può diventare il blob");
    CK(size_of(t) == W3_LEN - 1, "il temporaneo ha %lld byte, attendevo %d", (long long)size_of(t),
       W3_LEN - 1);

    Fake g;
    FmlxHfFetchOpts g2;
    fake_init(&g, COMMIT_NEW, W3_GIT, W3_LEN, W3);
    opts_for(&g2);
    CK_ST(fmlx_hf_download_with(REPO, "main", "model.bin", &g2, &FAKE_VT, NULL), FMLX_HF_OK);
    CK(g.got_from == W3_LEN - 1, "ripresa da %lld, attendevo %d", (long long)g.got_from, W3_LEN - 1);
    /* Il limite è il FILE INTERO, non il frammento mancante: vedi il contratto in fmlx_fetch.h. */
    CK(g.got_limit == W3_LEN, "limit %lld dopo la ripresa, attendevo %d", (long long)g.got_limit,
       W3_LEN);
    CK(same_bytes(b, W3, W3_LEN), "la ripresa non ha ricomposto i byte");
    CK(!exists(t), "il temporaneo è sopravvissuto al rename");
}

/* Temporaneo già completo: la GET non si fa proprio (il server risponderebbe 416). */
static void case_temp_complete_skips_get(void) {
    fresh("full");
    Fake f;
    FmlxHfFetchOpts o;
    char b[FMLX_HF_PATH_MAX], t[FMLX_HF_PATH_MAX * 2];

    ptemp(t, sizeof t, W1_GIT);
    write_at(t, W1, sizeof W1 - 1);
    fake_init(&f, COMMIT_NEW, W1_GIT, (int64_t)sizeof W1 - 1, W1);
    opts_for(&o);
    CK_ST(fmlx_hf_download_with(REPO, "main", "model.bin", &o, &FAKE_VT, NULL), FMLX_HF_OK);
    CK(f.get_calls == 0, "%d GET su un temporaneo già completo", f.get_calls);
    pblob(b, sizeof b, W1_GIT);
    CK(same_bytes(b, W1, sizeof W1 - 1), "rename non avvenuto");
    CK(!exists(t), "il temporaneo completo doveva diventare il blob");
}

/* Temporaneo PIÙ lungo del dichiarato: non è il nostro frammento, si riparte da zero. */
static void case_temp_too_long_restarts(void) {
    fresh("toolong");
    Fake f;
    FmlxHfFetchOpts o;
    char b[FMLX_HF_PATH_MAX], t[FMLX_HF_PATH_MAX * 2], junk[64];

    memset(junk, 'Z', sizeof junk);
    ptemp(t, sizeof t, W1_GIT);
    write_at(t, junk, 40);
    fake_init(&f, COMMIT_NEW, W1_GIT, (int64_t)sizeof W1 - 1, W1);
    opts_for(&o);
    CK_ST(fmlx_hf_download_with(REPO, "main", "model.bin", &o, &FAKE_VT, NULL), FMLX_HF_OK);
    CK(f.got_from == 0, "si riparte da %lld su un temporaneo troppo lungo, attendevo 0",
       (long long)f.got_from);
    pblob(b, sizeof b, W1_GIT);
    CK(same_bytes(b, W1, sizeof W1 - 1), "i 40 byte di spazzatura sono ancora lì");
}

/* Un server che risponde 200 a una richiesta Range: il transport azzera, il risultato NON raddoppia. */
static void case_range_ignored(void) {
    fresh("range");
    Fake f;
    FmlxHfFetchOpts o;
    char b[FMLX_HF_PATH_MAX], t[FMLX_HF_PATH_MAX * 2];

    ptemp(t, sizeof t, W3_GIT);
    write_at(t, W3, 4); /* quattro byte già pagati */
    fake_init(&f, COMMIT_NEW, W3_GIT, W3_LEN, W3);
    f.ignore_range = 1;
    opts_for(&o);
    CK_ST(fmlx_hf_download_with(REPO, "main", "model.bin", &o, &FAKE_VT, NULL), FMLX_HF_OK);
    CK(f.got_from == 4 && f.got_written == W3_LEN, "from=%lld scritti=%lld", (long long)f.got_from,
       (long long)f.got_written);
    pblob(b, sizeof b, W3_GIT);
    CK(size_of(b) == W3_LEN, "il blob ha %lld byte: il corpo si è raddoppiato", (long long)size_of(b));
    CK(same_bytes(b, W3, W3_LEN), "contenuto sbagliato dopo un 200 a richiesta Range");
}

/* Stream fermo: il watchdog scatta e la GET successiva RIPARTE dai byte già scritti. È il caso
 * per cui il §12.5 esiste: su un modello da 30 GB al 90% la scelta fra "ricomincia" e "riprendi"
 * è un'ora di riscaricamento contro pochi minuti. */
static void case_stall_resumes(void) {
    fresh("stall");
    Fake f;
    FmlxHfFetchOpts o;
    FmlxHfRef out;
    char b[FMLX_HF_PATH_MAX], t[FMLX_HF_PATH_MAX * 2];

    fake_init(&f, COMMIT_NEW, W3_GIT, W3_LEN, W3);
    f.stall_times = 2;  /* due troncamenti, poi la terza risposta arriva in fondo */
    f.stall_bytes = 3;  /* 3 byte, poi altri 3: quelli "pagati" sono 6 su 10 */
    opts_for(&o);
    /* Il valore NON è una soglia misurata dal fake (sarebbe inutile: il fake non ha un clock da
     * fermare). Serve a provare che l'opzione ARRIVI al transport: se `configure` non fosse
     * chiamato, `stall_timeout_s` sarebbe un campo decorativo del struct. */
    o.stall_timeout_s = 1;
    o.stall_retries = 2;
    CK_ST(fmlx_hf_download_with(REPO, "main", "model.bin", &o, &FAKE_VT, &out), FMLX_HF_OK);
    CK(f.configure_calls == 1, "configure chiamato %d volte, attendevo 1", f.configure_calls);
    CK(f.saw_stall_timeout == 1 && f.saw_stall_retries == 2,
       "opzioni non arrivate al transport: timeout=%d retries=%d", f.saw_stall_timeout,
       f.saw_stall_retries);
    CK(f.get_calls == 3, "GET %d volte, attendevo 3 (due stall + una risposta completa)",
       f.get_calls);
    /* La sequenza dei `from` È la prova: 0, 3, 6. Un "resume" che ripartisse da zero sfuggirebbe
     * al controllo sull'hash — il corpo finale sarebbe comunque giusto — quindi qui si guardano le
     * CHIAMATE, non il blob. */
    CK(f.n_seq == 3 && f.from_seq[0] == 0 && f.from_seq[1] == 3 && f.from_seq[2] == 6,
       "sequenza from = %lld,%lld,%lld su %d chiamate, attendevo 0,3,6 su 3",
       (long long)f.from_seq[0], (long long)f.from_seq[1], (long long)f.from_seq[2], f.n_seq);
    pblob(b, sizeof b, W3_GIT);
    CK(same_bytes(b, W3, W3_LEN), "la ricomposizione dopo gli stall ha un altro contenuto");
    ptemp(t, sizeof t, W3_GIT);
    CK(!exists(t), "il temporaneo è sopravvissuto al rename");
    CK_ST(fmlx_hf_verify(&out, NULL), FMLX_HF_OK);
}

/* Ritentativi esauriti: ERR_STALL risale com'è e i byte arrivati RESTANO nel `.incomplete`. Il
 * punto non è il codice d'errore, è che il lavoro non si perde: la prossima invocazione riparte
 * di lì, che è il comportamento del `pull` con `--stall-retries`. */
static void case_stall_exhausted(void) {
    fresh("stallmax");
    Fake f;
    FmlxHfFetchOpts o;
    char b[FMLX_HF_PATH_MAX], t[FMLX_HF_PATH_MAX * 2];

    fake_init(&f, COMMIT_NEW, W3_GIT, W3_LEN, W3);
    f.stall_times = 5; /* più troncamenti dei ritentativi concessi */
    f.stall_bytes = 3;
    opts_for(&o);
    o.stall_timeout_s = 1;
    o.stall_retries = 1;
    CK_ST(fmlx_hf_download_with(REPO, "main", "model.bin", &o, &FAKE_VT, NULL),
           FMLX_HF_ERR_STALL);
    CK(f.get_calls == 2, "GET %d volte con retries=1, attendevo 2 (una sola ripartenza)",
       f.get_calls);
    pblob(b, sizeof b, W3_GIT);
    CK(!exists(b), "uno stream interrotto ha prodotto il blob definitivo");
    ptemp(t, sizeof t, W3_GIT);
    CK(size_of(t) == 6, "il temporaneo ha %lld byte, attendevo i 6 già pagati",
       (long long)size_of(t));
}

/* Un errore DIVERSO dallo stall non si ritenta MAI: un 404, un lock in mano ad altri o un HTTP
 * ripetuti un secondo dopo ridicono la stessa cosa, e il ritentativo sarebbe solo rumore — o un
 * pestaggio del server. `stall_retries` alto qui serve a provare che il filtro è sul CODICE. */
static void case_nostall_not_retried(void) {
    fresh("norestall");
    Fake f;
    FmlxHfFetchOpts o;

    fake_init(&f, COMMIT_NEW, W3_GIT, W3_LEN, W3);
    f.get_status = FMLX_HF_ERR_HTTP;
    opts_for(&o);
    o.stall_timeout_s = 1;
    o.stall_retries = 3;
    CK_ST(fmlx_hf_download_with(REPO, "main", "model.bin", &o, &FAKE_VT, NULL),
           FMLX_HF_ERR_HTTP);
    CK(f.get_calls == 1, "GET %d volte su un errore non-stall, attendevo 1", f.get_calls);
}

/* Un altro processo tiene il lock: ERR_LOCKED, e nessun file nostro. */
static void case_lock_held(void) {
    fresh("locked");
    Fake f;
    FmlxHfFetchOpts o;
    char lk[FMLX_HF_PATH_MAX * 2], b[FMLX_HF_PATH_MAX], t[FMLX_HF_PATH_MAX * 2];
    struct timespec t0, t1;

    plock(lk, sizeof lk, W1_GIT);
    if (mkdir_parent(lk) != 0) { printf("  FIXTURE: mkdir %s\n", lk); failures++; return; }
    int fd = open(lk, O_CREAT | O_RDWR, 0666);
    CK(fd >= 0, "open lock: %s", strerror(errno));
    if (fd < 0) return;
    CK(flock(fd, LOCK_EX | LOCK_NB) == 0, "il test non riesce trattenere il lock: niente da provare");

    fake_init(&f, COMMIT_NEW, W1_GIT, (int64_t)sizeof W1 - 1, W1);
    opts_for(&o);
    o.wait_lock_ms = 0;
    CK_ST(fmlx_hf_download_with(REPO, "main", "model.bin", &o, &FAKE_VT, NULL), FMLX_HF_ERR_LOCKED);
    CK(f.get_calls == 0, "con il lock altrui ho scaricato comunque");
    pblob(b, sizeof b, W1_GIT);
    ptemp(t, sizeof t, W1_GIT);
    CK(!exists(t), "creato un temporaneo senza tenere il lock");
    CK(!exists(b), "creato il blob senza tenere il lock");

    /* Con l'attesa concessa il fallimento arriva DOPO l'attesa: è la differenza tra "bloccato
     * adesso" e "non ho voluto aspettare", e deve essere osservabile. */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    o.wait_lock_ms = 150;
    CK_ST(fmlx_hf_download_with(REPO, "main", "model.bin", &o, &FAKE_VT, NULL), FMLX_HF_ERR_LOCKED);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = ((double)(t1.tv_sec - t0.tv_sec) * 1000.0) + ((double)(t1.tv_nsec - t0.tv_nsec) / 1e6);
    CK(ms >= 140.0, "attesa di %5.1f ms con wait_lock_ms=150: non ho aspettato", ms);

    close(fd); /* rilasciato: ora deve passare */
    Fake g;
    FmlxHfFetchOpts g2;
    fake_init(&g, COMMIT_NEW, W1_GIT, (int64_t)sizeof W1 - 1, W1);
    opts_for(&g2);
    CK_ST(fmlx_hf_download_with(REPO, "main", "model.bin", &g2, &FAKE_VT, NULL), FMLX_HF_OK);
}

/* Entry scritta da un ALTRO tool come file regolare: non si tocca, e la rete non serve. */
static void case_regular_entry_untouched(void) {
    fresh("regular");
    Fake f;
    FmlxHfFetchOpts o;
    char e[FMLX_HF_PATH_MAX], r[FMLX_HF_PATH_MAX];

    pentry(e, sizeof e, COMMIT_OLD, "config.json");
    write_str(e, "scritto-da-un-altro-tool");
    pref(r, sizeof r, "main");
    write_str(r, COMMIT_OLD);

    fake_init(&f, COMMIT_OLD, W1_GIT, (int64_t)sizeof W1 - 1, W1);
    opts_for(&o);
    CK_ST(fmlx_hf_download_with(REPO, "main", "config.json", &o, &FAKE_VT, NULL), FMLX_HF_OK);
    CK(f.meta_calls == 0 && f.get_calls == 0, "la rete non andava toccata");
    CK(is_regular(e), "%s non è più un file regolare: l'ho sostituito", e);
    CK(same_bytes(e, "scritto-da-un-altro-tool", 24), "i byte altrui sono cambiati");
}

/* Blob cattivo già in casa: senza repair si DICE, con repair si sostituisce. */
static void case_repair(void) {
    char b[FMLX_HF_PATH_MAX], e[FMLX_HF_PATH_MAX], r[FMLX_HF_PATH_MAX], tgt[FMLX_HF_PATH_MAX * 2];
    const char *bad = "BYTE-SBALLIATI";

    fresh("norepair");
    pblob(b, sizeof b, W1_GIT);
    write_str(b, bad);
    pentry(e, sizeof e, COMMIT_OLD, "model.bin");
    snprintf(tgt, sizeof tgt, "../../blobs/%s", W1_GIT);
    if (make_link(tgt, e) != 0) { printf("  FIXTURE: symlink\n"); failures++; return; }
    pref(r, sizeof r, "main");
    write_str(r, COMMIT_OLD);

    Fake f;
    FmlxHfFetchOpts o;
    fake_init(&f, COMMIT_OLD, W1_GIT, (int64_t)sizeof W1 - 1, W1);
    opts_for(&o);
    CK_ST(fmlx_hf_download_with(REPO, "main", "model.bin", &o, &FAKE_VT, NULL),
          FMLX_HF_ERR_HASH_MISMATCH);
    CK(f.meta_calls == 0 && f.get_calls == 0, "senza repair non si deve andare in rete");
    CK(same_bytes(b, bad, strlen(bad)), "repair=0 ha già sostituito il blob");

    fresh("repair");
    pblob(b, sizeof b, W1_GIT);
    write_str(b, bad);
    pentry(e, sizeof e, COMMIT_OLD, "model.bin");
    if (make_link(tgt, e) != 0) { printf("  FIXTURE: symlink\n"); failures++; return; }
    pref(r, sizeof r, "main");
    write_str(r, COMMIT_OLD);
    ptemp(tgt, sizeof tgt, W1_GIT);
    write_str(tgt, "frammento-da-but"); /* force_download non riparte DA questo */

    Fake g;
    FmlxHfFetchOpts g2;
    fake_init(&g, COMMIT_OLD, W1_GIT, (int64_t)sizeof W1 - 1, W1);
    opts_for(&g2);
    g2.repair = 1;
    CK_ST(fmlx_hf_download_with(REPO, "main", "model.bin", &g2, &FAKE_VT, NULL), FMLX_HF_OK);
    CK(g.got_from == 0, "repair ha ripreso dal framamento rotto: from=%lld", (long long)g.got_from);
    CK(same_bytes(b, W1, sizeof W1 - 1), "repair non ha riparato");
    CK(is_link(e), "lo snapshot non è tornato un symlink");
}

/* Rifiuti che arrivano PRIMA di toccare il disco: niente lock, niente blobs/. */
static void case_refusals_before_disk(void) {
    struct {
        const char *name, *etag;
        int64_t size, max_bytes;
        FmlxHfStatus want;
    } V[] = {
        {"noetag", "", 11, 0, FMLX_HF_ERR_NO_ETAG},
        {"nosize", W1_GIT, -1, 0, FMLX_HF_ERR_NO_SIZE},
        {"maxbytes", W1_GIT, 11, 8, FMLX_HF_ERR_SIZE},
        {"pathtrail", "../via", 11, 0, FMLX_HF_ERR_BAD_ETAG},
        {"nested", "a/b", 11, 0, FMLX_HF_ERR_BAD_ETAG},
        {"dotname", ".", 11, 0, FMLX_HF_ERR_BAD_ETAG},
        {"hidden", ".git", 11, 0, FMLX_HF_ERR_BAD_ETAG},
    };
    for (size_t i = 0; i < sizeof V / sizeof V[0]; i++) {
        fresh(V[i].name);
        Fake f;
        FmlxHfFetchOpts o;
        char locks[FMLX_HF_PATH_MAX * 2], blobs[FMLX_HF_PATH_MAX];

        fake_init(&f, COMMIT_NEW, V[i].etag, V[i].size, W1);
        opts_for(&o);
        o.max_bytes = V[i].max_bytes;
        CK_ST(fmlx_hf_download_with(REPO, "main", "model.bin", &o, &FAKE_VT, NULL), V[i].want);
        snprintf(locks, sizeof locks, "%s/.locks", ROOT);
        snprintf(blobs, sizeof blobs, "%s/blobs", RD);
        CK(!exists(locks), "%s: creati i lock per una richiesta rifiutata", V[i].name);
        CK(!exists(blobs), "%s: creato blobs/ per una richiesta rifiutata", V[i].name);
        CK(f.get_calls == 0, "%s: rifiutato DOPO aver scaricato", V[i].name);
    }
    /* Un path malformato non arriva nemmeno a sfiorare la rete. */
    fresh("badname");
    Fake f;
    FmlxHfFetchOpts o;
    fake_init(&f, COMMIT_NEW, W1_GIT, (int64_t)sizeof W1 - 1, W1);
    opts_for(&o);
    CK_ST(fmlx_hf_download_with(REPO, "main", "../escape.bin", &o, &FAKE_VT, NULL), FMLX_HF_ERR_NAME);
    CK(f.meta_calls == 0, "chiesto al server per un path vietato");
    CK_ST(fmlx_hf_download_with(REPO, "main", "ok.bin", &o, NULL, NULL), FMLX_HF_ERR_IO);
}

/* Il token: mai in un messaggio, mai su un host diverso, dall'ambiente se non dato. */
static void case_token_hygiene(void) {
    char e[FMLX_HF_PATH_MAX];

    /* (a) host DIVERSO (il CDN): il token non si manda — l'URL firmato È l'autorizzazione. */
    fresh("token-cdn");
    Fake f;
    FmlxHfFetchOpts o;
    fake_init(&f, COMMIT_NEW, W1_GIT, (int64_t)sizeof W1 - 1, W1);
    snprintf(f.reply.location, sizeof f.reply.location,
             "https://cdn-lfs.huggingface.co/org/repo/x?sig=abc");
    opts_for(&o);
    o.token = "hf_FINTO-TOKEN-DEL-TEST";
    CK_ST(fmlx_hf_download_with(REPO, "main", "model.bin", &o, &FAKE_VT, NULL), FMLX_HF_OK);
    CK(f.get_token_was_null, "token spedito a un host terzo: '%s'", f.get_token);
    CK(strcmp(f.get_url, "https://cdn-lfs.huggingface.co/org/repo/x?sig=abc") == 0,
       "GET su '%s', atteso il Location del CDN", f.get_url);
    CK(strcmp(f.meta_token, "hf_FINTO-TOKEN-DEL-TEST") == 0, "token non arrivato al primo host");
    CK(strstr(fmlx_hf_last_error(), "hf_FINTO") == NULL, "il token è finito in un messaggio");

    /* (b) STESSO host, maiuscole diverse (l'host non le distingue): il token va. */
    fresh("token-same");
    Fake g;
    FmlxHfFetchOpts g2;
    fake_init(&g, COMMIT_NEW, W1_GIT, (int64_t)sizeof W1 - 1, W1);
    snprintf(g.reply.location, sizeof g.reply.location, "https://HUGGINGFACE.CO/org/repo/x");
    opts_for(&g2);
    g2.token = "hf_FINTO-TOKEN-DEL-TEST";
    CK_ST(fmlx_hf_download_with(REPO, "main", "model.bin", &g2, &FAKE_VT, NULL), FMLX_HF_OK);
    CK(strcmp(g.get_token, "hf_FINTO-TOKEN-DEL-TEST") == 0,
       "token trattenuto sullo stesso host (was_null=%d)", g.get_token_was_null);

    /* (c) nessun Location: GET sull'URL del resolve. */
    fresh("token-url");
    Fake h;
    FmlxHfFetchOpts h2;
    fake_init(&h, COMMIT_NEW, W1_GIT, (int64_t)sizeof W1 - 1, W1);
    opts_for(&h2);
    CK_ST(fmlx_hf_download_with(REPO, "main", "model.bin", &h2, &FAKE_VT, NULL), FMLX_HF_OK);
    CK(strcmp(h.meta_url, "https://huggingface.co/org/repo/resolve/main/model.bin") == 0,
       "URL = '%s'", h.meta_url);
    CK(strcmp(h.get_url, h.meta_url) == 0, "GET su '%s'", h.get_url);

    /* (d) nessun token e ambiente vuoto: nessuna intestazione, non una stringa vuota. */
    fresh("token-none");
    unsetenv("HF_TOKEN");
    Fake i;
    FmlxHfFetchOpts i2;
    fake_init(&i, COMMIT_NEW, W1_GIT, (int64_t)sizeof W1 - 1, W1);
    opts_for(&i2);
    CK_ST(fmlx_hf_download_with(REPO, "main", "model.bin", &i2, &FAKE_VT, NULL), FMLX_HF_OK);
    CK(i.meta_token[0] == '\0', "token inesistente spedito come '%s'", i.meta_token);

    /* (e) il token dall'ambiente, quando opts non ne dà uno. */
    fresh("token-env");
    setenv("HF_TOKEN", "hf_DALLAMBIENTE", 1);
    Fake k;
    FmlxHfFetchOpts k2;
    fake_init(&k, COMMIT_NEW, W1_GIT, (int64_t)sizeof W1 - 1, W1);
    opts_for(&k2);
    CK_ST(fmlx_hf_download_with(REPO, "main", "model.bin", &k2, &FAKE_VT, NULL), FMLX_HF_OK);
    CK(strcmp(k.meta_token, "hf_DALLAMBIENTE") == 0, "ambiente ignorato: '%s'", k.meta_token);
    unsetenv("HF_TOKEN");
    (void)e;
}

/* Commit del server contro refs/ stantio: vincono i byte di OGGI, e il ref si allinea. */
static void case_server_commit_wins(void) {
    fresh("stale");
    Fake f;
    FmlxHfFetchOpts o;
    char r[FMLX_HF_PATH_MAX], cur[128], old_dir[FMLX_HF_PATH_MAX * 2], ne[FMLX_HF_PATH_MAX * 2];

    pref(r, sizeof r, "main");
    write_str(r, COMMIT_OLD);
    fake_init(&f, COMMIT_NEW, W1_GIT, (int64_t)sizeof W1 - 1, W1);
    opts_for(&o);
    CK_ST(fmlx_hf_download_with(REPO, "main", "model.bin", &o, &FAKE_VT, NULL), FMLX_HF_OK);
    pentry(ne, sizeof ne, COMMIT_NEW, "model.bin");
    snprintf(old_dir, sizeof old_dir, "%s/snapshots/%s", RD, COMMIT_OLD);
    CK(exists(ne), "i byte non sono nello snapshot del commit del server");
    CK(!exists(old_dir), "creato lo snapshot del commit stantio");
    CK(read_str(r, cur, sizeof cur) > 0 && strcmp(cur, COMMIT_NEW) == 0, "refs/main='%s' dopo", cur);
}

/* Il commit del server NON è un commit (spazzatura) e non c'è un ref: si dice, non si indovina. */
static void case_commit_untrusted(void) {
    fresh("badcommit");
    Fake f;
    FmlxHfFetchOpts o;
    char blobs[FMLX_HF_PATH_MAX];

    fake_init(&f, "non-sono-un-commit", W1_GIT, (int64_t)sizeof W1 - 1, W1);
    opts_for(&o);
    CK_ST(fmlx_hf_download_with(REPO, "main", "model.bin", &o, &FAKE_VT, NULL), FMLX_HF_ERR_HTTP);
    snprintf(blobs, sizeof blobs, "%s/blobs", RD);
    CK(!exists(blobs), "scaricato comunque, senza sapere in quale snapshot");

    /* ... ma se il ref è in casa, quel ref BASTA: si scarica lì. */
    fresh("badcommit-ref");
    char r[FMLX_HF_PATH_MAX];
    pref(r, sizeof r, "main");
    write_str(r, COMMIT_OLD);
    Fake g;
    FmlxHfFetchOpts g2;
    fake_init(&g, "non-sono-un-commit", W1_GIT, (int64_t)sizeof W1 - 1, W1);
    opts_for(&g2);
    CK_ST(fmlx_hf_download_with(REPO, "main", "model.bin", &g2, &FAKE_VT, NULL), FMLX_HF_OK);
    pentry(r, sizeof r, COMMIT_OLD, "model.bin");
    CK(is_link(r), "i byte non sono nello snapshot del ref locale");
}

/* Una revisione che È già un commit: refs/ non si tocca (non c'è un nome da legare). */
static void case_revision_is_commit(void) {
    fresh("revcommit");
    Fake f;
    FmlxHfFetchOpts o;
    char refs[FMLX_HF_PATH_MAX * 2], url[FMLX_HF_PATH_MAX * 2];

    fake_init(&f, COMMIT_NEW, W1_GIT, (int64_t)sizeof W1 - 1, W1);
    opts_for(&o);
    CK_ST(fmlx_hf_download_with(REPO, COMMIT_NEW, "model.bin", &o, &FAKE_VT, NULL), FMLX_HF_OK);
    snprintf(refs, sizeof refs, "%s/refs", RD);
    CK(!exists(refs), "refs/ creato per una revisione che era già un commit");
    snprintf(url, sizeof url, "https://huggingface.co/org/repo/resolve/%s/model.bin", COMMIT_NEW);
    CK(strcmp(f.meta_url, url) == 0, "URL '%s' != '%s'", f.meta_url, url);
}

/* Percent-encoding all'esatto modo di huggingface_hub: quote(filename) con safe="/". */
static void case_url_encoding(void) {
    fresh("encoding");
    Fake f;
    FmlxHfFetchOpts o;

    fake_init(&f, COMMIT_NEW, W1_GIT, (int64_t)sizeof W1 - 1, W1);
    opts_for(&o);
    CK_ST(fmlx_hf_download_with(REPO, "main", "a/è b.bin", &o, &FAKE_VT, NULL), FMLX_HF_OK);
    CK(strcmp(f.meta_url, "https://huggingface.co/org/repo/resolve/main/a/%C3%A8%20b.bin") == 0,
       "URL non codificato come huggingface_hub: '%s'", f.meta_url);
}

/* Offline: il rifiuto arriva prima di qualsiasi domanda alla rete. */
static void case_offline(void) {
    fresh("offline");
    Fake f;
    FmlxHfFetchOpts o;

    fake_init(&f, COMMIT_NEW, W1_GIT, (int64_t)sizeof W1 - 1, W1);
    opts_for(&o);
    o.offline = 1;
    CK_ST(fmlx_hf_download_with(REPO, "main", "model.bin", &o, &FAKE_VT, NULL), FMLX_HF_ERR_OFFLINE);
    CK(f.meta_calls == 0 && f.get_calls == 0, "offline ha comunque chiesto al server");

    fresh("offline-env");
    setenv("HF_HUB_OFFLINE", "1", 1);
    Fake g;
    FmlxHfFetchOpts g2;
    fake_init(&g, COMMIT_NEW, W1_GIT, (int64_t)sizeof W1 - 1, W1);
    opts_for(&g2);
    CK_ST(fmlx_hf_download_with(REPO, "main", "model.bin", &g2, &FAKE_VT, NULL), FMLX_HF_ERR_OFFLINE);
    /* con l'entry già in casa, offline è il caso NORMALE: deve passare */
    unsetenv("HF_HUB_OFFLINE");
    unsetenv("TRANSFORMERS_OFFLINE");
}

/* normalize_etag: è la regola che decide il NOME del blob, provata da sola. */
static void case_normalize_etag(void) {
    char out[128];

    fmlx_hf_normalize_etag("W/\"b501f7\"", out, sizeof out);
    CK(strcmp(out, "b501f7") == 0, "'%s' da W/\"b501f7\"", out);
    fmlx_hf_normalize_etag("\"x\"", out, sizeof out);
    CK(strcmp(out, "x") == 0, "'%s'", out);
    fmlx_hf_normalize_etag("\"\"y\"\"", out, sizeof out);
    CK(strcmp(out, "y") == 0, "strip('\"') toglie TUTTE le quote: '%s'", out);
    fmlx_hf_normalize_etag("W/W/z", out, sizeof out);
    CK(strcmp(out, "z") == 0, "lstrip(\"W/\") non è un prefisso: '%s'", out);
    fmlx_hf_normalize_etag("", out, sizeof out);
    CK(out[0] == '\0', "vuoto deve restare vuoto");
    fmlx_hf_normalize_etag(NULL, out, sizeof out);
    CK(out[0] == '\0', "NULL deve dare vuoto, non un crash");
    fmlx_hf_normalize_etag("abc", out, 3);
    CK(strcmp(out, "ab") == 0 && out[2] == '\0', "trattato il buffer corto: '%s'", out);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("uso: %s <dir-di-lavoro>\n", argv[0]);
        return 2;
    }
    snprintf(BASE, sizeof BASE, "%s", argv[1]);
    /* Output non bufferizzato: se questo programma va in crash, l'ultimo printf che NON si vede è
     * la metà dell'informazione che serve per capire dove. */
    setvbuf(stdout, NULL, _IONBF, 0);
    if (mkdir_p(BASE) != 0) {
        printf("FIXTURE: mkdir %s: %s\n", BASE, strerror(errno));
        return 2;
    }
    /* L'ambiente altrui non deve decidere i risultati: si parte da pulito, e i token fintizi sono
     * inventati qui — mai l'HF_TOKEN di chi esegue i test. */
    unsetenv("HF_TOKEN");
    unsetenv("HF_HUB_OFFLINE");
    unsetenv("TRANSFORMERS_OFFLINE");
    unsetenv("HF_ENDPOINT");
    unsetenv("HF_HOME");
    unsetenv("HF_HUB_CACHE");

    case_normalize_etag();
    case_hit_no_network();
    case_404_marker();
    case_download_gitblob();
    case_download_sha256_deep();
    case_hash_mismatch();
    case_truncated_keeps_resume();
    case_temp_complete_skips_get();
    case_temp_too_long_restarts();
    case_range_ignored();
    case_stall_resumes();
    case_stall_exhausted();
    case_nostall_not_retried();
    case_lock_held();
    case_regular_entry_untouched();
    case_repair();
    case_refusals_before_disk();
    case_token_hygiene();
    case_server_commit_wins();
    case_commit_untrusted();
    case_revision_is_commit();
    case_url_encoding();
    case_offline();

    printf("test_hf_fetch: %d verifiche, %d fallimenti\n", checks, failures);
    return failures ? 1 : 0;
}
