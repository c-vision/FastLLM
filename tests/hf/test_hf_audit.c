/* test_hf_audit.c — l'integrità del cache: gli hash TORNAO, e ciò che non torna ha una riga.
 *
 * Le fixture sono cache finti con hash VERI. È il punto di tutto il file: un verificatore provato
 * su nomi-esadecimali-a-caso non ha mai verificato niente, e potrebbe essere verde per sempre
 * mentre il `pull` scarta ogni blob. I nomi dei blob qui sotto vengono da `git hash-object` e da
 * `shasum` su questa macchina (li stessi vettori di `tests/hf/test_hf_cache.c`, copiati perché un
 * test che prendesse l'attesa dal codice sotto prova non sta provando niente).
 *
 * Cosa si guarda, in ordine di valore:
 *  1. il caso pulito — che NON dica niente (un checker che segnala è inutile quanto uno che tace);
 *  2. la corruzione vera (contenuto diverso dal nome) e il blob che non c'è più;
 *  3. le due note che sembrano un successo e non lo sono: zero file verificabili, e il
 *     mezzo-download `.incomplete` (che NON è un blob orfano: il suffisso lungo è un caso suo);
 *  4. i conti che devono tornare SEMPRE: `n_links + n_plain == n_files`, `n_verified <= n_verifiable`,
 *     e il tetto dei finding che trattiene 32 righe ma conta 40 problemi.
 *
 * Il cache REALE della macchina chiude il file, con le sole invarianti di aritmetica: se un altro
 * processo sta scaricando in questo momento, un hash che non torna è un fatto VERO ma non è una
 * regressione di questo codice, e un test che fallisce per quello insegna a ignorarlo.
 */
#include "fmlx/fmlx_hf.h"

#include <errno.h>
#include <ftw.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int checks, failures;
static char ROOT[FMLX_HF_PATH_MAX];
/* Il cache VERO della macchina, catturato PRIMA di `setenv`: se lo si leggesse dopo, il «cache
 * reale» audirebbe le proprie fixture e il caso perderebbe ogni valore. */
static char REAL_ROOT[FMLX_HF_PATH_MAX];

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

/* I vettori: contenuto -> nome che il cache gli darebbe. */
#define GIT_A "2e65efe2a145dda7ee51d1741299f848e5bf752e" /* git blob id di "a" (1 byte) */
#define GIT_HELLO "95d09f2b10159347eece71399a7e2e907ea3df4f" /* git blob id di "hello world" */
#define SHA_ABC "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" /* sha256 "abc" */
#define SHA_EMPTY "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
#define REV "0123456789abcdef0123456789abcdef01234567"
#define REV2 "1111111111111111111111111111111111111111"
#define NOTACOMMIT "non-sono-un-commit"

/* ---- i mattoni ---- */

static int remove_one(const char *p, const struct stat *st, int flags, struct FTW *ftwbuf) {
    (void)st;
    (void)flags;
    (void)ftwbuf;
    return remove(p);
}

/* FTW_PHYS: senza, i symlink verrebbero SEGUITI e si cancellerebbero i BLOB che condividono
 * l'inode con lo snapshot. Qui si smonta una fixture, non un modello. */
static void wipe(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return;
    if (nftw(path, remove_one, 6, FTW_DEPTH | FTW_PHYS) != 0)
        printf("  ATTENZIONE: wipe di %s incompleto: %s\n", path, strerror(errno));
}

/* UN SOLO buffer statico: due `at()` nella stessa espressione passerebbero lo STESSO path (la
 * trappola è documentata in test_hf_catalog.c, e vale uguale qui). */
static const char *at(const char *rel) {
    static char buf[FMLX_HF_PATH_MAX * 2];
    snprintf(buf, sizeof buf, "%s/hub/%s", ROOT, rel);
    return buf;
}

/* Buffer statico PROPRIO, non quello di `at()`: `fresh()` chiama l'una e l'altro nello stesso
 * giro, e condividerli vorrebbe dire mkdir-are il path sbagliato. */
static const char *hub(void) {
    static char buf[FMLX_HF_PATH_MAX * 2];
    snprintf(buf, sizeof buf, "%s/hub", ROOT);
    return buf;
}

static void dirr(const char *rel) {
    if (fmlx_hf_mkdir_p(at(rel)) != 0) {
        printf("  FIXTURE: mkdir %s: %s\n", rel, strerror(errno));
        failures++;
    }
}

static void put(const char *rel, const char *body) {
    const char *p = at(rel);
    FILE *f;
    if (fmlx_hf_ensure_parent(p) != 0) {
        printf("  FIXTURE: genitore di %s: %s\n", rel, strerror(errno));
        failures++;
        return;
    }
    f = fopen(p, "wb");
    if (!f) {
        printf("  FIXTURE: open %s: %s\n", rel, strerror(errno));
        failures++;
        return;
    }
    fputs(body, f);
    fclose(f);
}

/* Il bersaglio di un symlink del cache è RELATIVO (`../../blobs/<nome>`), e `symlink(2)` lo salva
 * così com'è dato: qui NON si passa da `at()`, che resolverebbe un path assoluto e il `locate`
 * lo rifiuterebbe come ERR_LINK_FORM — che è proprio una delle cose da provare, e si prova a parte. */
static void putlink(const char *entry_rel, const char *target_rel) {
    const char *p = at(entry_rel);
    if (fmlx_hf_ensure_parent(p) != 0) {
        printf("  FIXTURE: genitore di %s: %s\n", entry_rel, strerror(errno));
        failures++;
        return;
    }
    if (symlink(target_rel, p) != 0) {
        printf("  FIXTURE: symlink %s -> %s: %s\n", entry_rel, target_rel, strerror(errno));
        failures++;
    }
}

/* Il path del repo si costruisce a MANO qui, non con `fmlx_hf_repo_dir`: se la fixture usasse la
 * stessa funzione che sta sotto prova, un bug di mappatura id->directory sarebbe invisibile. */
static void repo_prefix(const char *dirname, const char *what, char *out, size_t n) {
    snprintf(out, n, "%s/%s", dirname, what);
}

static void fresh(const char *dirname) {
    if (fmlx_hf_mkdir_p(hub()) != 0) {
        printf("  FIXTURE: radice %s: %s\n", hub(), strerror(errno));
        failures++;
    }
    wipe(at(dirname));
}

static void repo_dirs(const char *dirname) {
    char b[FMLX_HF_PATH_MAX];
    repo_prefix(dirname, "refs", b, sizeof b);
    dirr(b);
    repo_prefix(dirname, "blobs", b, sizeof b);
    dirr(b);
}

static void ref_main(const char *dirname, const char *sha) {
    char b[FMLX_HF_PATH_MAX];
    repo_prefix(dirname, "refs/main", b, sizeof b);
    put(b, sha);
    { /* il file reale finisce con "\n" (misurato): il parser dei ref si prova sul formato vero */
        FILE *f = fopen(at(b), "ab");
        if (f) {
            fputc('\n', f);
            fclose(f);
        }
    }
}

static void blob(const char *dirname, const char *name, const char *body) {
    char b[FMLX_HF_PATH_MAX];
    repo_prefix(dirname, "blobs", b, sizeof b);
    {
        char p[FMLX_HF_PATH_MAX * 2];
        snprintf(p, sizeof p, "%s/%s/%s", dirname, "blobs", name);
        (void)b;
        put(p, body);
    }
}

static void snaplink(const char *dirname, const char *rev, const char *rel, const char *blobname) {
    char p[FMLX_HF_PATH_MAX * 2];
    snprintf(p, sizeof p, "%s/snapshots/%s/%s", dirname, rev, rel);
    putlink(p, blobname);
}

static void snapfile(const char *dirname, const char *rev, const char *rel, const char *body) {
    char p[FMLX_HF_PATH_MAX * 2];
    snprintf(p, sizeof p, "%s/snapshots/%s/%s", dirname, rev, rel);
    put(p, body);
}

/* ---- il repo di riferimento: due blob, due link, tutto a posto ---- */
static void build_clean(const char *dirname) {
    fresh(dirname);
    repo_dirs(dirname);
    ref_main(dirname, REV);
    blob(dirname, GIT_A, "a");
    blob(dirname, SHA_ABC, "abc");
    snaplink(dirname, REV, "config.json", "../../blobs/" GIT_A);
    snaplink(dirname, REV, "weights/model.safetensors", "../../../blobs/" SHA_ABC);
}

static const FmlxHfFinding *find_f(const FmlxHfAudit *a, FmlxHfStatus code, const char *rel) {
    size_t lim = a->n_problems + a->n_notes;
    size_t i;
    if (lim > FMLX_HF_AUDIT_FINDING_MAX) lim = FMLX_HF_AUDIT_FINDING_MAX;
    for (i = 0; i < lim; i++)
        if (a->findings[i].code == code && !strcmp(a->findings[i].rel, rel)) return &a->findings[i];
    return NULL;
}

/* ---- i casi ---- */

static void case_pulito(void) {
    FmlxHfAudit a;
    build_clean("models--org--buono");
    CK_ST(fmlx_hf_audit_repo(NULL, "org/buono", &a), FMLX_HF_OK);
    CK(a.n_problems == 0, "%zu problemi su un cache pulito", a.n_problems);
    CK(a.n_notes == 0, "%zu note su un cache pulito", a.n_notes);
    CK(a.n_snaps == 1 && a.n_refs == 1, "snaps=%zu refs=%zu, attesi 1 e 1", a.n_snaps, a.n_refs);
    CK(a.n_files == 2, "%zu entry, attesi 2", a.n_files);
    CK(a.n_links == 2 && a.n_plain == 0, "links=%zu plain=%zu", a.n_links, a.n_plain);
    CK(a.n_verifiable == 2, "%zu verificabili, attesi 2", a.n_verifiable);
    CK(a.n_verified == 2, "%zu verificati, attesi 2", a.n_verified);
    CK(a.n_blobs == 2, "%zu blob contati, attesi 2", a.n_blobs);
    CK(a.bytes_hashed == 4, "hashiti %" PRId64 " byte, attesi 4 (1 + 3)", (int64_t)a.bytes_hashed);
    CK(a.n_orphan_blobs == 0, "%zu orfani su un cache dove tutto è richiamato", a.n_orphan_blobs);
    CK(strcmp(a.repo_id, "org/buono") == 0, "repo_id '%s'", a.repo_id);
}

/* Il cache VERO di questa macchina NON ha una radice passata: `NULL` è il cammino di chi non dà
 * `--cache`, e se la regola d'ambiente si rompesse qui si vedrebbe con il messaggio giusto. */
static void case_corrotto(void) {
    FmlxHfAudit a;
    const FmlxHfFinding *f;
    build_clean("models--org--rotto");
    { /* lo STESSO nome, altri byte: è il caso del download interrotto e "riempito" a metà */
        char p[FMLX_HF_PATH_MAX * 2];
        snprintf(p, sizeof p, "models--org--rotto/blobs/%s", SHA_ABC);
        put(p, "abd");
    }
    CK_ST(fmlx_hf_audit_repo(NULL, "org/rotto", &a), FMLX_HF_OK);
    CK(a.n_problems == 1, "%zu problemi, atteso 1", a.n_problems);
    CK(a.n_verifiable == 2 && a.n_verified == 1, "verifiable=%zu verified=%zu, attesi 2 e 1",
       a.n_verifiable, a.n_verified);
    f = find_f(&a, FMLX_HF_ERR_HASH_MISMATCH, "weights/model.safetensors");
    CK(f != NULL, "nessun finding hash-mismatch sul file giusto");
    if (f) {
        CK(f->is_problem == 1, "l'hash che non torna è passato come NOTA");
        CK(strcmp(f->rev, REV) == 0, "rev '%s', atteso il commit", f->rev);
        CK(strstr(f->msg, SHA_ABC) != NULL, "il messaggio non nomina il blob: '%s'", f->msg);
    }
}

static void case_blob_mancante(void) {
    FmlxHfAudit a;
    char p[FMLX_HF_PATH_MAX * 2];
    build_clean("models--org--mancante");
    snprintf(p, sizeof p, "%s/hub/models--org--mancante/blobs/%s", ROOT, GIT_A);
    CK(remove(p) == 0, "rm del blob: %s", strerror(errno));
    CK_ST(fmlx_hf_audit_repo(NULL, "org/mancante", &a), FMLX_HF_OK);
    CK(a.n_problems == 1, "%zu problemi, atteso 1 (il symlink pende)", a.n_problems);
    CK(find_f(&a, FMLX_HF_ERR_BLOB_MISSING, "config.json") != NULL, "non è BLOB_MISSING su config.json");
    /* Il file perduto NON è "verificabile e fallito": non è stato nemmeno aperto. */
    CK(a.n_verifiable == 1 && a.n_verified == 1, "verifiable=%zu verified=%zu, attesi 1 e 1",
       a.n_verifiable, a.n_verified);
}

static void case_link_fuori_forma(void) {
    FmlxHfAudit a;
    build_clean("models--org--formafuori");
    /* Un symlink ASSOLUTO: è la forma che `locate` rifiuta per regola (un cache non è un modo per
     * far leggere /etc a un tool che scarica modelli). */
    snaplink("models--org--formafuori", REV, "trappola.json", "/etc/hosts");
    CK_ST(fmlx_hf_audit_repo(NULL, "org/formafuori", &a), FMLX_HF_OK);
    CK(a.n_problems == 1, "%zu problemi, atteso 1", a.n_problems);
    CK(find_f(&a, FMLX_HF_ERR_LINK_FORM, "trappola.json") != NULL, "non è LINK_FORM su trappola.json");
    CK(a.n_verifiable == 2, "%zu verificabili: il rifiutato non entra nel conteggio", a.n_verifiable);
}

/* Un repo di SOLI file regolari: `locate` risponde OK, l'hash non è promesso da niente. La riga
 * che conta è la NOTA — il "tutto a posto" di un verificatore che non ha verificato è il modo in
 * cui si scopre tardi che i controlli erano due e se n'è fatto uno. */
static void case_zero_verificabili(void) {
    FmlxHfAudit a;
    const FmlxHfFinding *f;
    fresh("models--org--piatti");
    repo_dirs("models--org--piatti");
    ref_main("models--org--piatti", REV);
    snapfile("models--org--piatti", REV, "README.md", "ciao\n");
    CK_ST(fmlx_hf_audit_repo(NULL, "org/piatti", &a), FMLX_HF_OK);
    CK(a.n_problems == 0, "%zu problemi: un file regolare non è corruzione", a.n_problems);
    CK(a.n_files == 1 && a.n_plain == 1 && a.n_links == 0, "files=%zu plain=%zu links=%zu", a.n_files,
       a.n_plain, a.n_links);
    CK(a.n_verifiable == 0 && a.n_verified == 0, "verifiable=%zu verified=%zu", a.n_verifiable,
       a.n_verified);
    f = find_f(&a, FMLX_HF_ERR_NOT_VERIFIABLE, "snapshots");
    CK(f != NULL && f->is_problem == 0, "nessuna nota «nulla di verificabile»");
    CK(a.n_notes == 1, "%zu note, attesa 1 (la sola «non ho verificato»)", a.n_notes);
}

/* Ref che punta un commit assente, snapshot che nessun ref punta, directory che non è un commit:
 * tre NOTE, ZERO problemi. È la linea che separa «non l'hai scaricato» da «è rotto». */
static void case_ref_e_snapshot_solitary(void) {
    FmlxHfAudit a;
    fresh("models--org--solitario");
    repo_dirs("models--org--solitario");
    ref_main("models--org--solitario", REV2); /* nessun snapshots/REV2 */
    blob("models--org--solitario", GIT_A, "a");
    snaplink("models--org--solitario", REV, "config.json", "../../blobs/" GIT_A);
    dirr("models--org--solitario/snapshots/" NOTACOMMIT);
    CK_ST(fmlx_hf_audit_repo(NULL, "org/solitario", &a), FMLX_HF_OK);
    CK(a.n_problems == 0, "%zu problemi: qui non c'è corruzione", a.n_problems);
    CK(find_f(&a, FMLX_HF_ERR_SNAPSHOT_MISSING, REV2) != NULL, "manca la nota sul ref non scaricato");
    CK(find_f(&a, FMLX_HF_ERR_REF_MISSING, "snapshots/" REV) != NULL,
       "manca la nota sullo snapshot non puntato");
    CK(find_f(&a, FMLX_HF_ERR_REVISION, "snapshots/" NOTACOMMIT) != NULL,
       "manca la nota sulla directory che non è un commit");
    CK(a.n_snaps == 2, "%zu directory in snapshots/, attese 2", a.n_snaps);
    /* Il file dello snapshot non-puntato è controllato LO STESSO: "orfano" non vuol dire "non
     * leggibile", e un hash che non torna lì dentro sarebbe un problema da dire. */
    CK(a.n_verified == 1, "%zu verificati dentro uno snapshot orfano", a.n_verified);
}

static void case_ref_contenuto_errato(void) {
    FmlxHfAudit a;
    fresh("models--org--refcattivo");
    repo_dirs("models--org--refcattivo");
    {
        char p[FMLX_HF_PATH_MAX];
        repo_prefix("models--org--refcattivo", "refs/main", p, sizeof p);
        put(p, "non-sono-un-commit\n");
    }
    CK_ST(fmlx_hf_audit_repo(NULL, "org/refcattivo", &a), FMLX_HF_OK);
    CK(a.n_problems == 1, "%zu problemi, atteso 1 (il ref non decodifica)", a.n_problems);
    CK(find_f(&a, FMLX_HF_ERR_REF_CONTENT, "refs/main") != NULL, "non è REF_CONTENT su refs/main");
}

/* I quattro «fatti» del cache: orfani, mezzo-download, marker .no_exist e lock. I lock NON
 * diventano una nota (sono normali, misurato sul cache reale); gli altri tre sì o sono contati. */
static void case_servizio(void) {
    FmlxHfAudit a;
    char p[FMLX_HF_PATH_MAX * 2];
    build_clean("models--org--servizio");
    blob("models--org--servizio", GIT_HELLO, "hello world"); /* nessuno lo richiama: orfano */
    {
        char incomp[FMLX_HF_PATH_MAX * 2];
        snprintf(incomp, sizeof incomp, "models--org--servizio/blobs/%s.incomplete", GIT_HELLO);
        put(incomp, "metà");
    }
    repo_prefix("models--org--servizio", ".no_exist/" REV "/melo.txt", p, sizeof p);
    put(p, "");
    repo_prefix("models--org--servizio", "blobs/" SHA_EMPTY, p, sizeof p);
    (void)p; /* uno SHA256 di zero byte: orfano anche lui, e verifica OK se qualcuno lo leggesse */
    blob("models--org--servizio", SHA_EMPTY, "");
    {
        char lk[FMLX_HF_PATH_MAX * 2];
        snprintf(lk, sizeof lk, ".locks/models--org--servizio/%s.lock", GIT_A);
        put(lk, "");
        snprintf(lk, sizeof lk, ".locks/models--org--servizio/leggimi.txt");
        put(lk, "non è un lock");
    }
    CK_ST(fmlx_hf_audit_repo(NULL, "org/servizio", &a), FMLX_HF_OK);
    CK(a.n_problems == 0, "%zu problemi: nessuno di questi è corruzione", a.n_problems);
    CK(a.n_partial == 1, "%zu .incomplete, atteso 1", a.n_partial);
    CK(a.n_orphan_blobs == 2, "%zu blob orfani, attesi 2 (hello world + lo vuoto)", a.n_orphan_blobs);
    CK(a.bytes_orphan == 11, "orfani di %" PRId64 " byte, attesi 11", (int64_t)a.bytes_orphan);
    CK(a.n_noexist == 1, "%zu marker .no_exist, atteso 1", a.n_noexist);
    CK(a.n_lock == 1, "%zu lock contati, atteso 1 (leggimi.txt non lo è)", a.n_lock);
    CK(find_f(&a, FMLX_HF_ERR_STALL, "blobs") != NULL, "nessuna nota sul mezzo-download");
    /* L'orfanotrofio si NOMINA: è lo spazio che l'utente vuole quando dice "fammi spazio". */
    CK(find_f(&a, FMLX_HF_OK, "blobs") != NULL, "nessuna nota sui blob non richiamati");
    CK(a.n_notes == 2, "%zu note, attese 2 (orfani + incomplete)", a.n_notes);
}

/* Il ricorsivo: un file annidato con i suoi "../../../" giusti, e una directory oltre il tetto che
 * va DETTA invece che saltata in silenzio. Il tetto è `AUDIT_DEPTH_MAX = 4` e il conteggio parte
 * dalla directory dello snapshot, che È il livello 1: la quarta (`d1/d2/d3/d4`) è quella rifiutata,
 * e il nome che la nota porta è quello della directory NON discesa — non del suo genitore. La prova
 * che il giro si è fermato davvero è il file dentro di essa: NON deve comparire nei conti. */
static void case_annidato(void) {
    FmlxHfAudit a;
    build_clean("models--org--annidato");
    snaplink("models--org--annidato", REV, "coreml/config.json", "../../../blobs/" GIT_A);
    dirr("models--org--annidato/snapshots/" REV "/d1/d2/d3/d4");
    {
        char p[FMLX_HF_PATH_MAX * 2];
        snprintf(p, sizeof p, "models--org--annidato/snapshots/%s/d1/d2/d3/d4/nascosto.json", REV);
        put(p, "a");
    }
    CK_ST(fmlx_hf_audit_repo(NULL, "org/annidato", &a), FMLX_HF_OK);
    CK(a.n_verified == 3, "%zu verificati (2 + l'annidato), attesi 3", a.n_verified);
    CK(a.n_files == 3, "%zu entry contate, attese 3: il file oltre il tetto NON si è visto",
       a.n_files);
    CK(find_f(&a, FMLX_HF_ERR_NAME, "d1/d2/d3/d4") != NULL,
       "il tetto di profondità non è stato annunciato");
    CK(a.n_problems == 0, "%zu problemi: la profondità non è corruzione", a.n_problems);
    CK(a.n_notes == 1, "%zu note, attesa 1 (la sola directory non discesa)", a.n_notes);
}

/* Trenta e più problemi: l'array si ferma a 32, i CONTATORI no. È l'unica modo di provare che il
 * report non sembra completo quando non lo è. */
static void case_tetto(void) {
    FmlxHfAudit a;
    int i, full = 0;
    fresh("models--org--molti");
    repo_dirs("models--org--molti");
    ref_main("models--org--molti", REV);
    for (i = 0; i < 40; i++) {
        char rel[64];
        snprintf(rel, sizeof rel, "peso%02d.dat", i);
        snaplink("models--org--molti", REV, rel, "../../blobs/" GIT_A); /* il blob non c'è */
    }
    CK_ST(fmlx_hf_audit_repo(NULL, "org/molti", &a), FMLX_HF_OK);
    CK(a.n_problems == 40, "%zu problemi, attesi 40", a.n_problems);
    CK(a.n_files == 40, "%zu entry, attese 40", a.n_files);
    CK(a.findings[FMLX_HF_AUDIT_FINDING_MAX - 1].code == FMLX_HF_ERR_BLOB_MISSING,
       "l'ultima riga trattenuta non è un problema");
    /* Le 32 fessure sono PIENE e la 33ª non è stata scritta: si prova CONTANDO, perché leggere il
     * byte oltre l'array sarebbe esso stesso uno sconfinamento — l'errore che volevo escludere
     * sarebbe stato commesso dal test che lo cerca. */
    for (i = 0; i < FMLX_HF_AUDIT_FINDING_MAX; i++)
        if (a.findings[i].rel[0]) full++;
    CK(full == FMLX_HF_AUDIT_FINDING_MAX, "%d righe trattenute su %d", full,
       FMLX_HF_AUDIT_FINDING_MAX);
}

static void case_errori_duri(void) {
    FmlxHfAudit a;
    build_clean("models--org--duro");
    CK_ST(fmlx_hf_audit_repo(NULL, "", &a), FMLX_HF_ERR_REPO_ID);
    CK_ST(fmlx_hf_audit_repo(NULL, "org/inesistente", &a), FMLX_HF_ERR_NO_REPO);
    /* La radice che non esiste è un'altra frase: «HF_HOME punta nel posto sbagliato». */
    {
        char ghost[FMLX_HF_PATH_MAX * 2];
        snprintf(ghost, sizeof ghost, "%s/hub-che-non-c_e", ROOT);
        CK_ST(fmlx_hf_audit_repo(ghost, "org/duro", &a), FMLX_HF_ERR_NO_CACHE);
    }
    /* `*out` azzera sempre, ANCHE sull'errore: un chiamante che leggesse il residuo del giro
     * precedente stamperebbe "0 problemi" per un audit mai partito. */
    CK(a.n_problems == 0 && a.repo_id[0] == '\0', "l'audit fallito ha lasciato residui nel report");
    memset(&a, 0xAA, sizeof a);
    CK_ST(fmlx_hf_audit_repo(NULL, "org/inesistente", &a), FMLX_HF_ERR_NO_REPO);
    CK(a.n_problems == 0 && a.repo_id[0] == '\0', "azzzeramento mancante dopo un report sporco");
}

/* Il cache REALE: sola lettura, nessuna asserzione su ciò che contiene (un altro processo può
 * stare scaricando proprio adesso), tutte le asserzioni sugli INSIEMI che riporta. */
static void case_reale(void) {
    FmlxHfRepo *v = NULL;
    FmlxHfAudit *a;
    size_t n = 0, i, audited = 0, cap = 256;
    FmlxHfStatus s;

    if (!REAL_ROOT[0]) {
        printf("  cache reale: nessuna radice risolta all'avvio, caso saltato\n");
        return;
    }
    /* Due passi, e NON `cap = 0` per «sapere quanti sono»: la lane risponde a quella chiamata con
     * ERR_IO («catalogo senza array dove scrivere», fmlx_hf_catalog.c:219). Il numero si impara
     * DALL'ERRORE GIUSTO — TOO_MANY riporta il conteggio VERO anche quando non scrive niente. */
    v = (FmlxHfRepo *)malloc(cap * sizeof *v);
    a = (FmlxHfAudit *)malloc(sizeof *a);
    if (!v || !a) {
        printf("  cache reale: memoria rifiutata, caso saltato\n");
        free(v);
        free(a);
        return;
    }
    s = fmlx_hf_catalog(REAL_ROOT, v, cap, &n, NULL);
    if (s == FMLX_HF_ERR_TOO_MANY) {
        cap = n;
        free(v);
        v = (FmlxHfRepo *)malloc(cap * sizeof *v);
        if (!v) {
            printf("  cache reale: %zu repo non stanno in memoria, caso saltato\n", cap);
            free(a);
            return;
        }
        s = fmlx_hf_catalog(REAL_ROOT, v, cap, &n, NULL);
    }
    if (s != FMLX_HF_OK) {
        printf("  cache reale (%s): %s — %s\n", REAL_ROOT, fmlx_hf_status_str(s),
               fmlx_hf_last_error() ? fmlx_hf_last_error() : "-");
        free(v);
        free(a);
        return;
    }
    for (i = 0; i < n; i++) {
        size_t k, said_yes = 0;
        s = fmlx_hf_audit_repo(REAL_ROOT, v[i].repo_id, a);
        if (s != FMLX_HF_OK) {
            printf("  cache reale: %s -> %s\n", v[i].repo_id, fmlx_hf_status_str(s));
            continue;
        }
        audited++;
        CK(a->n_links + a->n_plain <= a->n_files, "%s: link+plain (%zu+%zu) oltre i file (%zu)",
           v[i].repo_id, a->n_links, a->n_plain, a->n_files);
        CK(a->n_verified <= a->n_verifiable && a->n_verifiable <= a->n_files,
           "%s: verificati %zu > verificabili %zu > entry %zu", v[i].repo_id, a->n_verified,
           a->n_verifiable, a->n_files);
        /* Il verdetto e il report non devono mai potersi contraddire: se dice «nessun problema»,
         * nessuna riga trattenuta può portare il marchio `is_problem`. */
        for (k = 0; k < a->n_problems + a->n_notes && k < FMLX_HF_AUDIT_FINDING_MAX; k++)
            if (a->findings[k].is_problem) said_yes++;
        CK(!a->n_problems || said_yes, "%s: n_problems=%zu ma nessuna riga marcata problema",
           v[i].repo_id, a->n_problems);
        CK(a->n_problems ? said_yes <= a->n_problems : !said_yes,
           "%s: %zu righe marcate contro n_problems=%zu", v[i].repo_id, said_yes, a->n_problems);
        if (a->n_problems)
            printf("  ATTENZIONE (non è un fallimento): %s ha %zu problemi\n", v[i].repo_id,
                   a->n_problems);
    }
    printf("  cache reale (%s): %zu repo, %zu auditi\n", REAL_ROOT, n, audited);
    free(v);
    free(a);
}

int main(int argc, char **argv) {
    char fixture[FMLX_HF_PATH_MAX * 2];

    if (argc < 2) {
        printf("uso: %s <directory-di-lavoro>\n", argv[0]);
        return 2;
    }
    snprintf(ROOT, sizeof ROOT, "%s", argv[1]);
    /* Primo il cache VERO, poi si avvelena l'ambiente: l'ordine è il test. */
    if (fmlx_hf_cache_root(REAL_ROOT, sizeof REAL_ROOT) != FMLX_HF_OK) REAL_ROOT[0] = '\0';
    wipe(ROOT);
    if (fmlx_hf_mkdir_p(hub()) != 0) {
        printf("FIXTURE: radice %s: %s\n", hub(), strerror(errno));
        return 1;
    }
    /* `HF_HUB_CACHE` è la variabile che la lane legge per PRIMA: puntandola qui, ogni chiamata con
     * `NULL` cade nelle fixture. Senza, il `NULL` di `case_pulito` avrebbe risposto con il cache
     * della macchina — e un verificatore che guarda la directory giusta con gli hash sbagliati è
     * il modo peggiore di essere verdi. Le altre due NON si toccano: hanno priorità più bassa, e
     * `HF_HOME` vorrebbe `<HF_HOME>/hub`, non la radice stessa. */
    snprintf(fixture, sizeof fixture, "%s/hub", ROOT);
    setenv("HF_HUB_CACHE", fixture, 1);

    case_pulito();
    case_corrotto();
    case_blob_mancante();
    case_link_fuori_forma();
    case_zero_verificabili();
    case_ref_e_snapshot_solitary();
    case_ref_contenuto_errato();
    case_servizio();
    case_annidato();
    case_tetto();
    case_errori_duri();
    case_reale();

    printf("test_hf_audit: %d verifiche, %d fallimenti\n", checks, failures);
    return failures ? 1 : 0;
}
