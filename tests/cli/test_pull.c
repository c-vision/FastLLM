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
 *  e) l'avanzamento: su un terminale UNA riga che si riscrive, su tutto il resto nessun carattere
 *     di controllo. Sono due decisioni diverse (quando disegnare / se disegnare) e hanno due prove
 *     diverse — i casi in fondo al file.
 *  f) LA COPIA dei byte di `--dest`, eseguita diretta: il ramo che la invoca (`link` che rende
 *     `EXDEV`) non esiste su questa macchina, e senza la chiamata diretta nessun test avrebbe mai
 *     eseguito il codice che sposta i byte di un modello.
 *
 * I valori esadecimali qui sotto non sono inventati: `git hash-object` e `shasum -a 256` sui due
 * corpi, misurati su questa macchina. Il test costruisce il cache con le regole della lane hf
 * (pjoin/mkdir), non con path scritti a mano: due modi diversi di costruire lo stesso path
 * maskerebbero un bug del primo.
 */
#include "fmlx/fmlx_cli.h"
#include "fmlx_cli_internal.h" /* il renderer si prova DA SOLO, con l'orologio in mano */

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h> /* fork/waitpid: il drenatore della pty, vedi `case_progress_pty` */
#include <util.h> /* openpty: provare che la decisione `isatty` è presa sul FILE giusto */
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
static int n_buf, n_meta, n_get, n_configure, n_ticks;
static FmlxHfFetchOpts GIVEN; /* le opzioni della chiamata, come le vede il transport */
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
        /* Il contratto di `on_bytes`, nella forma esatta in cui lo onora `body_cb`: DOPO la write,
         * con la POSIZIONE NEL FILE (quindi `off`, che parte da `from`) e la promessa del file
         * intero. Un finto che non la chiama lascia il renderer provato solo a mano. */
        if (GIVEN.on_bytes) {
            n_ticks++;
            GIVEN.on_bytes(GIVEN.bytes_ctx, off, limit);
        }
    }
    *written = off - from;
    return FMLX_HF_OK;
}

/* `configure` non è un ornamento: è il CANALE con cui le opzioni della chiamata arrivano al
 * transport (il watchdog, e adesso l'hook di avanzamento). Un finto che lo lascia NULL prova meno
 * della realtà, perché il `pull` cabla l'hook esattamente lì dentro. */
static void fake_configure(void *c, const FmlxHfFetchOpts *o) {
    (void)c;
    n_configure++;
    /* Copia INTEGRA, anche a vuoto: `GIVEN` deve raccontare le opzioni di QUESTA chiamata, non
     * l'ultima che ne aveva. È lo stesso obbligo che il transport vero documenta in `Ctx`. */
    memset(&GIVEN, 0, sizeof GIVEN);
    if (o) GIVEN = *o;
}

static void fake_reset(void) {
    n_buf = n_meta = n_get = n_configure = n_ticks = 0;
    buf_status = FMLX_HF_OK;
    memset(&GIVEN, 0, sizeof GIVEN);
}

/* Ordine dei campi: ctx, meta, get, configure, get_buf. */
static const FmlxHfTransport VT = {NULL, fake_meta, fake_get, fake_configure, fake_buf};

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

/* ---- l'avanzamento: due livelli, perché sono DUE decisioni ----
 *
 *  · QUANDO disegnare (il renderer: throttling, velocità, eta, cosa non inventare) si prova qui
 *    dentro con l'OROLOGIO FINTO. Con `time(NULL)` dentro, «un ridisegno al secondo» si proverebbe
 *    solo dormendo un secondo per ogni asserzione.
 *  · SE disegnare (la scelta `isatty` sul FILE* del log) vive nel `pull`, e si prova con un vero
 *    terminale finto — una pty — e con un non-terminale vero — un `open_memstream`.
 *
 * L'uno non prova l'altro: un renderer perfetto che non viene mai invocato, e un invocatore che
 * invoca il renderer su una pipe, sono due errori che sembrano lo stesso verde.
 */
static char *RB;
static size_t RB_N;

static FILE *rb_file(void) {
    RB = NULL;
    RB_N = 0;
    return open_memstream(&RB, &RB_N);
}

/* `open_memstream` scrive il terminatore solo su fflush/fclose. */
static char *rb_end(FILE *f) {
    fclose(f);
    if (!RB) RB = strdup("");
    return RB;
}

static int count_ch(const char *s, char c) {
    int k = 0;
    for (; s && *s; s++)
        if (*s == c) k++;
    return k;
}

/* Il k-esimo ridisegno, senza il `\r` e senza gli spazi di cancellazione: la riga COME LA VEDE
 * chi guarda, non come l'ha messa il buffer. */
static char *seg(const char *s, int k) {
    static char out[700];
    int line = -1, i, j = 0;

    for (i = 0; s[i]; i++) {
        if (s[i] == '\r') {
            line++;
            /* `j` SI AZZERA SOLO sul segmento chiesto: rimetterlo a zero a ogni `\r` cancellava
             * ciò che era stato copiato, e la riga 0 usciva vuota da ogni asserzione. */
            if (line == k) j = 0;
            continue;
        }
        if (s[i] == '\n') break;
        if (line == k && j < (int)sizeof out - 1) out[j++] = s[i];
    }
    out[j] = '\0';
    while (j > 0 && out[j - 1] == ' ') out[--j] = '\0';
    return out;
}

/* La lunghezza GREZZA del k-esimo ridisegno, padding compreso. `seg` sfoltisce gli spazi perché
 * serve alle asserzioni sul testo; questa serve a parlare del padding, che QUEGLI spazi è. */
static int seg_len(const char *s, int k) {
    int line = -1, i, start = -1, len = 0;

    for (i = 0; s[i] && s[i] != '\n'; i++) {
        if (s[i] == '\r') {
            if (line == k) break;
            line++;
            if (line == k) start = i + 1;
            continue;
        }
    }
    if (start < 0) return -1;
    for (len = 0; start + len < i; len++)
        ;
    return len;
}

static void case_progress_disegna(void) {
    FmlxPullProgress p;
    FILE *f;
    char *s;

    /* Il filo principale: begin, un tick nello STESSO secondo (non deve disegnare), un tick al
     * secondo dopo (UN ridisegno), end. Tre `\r` e un solo `\n`. */
    f = rb_file();
    fmlx_pull_progress_init(&p, f, 1);
    fmlx_pull_progress_begin(&p, 1, 3, "model-00001-of-00004.safetensors", 1000, 100);
    fmlx_pull_progress_bytes(&p, 100, 1000, 100);
    fmlx_pull_progress_bytes(&p, 500, 1000, 101);
    fmlx_pull_progress_end(&p, 1000, 101);
    s = rb_end(f);
    CK(count_ch(s, '\r') == 3, "ridisegni=%d attesi 3 (begin, il tick del secondo dopo, end):\n%s",
       count_ch(s, '\r'), s);
    CK(count_ch(s, '\n') == 1, "a capo=%d: la riga di un file deve chiudersi UNA volta\n%s",
       count_ch(s, '\n'), s);
    CK(strstr(seg(s, 0), "[1/3]") != NULL && strstr(seg(s, 0), "model-00001") != NULL,
       "la prima riga non nomina file e posizione:\n%s", seg(s, 0));
    /* Al primo disegno non c'è velocità: un campione solo non ha un denominatore, e "0 B/s" è il
     * modo in cui una CLI insegna a non fidarsi delle sue cifre. */
    CK(strstr(seg(s, 0), "/s") == NULL, "la velocità al primo campione:\n%s", seg(s, 0));
    CK(strstr(seg(s, 1), "50%") != NULL, "il tick del secondo dopo non dice mezzo:\n%s", seg(s, 1));
    CK(strstr(seg(s, 2), "100%") != NULL, "l'ultima foto non dice tutto finito:\n%s", seg(s, 2));
    CK(strstr(seg(s, 1), "/s") != NULL, "col secondo campione la velocità manca:\n%s", seg(s, 1));
    /* I ridisegni non accorciano MAI: il padding cancella ciò che era più lungo, e senza questa
     * regola sotto una riga da 90 colonne resterebbero 30 caratteri di un file finito. */
    {
        int l0 = seg_len(s, 0), l1 = seg_len(s, 1), l2 = seg_len(s, 2);
        CK(l0 > 0 && l1 >= l0 && l2 >= l1,
           "righe che si accorciano, padding perduto: %d %d %d", l0, l1, l2);
    }

    /* Ripresa: il primo campione fissa il BASE, e la velocità si calcola da lì. 900 byte che
     * c'erano già non sono 900 byte/s: senza la sottrazione questa riga direbbe 950. */
    f = rb_file();
    fmlx_pull_progress_init(&p, f, 1);
    fmlx_pull_progress_begin(&p, 2, 3, "model-00002-of-00004.safetensors", 1000, 100);
    fmlx_pull_progress_bytes(&p, 900, 1000, 100);
    fmlx_pull_progress_bytes(&p, 950, 1000, 101);
    s = rb_end(f);
    /* Lo SPAZIO prima del numero è parte dell'asserzione: "50 byte/s" sta anche dentro "950
     * byte/s", e senza lo spazio questo controllo è sopravvissuto alla mutazione che azzera il
     * base — cioè un controllo verde che non prova niente. */
    CK(strstr(seg(s, 1), " 50 byte/s") != NULL,
       "la ripresa conta come merito di adesso (atteso 50 byte/s):\n%s", seg(s, 1));

    /* Nessuna lunghezza dichiarata: nessun segno di percentuale e nessuna barra. I byte sì, che
     * sono un fatto; la percentuale sarebbe un'invenzione. */
    f = rb_file();
    fmlx_pull_progress_init(&p, f, 1);
    fmlx_pull_progress_begin(&p, 3, 3, "pezzo.bin", -1, 100);
    fmlx_pull_progress_bytes(&p, 400, -1, 101);
    s = rb_end(f);
    CK(strchr(seg(s, 1), '%') == NULL, "una percentuale inventata senza promessa del server:\n%s",
       seg(s, 1));
    CK(strstr(seg(s, 1), "[--") == NULL, "la barra senza totale:\n%s", seg(s, 1));
    CK(strstr(seg(s, 1), "400 byte") != NULL, "i byte mancherebbero:\n%s", seg(s, 1));

    /* Un server che ha dichiarato MENO di quanto ha mandato: la barra si ferma, non va oltre. */
    f = rb_file();
    fmlx_pull_progress_init(&p, f, 1);
    fmlx_pull_progress_begin(&p, 1, 1, "x", 1000, 100);
    fmlx_pull_progress_bytes(&p, 1500, 1000, 101);
    s = rb_end(f);
    CK(strstr(seg(s, 1), "100%") != NULL && strstr(seg(s, 1), "150%") == NULL,
       "la barra oltre il cento:\n%s", seg(s, 1));

    /* abort: si va a capo, e BASTA. Riscrivere la riga direbbe una cosa diversa da quella che
     * l'utente ha visto, e il messaggio d'errore del chiamante deve arrivare sulla riga dopo. */
    f = rb_file();
    fmlx_pull_progress_init(&p, f, 1);
    fmlx_pull_progress_begin(&p, 1, 2, "meta.json", 1000, 100);
    fmlx_pull_progress_bytes(&p, 500, 1000, 101);
    fmlx_pull_progress_abort(&p);
    s = rb_end(f);
    CK(count_ch(s, '\r') == 2 && count_ch(s, '\n') == 1, "abort ha riscritto la riga:\n%s", s);
    CK(s[strlen(s) - 1] == '\n', "la riga abortita non è chiusa a capo:\n%s", s);

    /* IL NON-TERMINALE: qui dentro non esce un byte. È la regola 1 del renderer, ed è la stessa
     * asserzione fatta a monte (`case_progress_pipe`) da un'altra parte del confine. */
    f = rb_file();
    fmlx_pull_progress_init(&p, f, 0);
    fmlx_pull_progress_begin(&p, 1, 2, "meta.json", 1000, 100);
    fmlx_pull_progress_bytes(&p, 500, 1000, 101);
    fmlx_pull_progress_end(&p, 1000, 101);
    fmlx_pull_progress_abort(&p);
    s = rb_end(f);
    CK(RB_N == 0, "su un non-tty il renderer ha scritto %zu byte:\n%s", RB_N, s);
    CK(fmlx_pull_progress_inplace(&p) == 0, "inplace dice il contrario di tty=0");
}

/* LA STESSA FOTO NON SI RIDISEGNA. Il caso non è inventato: è ciò che si è misurato sul terminale
 * vero col server vero. L'ultimo chunk di un file arriva in un secondo DIVERSO da quello del
 * penultimo, il ridisegno lo porta al 100%, e `end` — che ha gli STESSI numeri — ne disegnava un
 * secondo identico. Su una riga che si riscrive l'utente vede un «100%» che trema, e in un log sono
 * byte doppi per ogni file. Il numero contato è la prova: senza il dedupe sono 3, non 2. */
static void case_progress_stessa_foto(void) {
    FmlxPullProgress p;
    FILE *f;
    char *s;

    f = rb_file();
    fmlx_pull_progress_init(&p, f, 1);
    fmlx_pull_progress_begin(&p, 3, 9, "flax_model.msgpack", 1000, 100);
    fmlx_pull_progress_bytes(&p, 1000, 1000, 101); /* l'ultimo chunk: disegna il 100% */
    fmlx_pull_progress_end(&p, 1000, 101);         /* la STESSA foto, un istante dopo */
    s = rb_end(f);
    CK(count_ch(s, '\r') == 2, "la foto duplicata è ancora a schermo: %d ridisegni, attesi 2\n%s",
       count_ch(s, '\r'), s);
    CK(count_ch(s, '\n') == 1, "a capo=%d: la riga di un file si chiude UNA volta\n%s",
       count_ch(s, '\n'), s);
    CK(strstr(seg(s, 1), "100%") != NULL, "l'unica foto finale non dice finito:\n%s", seg(s, 1));

    /* Un file già nel cache: zero chunk, zero callback. `begin` disegna lo zero, `end` ha lo stesso
     * corpo perché non c'è nessuna misura in mezzo: una riga, non due. */
    f = rb_file();
    fmlx_pull_progress_init(&p, f, 1);
    fmlx_pull_progress_begin(&p, 1, 1, "config.json", 7, 200);
    fmlx_pull_progress_end(&p, 0, 203);
    s = rb_end(f);
    CK(count_ch(s, '\r') == 1 && count_ch(s, '\n') == 1,
       "il file senza chunk ridisegna la stessa riga: %d ritorno carrello\n%s", count_ch(s, '\r'), s);

    /* IL ROVESCIO, che è la parte che conta: un numero DIVERSO non deve mai essere inghiottito. Un
     * dedupe fatto male — confronta un prefisso, o non azzera il memorizzato a ogni `begin` — ferma
     * la barra e il test sopra resta verde: è questo blocco che lo impedisce. */
    f = rb_file();
    fmlx_pull_progress_init(&p, f, 1);
    fmlx_pull_progress_begin(&p, 1, 1, "peso.bin", 1000, 300);
    fmlx_pull_progress_bytes(&p, 500, 1000, 301);
    fmlx_pull_progress_bytes(&p, 900, 1000, 302);
    fmlx_pull_progress_end(&p, 1000, 303);
    s = rb_end(f);
    CK(count_ch(s, '\r') == 4, "quattro foto diverse, ridisegni=%d:\n%s", count_ch(s, '\r'), s);
    CK(strstr(seg(s, 2), "90%") != NULL, "il novanta per cento è stato inghiottito:\n%s", seg(s, 2));
    CK(strstr(seg(s, 3), "100%") != NULL, "mancata l'ultima foto:\n%s", seg(s, 3));
}

/* LA COPIA, ESEGUITA DAVVERO (caso f in testa al file). Si guarda lo STATO del renderer e non il
 * testo disegnato: `copy_stream` prende l'orologio VERO da sé — stessa regola della lane download,
 * dove il tempo si prende alla fonte e non dentro il renderer — e un conto dei ridisegni dipenderebbe
 * da quanto impiega la scrittura su disco, cioè da ciò che questa macchina fa mentre un'altra AI la
 * usa. `t0`, `done` e `total` non dipendono dal tempo: dicono che l'hook è partito, che l'ultima
 * cifra è il file INTERO e non l'ultimo chunk, e che la promessa viene dalla `stat` del sorgente. A
 * questo serve il `total_hint` bugiardo del `begin`: se la copia non portasse la sua lunghezza, la
 * barra resterebbe incollata a 999999. */
static void case_copy_stream(void) {
    FmlxPullProgress p;
    char src[FMLX_HF_PATH_MAX], dst[FMLX_HF_PATH_MAX], dir[FMLX_HF_PATH_MAX], tmp[FMLX_HF_PATH_MAX];
    char missing[FMLX_HF_PATH_MAX], dst2[FMLX_HF_PATH_MAX];
    const size_t n = 65536 * 2 + 1000; /* più di un chunk, con la coda STORTA: due rami di read */
    unsigned char *a, *b;
    size_t i, got = 0;
    struct stat st;
    FILE *f, *log;

    fresh("copy-stream");
    snprintf(dir, sizeof dir, "%s/out", ROOT);
    if (fmlx_hf_mkdir_p(dir) != 0) {
        CK(0, "fixture mkdir %s: %s", dir, strerror(errno));
        return;
    }
    a = malloc(n);
    b = malloc(n);
    if (!a || !b) {
        CK(0, "fixture malloc di %zu byte", n);
        free(a);
        free(b);
        return;
    }
    for (i = 0; i < n; i++) a[i] = (unsigned char)(i * 31 + 7);
    snprintf(src, sizeof src, "%s/src.bin", ROOT);
    f = fopen(src, "wb");
    if (!f || fwrite(a, 1, n, f) != n) {
        CK(0, "fixture %s", src);
        if (f) fclose(f);
        free(a);
        free(b);
        return;
    }
    fclose(f);

    snprintf(dst, sizeof dst, "%s/peso.bin", dir);
    log = rb_file();
    fmlx_pull_progress_init(&p, log, 1);
    fmlx_pull_progress_begin(&p, 1, 1, "peso.bin", 999999, 100); /* mente di proposito, vedi sopra */
    CK_ST(fmlx_pull_copy_stream(src, dst, &p), FMLX_HF_OK);
    fclose(log);

    CK(stat(dst, &st) == 0, "la copia non esiste: %s: %s", dst, strerror(errno));
    CK(stat(dst, &st) == 0 && (size_t)st.st_size == n, "la copia misura %lld byte, attesi %zu",
       (long long)st.st_size, n);
    f = fopen(dst, "rb");
    if (f) {
        got = fread(b, 1, n, f);
        fclose(f);
    }
    CK(got == n && memcmp(a, b, n) == 0,
       "i byte copiati NON sono gli stessi: %zu su %zu%s", got, n, got == n ? "" : " (lunghezza)");
    snprintf(tmp, sizeof tmp, "%s.fmlx-part", dst);
    CK(stat(tmp, &st) != 0, "il temporaneo %s è rimasto in giro", tmp);

    CK(p.t0 >= 0, "l'hook non è mai partito durante la copia: t0=%" PRId64, p.t0);
    CK(p.done == (int64_t)n, "l'ultima cifra della copia è %" PRId64 ", attesi %zu", p.done, n);
    CK(p.total == (int64_t)n, "la promessa non viene dal sorgente: total=%" PRId64, p.total);

    /* Il sorgente MANCA. La directory di destinazione ESISTE (l'abbiamo creata sopra): se non fosse
     * così l'ERR_IO arriverebbe dall'open del temporaneo e il controllo passerebbe per il motivo
     * sbagliato. `prog == NULL` è l'altro ramo: il `--quiet`, che non ha una riga da aggiornare. */
    snprintf(missing, sizeof missing, "%s/noncix.bin", ROOT);
    snprintf(dst2, sizeof dst2, "%s/mai.bin", dir);
    CK_ST(fmlx_pull_copy_stream(missing, dst2, NULL), FMLX_HF_ERR_IO);
    CK(stat(dst2, &st) != 0, "%s è stata creata pur per un sorgente mancante", dst2);
    snprintf(tmp, sizeof tmp, "%s.fmlx-part", dst2);
    CK(stat(tmp, &st) != 0, "il temporaneo %s resta dopo un fallimento", tmp);

    /* E il fallimento DENTRO il loop: `src` è una directory, quindi l'open riesce e il primo
     * `read` rende `EISDIR`. È l'unico modo di arrivare all'`unlink` del temporaneo — che è poi la
     * promessa che conta: una copia interrotta non lascia NÉ la destinazione NÉ un `.fmlx-part` da
     * 30 GB in giro per il disco. Senza questo ramo l'`unlink` non lo esegue mai nessun test, e
     * «l'ho scritto» varrebbe quanto «funziona». */
    CK_ST(fmlx_pull_copy_stream(ROOT, dst2, NULL), FMLX_HF_ERR_IO);
    CK(stat(dst2, &st) != 0, "%s creata da una copia fallita a metà", dst2);
    snprintf(tmp, sizeof tmp, "%s.fmlx-part", dst2);
    CK(stat(tmp, &st) != 0, "il temporaneo %s sopravvive a una copia fallita a metà", tmp);

    free(a);
    free(b);
}

/* Il `pull` completo su un non-terminale (memstream): nessuna riga di controllo, e le righe
 * per-file di sempre restano dove erano. `n_ticks == 0` è l'altra metà della prova: l'hook non è
 * nemmeno offerto al transport, quindi non è che si offre e si finge di non vederlo. */
static void case_progress_pipe(void) {
    FmlxPullOpts o;
    FmlxPullReport rep;
    char *buf;
    size_t n = 0;
    FILE *log = open_memstream(&buf, &n);

    fresh("progress-pipe");
    opts_for(&o);
    memset(&rep, 0, sizeof rep);
    CK_ST(fmlx_pull_with(REPO, "main", &o, &VT, &rep, log), FMLX_HF_OK);
    fclose(log);
    if (!buf) buf = strdup("");
    buf[n] = '\0';
    CK(strchr(buf, '\r') == NULL, "la pipe ha ricevuto un ritorno carrello:\n%s", buf);
    CK(strstr(buf, "  [1/2] config.json  7 byte\n") != NULL,
       "la riga per-file classica è sparita:\n%s", buf);
    CK(strstr(buf, "  [2/2] weights/model.safetensors  4 byte\n") != NULL,
       "manca la seconda riga:\n%s", buf);
    CK(n_ticks == 0, "su un non-tty l'hook è stato offerto lo stesso: %d tick", n_ticks);
    CK(n_configure == 2, "configure=%d: due file, il manifesto non chiede un GET a fd", n_configure);
    free(buf);
}

/* E il terminale VERO: una pty. È l'unico modo di provare che la decisione è presa sul FILE*
 * giusto — `isatty` su un fd diverso, o peggio su stdout sempre, darebbe lo stesso verde qui
 * sopra. Se la pty non si può aprire il test lo DICE come fallimento: un salto silenzioso
 * lascerebbe la regola scoperta con l'aria di provata. */
static void case_progress_pty(void) {
    FmlxPullOpts o;
    FmlxPullReport rep;
    int m = -1, sl = -1;
    struct winsize ws;
    char stack[4096], *buf = stack;
    char dest_root[FMLX_HF_PATH_MAX], dst[FMLX_HF_PATH_MAX], outfile[FMLX_HF_PATH_MAX];
    size_t cap = sizeof stack, got = 0;
    pid_t kid;
    FILE *log, *rf;

    fresh("progress-pty");
    memset(&ws, 0, sizeof ws);
    ws.ws_col = 80;
    ws.ws_row = 24;
    if (openpty(&m, &sl, NULL, NULL, &ws) != 0) {
        CK(0, "openpty: %s — senza terminale finto la scelta `isatty` resta NON provata",
           strerror(errno));
        return;
    }
    opts_for(&o);
    /* Lo specchio entra in QUESTO caso perché è qui che ha una prova da fare: su un terminale deve
     * comparire una riga che dice cosa sta succedendo (altrimenti le barre `[i/n]` dello specchio
     * sono indistinguibili da quelle del download) e NON deve comparire la riga per-file che
     * `case_progress_pipe` esige. Le due modalità restano alternative anche sotto `--dest`. */
    snprintf(dest_root, sizeof dest_root, "%s/models", ROOT);
    o.dest = dest_root;
    memset(&rep, 0, sizeof rep);
    snprintf(outfile, sizeof outfile, "%s/pty-out.bin", ROOT);

    /* Il master lo drena un FIGLIO. Non è pignoleria: il buffer del driver di una pty è dell'ordine
     * del KILOBYTO, e quando l'output lo supera la `write` sullo schiavo si BLOCCA — il test si
     * impalla invece di fallire, che è il modo peggiore di sbagliare perché da fuori sembra «sta
     * ancora girando» e in una build automatica diventa un timeout senza colpevoli. Misurato su
     * questo caso: 898 byte passano, ~170 byte di log in più (il mutante `doublelog`) bloccano.
     * Il padre chiude lo schiavo PRIMA di `waitpid`: è quello, e non un tempo di attesa, a dire al
     * drenatore che è finita (il master rende `EIO` quando l'ultimo schiavo è chiuso). */
    kid = fork();
    if (kid < 0) {
        CK(0, "fork del drenatore: %s", strerror(errno));
        close(m);
        close(sl);
        return;
    }
    if (kid == 0) {
        char b[4096];
        ssize_t r;
        int fd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        close(sl); /* il figlio NON deve tenere lo schiavo: se lo tiene, il master non vede mai la
                    * fine e il `waitpid` del padre non torna mai */
        if (fd >= 0)
            while ((r = read(m, b, sizeof b)) > 0)
                if (write(fd, b, (size_t)r) != r) break;
        if (fd >= 0) close(fd);
        _exit(0);
    }
    log = fdopen(sl, "w");
    if (!log) {
        CK(0, "fdopen della pty: %s", strerror(errno));
        close(m);
        close(sl); /* anche il figlio, bloccato in `read`, si sveglia */
        return;
    }
    CK_ST(fmlx_pull_with(REPO, "main", &o, &VT, &rep, log), FMLX_HF_OK);
    fclose(log); /* = close(sl): chiude LO SCHIAVO, ed è questo a terminare il drenatore */
    waitpid(kid, NULL, 0);
    close(m);
    rf = fopen(outfile, "rb");
    if (!rf) {
        CK(0, "il drenatore non ha lasciato %s: %s", outfile, strerror(errno));
        return;
    }
    buf[0] = '\0';
    got = fread(buf, 1, cap - 1, rf);
    buf[got] = '\0';
    if (got + 1 >= cap - 1) {
        /* Lo spazio è finito mentre si leggeva: si dice, non si asserisce su un troncone. */
        CK(0, "l'output del terminale supera %zu byte: il caso va fatto leggere da un buffer più grande",
           cap);
    }
    fclose(rf);
    CK(n_ticks >= 2, "sul terminale l'hook non è mai arrivato al transport: %d tick", n_ticks);
    CK(strchr(buf, '\r') != NULL, "su un terminale VERO nessun ridisegno:\n%s", buf);
    CK(strstr(buf, "[1/2]") != NULL && strstr(buf, "config.json") != NULL,
       "la riga del terminale non nomina il primo file:\n%s", buf);
    CK(strstr(buf, "[2/2]") != NULL, "il secondo file non ha una riga sua:\n%s", buf);
    /* LA STESSA stringa che `case_progress_pipe` esige presente qui deve essere ASSENTE: le due
     * modalità sono alternative, non sovrapposte — una riga per-file sotto la barra sarebbe il
     * doppio log che l'utente non ha chiesto. */
    CK(strstr(buf, "  [1/2] config.json  7 byte\n") == NULL,
       "barra E riga per-file insieme (doppio log):\n%s", buf);

    /* E lo specchio, sul terminale: UNA riga che dice dove sta copiando, e nessuna delle righe
     * per-file che il caso della pipe esige. Sono le due metà della stessa regola, applicate al
     * secondo ciclo di `[i/n]` — quello che senza questa riga sembrerebbe un download doppio. */
    CK(strstr(buf, "specchio →") != NULL, "lo specchio non annuncia cosa fa:\n%s", buf);
    CK(strstr(buf, "  specchio ") == NULL, "riga per-file dello specchio sotto la barra:\n%s", buf);
    CK(rep.n_mirrored == 2, "n_mirrored=%zu: lo specchio del terminale non è girato", rep.n_mirrored);
    snprintf(dst, sizeof dst, "%s/models/" LEAF "/weights/model.safetensors", ROOT);
    CK(is_file(dst, NULL) && content_is(dst, BODY_B), "manca il file specchiato %s", dst);
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
    case_progress_disegna();
    case_progress_stessa_foto();
    case_copy_stream();
    case_progress_pipe();
    case_progress_pty();

    printf("test_pull: %d verifiche, %d fallimenti\n", checks, failures);
    return failures ? 1 : 0;
}
