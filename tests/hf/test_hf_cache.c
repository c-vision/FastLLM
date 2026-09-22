/* test_hf_cache.c — lane hf: percorsi, cache, verifica. Nessun uso di rete.
 *
 * Due cose rendono questo test utile invece che decorativo:
 *  1) le aspettative di hash vengono da UN ALTRO codice, non dal nostro: i git blob id sono
 *     stati prodotti da `git hash-object` su questa macchina e gli SHA256 sono i vettori
 *     pubblicati. La volta scorsa un reference copiato da me stesso ha nascosto un bug per
 *     ore (docs/quant-coverage.md §5): qui il reference non passa dal nostro SHA1.
 *  2) l'albero di fixture riproduce il cache REALE misurato, inclusi i casi che sembrano
 *     marginali e invece decidono se un downloader lavora o scarica a vuoto: entry come file
 *     regolare (scritte da un altro tool), symlink con "../" in numero diverso secondo la
 *     profondità, marker .no_exist, symlink pendenti e symlink che escono dal repo.
 */
#include "fmlx/fmlx_hf.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int checks, failures;

#define CK(cond, ...)                                                                                            \
    do {                                                                                                         \
        checks++;                                                                                                \
        if (!(cond)) {                                                                                           \
            failures++;                                                                                          \
            printf("  FALLIMENTO %s:%d: ", __FILE__, __LINE__);                                                 \
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
            printf("  FALLIMENTO %s:%d: %s -> %s [%s], atteso %s\n", __FILE__, __LINE__, #call,             \
                   fmlx_hf_status_str(got_), fmlx_hf_last_error() ? fmlx_hf_last_error() : "-",            \
                   fmlx_hf_status_str(want));                                                              \
        }                                                                                                        \
    } while (0)

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

static int mkdir_p_parent(const char *path) {
    char dir[FMLX_HF_PATH_MAX];
    const char *slash = strrchr(path, '/');
    if (!slash) return 0; /* path relativo senza directory: esiste già per costruzione */
    size_t n = (size_t)(slash - path);
    if (n == 0 || n >= sizeof dir) return -1;
    memcpy(dir, path, n);
    dir[n] = '\0';
    return mkdir_p(dir);
}

static void write_file(const char *path, const char *content, size_t len) {
    if (mkdir_p_parent(path) != 0) { printf("  FIXTURE: mkdir fallita per %s\n", path); failures++; return; }
    FILE *f = fopen(path, "wb");
    if (!f) { printf("  FIXTURE: fopen %s\n", path); failures++; return; }
    fwrite(content, 1, len, f);
    fclose(f);
}

/* ---- vettori da fonte INDIPENDENTE ----
 * git blob id: `git hash-object` su questa macchina (SHA1 di "blob <len>\0" + contenuto).
 * SHA1 nudo e SHA256: `shasum` / vettori pubblicati. Le colonne SHA1 nudo esistono perché
 * il punto del test è che NON coincidono: chi confonde le due scarta ogni file non-LFS. */
struct hashvec {
    const char *content;
    size_t len;
    const char *git_blob; /* 40 hex */
    const char *sha1_bare;
    const char *sha256; /* 64 hex */
};
static const struct hashvec VEC[] = {
    {"", 0, "e69de29bb2d1d6434b8b29ae775ad8c2e48c5391", "da39a3ee5e6b4b0d3255bfef95601890afd80709",
     "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
    {"a", 1, "2e65efe2a145dda7ee51d1741299f848e5bf752e", "86f7e437faa5a7fce15d1ddcb9eaeaea377667b8",
     "ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb"},
    {"a\n", 2, "78981922613b2afb6025042ff6bd878ac1994e85", NULL, NULL},
    {"hello world", 11, "95d09f2b10159347eece71399a7e2e907ea3df4f", NULL, NULL},
    {"abc", 3, NULL, "a9993e364706816aba3e25717850c26c9cd0d89d",
     "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
};

static const char *ROOT; /* radice del cache finto, sotto work/ */

static void fixture_path(char *out, size_t n, const char *rel) { snprintf(out, n, "%s/%s", ROOT, rel); }

static void build_fixture(void) {
    /* root/models--org--repo, come il cache reale */
    const char *rev = "0123456789abcdef0123456789abcdef01234567";
    char p[FMLX_HF_PATH_MAX], q[FMLX_HF_PATH_MAX];

    fixture_path(p, sizeof p, "models--org--repo/refs/main");
    write_file(p, rev, strlen(rev));
    /* Nel cache reale il ref finisce con "\n" (misurato su
     * models--mlx-community--Qwen3-0.6B-4bit/refs/main): lo scriviamo anche qui, cosi' il
     * parser dei ref viene provato sul formato vero e non su quello comodo per noi. */
    {
        FILE *f = fopen(p, "ab");
        fputc('\n', f);
        fclose(f);
    }

    fixture_path(p, sizeof p, "models--org--repo/refs/badsha");
    { FILE *f = fopen(p, "wb"); fputs("not-a-commit\n", f); fclose(f); }

    /* blob 40-hex = git blob id di "a" */
    fixture_path(p, sizeof p, "models--org--repo/blobs/2e65efe2a145dda7ee51d1741299f848e5bf752e");
    write_file(p, "a", 1);
    /* blob 64-hex = sha256 di "abc" */
    fixture_path(p, sizeof p, "models--org--repo/blobs/ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    write_file(p, "abc", 3);
    /* blob col nome giusto ma contenuto diverso (corruzione da intercettare) */
    fixture_path(p, sizeof p, "models--org--repo/blobs/95d09f2b10159347eece71399a7e2e907ea3df4f");
    write_file(p, "HELLO WORLD", 11);

    /* symlink di primo livello: ../../blobs/<nome> */
    fixture_path(p, sizeof p, "models--org--repo/snapshots");
    mkdir_p(p);
    snprintf(q, sizeof q, "%s/models--org--repo/snapshots/%s", ROOT, rev);
    mkdir_p(q);
    fixture_path(p, sizeof p, "models--org--repo/snapshots/0123456789abcdef0123456789abcdef01234567/a.txt");
    unlink(p);
    if (symlink("../../blobs/2e65efe2a145dda7ee51d1741299f848e5bf752e", p) != 0) printf("  FIXTURE symlink %s\n", p);

    /* symlink annidato: un "../" in piu per ogni sottocartella */
    fixture_path(q, sizeof q, "models--org--repo/snapshots/0123456789abcdef0123456789abcdef01234567/sub");
    mkdir_p(q);
    fixture_path(p, sizeof p, "models--org--repo/snapshots/0123456789abcdef0123456789abcdef01234567/sub/b.txt");
    unlink(p);
    if (symlink("../../../blobs/ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", p) != 0)
        printf("  FIXTURE symlink annidato\n");

    /* entry come file regolare: esiste, scritta da un altro tool (Rapid-MLX) */
    fixture_path(p, sizeof p, "models--org--repo/snapshots/0123456789abcdef0123456789abcdef01234567/plain.json");
    write_file(p, "{}", 2);

    /* symlink pendente */
    fixture_path(p, sizeof p, "models--org--repo/snapshots/0123456789abcdef0123456789abcdef01234567/dangling.bin");
    unlink(p);
    (void)symlink("../../blobs/deadbeefdeadbeefdeadbeefdeadbeefdeadbeef", p);

    /* symlink che escono dal repo */
    fixture_path(p, sizeof p, "models--org--repo/snapshots/0123456789abcdef0123456789abcdef01234567/evil_deep");
    unlink(p);
    (void)symlink("../../../../../../etc/passwd", p);
    fixture_path(p, sizeof p, "models--org--repo/snapshots/0123456789abcdef0123456789abcdef01234567/evil_abs");
    unlink(p);
    (void)symlink("/etc/passwd", p);
    /* ../../blobs/... MA con un segmento in piu: ".." dentro il nome del blob */
    fixture_path(p, sizeof p, "models--org--repo/snapshots/0123456789abcdef0123456789abcdef01234567/evil_inner");
    unlink(p);
    (void)symlink("../../blobs/../../outside", p);

    /* marker .no_exist (file vuoto, come misurato) */
    fixture_path(q, sizeof q, "models--org--repo/.no_exist/0123456789abcdef0123456789abcdef01234567");
    mkdir_p(q);
    fixture_path(p, sizeof p, "models--org--repo/.no_exist/0123456789abcdef0123456789abcdef01234567/missing_on_remote.txt");
    write_file(p, "", 0);

    /* blob dal nome non esadecimale: usabile ma non verificabile */
    fixture_path(p, sizeof p, "models--org--repo/blobs/README.md.etag-xyz");
    write_file(p, "ciao", 4);
    fixture_path(p, sizeof p, "models--org--repo/snapshots/0123456789abcdef0123456789abcdef01234567/weird.txt");
    unlink(p);
    (void)symlink("../../blobs/README.md.etag-xyz", p);
}

static void rm_rf(const char *path) {
    /* solo dentro work/: la usa SOLO il fixture setup per partire pulito */
    char cmd[FMLX_HF_PATH_MAX + 32];
    if (strncmp(path, ROOT, strlen(ROOT)) != 0 || !*path) return;
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", path);
    if (system(cmd) != 0) { /* il fixture si ricostruisce comunque */ }
}

/* ---- test ---- */

static void test_blob_kind(void) {
    CK(fmlx_hf_blob_hash_kind("2e65efe2a145dda7ee51d1741299f848e5bf752e") == FMLX_HF_HASH_GIT_BLOB,
       "40 hex non riconosciuto come git blob");
    CK(fmlx_hf_blob_hash_kind("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") ==
           FMLX_HF_HASH_SHA256,
       "64 hex non riconosciuto come sha256");
    CK(fmlx_hf_blob_hash_kind("2E65EFE2A145DDA7EE51D1741299F848E5BF752E") == FMLX_HF_HASH_NONE,
       "i nomi del cache sono minuscoli: l'uppercase non deve promettere nulla");
    CK(fmlx_hf_blob_hash_kind("README.md") == FMLX_HF_HASH_NONE, "nome con estensione promette un hash");
    CK(fmlx_hf_blob_hash_kind("") == FMLX_HF_HASH_NONE, "nome vuoto");
    CK(fmlx_hf_blob_hash_kind("2e65efe2a145dda7ee51d1741299f848e5bf752") == FMLX_HF_HASH_NONE, "39 hex");
    CK(fmlx_hf_blob_hash_kind("2e65efe2a145dda7ee51d1741299f848e5bf752e0") == FMLX_HF_HASH_NONE, "41 hex");
}

static void test_repo_dirname(void) {
    char out[FMLX_HF_PATH_MAX];
    struct { const char *id; const char *want; } OKV[] = {
        {"mlx-community/Qwen3-0.6B-4bit", "models--mlx-community--Qwen3-0.6B-4bit"},
        {"suno/bark", "models--suno--bark"},
        {"mzbac/flux1.dev.4bit.mlx", "models--mzbac--flux1.dev.4bit.mlx"},
        {"t5-base", "models--t5-base"},    /* senza namespace: misurato nel cache di esempio HF */
        {"JetBrains/iOS Dev Guide", "models--JetBrains--iOS Dev Guide"}, /* lo spazio è legittimo */
    };
    for (size_t i = 0; i < sizeof OKV / sizeof *OKV; i++) {
        FmlxHfStatus s = fmlx_hf_repo_dirname(OKV[i].id, out, sizeof out);
        CK(s == FMLX_HF_OK && strcmp(out, OKV[i].want) == 0, "'%s' -> '%s' (stato %s), atteso '%s'", OKV[i].id, out,
           fmlx_hf_status_str(s), OKV[i].want);
    }
    const char *BAD[] = {"", "/", "x/", "/x", "a/b/c", "a//b", "..", "../x", "a/../../b", "a/.hidden", "./x", "a/.",
                         "a/..", "a/b ", " a/b", "a/\tb"};
    for (size_t i = 0; i < sizeof BAD / sizeof *BAD; i++) {
        checks++;
        if (fmlx_hf_repo_dirname(BAD[i], out, sizeof out) == FMLX_HF_OK) {
            failures++;
            printf("  repo id accettato ma vietato: '%s' -> '%s'\n", BAD[i], out);
        }
    }
}

static void test_relpath(void) {
    /* `.gitattributes` e gli spazi NON sono casi inventati: il primo è misurato dentro
     * snapshots/<rev>/ su questa macchina, i secondi esistono nei nomi di repo su Hub. */
    const char *OK[] = {"model.safetensors", "a/b.c-d_e", "sub/dir/file.bin", ".gitattributes",
                        "My Model.bin", "a/日本語/config.json", "x.bin"};
    for (size_t i = 0; i < sizeof OK / sizeof *OK; i++)
        CK(fmlx_hf_check_relpath(OK[i]) == FMLX_HF_OK, "relpath lecito rifiutato: '%s' (%s)", OK[i],
           fmlx_hf_last_error());
    const char *BAD[] = {"", "/abs", "a//b", "../x", "a/../../x", "x/", ".", "..", "a/./b", "a\nb", "a\\b", "sub/"};
    for (size_t i = 0; i < sizeof BAD / sizeof *BAD; i++) {
        checks++;
        if (fmlx_hf_check_relpath(BAD[i]) == FMLX_HF_OK) {
            failures++;
            printf("  relpath vietato accettato: '%s'\n", BAD[i]);
        }
    }
}

static void clear_env(void) {
    unsetenv("HF_HUB_CACHE");
    unsetenv("HUGGINGFACE_HUB_CACHE");
    unsetenv("HF_HOME");
    unsetenv("XDG_CACHE_HOME");
    unsetenv("HF_ENDPOINT");
    unsetenv("HF_HUB_OFFLINE");
    unsetenv("TRANSFORMERS_OFFLINE");
}

static void test_cache_root(void) {
    char out[FMLX_HF_PATH_MAX];
    clear_env();
    setenv("HOME", "/home/tester", 1);

    setenv("HF_HUB_CACHE", "/a/b/", 1);
    CK_ST(fmlx_hf_cache_root(out, sizeof out), FMLX_HF_OK);
    CK(strcmp(out, "/a/b") == 0, "taglio dello slash finale: '%s'", out);

    setenv("HF_HUB_CACHE", "~/hc", 1);
    CK_ST(fmlx_hf_cache_root(out, sizeof out), FMLX_HF_OK);
    CK(strcmp(out, "/home/tester/hc") == 0, "'~' non espanso: '%s'", out);

    setenv("PROVA", "/opt/f", 1);
    setenv("HF_HUB_CACHE", "$PROVA/hub", 1);
    CK_ST(fmlx_hf_cache_root(out, sizeof out), FMLX_HF_OK);
    CK(strcmp(out, "/opt/f/hub") == 0, "'$VAR' non espansa: '%s'", out);
    setenv("HF_HUB_CACHE", "${PROVA}/hub", 1);
    CK_ST(fmlx_hf_cache_root(out, sizeof out), FMLX_HF_OK);
    CK(strcmp(out, "/opt/f/hub") == 0, "'${VAR}' non espansa: '%s'", out);

    /* catena di preferenza, come constants.py:129-155 */
    clear_env();
    setenv("HOME", "/home/tester", 1);
    setenv("HF_HOME", "/h", 1);
    setenv("HUGGINGFACE_HUB_CACHE", "/legacy", 1);
    CK_ST(fmlx_hf_cache_root(out, sizeof out), FMLX_HF_OK);
    CK(strcmp(out, "/legacy") == 0, "il legacy deve vincere su HF_HOME: '%s'", out);

    clear_env();
    setenv("HOME", "/home/tester", 1);
    unsetenv("HUGGINGFACE_HUB_CACHE");
    setenv("HF_HOME", "/h/", 1);
    CK_ST(fmlx_hf_cache_root(out, sizeof out), FMLX_HF_OK);
    CK(strcmp(out, "/h/hub") == 0, "HF_HOME -> '%s'", out);

    clear_env();
    setenv("HOME", "/home/tester", 1);
    setenv("XDG_CACHE_HOME", "/x", 1);
    CK_ST(fmlx_hf_cache_root(out, sizeof out), FMLX_HF_OK);
    CK(strcmp(out, "/x/huggingface/hub") == 0, "XDG -> '%s'", out);

    clear_env();
    setenv("HOME", "/home/tester", 1);
    CK_ST(fmlx_hf_cache_root(out, sizeof out), FMLX_HF_OK);
    CK(strcmp(out, "/home/tester/.cache/huggingface/hub") == 0, "HOME di default -> '%s'", out);

    clear_env();
    unsetenv("HOME");
    CK_ST(fmlx_hf_cache_root(out, sizeof out), FMLX_HF_ERR_NO_HOME);

    char tiny[4];
    setenv("HOME", "/home/tester", 1);
    CK_ST(fmlx_hf_cache_root(tiny, sizeof tiny), FMLX_HF_ERR_BUF_SMALL);
    clear_env();
}

static void test_offline_endpoint(void) {
    clear_env();
    CK(fmlx_hf_offline() == 0, "offline di default");
    setenv("HF_HUB_OFFLINE", "1", 1);
    CK(fmlx_hf_offline() == 1, "HF_HUB_OFFLINE=1");
    unsetenv("HF_HUB_OFFLINE");
    setenv("TRANSFORMERS_OFFLINE", "TRUE", 1);
    CK(fmlx_hf_offline() == 1, "TRANSFORMERS_OFFLINE=TRUE (maiuscole indifferenti)");
    unsetenv("TRANSFORMERS_OFFLINE");
    setenv("HF_HUB_OFFLINE", "0", 1);
    CK(fmlx_hf_offline() == 0, "HF_HUB_OFFLINE=0 non e' offline");
    unsetenv("HF_HUB_OFFLINE");
    setenv("HF_HUB_OFFLINE", "sì", 1);
    CK(fmlx_hf_offline() == 0, "valore non nella lista {1,ON,YES,TRUE}");
    unsetenv("HF_HUB_OFFLINE");

    char ep[FMLX_HF_PATH_MAX], url[FMLX_HF_PATH_MAX * 2];
    CK_ST(fmlx_hf_endpoint(ep, sizeof ep), FMLX_HF_OK);
    CK(strcmp(ep, "https://huggingface.co") == 0, "endpoint di default '%s'", ep);
    setenv("HF_ENDPOINT", "https://hub-ci.huggingface.co/", 1);
    CK_ST(fmlx_hf_endpoint(ep, sizeof ep), FMLX_HF_OK);
    CK(strcmp(ep, "https://hub-ci.huggingface.co") == 0, "rstrip('/') '%s'", ep);
    unsetenv("HF_ENDPOINT");

    /* forma esatta di HUGGINGFACE_CO_URL_TEMPLATE (constants.py:68) */
    CK_ST(fmlx_hf_url("mlx-community/Qwen3-0.6B-4bit", "main", "config.json", 1, url, sizeof url), FMLX_HF_OK);
    CK(strcmp(url, "https://huggingface.co/mlx-community/Qwen3-0.6B-4bit/resolve/main/config.json") == 0,
       "URL resolve '%s'", url);
    CK_ST(fmlx_hf_url("org/repo", "deadbeefdeadbeefdeadbeefdeadbeefdeadbeef", NULL, 0, url, sizeof url),
          FMLX_HF_ERR_NAME);
    CK_ST(fmlx_hf_url("org/repo", "deadbeefdeadbeefdeadbeefdeadbeefdeadbeef", "", 0, url, sizeof url),
          FMLX_HF_ERR_NAME);
    snprintf(url, sizeof url, "%s", "");
    CK_ST(fmlx_hf_url("org/repo", "main", NULL, 1, url, sizeof url), FMLX_HF_ERR_NAME);
    char small[16];
    CK_ST(fmlx_hf_url("org/repo", "main", "config.json", 1, small, sizeof small), FMLX_HF_ERR_BUF_SMALL);
}

static void test_resolve_and_locate(void) {
    const char *rev = "0123456789abcdef0123456789abcdef01234567";
    char repo[FMLX_HF_PATH_MAX], sha[64];
    FmlxHfRef ref;

    CK_ST(fmlx_hf_repo_dir(ROOT, "org/repo", repo, sizeof repo), FMLX_HF_OK);
    CK_ST(fmlx_hf_resolve_ref(repo, "main", sha, sizeof sha), FMLX_HF_OK);
    CK(strcmp(sha, rev) == 0, "ref 'main' risolto in '%s'", sha);
    CK_ST(fmlx_hf_resolve_ref(repo, "badsha", sha, sizeof sha), FMLX_HF_ERR_REF_CONTENT);
    CK_ST(fmlx_hf_resolve_ref(repo, "inesistente", sha, sizeof sha), FMLX_HF_ERR_REF_MISSING);
    CK_ST(fmlx_hf_resolve_ref(repo, "../escape", sha, sizeof sha), FMLX_HF_ERR_REVISION);
    CK_ST(fmlx_hf_resolve_ref(repo, rev, sha, sizeof sha), FMLX_HF_OK); /* 40-hex usato diretto */
    CK(strcmp(sha, rev) == 0, "sha passata a mano modificata: '%s'", sha);

    /* i quattro stati distinti di "non trovato" */
    CK_ST(fmlx_hf_locate(ROOT, "org/assente", "main", "a.txt", &ref), FMLX_HF_ERR_NO_REPO);
    CK_ST(fmlx_hf_locate(ROOT, "org/repo", "0000000000000000000000000000000000000000", "a.txt", &ref),
          FMLX_HF_ERR_SNAPSHOT_MISSING);
    CK_ST(fmlx_hf_locate(ROOT, "org/repo", "main", "mai_chiesto.txt", &ref), FMLX_HF_ERR_NOT_CACHED);
    CK_ST(fmlx_hf_locate(ROOT, "org/repo", "main", "missing_on_remote.txt", &ref), FMLX_HF_ERR_NOT_ON_REMOTE);
    CK_ST(fmlx_hf_locate(ROOT, "org/repo", "main", "../../passwd", &ref), FMLX_HF_ERR_NAME);

    /* git blob: locate + verify contro il vettore di `git hash-object` */
    CK_ST(fmlx_hf_locate(ROOT, "org/repo", "main", "a.txt", &ref), FMLX_HF_OK);
    CK(ref.is_blob_link == 1, "a.txt non marcata come blob link");
    CK(ref.hash_kind == FMLX_HF_HASH_GIT_BLOB, "hash_kind %d", (int)ref.hash_kind);
    CK(ref.size == 1, "size %lld", (long long)ref.size);
    CK(strstr(ref.path, "/blobs/2e65efe2a145dda7ee51d1741299f848e5bf752e") != NULL, "path blob '%s'", ref.path);
    CK_ST(fmlx_hf_verify(&ref, NULL), FMLX_HF_OK);

    /* annidato: tre "../" e sha256 */
    CK_ST(fmlx_hf_locate(ROOT, "org/repo", "main", "sub/b.txt", &ref), FMLX_HF_OK);
    CK_ST(fmlx_hf_verify(&ref, NULL), FMLX_HF_OK);
    /* la stessa entry NON deve risolversi con il prefisso di_depth sbagliato */
    CK_ST(fmlx_hf_locate(ROOT, "org/repo", "main", "sub/../sub/b.txt", &ref), FMLX_HF_ERR_NAME);

    /* entry file regolare: usabile, MA non verificabile (non e' un successo) */
    CK_ST(fmlx_hf_locate(ROOT, "org/repo", "main", "plain.json", &ref), FMLX_HF_OK);
    CK(ref.is_blob_link == 0 && ref.hash_kind == FMLX_HF_HASH_NONE, "plain.json classificata male");
    CK_ST(fmlx_hf_verify(&ref, NULL), FMLX_HF_ERR_NOT_VERIFIABLE);

    /* nome non esadecimale: usabile, non verificabile */
    CK_ST(fmlx_hf_locate(ROOT, "org/repo", "main", "weird.txt", &ref), FMLX_HF_OK);
    CK_ST(fmlx_hf_verify(&ref, NULL), FMLX_HF_ERR_NOT_VERIFIABLE);

    /* corruzione: il nome promette "hello world", il blob contiene "HELLO WORLD" */
    CK_ST(fmlx_hf_locate(ROOT, "org/repo", "main", "corrupted.marker", &ref), FMLX_HF_ERR_NOT_CACHED);
    {
        char p[FMLX_HF_PATH_MAX];
        fixture_path(p, sizeof p, "models--org--repo/snapshots");
        char ent[FMLX_HF_PATH_MAX];
        snprintf(ent, sizeof ent, "%s/%s/corrupted.marker", p, rev);
        if (symlink("../../blobs/95d09f2b10159347eece71399a7e2e907ea3df4f", ent) != 0)
            printf("  FIXTURE symlink corrupted\n");
        CK_ST(fmlx_hf_locate(ROOT, "org/repo", "main", "corrupted.marker", &ref), FMLX_HF_OK);
        CK_ST(fmlx_hf_verify(&ref, NULL), FMLX_HF_ERR_HASH_MISMATCH);
    }

    /* symlink pendente e symlink ostili: mai un path da seguire */
    CK_ST(fmlx_hf_locate(ROOT, "org/repo", "main", "dangling.bin", &ref), FMLX_HF_ERR_BLOB_MISSING);
    CK_ST(fmlx_hf_locate(ROOT, "org/repo", "main", "evil_deep", &ref), FMLX_HF_ERR_LINK_FORM);
    CK_ST(fmlx_hf_locate(ROOT, "org/repo", "main", "evil_abs", &ref), FMLX_HF_ERR_LINK_FORM);
    CK_ST(fmlx_hf_locate(ROOT, "org/repo", "main", "evil_inner", &ref), FMLX_HF_ERR_LINK_FORM);
}

/* Il test della forma esplicita del symlink, separata dal resto perche' e' la parte che
 * difende dall'uscita dal cache. */
static void test_link_form_depth(void) {
    const char *rev = "0123456789abcdef0123456789abcdef01234567";
    char dir[FMLX_HF_PATH_MAX], ent[FMLX_HF_PATH_MAX];
    FmlxHfRef ref;
    snprintf(dir, sizeof dir, "%s/models--org--repo/snapshots/%s/deeper/deepest", ROOT, rev);
    mkdir_p(dir);
    snprintf(ent, sizeof ent, "%s/c.txt", dir);

    /* 4 livelli di "../" (2 di base + 2 sottocartelle): corretto */
    unlink(ent);
    CK(symlink("../../../../blobs/2e65efe2a145dda7ee51d1741299f848e5bf752e", ent) == 0, "symlink 4 livelli");
    CK_ST(fmlx_hf_locate(ROOT, "org/repo", "main", "deeper/deepest/c.txt", &ref), FMLX_HF_OK);

    /* 3: troppo corto -> rifiutato, non risolto a caso */
    unlink(ent);
    CK(symlink("../../../blobs/2e65efe2a145dda7ee51d1741299f848e5bf752e", ent) == 0, "symlink 3 livelli");
    CK_ST(fmlx_hf_locate(ROOT, "org/repo", "main", "deeper/deepest/c.txt", &ref), FMLX_HF_ERR_LINK_FORM);

    /* 5: troppo lungo -> rifiutato */
    unlink(ent);
    CK(symlink("../../../../../blobs/2e65efe2a145dda7ee51d1741299f848e5bf752e", ent) == 0, "symlink 5 livelli");
    CK_ST(fmlx_hf_locate(ROOT, "org/repo", "main", "deeper/deepest/c.txt", &ref), FMLX_HF_ERR_LINK_FORM);
    unlink(ent);

    /* Il caso REALE misurato su models--bert-base-uncased: un file tre livelli sotto lo
     * snapshot (coreml/fill-mask/float32_model.mlpackage/Manifest.json) usa CINQUE "../".
     * Riprodotto qui perché è la conferma che la profondità = 2 + barre del relpath. */
    {
        char deep[FMLX_HF_PATH_MAX], deepent[FMLX_HF_PATH_MAX];
        snprintf(deep, sizeof deep, "%s/models--org--repo/snapshots/%s/l1/l2/l3", ROOT, rev);
        mkdir_p(deep);
        snprintf(deepent, sizeof deepent, "%s/Manifest.json", deep);
        unlink(deepent);
        CK(symlink("../../../../../blobs/2e65efe2a145dda7ee51d1741299f848e5bf752e", deepent) == 0,
           "symlink del caso reale a 5 livelli");
        CK_ST(fmlx_hf_locate(ROOT, "org/repo", "main", "l1/l2/l3/Manifest.json", &ref), FMLX_HF_OK);
        CK_ST(fmlx_hf_verify(&ref, NULL), FMLX_HF_OK);

        /* una directory NON è un file: errore proprio, non "forma del symlink" */
        CK_ST(fmlx_hf_locate(ROOT, "org/repo", "main", "l1", &ref), FMLX_HF_ERR_NOT_A_FILE);
        CK_ST(fmlx_hf_locate(ROOT, "org/repo", "main", "l1/l2", &ref), FMLX_HF_ERR_NOT_A_FILE);
    }
}

static void test_hash_vectors(void) {
    /* Invoca l'hash della lane SUI file della fixture, ma i valori attesi vengono da
     * `git hash-object`/`shasum`: se cambiamo il nostro SHA1 il test si rompe, se cambia
     * il reference no. */
    char path[FMLX_HF_PATH_MAX];
    fixture_path(path, sizeof path, "hvec_a");
    write_file(path, "a", 1);
    FmlxHfRef r;
    memset(&r, 0, sizeof r);
    snprintf(r.path, sizeof r.path, "%s", path);
    r.size = 1;
    /* 1 byte di contenuto: git blob id atteso = colonna git_blob */
    snprintf(r.blob_name, sizeof r.blob_name, "%s", VEC[1].git_blob);
    r.hash_kind = FMLX_HF_HASH_GIT_BLOB;
    CK_ST(fmlx_hf_verify(&r, NULL), FMLX_HF_OK);

    /* e lo stesso file NON deve verificare come SHA1 nudo: la distinzione è il punto */
    snprintf(r.blob_name, sizeof r.blob_name, "%s", VEC[1].sha1_bare);
    CK_ST(fmlx_hf_verify(&r, NULL), FMLX_HF_ERR_HASH_MISMATCH);

    fixture_path(path, sizeof path, "hvec_empty");
    write_file(path, "", 0);
    memset(&r, 0, sizeof r);
    snprintf(r.path, sizeof r.path, "%s", path);
    snprintf(r.blob_name, sizeof r.blob_name, "%s", VEC[0].git_blob);
    r.hash_kind = FMLX_HF_HASH_GIT_BLOB;
    CK_ST(fmlx_hf_verify(&r, NULL), FMLX_HF_OK);

    fixture_path(path, sizeof path, "hvec_abc");
    write_file(path, "abc", 3);
    memset(&r, 0, sizeof r);
    snprintf(r.path, sizeof r.path, "%s", path);
    r.size = 3;
    snprintf(r.blob_name, sizeof r.blob_name, "%s", VEC[4].sha256);
    r.hash_kind = FMLX_HF_HASH_SHA256;
    int64_t counted = -1;
    CK_ST(fmlx_hf_verify(&r, &counted), FMLX_HF_OK);
    CK(counted == 3, "byte contati %lld", (long long)counted);

    /* vuoto come sha256: vettore pubblicato */
    memset(&r, 0, sizeof r);
    snprintf(r.path, sizeof r.path, "%s", path);
    unlink(r.path);
    write_file(r.path, "", 0);
    snprintf(r.blob_name, sizeof r.blob_name, "%s", VEC[0].sha256);
    r.hash_kind = FMLX_HF_HASH_SHA256;
    CK_ST(fmlx_hf_verify(&r, NULL), FMLX_HF_OK);
}

int main(int argc, char **argv) {
    (void)argc;
    char work[FMLX_HF_PATH_MAX];
    snprintf(work, sizeof work, "%s", argc > 1 ? argv[1] : "work");
    if (mkdir_p(work) != 0) { printf("work non creabile\n"); return 2; }
    char fx[FMLX_HF_PATH_MAX];
    snprintf(fx, sizeof fx, "%s/hf-fixture", work);
    ROOT = fx;
    rm_rf(fx);
    if (mkdir_p(ROOT) != 0) { printf("fixture non creabile in %s\n", ROOT); return 2; }
    build_fixture();

    printf("== lane hf: cache, percorsi, verifica ==\n");
    test_blob_kind();
    test_repo_dirname();
    test_relpath();
    test_cache_root();
    test_offline_endpoint();
    test_resolve_and_locate();
    test_link_form_depth();
    test_hash_vectors();
    printf("%d verifiche, %d fallimenti\n", checks, failures);
    return failures ? 1 : 0;
}
