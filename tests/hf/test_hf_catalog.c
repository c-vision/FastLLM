/* test_hf_catalog.c — l'elenco di ciò che è sul disco, SENZA rete e SENZA server finto.
 *
 * Qui non c'è un transport da falsificare: il catalogo legge un cache VERO, quindi il cache se lo
 * costruisce da sé, file per file. Ogni caso corrisponde a una cosa MISURATA sul cache reale di
 * questa macchina (34 directory su due radici), non a un'idea di come HF dovrebbe essere:
 * `refs/` senza `blobs/` (`models--mlx-community--Mixtral-8x22B-4bit`), `xet/` e `version.txt` nella
 * radice, directory reali dentro uno snapshot (il `coreml/` di bert), ref annidati (`refs/pr/3`), e
 * il fatto che due NOMI possono essere lo STESSO inode.
 *
 * Le asserzioni che contano sono sui NUMERI (byte, blob distinti) e su ciò che NON è contato: un
 * catalogo che sommasse anche gli symlink dello snapshot raddoppierebbe ogni peso, e un `models`
 * che raddoppia i GB è peggio di un `models` che non li stampa.
 */
#include "fmlx/fmlx_hf.h"

#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int checks, failures;
static char ROOT[FMLX_HF_PATH_MAX];

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

/* ---- i mattoni: un cache finto costruito su disco ---- */

/* FTW_PHYS: senza, un symlink che punta fuori verrebbe SEGUITO e cancellato. Il catalogo gira su un
 * cache fatto di symlink, e pulirne uno seguendo i link distruggerebbe il bersaglio. */
static int remove_one(const char *p, const struct stat *st, int flags, struct FTW *ftwbuf) {
    (void)st;
    (void)flags;
    (void)ftwbuf;
    return remove(p);
}

static void wipe(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return;
    if (nftw(path, remove_one, 6, FTW_DEPTH | FTW_PHYS) != 0)
        printf("  ATTENZIONE: wipe di %s incompleto: %s\n", path, strerror(errno));
}

/* Il work directory arriva come argomento (CMake), quindi OGNI path del test è ROOT + rel. Le
 * fixture si scrivono con lo stesso helper: se le due metà (scrivere e leggere) usassero due modi
 * diversi di comporre il path, il caso "radice sbagliata" passerebbe lo stesso. */
static const char *at(const char *rel) {
    static char buf[FMLX_HF_PATH_MAX * 2];
    snprintf(buf, sizeof buf, "%s/%s", ROOT, rel);
    return buf;
}

static void dir(const char *rel) {
    if (fmlx_hf_mkdir_p(at(rel)) != 0) {
        printf("  FIXTURE: mkdir %s: %s\n", rel, strerror(errno));
        failures++;
    }
}

static void put(const char *rel, const char *body) {
    const char *p = at(rel);
    if (fmlx_hf_ensure_parent(p) != 0) {
        printf("  FIXTURE: genitore di %s: %s\n", rel, strerror(errno));
        failures++;
        return;
    }
    FILE *f = fopen(p, "wb");
    if (!f) {
        printf("  FIXTURE: open %s: %s\n", rel, strerror(errno));
        failures++;
        return;
    }
    fputs(body, f);
    fclose(f);
}

/* `at()` ha UN SOLO buffer statico: `link(at(a), at(b))` passerebbe due pointer STESSI e il source
 * non esisterebbe (ENOENT misurato al primo giro). Il primo path va copiato prima di calcolare il
 * secondo — la stessa trappola vale per chiunque usi due volte un helper che risponde un buffer. */
static void hardlink(const char *from_rel, const char *to_rel) {
    char from[FMLX_HF_PATH_MAX * 2];
    snprintf(from, sizeof from, "%s", at(from_rel));
    if (link(from, at(to_rel)) != 0) {
        printf("  FIXTURE: link %s: %s\n", to_rel, strerror(errno));
        failures++;
    }
}

static int has_ref(const FmlxHfRepo *r, const char *name) {
    for (size_t i = 0; i < FMLX_HF_CATALOG_REFS_MAX; i++)
        if (r->refs[i][0] && strcmp(r->refs[i], name) == 0) return 1;
    return 0;
}

static const FmlxHfRepo *find(const FmlxHfRepo *v, size_t n, const char *id) {
    for (size_t i = 0; i < n; i++)
        if (strcmp(v[i].repo_id, id) == 0) return &v[i];
    return NULL;
}

/* ---- i casi ---- */

/* La radice che non esiste NON è "cache vuoto": sono due consigli diversi, e il codice deve poterli
 * distinguere. */
static void case_radice_assente(void) {
    FmlxHfRepo v[4];
    size_t n = 99, ign = 99;
    CK_ST(fmlx_hf_catalog(at("radice-che-non-c-e"), v, 4, &n, &ign), FMLX_HF_ERR_NO_CACHE);
    CK(n == 0 && ign == 0, "su errore n=%zu ign=%zu, attesi 0/0", n, ign);
    const char *e = fmlx_hf_last_error();
    CK(e && strstr(e, "radice-che-non-c-e") != NULL, "il messaggio non nomina il path: %s",
       e ? e : "(null)");
    /* `cap == 0` e `out == NULL` non devono nemmeno provare a leggere il disco. */
    CK_ST(fmlx_hf_catalog(at("hub"), NULL, 4, &n, NULL), FMLX_HF_ERR_IO);
    CK_ST(fmlx_hf_catalog(at("hub"), v, 0, &n, NULL), FMLX_HF_ERR_IO);
    CK(n == 0, "n=%zu (azzerrato anche sull'errore d'argomento)", n);
}

static void case_cache_vuoto(void) {
    FmlxHfRepo v[4];
    size_t n = 99;
    dir("hub-empty");
    CK_ST(fmlx_hf_catalog(at("hub-empty"), v, 4, &n, NULL), FMLX_HF_OK);
    CK(n == 0, "n=%zu (radice che ESISTE e non ha repo: 0, non un errore)", n);
}

/* Il caso normale, con i NUMERI giusti — e con lo symlink dello snapshot che NON deve essere
 * contato una seconda volta. */
static void case_repo_completo(void) {
    FmlxHfRepo v[8];
    size_t n = 0, ign = 99;
    const char *C = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    dir("hub/models--org--due");
    put("hub/models--org--due/refs/main", C);
    put("hub/models--org--due/blobs/b1", "12345");
    put("hub/models--org--due/blobs/b2", "1234567");
    /* Lo symlink VERO, nella forma del mondo ("../../blobs/<nome>"): è lui che non deve essere
     * contato, e un file regolare al suo posto non proverebbe niente. */
    {
        const char *snap = at("hub/models--org--due/snapshots/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        char link_to[FMLX_HF_PATH_MAX];
        if (fmlx_hf_mkdir_p(snap) == 0) {
            snprintf(link_to, sizeof link_to, "%s/config.json", snap);
            if (symlink("../../blobs/b1", link_to) != 0) {
                printf("  FIXTURE: symlink: %s\n", strerror(errno));
                failures++;
            }
        }
    }
    CK_ST(fmlx_hf_catalog(at("hub"), v, 8, &n, &ign), FMLX_HF_OK);
    CK(n == 1, "n=%zu", n);
    CK(ign == 0, "ignored=%zu", ign);
    const FmlxHfRepo *r = find(v, n, "org/due");
    CK(r != NULL, "repo_id ricostruito: %s", v[0].repo_id);
    if (!r) return;
    CK(strcmp(r->kind, "model") == 0, "kind=%s", r->kind);
    CK(strcmp(r->dirname, "models--org--due") == 0, "dirname=%s", r->dirname);
    CK(r->n_refs == 1, "n_refs=%zu", r->n_refs);
    CK(has_ref(r, "main"), "refs[0]=%s", r->refs[0]);
    CK(r->n_blobs == 2, "n_blobs=%zu", r->n_blobs);
    CK(r->bytes == 12, "bytes=%lld (5+7 attesi: lo symlink dello snapshot NON si doppia)",
       (long long)r->bytes);
    CK(r->mtime > 0, "mtime=%lld", (long long)r->mtime);
    CK(r->refs_only == 0, "refs_only=%d", r->refs_only);
    CK(r->ambiguous_name == 0, "ambiguous=%d", r->ambiguous_name);
}

/* Due NOMI, uno STESSO inode: l'hardlink è un blob in due posti, non due blob. */
static void case_inode_distinti(void) {
    FmlxHfRepo v[8];
    size_t n = 0;
    dir("hub2/models--a--b");
    put("hub2/models--a--b/blobs/x", "1234");
    hardlink("hub2/models--a--b/blobs/x", "hub2/models--a--b/blobs/y");
    CK_ST(fmlx_hf_catalog(at("hub2"), v, 8, &n, NULL), FMLX_HF_OK);
    CK(n == 1, "n=%zu", n);
    CK(v[0].n_blobs == 1, "n_blobs=%zu (1 atteso: due nomi, un inode)", v[0].n_blobs);
    CK(v[0].bytes == 4, "bytes=%lld (4 attesi, non 8)", (long long)v[0].bytes);
}

/* Il caso MISURATO su disco: `refs/` senza `blobs/`. Non è "0 byte", è "un puntatore". */
static void case_refs_only(void) {
    FmlxHfRepo v[8];
    size_t n = 0;
    dir("hub3/models--solo--ref");
    put("hub3/models--solo--ref/refs/main", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    CK_ST(fmlx_hf_catalog(at("hub3"), v, 8, &n, NULL), FMLX_HF_OK);
    CK(n == 1 && v[0].refs_only == 1, "refs_only=%d n=%zu", v[0].refs_only, n);
    CK(v[0].bytes == 0 && v[0].n_blobs == 0, "bytes=%lld n_blobs=%zu", (long long)v[0].bytes,
       v[0].n_blobs);
    CK(v[0].mtime == 0, "mtime=%lld (nessun blob: nessuna data)", (long long)v[0].mtime);
    /* ...e un repo SENZA refs e senza blob non è "solo ref": non è niente, e va detto. */
    dir("hub3/models--niente--niente");
    CK_ST(fmlx_hf_catalog(at("hub3"), v, 8, &n, NULL), FMLX_HF_OK);
    const FmlxHfRepo *nn = find(v, n, "niente/niente");
    CK(nn && nn->refs_only == 0 && nn->n_blobs == 0, "directory vuota: refs_only=%d",
       nn ? nn->refs_only : -1);
}

/* `version.txt`, `CACHEDIR.TAG` e `xet/` stanno nella radice VERAMENTE (misurato). Non sono repo,
 * e "non sono repo" si CONTA, non si tace. */
static void case_non_repo_contati(void) {
    FmlxHfRepo v[8];
    size_t n = 0, ign = 0;
    dir("hub4/models--o--p");
    put("hub4/models--o--p/blobs/z", "x");
    put("hub4/version.txt", "v2");
    put("hub4/CACHEDIR.TAG", "tag");
    dir("hub4/xet");
    dir("hub4/altro-senza-prefisso");
    CK_ST(fmlx_hf_catalog(at("hub4"), v, 8, &n, &ign), FMLX_HF_OK);
    CK(n == 1, "n=%zu (1 solo repo)", n);
    CK(ign == 4, "ignored=%zu (2 file + 2 directory non-repo)", ign);
}

/* Un ref può essere una SOTTODIRECTORY (`refs/pr/3`): il nome della revisione è il path relativo,
 * non l'ultimo segmento. */
static void case_ref_annidato(void) {
    FmlxHfRepo v[8];
    size_t n = 0;
    dir("hub5/models--o--q");
    put("hub5/models--o--q/refs/main", "cccccccccccccccccccccccccccccccccccccccc");
    put("hub5/models--o--q/refs/pr/3", "dddddddddddddddddddddddddddddddddddddddd");
    put("hub5/models--o--q/blobs/z", "x");
    CK_ST(fmlx_hf_catalog(at("hub5"), v, 8, &n, NULL), FMLX_HF_OK);
    CK(n == 1 && v[0].n_refs == 2, "n_refs=%zu", v[0].n_refs);
    CK(has_ref(&v[0], "main"), "manca main: '%s' '%s'", v[0].refs[0], v[0].refs[1]);
    CK(has_ref(&v[0], "pr/3"), "manca pr/3: '%s' '%s'", v[0].refs[0], v[0].refs[1]);
}

/* Più ref di quanti ne stiano in `refs[]`: il NUMERO VERO deve restare 11. Il contrario è la
 * truncation silenziosa che il piano vieta.
 *
 * QUALE ref venga troncati NON si asserisce: `readdir` non dà un ordine, e la prima stesura di questo
 * caso pretendeva "r00..r07 dentro, r08..r10 fuori" — un test che passa solo se il filesystem è
 * d'accordo. Si asserisce ciò che vale per ogni ordine: 11 contati, 8 slot pieni, ogni slot una
 * revisione diversa e di forma legale. */
static void case_ref_troncati_ma_visibili(void) {
    FmlxHfRepo v[8];
    size_t n = 0;
    char rel[FMLX_HF_PATH_MAX], sha[41];
    dir("hub6/models--o--r");
    for (int i = 0; i < 11; i++) {
        snprintf(sha, sizeof sha, "%040d", i); /* 40 cifre: un commit finto ma di forma legale */
        snprintf(rel, sizeof rel, "hub6/models--o--r/refs/r%02d", i);
        put(rel, sha);
    }
    put("hub6/models--o--r/blobs/z", "x");
    CK_ST(fmlx_hf_catalog(at("hub6"), v, 8, &n, NULL), FMLX_HF_OK);
    CK(v[0].n_refs == 11, "n_refs=%zu, attesi 11 (il numero VERO, non il cap)", v[0].n_refs);
    size_t filled = 0;
    for (size_t i = 0; i < FMLX_HF_CATALOG_REFS_MAX; i++) {
        if (!v[0].refs[i][0]) continue;
        filled++;
        CK(strlen(v[0].refs[i]) == 3 && v[0].refs[i][0] == 'r', "slot %zu = '%s' (atteso 'rNN')", i,
           v[0].refs[i]);
        for (size_t k = 0; k < i; k++) /* nessun nome due volte: gli slot non si riciclano */
            CK(strcmp(v[0].refs[k], v[0].refs[i]) != 0, "'%s' ripetuto negli slot %zu e %zu",
               v[0].refs[i], k, i);
    }
    CK(filled == FMLX_HF_CATALOG_REFS_MAX, "slot riempiti=%zu, attesi %d", filled,
       FMLX_HF_CATALOG_REFS_MAX);
}

/* Il "--" in MEZZO a un nome: la regola di HF produce un id con un "/" in più, e `ambiguous_name`
 * è la parola che lo dice invece del silenzio. */
static void case_nome_ambiguo(void) {
    FmlxHfRepo v[8];
    size_t n = 0;
    dir("hub7/models--a--b--c");
    put("hub7/models--a--b--c/blobs/z", "x");
    dir("hub7/models--senza--namespace");
    put("hub7/models--senza--namespace/blobs/z", "x");
    CK_ST(fmlx_hf_catalog(at("hub7"), v, 8, &n, NULL), FMLX_HF_OK);
    CK(n == 2, "n=%zu", n);
    const FmlxHfRepo *am = find(v, n, "a/b/c");
    CK(am != NULL, "id ricostruito: '%s' / '%s'", v[0].repo_id, v[1].repo_id);
    if (am) CK(am->ambiguous_name == 1, "ambiguous_name=%d (atteso 1: due '--' dopo il tipo)",
               am->ambiguous_name);
    const FmlxHfRepo *ok = find(v, n, "senza/namespace");
    if (ok) CK(ok->ambiguous_name == 0, "ambiguous_name=%d (atteso 0)", ok->ambiguous_name);
}

/* Dataset e space: il tipo viene dal PREFISSO, e `models` deve poterli separare dai model. */
static void case_tipi(void) {
    FmlxHfRepo v[8];
    size_t n = 0;
    dir("hub8/datasets--o--d");
    put("hub8/datasets--o--d/blobs/z", "x");
    dir("hub8/spaces--o--s");
    put("hub8/spaces--o--s/blobs/z", "x");
    CK_ST(fmlx_hf_catalog(at("hub8"), v, 8, &n, NULL), FMLX_HF_OK);
    CK(n == 2, "n=%zu", n);
    const FmlxHfRepo *d = find(v, n, "o/d"), *s = find(v, n, "o/s");
    CK(d && strcmp(d->kind, "dataset") == 0, "kind dataset: '%s'", d ? d->kind : "-");
    CK(s && strcmp(s->kind, "space") == 0, "kind space: '%s'", s ? s->kind : "-");
}

/* `readdir` non dà un ordine: un test che dipende dall'ordine del filesystem fallisce su un altro
 * disco. L'ordine è per `repo_id`, e questo lo prova. */
static void case_ordine_stabile(void) {
    FmlxHfRepo v[8];
    size_t n = 0;
    dir("hub9/models--zeta--z");
    put("hub9/models--zeta--z/blobs/z", "x");
    dir("hub9/models--alfa--a");
    put("hub9/models--alfa--a/blobs/z", "x");
    dir("hub9/models--beta--b");
    put("hub9/models--beta--b/blobs/z", "x");
    CK_ST(fmlx_hf_catalog(at("hub9"), v, 8, &n, NULL), FMLX_HF_OK);
    CK(n == 3, "n=%zu", n);
    CK(strcmp(v[0].repo_id, "alfa/a") == 0 && strcmp(v[1].repo_id, "beta/b") == 0 &&
           strcmp(v[2].repo_id, "zeta/z") == 0,
       "ordine: %s, %s, %s", v[0].repo_id, v[1].repo_id, v[2].repo_id);
}

/* L'array del chiamante è più piccolo di ciò che c'è: errore DISTINTIVO e numero VERO, non un
 * elenco monco con esito di successo. */
static void case_troppi(void) {
    FmlxHfRepo v[2];
    size_t n = 0;
    dir("hubA/models--o--one");
    put("hubA/models--o--one/blobs/z", "x");
    dir("hubA/models--o--two");
    put("hubA/models--o--two/blobs/z", "x");
    dir("hubA/models--o--three");
    put("hubA/models--o--three/blobs/z", "x");
    CK_ST(fmlx_hf_catalog(at("hubA"), v, 2, &n, NULL), FMLX_HF_ERR_TOO_MANY);
    CK(n == 3, "n=%zu, attesi 3 (il conteggio VERO, non il cap)", n);
}

/* Ciò che non è un blob NON si conta: una directory dentro `blobs/` e un dotfile. */
static void case_non_blob(void) {
    FmlxHfRepo v[8];
    size_t n = 0;
    dir("hubB/models--o--nb");
    put("hubB/models--o--nb/blobs/vero", "123456789");
    put("hubB/models--o--nb/blobs/.DS_Store", "sporcizia");
    dir("hubB/models--o--nb/blobs/sottocartella");
    put("hubB/models--o--nb/blobs/sottocartella/nido", "non-è-un-blob");
    CK_ST(fmlx_hf_catalog(at("hubB"), v, 8, &n, NULL), FMLX_HF_OK);
    CK(v[0].n_blobs == 1, "n_blobs=%zu (solo `vero`)", v[0].n_blobs);
    CK(v[0].bytes == 9, "bytes=%lld (attesi 9)", (long long)v[0].bytes);
}

/* catalog_repo: la stessa lettura, per uno solo — e i rifiuti DISTINTI. */
static void case_uno_solo(void) {
    FmlxHfRepo r;
    CK_ST(fmlx_hf_catalog(at("hub"), NULL, 8, NULL, NULL), FMLX_HF_ERR_IO);
    CK_ST(fmlx_hf_catalog_repo(at("hub"), "org/due", &r), FMLX_HF_OK);
    CK(r.bytes == 12 && r.n_blobs == 2, "bytes=%lld n_blobs=%zu", (long long)r.bytes, r.n_blobs);
    CK_ST(fmlx_hf_catalog_repo(at("hub"), "org/mai-visto", &r), FMLX_HF_ERR_NO_REPO);
    CK_ST(fmlx_hf_catalog_repo(at("hub"), "due/slash/in/piu/x", &r), FMLX_HF_ERR_REPO_ID);
    CK_ST(fmlx_hf_catalog_repo(at("hub"), "", &r), FMLX_HF_ERR_REPO_ID);
    CK_ST(fmlx_hf_catalog_repo(at("hub"), "org/due", NULL), FMLX_HF_ERR_IO);
}

/* ---- un repo che il filesystem rifiuta ---- */

/* «La directory C'È, il suo nome è quello di un repo, ma dentro non ci abbiamo guardato»: sono i
 * tre campi `unreadable`/`unread_part`/`unread_errno`, e stanno in piedi su una distinzione che qui
 * è tutta: i suoi numeri NON sono «zero», sono «non li sappiamo». Il difetto che questo caso chiude
 * era un `return` che al primo `chmod 000` si portava dietro anche l'elenco dei repo VIVIBILI e la
 * riga `AUDIT IMPOSSIBILE` del `doctor`: un `sudo huggingface-cli` che ha creato una directory
 * `root:700` rendeva invisibili gli altri ventidue modelli.
 *
 * Root non può provare questa cosa: con l'euid 0 i controlli di permesso sono saltati, il fixture si
 * apre lo stesso e il caso sarebbe verde su una premessa falsa. Si DICHIARA saltato. */
static void case_repo_murato(void) {
    FmlxHfRepo v[8], one;
    const FmlxHfRepo *g, *m;
    size_t n = 0, ign = 99;
    const char *e;

    if (geteuid() == 0) {
        printf("  (root: nessun chmod frena nessuno: caso del repo murato saltato)\n");
        return;
    }
    dir("hubC/models--org--buono");
    put("hubC/models--org--buono/blobs/b1", "12345");
    dir("hubC/models--org--murato/refs");
    put("hubC/models--org--murato/refs/main", "x");
    put("hubC/models--org--murato/blobs/b1", "1234567");
    if (chmod(at("hubC/models--org--murato"), 0) != 0) {
        printf("  FIXTURE: chmod su models--org--murato: %s\n", strerror(errno));
        failures++;
        return;
    }

    /* CIÒ CHE HA PRODOTTO IL BUG: il giro non si interrompe. */
    CK_ST(fmlx_hf_catalog(at("hubC"), v, 8, &n, &ign), FMLX_HF_OK);
    CK(n == 2, "n=%zu, attesi 2: un repo murato non cancella l'elenco degli altri", n);
    CK(ign == 0, "ignored=%zu: «non ho aperto» NON è «non è un repo», è un altro conteggio", ign);
    g = find(v, n, "org/buono");
    CK(g && g->bytes == 5 && g->n_blobs == 1 && g->unreadable == 0,
       "il repo leggibile ha perso i suoi numeri: %lld byte", g ? (long long)g->bytes : -1);
    m = find(v, n, "org/murato");
    CK(m != NULL, "il repo murato è sparito dall'elenco (il difetto, non un dettaglio)");
    if (m) {
        CK(m->unreadable == 1, "unreadable=%d", m->unreadable);
        CK(strcmp(m->unread_part, "dir") == 0,
           "unread_part='%s' (atteso 'dir': è la directory del repo che ha rifiutato, non un figlio"
           " dedotto)", m->unread_part);
        CK(m->unread_errno == EACCES, "unread_errno=%d (%s), atteso EACCES=%d", m->unread_errno,
           strerror(m->unread_errno), EACCES);
        CK(m->bytes == 0 && m->n_blobs == 0 && m->n_refs == 0 && m->mtime == 0,
           "i campi numerici restano zero (ZERO = «non misurato», ed è `unreadable` a dirlo)");
        /* Chiamarlo «solo ref» sarebbe il contrario esatto del motivo per cui `refs_only` esiste:
         * dire 0 byte di un repo che 0 byte non è. Qui a tenerlo spento è il `return` che viene
         * PRIMA del calcolo (non si arriva a contare); il caso sorello — un `blobs/` murato sopra
         * dei `refs/` leggibili — è in `case_pezzo_murato`, che è lì apposta per quello. */
        CK(m->refs_only == 0, "refs_only=%d su un repo NON aperto", m->refs_only);
        /* Il TIPO invece si sa: viene dal prefisso del nome, e il nome la radice ce l'ha dato. */
        CK(strcmp(m->kind, "model") == 0, "kind='%s'", m->kind);
        CK(strcmp(m->dirname, "models--org--murato") == 0, "dirname='%s'", m->dirname);
        CK(m->ambiguous_name == 0, "ambiguous_name=%d", m->ambiguous_name);
    }

    /* LA STESSA directory letta da sola: «non ho potuto aprire», NON «non c'è». Il secondo direbbe a
     * un `pull --repair` di riscrivere byte perfetti, ed è il codice che il `doctor` usa per
     * decidere fra «scaricalo» e «guardaci i permessi». */
    CK_ST(fmlx_hf_catalog_repo(at("hubC"), "org/murato", &one), FMLX_HF_ERR_IO);
    e = fmlx_hf_last_error();
    CK(e && strstr(e, "potuto aprire") != NULL, "la frase non nomina l'apertura: %s",
       e ? e : "(null)");
    CK(e && strstr(e, "Permission denied") != NULL, "la frase non porta l'errno: %s",
       e ? e : "(null)");
    CK(one.unreadable == 1, "anche sul fallimento `unreadable` si legge: %d", one.unreadable);
    CK(strcmp(one.dirname, "models--org--murato") == 0, "il fallimento non nomina il colpevole: '%s'",
       one.dirname);

    chmod(at("hubC/models--org--murato"), 0755); /* PRIMA di uscire: un fixture murato non si wipea */

    /* La RADICE murata resta FATALE, e con un'altra FRASE: qui non c'è nessuna entry da salvare, e
     * «non ho potuto guardare niente» che uscisse come «cache vuoto» sarebbe il risultato silenzioso
     * che questo file combatte da sempre. La distinzione fra «non esiste» e «non apre» decide QUALE
     * controllo fa perdere tempo all'utente: il path o i permessi. */
    dir("hubD/models--org--d");
    put("hubD/models--org--d/blobs/z", "x");
    if (chmod(at("hubD"), 0) != 0) {
        printf("  FIXTURE: chmod su hubD: %s\n", strerror(errno));
        failures++;
        return;
    }
    CK_ST(fmlx_hf_catalog(at("hubD"), v, 8, &n, NULL), FMLX_HF_ERR_NO_CACHE);
    CK(n == 0, "n=%zu su una radice che non apre, atteso 0", n);
    e = fmlx_hf_last_error();
    CK(e && strstr(e, "hubD") != NULL, "la radice non è nominata: %s", e ? e : "(null)");
    CK(e && strstr(e, "non si è potuto aprire") != NULL,
       "un `EACCES` sulla radice chiamato «non esiste»: %s", e ? e : "(null)");
    chmod(at("hubD"), 0755);
}

/* Il repo SI apre, ma uno dei suoi DUE pezzi no. È l'unico caso in cui `unread_part` serve davvero
 * — un booleano non bastava perché delle due metà si sa cose diverse:
 *
 *  - `refs/` murato, `blobs/` no: i BYTE SONO NOTI (li abbiamo contati), manca il numero delle
 *    revisioni. Un `unreadable` che azzerasse tutto qui mentirebbe in direzione opposta;
 *  - `blobs/` murato, `refs/` no: sappiamo quante revisioni ci sono, non quanto occupano;
 *  - tutti e due murati: vince il PRIMO (`mark_unreadable` non si sovrascrive) perché la prima
 *    directory che ha detto «no» è già il colpevole giusto, e sovrascriverla racconterebbe una
 *    sequenza diversa da quella in cui è andata.
 *
 * Da soli, questi tre casi sono anche l'unica prova che i rami `refs`/`blobs` di `mark_unreadable`
 * esistono: il caso del repo intero passa da `"dir"` e non li sfiora nemmeno. */
static void case_pezzo_murato(void) {
    FmlxHfRepo v[8];
    const FmlxHfRepo *sr, *sb, *sd;
    size_t n = 0;

    if (geteuid() == 0) {
        printf("  (root: nessun chmod frena nessuno: caso del pezzo murato saltato)\n");
        return;
    }
    dir("hubE/models--org--senfarefs");
    put("hubE/models--org--senfarefs/refs/main", "x");
    put("hubE/models--org--senfarefs/blobs/b1", "12345");
    dir("hubE/models--org--senzablobs/refs");
    put("hubE/models--org--senzablobs/refs/main", "x");
    put("hubE/models--org--senzablobs/blobs/b1", "1234567");
    dir("hubE/models--org--entrambi/refs");
    put("hubE/models--org--entrambi/refs/main", "x");
    put("hubE/models--org--entrambi/blobs/b1", "123456789");
    if (chmod(at("hubE/models--org--senfarefs/refs"), 0) != 0 ||
        chmod(at("hubE/models--org--senzablobs/blobs"), 0) != 0 ||
        chmod(at("hubE/models--org--entrambi/refs"), 0) != 0 ||
        chmod(at("hubE/models--org--entrambi/blobs"), 0) != 0) {
        printf("  FIXTURE: chmod sui pezzi: %s\n", strerror(errno));
        failures++;
    }
    CK_ST(fmlx_hf_catalog(at("hubE"), v, 8, &n, NULL), FMLX_HF_OK);
    CK(n == 3, "n=%zu, attesi 3 (nessuno dei tre sparisce)", n);

    sr = find(v, n, "org/senfarefs");
    CK(sr != NULL, "manca org/senfarefs");
    if (sr) {
        CK(sr->unreadable == 1 && strcmp(sr->unread_part, "refs") == 0,
           "part='%s' unreadable=%d", sr->unread_part, sr->unreadable);
        CK(sr->unread_errno == EACCES, "errno=%d", sr->unread_errno);
        /* LA parte che vale: i blob li abbiamo APERTI, quindi i byte si sanno — e un `unreadable`
         * che li buttasse via sarebbe bugiardo nella direzione opposta. */
        CK(sr->bytes == 5 && sr->n_blobs == 1, "bytes=%lld n_blobs=%zu (li abbiamo contati)",
           (long long)sr->bytes, sr->n_blobs);
        CK(sr->n_refs == 0, "n_refs=%zu (non è zero per il ref: è che `refs/` non ha aperto)",
           sr->n_refs);
        CK(sr->refs_only == 0, "refs_only=%d: senza `refs/` non si può affermare «solo ref»",
           sr->refs_only);
    }

    sb = find(v, n, "org/senzablobs");
    CK(sb != NULL, "manca org/senzablobs");
    if (sb) {
        CK(sb->unreadable == 1 && strcmp(sb->unread_part, "blobs") == 0,
           "part='%s' unreadable=%d", sb->unread_part, sb->unreadable);
        CK(sb->n_refs == 1 && sb->bytes == 0 && sb->n_blobs == 0,
           "n_refs=%zu bytes=%lld: i ref si sapevano, i byte no", sb->n_refs, (long long)sb->bytes);
        /* Il `refs/` c'è e i blob NON li abbiamo contati: `refs_only == 1` qui direbbe «occupa
         * niente» di un repo che occupa 7 byte. È il caso per cui il campo è un CAMPO. */
        CK(sb->refs_only == 0, "refs_only=%d su un `blobs/` non aperto", sb->refs_only);
    }

    sd = find(v, n, "org/entrambi");
    CK(sd != NULL, "manca org/entrambi");
    if (sd)
        CK(strcmp(sd->unread_part, "refs") == 0,
           "part='%s': la seconda directory murata non deve sovrascrivere la prima",
           sd->unread_part);

    chmod(at("hubE/models--org--senfarefs/refs"), 0755);
    chmod(at("hubE/models--org--senzablobs/blobs"), 0755);
    chmod(at("hubE/models--org--entrambi/refs"), 0755);
    chmod(at("hubE/models--org--entrambi/blobs"), 0755);
}

/* ---- il cache reale di questa macchina ---- */

/* Perché c'è, e cosa prova che gli altri casi non provano: le fixture le scriviamo noi, quindi non
 * possono smentirci. Qui l'input è il cache che `huggingface_hub` e `rapid-mlx` hanno scritto per
 * davvero, con dentro `trees/`, gli snapshot annidati, i `.lock` e i nomi che non ci aspettavamo.
 *
 * NON asserisce numeri: su questa macchina il cache cambia di ora in ora (altri processi scaricano).
 * Asserisce l'INVARIANTE che regge tutto il file — se una directory è nel catalogo, i suoi byte sono
 * la somma dei suoi blob — e che nessun path della radice reale faccia crashare o esploda un buffer. */
static void case_reale(void) {
    FmlxHfRepo *v = (FmlxHfRepo *)malloc(FMLX_HF_CATALOG_REPOS_MAX * sizeof *v);
    if (!v) { printf("  (memoria: il caso sul cache reale non può girare)\n"); return; }
    char root[FMLX_HF_PATH_MAX];
    if (fmlx_hf_cache_root(root, sizeof root) != FMLX_HF_OK) {
        printf("  (nessuna radice derivabile: caso sul cache reale saltato)\n");
        free(v);
        return;
    }
    size_t n = 0, ign = 0;
    FmlxHfStatus s = fmlx_hf_catalog(root, v, FMLX_HF_CATALOG_REPOS_MAX, &n, &ign);
    int bad = 0;
    for (size_t i = 0; i < n && s == FMLX_HF_OK; i++) {
        FmlxHfRepo one;
        if (strcmp(v[i].kind, "model") != 0) continue; /* dataset/space: id non invertibile qui */
        /* Un `unreadable` non viola l'invariante: i suoi byte NON sono «la somma dei suoi blob»,
         * sono «non li sappiamo» — ed è esattamente ciò che il campo dice. Chiamarci
         * `catalog_repo` sopra significherebbe pretendere che una seconda lettura, con lo STESSO
         * permesso, riesca dove la prima non è riuscita. */
        if (v[i].unreadable) continue;
        if (fmlx_hf_catalog_repo(root, v[i].repo_id, &one) != FMLX_HF_OK) {
            printf("  FALLIMENTO reale: '%s' nell'elenco ma catalog_repo dice [%s]\n",
                   v[i].repo_id, fmlx_hf_last_error());
            bad++;
            continue;
        }
        if (one.bytes != v[i].bytes || one.n_blobs != v[i].n_blobs) {
            printf("  FALLIMENTO reale: '%s' due letture diverse (%lld/%zu vs %lld/%zu)\n",
                   v[i].repo_id, (long long)v[i].bytes, v[i].n_blobs, (long long)one.bytes,
                   one.n_blobs);
            bad++;
        }
    }
    printf("  cache reale: %s -> %s, %zu repo, %zu entry non-repo\n", root, fmlx_hf_status_str(s), n,
           ign);
    checks++;
    if (bad) failures += bad;
    CK(s == FMLX_HF_OK || s == FMLX_HF_ERR_TOO_MANY, "esito %s", fmlx_hf_status_str(s));
    free(v);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("uso: %s <directory-di-lavoro>\n", argv[0]);
        return 2;
    }
    snprintf(ROOT, sizeof ROOT, "%s", argv[1]);
    wipe(ROOT);
    dir("hub"); /* esiste e resta vuota fino a case_repo_completo */

    case_radice_assente();
    case_cache_vuoto();
    case_repo_completo();
    case_inode_distinti();
    case_refs_only();
    case_non_repo_contati();
    case_ref_annidato();
    case_ref_troncati_ma_visibili();
    case_nome_ambiguo();
    case_tipi();
    case_ordine_stabile();
    case_troppi();
    case_non_blob();
    case_uno_solo();
    case_repo_murato();
    case_pezzo_murato();
    case_reale();

    printf("test_hf_catalog: %d verifiche, %d fallimenti\n", checks, failures);
    return failures ? 1 : 0;
}
