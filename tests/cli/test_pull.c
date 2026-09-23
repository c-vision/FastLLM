/* test_pull.c — `fastmlx pull` senza rete: un manifesto finto, due corpi finti, un cache vero.
 *
 * Le cose che questo file deve provare NON sono "scarica": la lane download è già provata nel
 * test_hf_fetch. Qui si prova l'ORDINE del pull, cioè ciò che esiste solo in questa lane:
 *
 *  a) TUTTI i file finiscono allo STESSO commit (quello del manifesto), e il nome del blob arriva
 *     dall'etag e NON dal `blobId` del manifesto — è la trappola misurata sul server (§12.1): se
 *     `pull` deducesse il nome dal `blobId`, un file LFS si cercherebbe un nome che non esiste.
 *  b) il secondo `pull` non chiede NULLA alla rete: è il `refs/<ramo>` scritto alla fine che lo
 *     permette, e i contatori del transport finto sono la prova (una riga "già nel cache" senza
 *     contatori sarebbe una speranza, non un fatto);
 *  c) `--dest` è un HARDLINK degli stessi byte, mai un symlink, e le directory annidate esistono;
 *  d) un fallimento a metà FERMA il pull e NON lascia uno specchio a metà né un ref scritto.
 *
 * I valori esadecimali qui sotto non sono inventati: `git hash-object` e `shasum -a 256` sui due
 * corpi, misurati su questa macchina. Il test costruisce il cache con le regole della lane hf
 * (pjoin/mkdir), non con path scritti a mano: due modi diversi di costruire lo stesso path
 * maskerebbero un bug del primo.
 */
#include "fmlx/fmlx_cli.h"

#include <errno.h>
#include <ftw.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int checks, failures;

#define CK(cond, ...)                                                                                              \
    do {                                                                                                           \
        checks++;                                                                                                  \
        if (!(cond)) {                                                                                             \
            failures++;                                                                                            \
            printf("  FALLIMENTO %s:%d: ", __FILE__, __LINE__);                                                    \
            printf(__VA_ARGS__);                                                                                   \
            printf("\n");                                                                                          \
        }                                                                                                          \
    } while (0)

#define CK_ST(call, want)                                                                                          \
    do {                                                                                                           \
        FmlxHfStatus got_ = (call);                                                                                \
        checks++;                                                                                                  \
        if (got_ != (want)) {                                                                                      \
            failures++;                                                                                            \
            printf("  FALLIMENTO %s:%d: %s -> %s [%s], atteso %s\n", __FILE__, __LINE__, #call,                   \
                   fmlx_hf_status_str(got_), fmlx_hf_last_error() ? fmlx_hf_last_error() : "-",                    \
                   fmlx_hf_status_str(want));                                                                      \
        }                                                                                                          \
    } while (0)

/* ---- i due corpi, e le loro identità MISURATE ---- */
#define COMMIT "3333333333333333333333333333333333333333"
#define BODY_A "{\"a\":1}" /* 7 byte */
#define BLOB_A "daa5053ecf5f9a37b2de733d0751cc1ab53ac010"  /* git hash-object su BODY_A */
#define BODY_B "MLX0"                                     /* 4 byte */
#define GIT_B "1b30c6264f1d567ca68281a578e7d5883e27bbe9"  /* git hash-object su BODY_B: è il blobId */
#define SHA_B "e0a2b500fb63410c68b3cbd5c0a447634624511ae38bde5fb8152880bf978f36" /* shasum -a 256 */
#define REPO "org/repo"
#define LEAF "repo" /* l'ultimo segmento: il nome che --dest dà alla directory */

typedef struct {
    const char *rel;
    const char *etag; /* ciò che il server DICE: il nome con cui il blob viene scritto */
    const char *body;
    FmlxHfStatus status; /* != OK: la risposta per QUESTO file fallisce */
} Entry;

static Entry ENTRIES[] = {
    {"config.json", BLOB_A, BODY_A, FMLX_HF_OK},
    {"weights/model.safetensors", SHA_B, BODY_B, FMLX_HF_OK},
};
static size_t N_ENTRIES = sizeof ENTRIES / sizeof ENTRIES[0];

static char MANIFEST[FMLX_HF_PATH_MAX];
static FmlxHfStatus buf_status; /* != OK: la richiesta del manifesto fallisce COSI' */
static int n_buf, n_meta, n_get;
static char last_meta_url[FMLX_HF_PATH_MAX];

static void build_manifest(void) {
    snprintf(MANIFEST, sizeof MANIFEST,
             "{\"sha\":\"" COMMIT "\",\"siblings\":["
             "{\"rfilename\":\"config.json\",\"blobId\":\"" BLOB_A "\",\"size\":7},"
             "{\"rfilename\":\"weights/model.safetensors\",\"blobId\":\"" GIT_B
             "\",\"size\":4,\"lfs\":{\"size\":4}}]}");
}

static Entry *entry_for(const char *url) {
    size_t i;
    for (i = 0; i < N_ENTRIES; i++)
        if (strstr(url, ENTRIES[i].rel)) return &ENTRIES[i];
    return NULL;
}

static FmlxHfStatus fake_buf(void *c, const char *url, const char *tok, char *buf, size_t cap,
                             size_t *written) {
    size_t len = strlen(MANIFEST);
    (void)c;
    (void)tok;
    n_buf++;
    snprintf(last_meta_url, sizeof last_meta_url, "%s", url);
    *written = 0;
    if (buf_status != FMLX_HF_OK) return buf_status;
    if (len > cap) return FMLX_HF_ERR_BUF_SMALL;
    memcpy(buf, MANIFEST, len);
    *written = len;
    return FMLX_HF_OK;
}

static FmlxHfStatus fake_meta(void *c, const char *url, const char *tok, FmlxHfMeta *out) {
    Entry *e;
    (void)c;
    (void)tok;
    n_meta++;
    snprintf(last_meta_url, sizeof last_meta_url, "%s", url);
    e = entry_for(url);
    if (!e) return FMLX_HF_ERR_NOT_ON_REMOTE; /* il manifesto nomina un file che non serviamo */
    if (e->status != FMLX_HF_OK) return e->status;
    memset(out, 0, sizeof *out);
    snprintf(out->commit, sizeof out->commit, "%s", COMMIT);
    snprintf(out->etag, sizeof out->etag, "%s", e->etag);
    out->size = (int64_t)strlen(e->body);
    out->status = 200;
    return FMLX_HF_OK;
}

static FmlxHfStatus fake_get(void *c, const char *url, const char *tok, int64_t from, int64_t limit,
                             int fd, int64_t *written) {
    Entry *e;
    int64_t off;
    (void)c;
    (void)tok;
    n_get++;
    e = entry_for(url);
    if (!e) return FMLX_HF_ERR_NOT_ON_REMOTE;
    if (e->status != FMLX_HF_OK) return e->status;
    if (from > (int64_t)strlen(e->body)) return FMLX_HF_ERR_SIZE;
    off = from;
    if (limit >= 0 && (int64_t)strlen(e->body) > limit) return FMLX_HF_ERR_SIZE;
    while (off < (int64_t)strlen(e->body)) {
        ssize_t w = write(fd, e->body + off, strlen(e->body) - (size_t)off);
        if (w < 0) return FMLX_HF_ERR_IO;
        off += w;
    }
    *written = off - from;
    return FMLX_HF_OK;
}

static void fake_reset(void) { n_buf = n_meta = n_get = 0; buf_status = FMLX_HF_OK; }

/* Ordine dei campi: ctx, meta, get, configure, get_buf. */
static const FmlxHfTransport VT = {NULL, fake_meta, fake_get, NULL, fake_buf};

/* ---- il palco: un cache per caso, costruito e cancellato qui ---- */
static char BASE[FMLX_HF_PATH_MAX];
static char ROOT[FMLX_HF_PATH_MAX]; /* <BASE>/pull-<nome-caso>: il cache di questo caso */

static int rm_one(const char *path, const struct stat *st, int flag, struct FTW *ftw) {
    (void)st;
    (void)ftw;
    if (flag == FTW_DP) return rmdir(path);
    return unlink(path); /* FTW_PHYS: un symlink si toglie, non si segue */
}

static void fresh(const char *name) {
    snprintf(ROOT, sizeof ROOT, "%s/pull-%s", BASE, name);
    if (nftw(ROOT, rm_one, 16, FTW_DEPTH | FTW_PHYS) != 0 && errno != ENOENT)
        printf("  FIXTURE: rm %s: %s\n", ROOT, strerror(errno));
    if (fmlx_hf_mkdir_p(ROOT) != 0) printf("  FIXTURE: mkdir %s: %s\n", ROOT, strerror(errno));
    fake_reset();
}

static void opts_for(FmlxPullOpts *o) {
    memset(o, 0, sizeof *o);
    o->fetch.cache_root = ROOT;
    o->fetch.endpoint = "https://huggingface.co";
}

static int is_file(const char *path, int64_t *size) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    if (!S_ISREG(st.st_mode)) return 0;
    if (size) *size = (int64_t)st.st_size;
    return 1;
}

static int is_symlink(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
}

static int content_is(const char *path, const char *want) {
    char buf[128];
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f) return 0;
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = '\0';
    fclose(f);
    return strcmp(buf, want) == 0;
}

static void blob_path(char *out, size_t n, const char *blob) {
    char dir[FMLX_HF_PATH_MAX], sub[FMLX_HF_PATH_MAX];
    fmlx_hf_pjoin(dir, sizeof dir, ROOT, "models--org--repo");
    fmlx_hf_pjoin(sub, sizeof sub, dir, "blobs");
    fmlx_hf_pjoin(out, n, sub, blob);
}

/* ---- i casi ---- */

/* a) + b) il caso per cui il comando esiste. */
static void case_both_at_one_commit(void) {
    FmlxPullOpts o;
    FmlxPullReport rep;
    char p[FMLX_HF_PATH_MAX], dir[FMLX_HF_PATH_MAX], sub[FMLX_HF_PATH_MAX], snap[FMLX_HF_PATH_MAX];

    fresh("onecommit");
    opts_for(&o);
    memset(&rep, 0, sizeof rep);
    CK_ST(fmlx_pull_with(REPO, "main", &o, &VT, &rep, NULL), FMLX_HF_OK);
    CK(rep.n_files == 2, "n_files=%zu", rep.n_files);
    CK(rep.n_ok == 2, "n_ok=%zu", rep.n_ok);
    CK(rep.bytes_declared == 11, "bytes_declared=%" PRId64, rep.bytes_declared);
    CK(strcmp(rep.commit, COMMIT) == 0, "commit=%s", rep.commit);
    CK(n_buf == 1, "richieste di manifesto=%d (una sola, per tutto il repo)", n_buf);
    CK(n_meta == 2 && n_get == 2, "meta=%d get=%d", n_meta, n_get);

    /* I DUE blob, con i nomi detti dall'etag. Il secondo è la trappola: il `blobId` del manifesto
     * (GIT_B) NON deve esistere come nome di blob. */
    blob_path(p, sizeof p, BLOB_A);
    CK(is_file(p, NULL) && content_is(p, BODY_A), "blob non-LFS %s", p);
    blob_path(p, sizeof p, SHA_B);
    CK(is_file(p, NULL) && content_is(p, BODY_B), "blob LFS %s", p);
    blob_path(p, sizeof p, GIT_B);
    CK(!is_file(p, NULL), "il blobId del manifesto NON è un nome di blob: esiste %s", p);

    /* Gli symlink dello snapshot, inclusa la FORMA annidata (tre barre, non due). */
    fmlx_hf_pjoin(dir, sizeof dir, ROOT, "models--org--repo");
    fmlx_hf_pjoin(sub, sizeof sub, dir, "snapshots");
    fmlx_hf_pjoin(snap, sizeof snap, sub, COMMIT);
    fmlx_hf_pjoin(p, sizeof p, snap, "config.json");
    CK(is_symlink(p), "symlink %s", p);
    fmlx_hf_pjoin(p, sizeof p, snap, "weights/model.safetensors");
    CK(is_symlink(p), "symlink annidato %s", p);
    CK(strcmp(rep.snapshot_dir, snap) == 0, "snapshot_dir=%s atteso %s", rep.snapshot_dir, snap);

    /* Il ramo: senza questo, il secondo pull non avrebbe modo di sapere il commit in casa. */
    fmlx_hf_pjoin(p, sizeof p, dir, "refs");
    fmlx_hf_pjoin(p, sizeof p, p, "main");
    CK(content_is(p, COMMIT), "refs/main non dice il commit");
}

/* b) la ripartenza: stesso comando, contatori Fermi. */
static void case_second_pull_says_nothing(void) {
    FmlxPullOpts o;
    FmlxPullReport rep;
    int meta_at, get_at;

    fresh("second");
    opts_for(&o);
    CK_ST(fmlx_pull_with(REPO, "main", &o, &VT, &rep, NULL), FMLX_HF_OK);
    meta_at = n_meta;
    get_at = n_get;
    CK_ST(fmlx_pull_with(REPO, "main", &o, &VT, &rep, NULL), FMLX_HF_OK);
    CK(rep.n_ok == 2, "secondo pull n_ok=%zu", rep.n_ok);
    CK(n_meta == meta_at, "meta è ripartito: %d -> %d", meta_at, n_meta);
    CK(n_get == get_at, "get è ripartito: %d -> %d", get_at, n_get);
    CK(n_buf == 2, "il manifesto si rifà (non si cachea): %d", n_buf);
}

/* c) lo specchio: hardlink, non symlink, sottodirectory create. */
static void case_dest_hardlinks(void) {
    FmlxPullOpts o;
    FmlxPullReport rep;
    char dst[FMLX_HF_PATH_MAX], src[FMLX_HF_PATH_MAX], p[FMLX_HF_PATH_MAX];
    struct stat a, b;
    char dest_root[FMLX_HF_PATH_MAX];

    fresh("dest");
    snprintf(dest_root, sizeof dest_root, "%s/models", ROOT);
    if (fmlx_hf_mkdir_p(dest_root) != 0) { printf("  FIXTURE: mkdir %s\n", dest_root); failures++; return; }
    opts_for(&o);
    o.dest = dest_root;
    CK_ST(fmlx_pull_with(REPO, "main", &o, &VT, &rep, NULL), FMLX_HF_OK);
    CK(rep.n_mirrored == 2, "n_mirrored=%zu", rep.n_mirrored);
    {
        char want[FMLX_HF_PATH_MAX];
        snprintf(want, sizeof want, "%s/" LEAF, dest_root);
        CK(strcmp(rep.dest_dir, want) == 0, "dest_dir=%s atteso %s", rep.dest_dir, want);
    }

    snprintf(p, sizeof p, "%s/" LEAF, dest_root);
    CK(is_file(p, NULL) == 0, "%s deve essere una directory, non un file", p);

    snprintf(dst, sizeof dst, "%s/" LEAF "/config.json", dest_root);
    snprintf(src, sizeof src, "%s/" LEAF "/weights/model.safetensors", dest_root);
    CK(is_file(dst, NULL) && content_is(dst, BODY_A), "specchio %s", dst);
    CK(is_file(src, NULL) && content_is(src, BODY_B), "specchio annidato %s (la directory va creata)", src);
    CK(!is_symlink(dst) && !is_symlink(src), "lo specchio non deve essere symlink");

    /* Stesso inode = gli STESSI byte con un secondo nome: né una copia (30 GB due volte) né un
     * symlink (la trappola del file da 132 byte). */
    blob_path(p, sizeof p, BLOB_A);
    if (stat(p, &a) != 0 || stat(dst, &b) != 0) {
        CK(0, "stat fallita su %s o %s", p, dst);
    } else {
        CK(a.st_ino == b.st_ino && a.st_dev == b.st_dev, "hardlink atteso su %s (ino %llu vs %llu)", dst,
           (unsigned long long)a.st_ino, (unsigned long long)b.st_ino);
        CK(b.st_nlink >= 2, "nlink=%llu", (unsigned long long)b.st_nlink);
    }

    /* Un secondo pull con lo specchio già a posto non deve duplicare nulla. */
    memset(&rep, 0, sizeof rep);
    CK_ST(fmlx_pull_with(REPO, "main", &o, &VT, &rep, NULL), FMLX_HF_OK);
    CK(rep.n_mirrored == 2, "ri-mirror n_mirrored=%zu", rep.n_mirrored);
    if (stat(dst, &b) == 0) CK(b.st_nlink == a.st_nlink, "il secondo pull ha cambiato nlink: %llu",
                              (unsigned long long)b.st_nlink);
}

/* d) il server mente su UN file: il pull si ferma, e NON finge uno snapshot completo. */
static void case_mismatch_stops_everything(void) {
    FmlxPullOpts o;
    FmlxPullReport rep;
    char refs[FMLX_HF_PATH_MAX], dir[FMLX_HF_PATH_MAX], dst[FMLX_HF_PATH_MAX];
    const char *saved = ENTRIES[1].etag;

    fresh("mismatch");
    ENTRIES[1].etag = COMMIT; /* un nome che promette uno sha1 di git blob, e un corpo che non lo è */
    opts_for(&o);
    snprintf(dst, sizeof dst, "%s/models2", ROOT);
    o.dest = dst;
    memset(&rep, 0, sizeof rep);
    CK_ST(fmlx_pull_with(REPO, "main", &o, &VT, &rep, NULL), FMLX_HF_ERR_HASH_MISMATCH);
    ENTRIES[1].etag = saved;

    CK(rep.n_ok == 1, "n_ok=%zu (il primo file c'è, il secondo no)", rep.n_ok);
    CK(rep.n_mirrored == 0, "n_mirrored=%zu: con un file mancante lo specchio non si fa", rep.n_mirrored);
    CK(strcmp(rep.error_file, "weights/model.safetensors") == 0, "error_file=%s", rep.error_file);
    CK(rep.error_msg[0] != '\0', "nessun messaggio per l'errore");
    CK(rep.dest_dir[0] == '\0', "dest_dir=%s non deve essere stato riempito", rep.dest_dir);

    fmlx_hf_pjoin(dir, sizeof dir, ROOT, "models--org--repo");
    fmlx_hf_pjoin(refs, sizeof refs, dir, "refs");
    fmlx_hf_pjoin(refs, sizeof refs, refs, "main");
    CK(!is_file(refs, NULL), "refs/main scritto su un pull fallito: %s", refs);
    snprintf(dst, sizeof dst, "%s/" LEAF, dst);
    CK(!is_file(dst, NULL) && !is_symlink(dst), "lo specchio parziale esiste: %s", dst);
}

/* Il manifesto non arriva: niente file, niente rete sprecata, messaggio detto. */
static void case_manifest_refused(void) {
    FmlxPullOpts o;
    FmlxPullReport rep;

    fresh("manifest");
    opts_for(&o);

    /* offline: il manifesto NON è nel cache, quindi la risposta è un rifiuto e non "zero file". */
    o.fetch.offline = 1;
    memset(&rep, 0, sizeof rep);
    CK_ST(fmlx_pull_with(REPO, "main", &o, &VT, &rep, NULL), FMLX_HF_ERR_OFFLINE);
    CK(rep.error_msg[0] != '\0', "rifiuto offline senza messaggio");
    CK(rep.n_files == 0, "n_files=%zu: senza manifesto non si nomina nessun file", rep.n_files);
    CK(n_meta == 0 && n_get == 0, "offline ha comunque toccato la lane download: meta=%d get=%d", n_meta,
       n_get);
    o.fetch.offline = 0;

    /* Il server non conosce il repo: il codice arriva com'è stato tradotto dalla lane manifesto, e
     * `error_file` resta vuoto perché il fallimento NON è di un file. */
    buf_status = FMLX_HF_ERR_NOT_ON_REMOTE;
    memset(&rep, 0, sizeof rep);
    CK_ST(fmlx_pull_with(REPO, "main", &o, &VT, &rep, NULL), FMLX_HF_ERR_NOT_ON_REMOTE);
    buf_status = FMLX_HF_OK;
    CK(rep.error_file[0] == '\0', "error_file=%s: nessun file era in gioco", rep.error_file);
    CK(n_meta == 0 && n_get == 0, "un repo inesistente ha comunque chiesto file: meta=%d get=%d", n_meta,
       n_get);
}

/* Il report è opzionale, la verità no: con rep == NULL il comando deve comunque rispondere il
 * codice giusto (e non crashare scrivendo where). */
static void case_null_report(void) {
    FmlxPullOpts o;
    fresh("nullreport");
    opts_for(&o);
    CK_ST(fmlx_pull_with(REPO, "main", &o, &VT, NULL, NULL), FMLX_HF_OK);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("uso: %s <dir-di-lavoro>\n", argv[0]);
        return 2;
    }
    snprintf(BASE, sizeof BASE, "%s", argv[1]);
    setvbuf(stdout, NULL, _IONBF, 0);
    if (fmlx_hf_mkdir_p(BASE) != 0) {
        printf("FIXTURE: mkdir %s: %s\n", BASE, strerror(errno));
        return 2;
    }
    /* L'ambiente di chi esegue non deve decidere i risultati. */
    unsetenv("HF_TOKEN");
    unsetenv("HF_HUB_OFFLINE");
    unsetenv("TRANSFORMERS_OFFLINE");
    unsetenv("HF_ENDPOINT");
    unsetenv("HF_HOME");
    unsetenv("HF_HUB_CACHE");
    build_manifest();

    case_both_at_one_commit();
    case_second_pull_says_nothing();
    case_dest_hardlinks();
    case_mismatch_stops_everything();
    case_manifest_refused();
    case_null_report();

    printf("test_pull: %d verifiche, %d fallimenti\n", checks, failures);
    return failures ? 1 : 0;
}
