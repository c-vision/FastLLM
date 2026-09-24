/* fmlx_hf_audit.c — l'integrità di UN repo del cache, letta e non toccata (`doctor`, piano §10.2).
 *
 * Perché un file suo e non un pezzo di `fmlx_hf_catalog.c`: il catalogo risponde a «COSA c'è»
 * (nomi, byte, età) e non legge mai UN byte di contenuto; qui la domanda è «ciò che c'è È quello
 * che promette», e per risponderla si hashano i blob. Sono due costi diversi di un ordine di
 * grandezza, e un `models` che hashasse non sarebbe più un elenco.
 *
 * La verifica non reinventa niente: passa da `fmlx_hf_locate` e `fmlx_hf_verify`, le STESSE
 * funzioni che usa il `pull` per decidere se un blob è buono. È una scelta, non pigrizia — è ciò
 * che rende il verdetto utile: se il `doctor` dice che uno snapshot è a posto, il `pull` lo
 * ritroverà a posto con lo stesso codice; se dice che l'hash non torna, `pull --repair` ha già il
 * permesso di sostituirlo. Un verificatore scritto a parte avrebbe potuto essere verde qui e
 * rosso lì, che è il modo migliore per non fidarsi più di nessuno dei due.
 *
 * Due nomi, due promesse (misurato, vedi l'intestazione di `fmlx_hf.h`): 64 hex = SHA256 del
 * contenuto, 40 hex = git blob id (SHA1 di `blob <len>\0` + contenuto, NON lo SHA1 nudo). Un nome
 * che non è hex non promette nulla: è contato `n_verifiable == 0` e diventa UNA nota sul repo —
 * «non ho potuto verificare niente» non deve mai assomigliare a «ho verificato tutto».
 *
 * Ciò che NON si fa qui: cancellare, riparare, toccare l'mtime, seguire symlink che escono dalla
 * forma `../../blobs/<nome>` (quello è `locate`, che lo rifiuta), o scrivere dove che non sia la
 * struct dell'appello. Un `doctor` che "sistema" è un `rm` che nessuno ha chiesto.
 */
#include "fmlx/fmlx_hf.h"
#include "fmlx_hf_internal.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define AUDIT_DEPTH_MAX 4 /* misurato: lo snapshot reale annida una directory (`coreml/`); oltre
                           * IL LIVELLO QUATTRO non si scende, e LO SI DICE (nota, non silenzio).
                           * Il conteggio parte dallo snapshot stesso, che È il livello 1: la
                           * quarta directory sotto di essa è la prima rifiutata: non è uno snapshot
                           * di Hugging Face, e scendere più a fondo per niente non è prudenza — è
                           * un loop sul disco di qualcun altro */

/* ---- un insieme di nomi, con crescita ----
 * Serve a DUE domande identiche («questo blob è richiamato da qualcuno?», «questo commit è
 * puntato da un ref?») e a nessuna delle due basta un ordinamento: i nomi arrivano da readdir.
 * La ricerca è lineare e il numero è piccolo (i blob di un repo sono decine, i commit unità):
 * un hash qui sarebbe una struttura da mantenere per niente. */
typedef struct {
    char **v;
    size_t n, cap;
    int failed; /* una malloc rifiutata NON deve sembrare "non referenziato": vedi sotto */
} NameSet;

static int ns_add(NameSet *s, const char *name) {
    size_t i;
    for (i = 0; i < s->n; i++)
        if (!strcmp(s->v[i], name)) return 1; /* già dentro: non è un errore */
    if (s->n == s->cap) {
        size_t want = s->cap ? s->cap * 2 : 32;
        char **grown = (char **)realloc(s->v, want * sizeof *grown);
        if (!grown) {
            s->failed = 1;
            return 0;
        }
        s->v = grown;
        s->cap = want;
    }
    s->v[s->n] = (char *)malloc(strlen(name) + 1);
    if (!s->v[s->n]) {
        s->failed = 1;
        return 0;
    }
    strcpy(s->v[s->n], name);
    s->n++;
    return 1;
}

static int ns_has(const NameSet *s, const char *name) {
    size_t i;
    for (i = 0; i < s->n; i++)
        if (!strcmp(s->v[i], name)) return 1;
    return 0;
}

static void ns_free(NameSet *s) {
    size_t i;
    for (i = 0; i < s->n; i++) free(s->v[i]);
    free(s->v);
    s->v = NULL;
    s->n = s->cap = 0;
}

/* ---- i verdetti ---- */

static void finding(FmlxHfAudit *a, const char *rev, const char *rel, FmlxHfStatus code,
                    int is_problem, const char *fmt, ...) {
    va_list ap;
    size_t idx;
    FmlxHfFinding *f;

    if (is_problem) a->n_problems++;
    else a->n_notes++;
    idx = a->n_problems + a->n_notes;
    if (idx > FMLX_HF_AUDIT_FINDING_MAX) return; /* contato, non trattenuto: i contatori restano
                                                  * il numero VERO, come `n_refs` nel catalogo */
    idx--;
    f = &a->findings[idx];
    f->code = code;
    f->is_problem = is_problem;
    snprintf(f->rel, sizeof f->rel, "%s", rel ? rel : "");
    snprintf(f->rev, sizeof f->rev, "%s", rev ? rev : "");
    va_start(ap, fmt);
    vsnprintf(f->msg, sizeof f->msg, fmt, ap);
    va_end(ap);
}

/* Un entry name che non sta in `rel` NON è un path da troncare e controllare: è un pezzo di cache
 * che non abbiamo guardato, e la cosa si NOMINA. Riempie `out` e dice se sta. */
static int rel_join(char *out, size_t n, const char *parent, const char *name) {
    if (snprintf(out, n, "%s%s%s", parent && *parent ? parent : "", parent && *parent ? "/" : "",
                 name) >= (int)n)
        return 0;
    return 1;
}

/* Sufisso, confrontato per INTERO. La prima versione guardava «gli ultimi 5 caratteri» per tutti e
 * due i casi: `.lock` (5) ci stava, `.incomplete` (11) no — e nessun mezzo-download sarebbe stato
 * riconosciuto come tale, contando al suo posto un blob orfano con il nome sporco. */
static int ends_with(const char *s, const char *suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    return ls >= lf && !strcmp(s + ls - lf, suf);
}

/* ---- uno snapshot, ricorsivo ---- */
static void walk_snapshot(FmlxHfAudit *a, const char *root, const char *repo_id, const char *rev,
                          const char *dir, const char *rel, int depth, NameSet *refs_blobs) {
    DIR *d = opendir(dir);
    if (!d) {
        finding(a, rev, rel, FMLX_HF_ERR_IO, 1, "opendir %s: %s", dir, strerror(errno));
        return;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        char p[FMLX_HF_PATH_MAX], r[FMLX_HF_AUDIT_REL_MAX];
        struct stat st;
        FmlxHfRef ref;
        FmlxHfStatus s;

        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        /* QUI i dotfile si tengono: `.gitattributes`, `.msc`, `.cache/` sono parti reali di uno
         * snapshot. Il catalogo salta i nomi con il punto nei `blobs/`, e qui la regola è opposta
         * perché là un punto-prefix è un residuo di tool, qua è un file del repo. */
        if (fmlx_hf_pjoin(p, sizeof p, dir, e->d_name) != FMLX_HF_OK) {
            finding(a, rev, rel, FMLX_HF_ERR_BUF_SMALL, 0, "il path di '%s' non sta in %d: non sceso",
                    e->d_name, FMLX_HF_PATH_MAX);
            continue;
        }
        if (!rel_join(r, sizeof r, rel, e->d_name)) {
            finding(a, rev, rel, FMLX_HF_ERR_BUF_SMALL, 0,
                    "il nome relativo '%s' non sta in %d: non controllato", e->d_name,
                    FMLX_HF_AUDIT_REL_MAX);
            continue;
        }
        if (lstat(p, &st) != 0) {
            finding(a, rev, r, FMLX_HF_ERR_IO, 1, "lstat %s: %s", p, strerror(errno));
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (depth >= AUDIT_DEPTH_MAX)
                finding(a, rev, r, FMLX_HF_ERR_NAME, 0,
                        "directory a profondità %d: non ispezionata oltre", AUDIT_DEPTH_MAX);
            else
                walk_snapshot(a, root, repo_id, rev, p, r, depth + 1, refs_blobs);
            continue;
        }
        if (!S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode)) {
            finding(a, rev, r, FMLX_HF_ERR_NAME, 0, "non è né un file né un symlink");
            continue;
        }
        a->n_files++;
        if (S_ISREG(st.st_mode)) a->n_plain++;

        /* È LA STESSA `locate` del download: se lei dice che il symlink è fuori forma, il `pull`
         * direbbe lo stesso. Un controllo scritto qui per conto suo avrebbe potuto essere verde su
         * un cache che il `pull` rifiuta. */
        s = fmlx_hf_locate(root, repo_id, rev, r, &ref);
        if (s != FMLX_HF_OK) {
            const char *m = fmlx_hf_last_error();
            finding(a, rev, r, s, 1, "%s%s%s", fmlx_hf_status_str(s), m && *m ? ": " : "",
                    m && *m ? m : "");
            continue;
        }
        if (ref.is_blob_link) {
            a->n_links++;
            if (!ns_add(refs_blobs, ref.blob_name) && refs_blobs->failed) {
                finding(a, rev, r, FMLX_HF_ERR_IO, 1, "memoria per l'insieme dei blob rifiutata");
                closedir(d);
                return;
            }
        }
        if (ref.hash_kind == FMLX_HF_HASH_NONE) continue; /* niente promessa: contato, non verificato */

        a->n_verifiable++;
        {
            int64_t hashed = 0;
            FmlxHfStatus v = fmlx_hf_verify(&ref, &hashed);
            if (v == FMLX_HF_OK) {
                a->n_verified++;
                a->bytes_hashed += hashed;
            } else if (v == FMLX_HF_ERR_NOT_VERIFIABLE) {
                a->n_verifiable--; /* la promessa c'era dal nome, ma il verificatore l'ha rifiutata */
                finding(a, rev, r, v, 0, "il nome promette un hash che questa lane non sa verificare");
            } else {
                const char *m = fmlx_hf_last_error();
                finding(a, rev, r, v, 1, "%s%s%s", fmlx_hf_status_str(v), m && *m ? ": " : "",
                        m && *m ? m : "");
            }
        }
    }
    closedir(d);
}

/* ---- le directory di servizio del repo ---- */

/* I marker `.no_exist/<rev>/<rel>`: file VUOTI che dicono «il remote non ha questo file». Non sono
 * dati e non sono un problema; sono un fatto che il `doctor` racconta (è così che si distingue un
 * file che mancherà da uno perso). */
static void count_noexist(const char *dir, int depth, size_t *n) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        char p[FMLX_HF_PATH_MAX * 2];
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p) continue;
        if (lstat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < AUDIT_DEPTH_MAX) count_noexist(p, depth + 1, n);
        } else if (S_ISREG(st.st_mode)) {
            (*n)++;
        }
    }
    closedir(d);
}

/* I `.lock` NON stanno dentro il repo: stanno in `<root>/.locks/<repo_folder_name>/` (misurato in
 * `file_download.py:1203` e confermato sul cache di questa macchina). Contarli qui serve a una
 * frase sola: «ci sono 5 lock residui», che è ciò che un utente guarda quando un `pull` sembra
 * bloccato. Sono normali, e per questo NON sono una nota. */
static void count_locks(const char *root, const char *repo_id, size_t *n) {
    char dn[FMLX_HF_CATALOG_NAME_MAX], dir[FMLX_HF_PATH_MAX * 2];
    DIR *d;
    struct dirent *e;

    if (fmlx_hf_repo_dirname(repo_id, dn, sizeof dn) != FMLX_HF_OK) return;
    if (snprintf(dir, sizeof dir, "%s/.locks/%s", root, dn) >= (int)sizeof dir) return;
    d = opendir(dir);
    if (!d) return;
    while ((e = readdir(d))) {
        char p[FMLX_HF_PATH_MAX * 3];
        struct stat st;
        size_t len = strlen(e->d_name);
        if (len < 5 || strcmp(e->d_name + len - 5, ".lock")) continue;
        if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p) continue;
        if (lstat(p, &st) == 0 && S_ISREG(st.st_mode)) (*n)++;
    }
    closedir(d);
}

/* ---- i blob che nessuno richiama ---- */
static void scan_orphans(FmlxHfAudit *a, const char *repo_dir, const NameSet *refs_blobs) {
    char dir[FMLX_HF_PATH_MAX * 2];
    DIR *d;
    struct dirent *e;

    if (snprintf(dir, sizeof dir, "%s/blobs", repo_dir) >= (int)sizeof dir) return;
    d = opendir(dir);
    if (!d) return;
    while ((e = readdir(d))) {
        char p[FMLX_HF_PATH_MAX * 3];
        struct stat st;
        if (ends_with(e->d_name, ".incomplete")) {
            if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) < (int)sizeof p &&
                lstat(p, &st) == 0 && S_ISREG(st.st_mode))
                a->n_partial++;
            continue;
        }
        if (e->d_name[0] == '.') continue;
        if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p) continue;
        if (lstat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (ns_has(refs_blobs, e->d_name)) continue;
        a->n_orphan_blobs++;
        a->bytes_orphan += (int64_t)st.st_size;
    }
    closedir(d);
}

/* ---- il giro ---- */

/* La stessa regola di `resolve_ref`: 40 esadecimali minuscoli. Non è pignoleria tipografica — una
 * directory di `snapshots/` che non è un commit NON è raggiungibile per revisione, quindi `locate`
 * la rifiuterebbe file per file e il doctor stamperebbe trenta "PROBLEMA" per un nome strano.
 * Un'anomalia di NOME è una riga, non una valanga: si dice una volta e si lascia stare. */
static int is_commit_dir(const char *s) {
    size_t n = strlen(s);
    return n == 40 && fmlx_hf_is_hex_lower(s, n);
}

FmlxHfStatus fmlx_hf_audit_repo(const char *cache_root, const char *repo_id, FmlxHfAudit *out) {
    char root[FMLX_HF_PATH_MAX], repo_dir[FMLX_HF_PATH_MAX], snaps[FMLX_HF_PATH_MAX * 2];
    FmlxHfRepo cat;
    FmlxHfStatus s;
    NameSet blobs, revs;
    DIR *d;
    size_t i;

    memset(out, 0, sizeof *out);
    memset(&blobs, 0, sizeof blobs);
    memset(&revs, 0, sizeof revs);
    if (!repo_id || !*repo_id) return fmlx_hf_fail(FMLX_HF_ERR_REPO_ID, "audit: repo id vuoto");

    if (!cache_root) {
        s = fmlx_hf_cache_root(root, sizeof root);
        if (s != FMLX_HF_OK) return s;
        cache_root = root;
    }
    s = fmlx_hf_repo_dir(cache_root, repo_id, repo_dir, sizeof repo_dir);
    if (s != FMLX_HF_OK) return s;
    {
        struct stat st;
        if (lstat(repo_dir, &st) != 0) {
            /* La RADICE prima del repo: `<root>/models--…` non esiste sia quando il repo non è mai
             * stato scaricato sia quando `HF_HOME` punta nel posto sbagliato, e i due casi chiedono
             * due consigli diversi («scaricalo» / «la radice è un'altra»). */
            struct stat rst;
            if (lstat(cache_root, &rst) != 0)
                return fmlx_hf_fail(FMLX_HF_ERR_NO_CACHE, "audit: la radice %s non esiste",
                                    cache_root);
            return fmlx_hf_fail(FMLX_HF_ERR_NO_REPO, "audit: %s non esiste nel cache", repo_dir);
        }
        if (!S_ISDIR(st.st_mode))
            return fmlx_hf_fail(FMLX_HF_ERR_NO_REPO, "audit: %s non è una directory", repo_dir);
    }
    snprintf(out->repo_id, sizeof out->repo_id, "%s", repo_id);

    /* Il giro dei ref con i conteggi già fatti: si riusa il catalogo invece di riscrivere le sue
     * regole (qual è un blob, quanto pesa, quanti ref esistono DAVVERO). Un `doctor` con le sue
     * regole di conteggio divergerebbe da `models` sullo stesso disco. */
    s = fmlx_hf_catalog_repo(cache_root, repo_id, &cat);
    if (s != FMLX_HF_OK) return s;
    out->n_refs = cat.n_refs;
    out->n_blobs = cat.n_blobs;
    if (cat.n_refs > FMLX_HF_CATALOG_REFS_MAX)
        finding(out, "", "refs", FMLX_HF_ERR_TOO_MANY, 0, "%zu ref, i primi %d controllati",
                cat.n_refs, FMLX_HF_CATALOG_REFS_MAX);

    /* 1) ref -> commit, e il commit DEVE avere lo snapshot. È il caso misurato di
     * `Mixtral-8x22B-4bit`: `refs/` senza `blobs/`, un puntamento e non un modello. */
    for (i = 0; i < cat.n_refs && i < FMLX_HF_CATALOG_REFS_MAX; i++) {
        char sha[64] = "", sp[FMLX_HF_PATH_MAX * 2];
        struct stat st;
        if (!cat.refs[i][0]) continue;
        s = fmlx_hf_resolve_ref(repo_dir, cat.refs[i], sha, sizeof sha);
        if (s != FMLX_HF_OK) {
            const char *m = fmlx_hf_last_error();
            char what[FMLX_HF_AUDIT_REL_MAX];
            rel_join(what, sizeof what, "refs", cat.refs[i]);
            finding(out, cat.refs[i], what, s, 1, "%s%s%s", fmlx_hf_status_str(s), m && *m ? ": " : "",
                    m && *m ? m : "");
            continue;
        }
        ns_add(&revs, sha);
        if (fmlx_hf_pjoin(sp, sizeof sp, repo_dir, "snapshots") != FMLX_HF_OK) continue;
        {
            char full[FMLX_HF_PATH_MAX * 3];
            if (snprintf(full, sizeof full, "%s/%s", sp, sha) >= (int)sizeof full) continue;
            if (lstat(full, &st) == 0 && S_ISDIR(st.st_mode)) continue;
        }
        finding(out, cat.refs[i], sha, FMLX_HF_ERR_SNAPSHOT_MISSING, 0,
                "il ref '%s' punta %s, che non è tra gli snapshot", cat.refs[i], sha);
    }

    /* 2) gli snapshot presenti, uno per uno. */
    if (fmlx_hf_pjoin(snaps, sizeof snaps, repo_dir, "snapshots") != FMLX_HF_OK) {
        s = FMLX_HF_ERR_BUF_SMALL;
        goto done;
    }
    d = opendir(snaps);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            char full[FMLX_HF_PATH_MAX * 2], rel[FMLX_HF_AUDIT_REL_MAX];
            struct stat st;
            if (e->d_name[0] == '.') continue;
            if (snprintf(full, sizeof full, "%s/%s", snaps, e->d_name) >= (int)sizeof full) continue;
            if (lstat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
            out->n_snaps++;
            rel_join(rel, sizeof rel, "snapshots", e->d_name);
            if (!is_commit_dir(e->d_name)) {
                finding(out, e->d_name, rel, FMLX_HF_ERR_REVISION, 0,
                        "non è un commit a 40 esadecimali minuscoli: non ispezionata");
                continue;
            }
            if (!ns_has(&revs, e->d_name))
                finding(out, e->d_name, rel, FMLX_HF_ERR_REF_MISSING, 0,
                        "snapshot non puntato da nessun ref: nessuno lo raggiunge per nome");
            walk_snapshot(out, cache_root, repo_id, e->d_name, full, "", 1, &blobs);
        }
        closedir(d);
    }
    /* Nessuna `snapshots/` NON è un errore: un repo con solo `refs/` (o solo `blobs/`) è il caso
     * reale di chi ha puntato un commit senza scaricarlo. Lo dicono i contatori, e la riga finale
     * del `doctor` lo nomina. */

    /* 3) i blob che nessuno richiama, e i mezzo-download. */
    scan_orphans(out, repo_dir, &blobs);
    if (blobs.failed) {
        s = fmlx_hf_fail(FMLX_HF_ERR_IO, "audit: memoria per l'insieme dei blob rifiutata");
        goto done;
    }
    if (out->n_orphan_blobs)
        finding(out, "", "blobs", FMLX_HF_OK, 0, "%zu blob non richiamati da alcuno snapshot",
                out->n_orphan_blobs);
    if (out->n_partial)
        finding(out, "", "blobs", FMLX_HF_ERR_STALL, 0,
                "%zu .incomplete: un download si è fermato a metà (si riprende da lì)",
                out->n_partial);
    if (out->n_files && !out->n_verifiable)
        finding(out, "", "snapshots", FMLX_HF_ERR_NOT_VERIFIABLE, 0,
                "%zu file, nessuno con un nome che promette un hash: NULLA di verificabile qui",
                out->n_files);

    {
        char nx[FMLX_HF_PATH_MAX * 2];
        if (snprintf(nx, sizeof nx, "%s/.no_exist", repo_dir) < (int)sizeof nx)
            count_noexist(nx, 1, &out->n_noexist);
    }
    count_locks(cache_root, repo_id, &out->n_lock);

    s = FMLX_HF_OK;
done:
    ns_free(&blobs);
    ns_free(&revs);
    return s;
}
