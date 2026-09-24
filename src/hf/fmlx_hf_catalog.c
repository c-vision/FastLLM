/* fmlx_hf_catalog.c — cosa c'è già sul disco, SENZA rete (piano §10.2, `models`/`list`).
 *
 * Perché un file suo e non un pezzo di `fmlx_hf_cache.c`: la cache lane risponde a "DOVE sono i
 * byte di questo file", qui la domanda è "QUALI repo ci sono, quanto occupano, e quali di essi non
 * sono un modello". Sono due letture del disco diverse, e la seconda ha le sue trappole — trovate
 * tutte MISURANDO il cache reale di questa macchina (34 directory su due radici), non immaginandole:
 *
 *  - `models--mlx-community--Mixtral-8x22B-4bit` ha SOLO `refs/`: nessuna `blobs/`, nessuna
 *    `snapshots/`. Un elenco che la mostrasse da 0 byte direbbe "occupa niente" di una directory che
 *    NON contiene dati; `refs_only` è il modo di dirlo con una parola;
 *  - `models--bert-base-uncased` ha una `trees/` (huggingface_hub >= 1.31, `_tree_cache.py:19-31`) e
 *    directory REALI dentro lo snapshot (`coreml/`): `trees/` non è un blob da contare, e il
 *    ricorsivo dove serve (i ref `pr/N`) non è paranoia;
 *  - la radice contiene `version.txt` e `CACHEDIR.TAG`, e il cache xet aggiunge `xet/`: "non è un
 *    repo" è una informazione, non un silenzio — da cui `n_ignored_out`.
 *
 * L'INGOMBRO è dei blob, non degli symlink. Uno snapshot è un fascio di link a `../../blobs/<nome>`:
 * sommare gli entry-point conterebbe una volta il link e una il bersaglio, e un repo con due
 * revisioni che condividono metà dei pesi risulterebbe il doppio di quello che pesa. Si contano gli
 * INODE distinti di `blobs/`: due hardlink allo stesso blob sono un blob.
 */
#include "fmlx/fmlx_hf.h"
#include "fmlx_hf_internal.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const struct {
    const char *prefix;
    const char *kind;
} KINDS[] = {{"models--", "model"}, {"datasets--", "dataset"}, {"spaces--", "space"}};
#define KINDS_N (sizeof KINDS / sizeof KINDS[0])

/* ---- refs/ ---- */

/* «La directory è un repo, ma dentro non ci abbiamo guardato»: si SEGNA e si continua. La prima
 * directory che rifiuta è quella che si nomina (un `refs/` murato è già tutta la bugia che serve
 * dire), e l'errno è QUELLO dell'`opendir`, non uno riassunto dal chiamante. */
static void mark_unreadable(FmlxHfRepo *o, const char *part, int err) {
    if (!o || o->unreadable) return;
    o->unreadable = 1;
    o->unread_errno = err;
    snprintf(o->unread_part, sizeof o->unread_part, "%s", part);
}

/* È RECURSIVO perché una revisione `refs/pr/3` diventa una SOTTODIRECTORY (HF scrive
 * `os.path.join(refs_dir, revision)`): il nome della revisione è il path relativo, non l'ultimo
 * segmento. `*n_true` conta TUTTI i ref trovati anche quando `shown` ha finito lo spazio: la
 * truncation si VEE, non si suppone. `rep` è passato SOLO dal livello 0: un `refs/pr/` che non apre
 * è un ref in meno, non un repo illeggibile — la domanda a cui deve rispondere è «QUESTO repo l'ho
 * potuto guardare?», e la risposta la dà la directory di primo livello. */
static void scan_refs(const char *dir, const char *rel,
                      char out[FMLX_HF_CATALOG_REFS_MAX][FMLX_HF_CATALOG_REF_MAX], size_t *n_true,
                      size_t *shown, int depth, FmlxHfRepo *rep) {
    DIR *d = opendir(dir);
    if (!d) {
        if (rep && depth == 0 && errno != ENOENT) mark_unreadable(rep, "refs", errno);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[FMLX_HF_PATH_MAX * 2], name[FMLX_HF_CATALOG_REF_MAX];
        if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p) continue;
        if (rel && *rel) {
            if (snprintf(name, sizeof name, "%s/%s", rel, e->d_name) >= (int)sizeof name) {
                (*n_true)++; /* esiste, non sta nel nome: contato e non mostrato */
                continue;
            }
        } else {
            snprintf(name, sizeof name, "%s", e->d_name);
        }

        struct stat st;
        if (lstat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 4) scan_refs(p, name, out, n_true, shown, depth + 1, NULL);
            continue;
        }
        (*n_true)++;
        if (*shown < FMLX_HF_CATALOG_REFS_MAX) {
            snprintf(out[*shown], FMLX_HF_CATALOG_REF_MAX, "%s", name);
            (*shown)++;
        }
    }
    closedir(d);
}

/* ---- blobs/ ---- */

typedef struct {
    dev_t dev;
    ino_t ino;
} Inode;

static int inode_seen(const Inode *v, size_t n, dev_t dev, ino_t ino) {
    for (size_t i = 0; i < n; i++)
        if (v[i].dev == dev && v[i].ino == ino) return 1;
    return 0;
}

/* Un SOLO livello, di proposito: i blob sono piatti (misurato su 34 repo) e ciò che potesse
 * annidarsi NON è un blob nostro — sarebbe il deposito di un altro tool, e contarlo come nostro
 * vorrebbe dire attribuire a FastMLX byte che FastMLX non ha scritto. */
static FmlxHfStatus scan_blobs(const char *dir, FmlxHfRepo *out) {
    DIR *d = opendir(dir);
    if (!d) {
        /* `blobs/` che NON ESISTE è legale (i repo con solo `refs/`, misurati su 13 directory del
         * cache vero); `blobs/` che non APRE no, e la differenza è tutta lì: nel primo caso i byte
         * sono zero perché non c'è niente, nel secondo sono zero perché non abbiamo guardato. */
        if (errno != ENOENT) {
            mark_unreadable(out, "blobs", errno);
            return fmlx_hf_fail(FMLX_HF_ERR_IO, "open %s: %s", dir, strerror(errno));
        }
        return FMLX_HF_OK;
    }
    size_t cap = 64, n = 0;
    Inode *v = (Inode *)malloc(cap * sizeof *v);
    if (!v) { closedir(d); return fmlx_hf_fail(FMLX_HF_ERR_IO, "malloc del dedupe inode rifiutata"); }

    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue; /* .DS_Store e simili non sono dati di modello */
        char p[FMLX_HF_PATH_MAX * 2];
        if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p) continue;
        struct stat st;
        if (lstat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (n == cap) {
            Inode *grown = (Inode *)realloc(v, (cap * 2) * sizeof *v);
            if (!grown) {
                free(v);
                closedir(d);
                return fmlx_hf_fail(FMLX_HF_ERR_IO, "realloc del dedupe inode rifiutata");
            }
            v = grown;
            cap *= 2;
        }
        if (inode_seen(v, n, st.st_dev, st.st_ino)) continue; /* altro nome, stessi byte */
        v[n].dev = st.st_dev;
        v[n].ino = st.st_ino;
        n++;
        out->n_blobs++;
        out->bytes += (int64_t)st.st_size;
        if ((int64_t)st.st_mtime > out->mtime) out->mtime = (int64_t)st.st_mtime;
    }
    free(v);
    closedir(d);
    return FMLX_HF_OK;
}

/* ---- una directory di repo -> FmlxHfRepo ---- */

/* NULL = "questa directory NON è un repo" (`xet/`, `version.txt`, un `hub/` annidato): un dato di
 * fatto, non un codice d'errore — la lista deve saltarla CONTANDOILA, non fingere che non ci fosse. */
static const char *dirname_kind(const char *dirname, size_t *plen_out) {
    for (size_t k = 0; k < KINDS_N; k++) {
        if (strncmp(dirname, KINDS[k].prefix, strlen(KINDS[k].prefix)) == 0) {
            if (plen_out) *plen_out = strlen(KINDS[k].prefix);
            return KINDS[k].kind;
        }
    }
    return NULL;
}

/* La regola di `huggingface_hub`, copia 1.31.0 `utils/_cache_manager.py:742-744`: il tipo è tutto ciò
 * che precede il PRIMO "--", e il resto dell'id è ogni "--" trasformato in "/". È PERDENTE per
 * costruzione (un repo il cui nome contiene "--" arriva a un id con un "/" in più): `ambiguous_name`
 * esiste per dire quello, non per nasconderlo. */
static FmlxHfStatus parse_dirname(const char *dirname, FmlxHfRepo *out) {
    size_t plen = 0;
    const char *kind = dirname_kind(dirname, &plen);
    if (!kind) return fmlx_hf_fail(FMLX_HF_ERR_NAME, "'%s': prefisso di tipo sconosciuto", dirname);
    snprintf(out->kind, sizeof out->kind, "%s", kind);

    const char *rest = dirname + plen;
    size_t at = 0, dashes = 0;
    for (size_t i = 0; rest[i];) {
        if (rest[i] == '-' && rest[i + 1] == '-') {
            if (at + 1 >= sizeof out->repo_id) return FMLX_HF_ERR_BUF_SMALL;
            out->repo_id[at++] = '/';
            i += 2; /* la COPPIA è un "/": se si cadesse qui sotto, il secondo "-" verrebbe anche
                     * copiato e `a--b` diventerebbe `a/-b` — l'errore che il primo `case_nome_ambiguo`
                     * ha misurato su 30 asserzioni. */
            dashes++;
            continue;
        }
        if (at + 1 >= sizeof out->repo_id) return FMLX_HF_ERR_BUF_SMALL;
        out->repo_id[at++] = rest[i];
        i++;
    }
    out->repo_id[at] = '\0';
    out->ambiguous_name = dashes >= 2 ? 1 : 0;
    return FMLX_HF_OK;
}

static FmlxHfStatus scan_repo_dir(const char *root, const char *dirname, FmlxHfRepo *out) {
    char dir[FMLX_HF_PATH_MAX * 2], sub[FMLX_HF_PATH_MAX * 2];
    /* AZZERATO prima di qualunque ritorno: `unreadable` si legge anche sul fallimento (è così che il
     * giro decide se nominare o ignorare), e un byte di inizializzazione letto al suo posto sarebbe
     * il campo più bugiardo del struct — direbbe «illeggibile» di un repo che non abbiamo mai
     * toccato, o «a posto» di uno che non abbiamo potuto aprire. */
    memset(out, 0, sizeof *out);
    snprintf(out->dirname, sizeof out->dirname, "%s", dirname);
    if (snprintf(dir, sizeof dir, "%s/%s", root, dirname) >= (int)sizeof dir) {
        mark_unreadable(out, "dir", ENAMETOOLONG);
        return fmlx_hf_fail(FMLX_HF_ERR_BUF_SMALL, "path '%s/%s' non sta nel buffer", root, dirname);
    }

    FmlxHfStatus s = parse_dirname(dirname, out);
    if (s != FMLX_HF_OK) return s; /* il NOME non è un repo: lo conta come `ignored`, non come danno */

    /* LA DIRECTORY STESSA, prima dei suoi pezzi. Un `chmod 000` (o un `sudo huggingface-cli` che ha
     * creato la directory `root:700`) fa fallire anche l'apertura di `refs/`, e il nome che saltava
     * fuori era `"refs"`: una directory murata descritta come un `refs/` rotto, cioè un consiglio
     * sbagliato dato con la faccia giusta. `unread_part` esiste per dire QUALE, quindi qui si chiede
     * alla directory il suo errno PRIMA di dedurlo dai figli. */
    {
        DIR *rd = opendir(dir);
        if (!rd) {
            if (errno != ENOENT) { /* un repo senza directory è già contato altrove, non è un danno */
                mark_unreadable(out, "dir", errno);
                return fmlx_hf_fail(FMLX_HF_ERR_IO, "open %s: %s", dir, strerror(errno));
            }
        } else
            closedir(rd);
    }

    size_t shown = 0;
    if (snprintf(sub, sizeof sub, "%s/refs", dir) >= (int)sizeof sub) {
        mark_unreadable(out, "dir", ENAMETOOLONG);
        return FMLX_HF_ERR_BUF_SMALL;
    }
    scan_refs(sub, "", out->refs, &out->n_refs, &shown, 0, out);

    if (snprintf(sub, sizeof sub, "%s/blobs", dir) >= (int)sizeof sub) {
        mark_unreadable(out, "dir", ENAMETOOLONG);
        return FMLX_HF_ERR_BUF_SMALL;
    }
    s = scan_blobs(sub, out);
    if (s != FMLX_HF_OK) return s;
    /* NON `(&& !out->unreadable)`: sembrava la guardia giusta e invece è codice morto, e un
     * mutante che lo toglie non cambia NESSUNA asserzione. `unreadable` si può aver preso qui solo
     * da `scan_refs` sul livello 0 — che segna quando NON apre, e quindi non ha contato nessun ref
     * — e `n_refs > 0` è già falso prima di lui. Su un `blobs/` che non apre non si arriva nemmeno
     * qui: è il `return` di sopra. Ciò che impedisce a un repo murato di dirsi «solo ref» è quella
     * catena di ritorni, non un AND in più che nessuno può vedere all'opera. */
    out->refs_only = (out->n_refs > 0 && out->n_blobs == 0) ? 1 : 0;
    return FMLX_HF_OK;
}

static int repo_cmp(const void *a, const void *b) {
    return strcmp(((const FmlxHfRepo *)a)->repo_id, ((const FmlxHfRepo *)b)->repo_id);
}

/* ---- la superficie ---- */

static FmlxHfStatus resolve_root(const char *cache_root, char *root, size_t n) {
    if (cache_root && *cache_root) {
        if (strlen(cache_root) >= n) return FMLX_HF_ERR_BUF_SMALL;
        snprintf(root, n, "%s", cache_root);
        return FMLX_HF_OK;
    }
    return fmlx_hf_cache_root(root, n);
}

FmlxHfStatus fmlx_hf_catalog(const char *cache_root, FmlxHfRepo *out, size_t cap, size_t *n_out,
                             size_t *n_ignored_out) {
    char root[FMLX_HF_PATH_MAX];
    if (n_out) *n_out = 0;
    if (n_ignored_out) *n_ignored_out = 0;
    if (!out || cap == 0) return fmlx_hf_fail(FMLX_HF_ERR_IO, "catalogo senza array dove scrivere");

    FmlxHfStatus s = resolve_root(cache_root, root, sizeof root);
    if (s != FMLX_HF_OK) return s;

    DIR *d = opendir(root);
    if (!d) {
        /* Un cache che non esiste NON è "cache vuoto": è il segnale che HF_HOME punta nel posto
         * sbagliato, e stampare "nessun modello" con la faccia di chi ha finito il lavoro è il
         * risultato silenzioso che il piano vieta.
         *
         * L'errno decide la FRASE, non il codice: `EACCES` sulla radice non è «non esiste», e chi
         * legge «non esiste» su un cache che invece c'è e non apre ricontrolla il path invece dei
         * permessi. È fatale uguale — non c'è nessuna entry da salvare — ma è un'altra cosa. */
        return fmlx_hf_fail(FMLX_HF_ERR_NO_CACHE, "il cache '%s' %s: %s", root,
                            errno == ENOENT    ? "non esiste"
                            : errno == ENOTDIR ? "non è una directory"
                                               : "non si è potuto aprire",
                            strerror(errno));
    }

    size_t n = 0, ignored = 0;
    int too_many = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[FMLX_HF_PATH_MAX * 2];
        if (snprintf(p, sizeof p, "%s/%s", root, e->d_name) >= (int)sizeof p) { ignored++; continue; }
        struct stat st;
        if (lstat(p, &st) != 0 || !S_ISDIR(st.st_mode)) { ignored++; continue; }
        if (!dirname_kind(e->d_name, NULL)) { ignored++; continue; } /* `xet/`, `version.txt`, `hub/` */
        FmlxHfRepo one;
        s = scan_repo_dir(root, e->d_name, &one);
        if (s != FMLX_HF_OK) {
            /* DUE fallimenti che sembrano uno, e non lo sono. «Non ho potuto guardare DENTRO» non è
             * «non è un repo»: se il nome è stato letto, l'entry ENTRA nell'elenco con
             * `unreadable = 1` — un `chmod 000` su UNA directory non deve cancellare la lista delle
             * altre ventidue, che è il difetto che `models` e il `doctor` avevano. Se invece non
             * abbiamo nemmeno un `repo_id` (un prefisso che non è un tipo, un path che non sta nel
             * buffer prima ancora di partire) non c'è niente da NOMINARE: si conta fra le cose che
             * non riconosco, che è ciò che `n_ignored_out` è sempre stato. */
            if (one.unreadable && one.repo_id[0]) s = FMLX_HF_OK;
            else {
                ignored++;
                continue;
            }
        }
        if (n >= cap) {
            too_many = 1;
            n++;
            continue;
        }
        out[n++] = one;
    }
    closedir(d);
    if (n_out) *n_out = n;
    if (n_ignored_out) *n_ignored_out = ignored;
    if (too_many)
        return fmlx_hf_fail(FMLX_HF_ERR_TOO_MANY, "il cache ha %zu repo, l'array ne tiene %zu", n, cap);
    qsort(out, n, sizeof *out, repo_cmp);
    return FMLX_HF_OK;
}

FmlxHfStatus fmlx_hf_catalog_repo(const char *cache_root, const char *repo_id, FmlxHfRepo *out) {
    char root[FMLX_HF_PATH_MAX], dir[FMLX_HF_PATH_MAX], base[FMLX_HF_CATALOG_NAME_MAX];
    if (out) memset(out, 0, sizeof *out);
    if (!repo_id || !*repo_id) return fmlx_hf_fail(FMLX_HF_ERR_REPO_ID, "repo id vuoto");
    if (!out) return fmlx_hf_fail(FMLX_HF_ERR_IO, "catalogo senza dove scrivere");

    FmlxHfStatus s = resolve_root(cache_root, root, sizeof root);
    if (s != FMLX_HF_OK) return s;
    /* Un solo posto dove la regola "id -> nome di directory" vive: `fmlx_hf_repo_dir`. */
    s = fmlx_hf_repo_dir(root, repo_id, dir, sizeof dir);
    if (s != FMLX_HF_OK) return s;

    const char *slash = strrchr(dir, '/');
    if (!slash) return fmlx_hf_fail(FMLX_HF_ERR_IO, "path del repo senza directory padre: %s", dir);
    if (snprintf(base, sizeof base, "%s", slash + 1) >= (int)sizeof base)
        return fmlx_hf_fail(FMLX_HF_ERR_BUF_SMALL, "nome di directory '%s' non sta in %zu byte",
                            slash + 1, sizeof base);
    DIR *d = opendir(dir);
    if (!d) {
        /* DUE cose che sembrano una, e il codice d'uscita del `doctor` dipende da quale: `ENOENT` è
         * «quel repo non c'è» (il consiglio è `pull`), ogni ALTRO errno è «c'è, ma non l'ho potuto
         * aprire» (il consiglio è guardarci i permessi). Restituire `ERR_NO_REPO` anche per
         * `EACCES` — com'era — voleva dire chiamare «non scaricato» un modello che l'utente HA
         * scaricato, e un `pull --repair` che legge quel codice avrebbe ricominciato a scrivere byte
         * perfetti su una directory che era solo murata.
         *
         * E il `out` cosa dice su quel fallimento? La STESSA cosa del codice, non un repo perfetto:
         * il struct è già azzerato, e lasciarlo così vorrebbe dire consegnare a chi lo leggesse un
         * «0 byte, 0 blob, `unreadable = 0`» — l'identikit di un repo vuoto. Una lettura sola che
         * porta la risposta nel solo codice d'uscita è legittima; un struct che mente no. */
        int missing = (errno == ENOENT);
        if (!missing) {
            snprintf(out->dirname, sizeof out->dirname, "%s", base);
            mark_unreadable(out, "dir", errno);
        }
        return fmlx_hf_fail(missing ? FMLX_HF_ERR_NO_REPO : FMLX_HF_ERR_IO,
                            missing ? "%s non è nel cache (%s): %s" : "%s non si è potuto aprire (%s): %s",
                            repo_id, dir, strerror(errno));
    }
    closedir(d);
    return scan_repo_dir(root, base, out);
}
