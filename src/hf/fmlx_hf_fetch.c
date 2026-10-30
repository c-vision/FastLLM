/* fmlx_hf_fetch.c — la parte di cache del download: nessuna rete, nessun curl.
 *
 * Tutto ciò che decide DOVE finiscono i byte è qui, e per questo è provabile senza rete dal
 * test con un transport finto (tests/hf/test_hf_fetch.c). Il trasporto reale è in fmlx_hf_curl.c
 * e non contiene decisioni sul cache.
 *
 * Ordine dei passi, che è la sostanza del file:
 *   locate → verify → (colpo di fortuna: è già tutto a posto)
 *   → offline? 404? → meta → lock → temp → GET → verify del temp → rename → symlink → locate
 * La verifica del temporaneo PRIMA del rename è ciò che rende impossibile trovare nel cache un
 * blob col nome giusto e i byte sbagliati: il nome compare solo quando i byte hanno già passato
 * l'hash che quel nome promette.
 */
#include "fmlx/fmlx_fetch.h"
#include "fmlx_hf_internal.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

void fmlx_hf_normalize_etag(const char *raw, char *out, size_t out_len) {
    if (out_len == 0) return;
    out[0] = '\0';
    if (!raw) return;
    /* Esattamente `etag.lstrip("W/").strip('"')` di huggingface_hub (`_normalize_etag`, file_download.py:596).
     * lstrip in Python elimina QUALSIASI carattere iniziale dell'insieme {W,/}, non il prefisso
     * "W/", e strip('"') ne elimina QUALSIASI numero alle due estremità, non uno: la copiamo tale
     * e quale perché il nome del blob deve coincidere con quello che scriverebbe huggingface_hub,
     * o lo stesso cache avrebbe due file per lo stesso etag. */
    while (*raw == 'W' || *raw == '/') raw++;
    while (*raw == '"') raw++;
    size_t len = strlen(raw);
    while (len && raw[len - 1] == '"') len--;
    if (len >= out_len) len = out_len - 1;
    memcpy(out, raw, len);
    out[len] = '\0';
}

/* `fmlx_hf_mkdir_p` e `fmlx_hf_ensure_parent` vivevano qui: sono regole di path senza rete, e ora
 * stanno in `fmlx_hf_cache.c` insieme a `fmlx_hf_pjoin` — così un test che non tocca la rete può
 * costruirsi un cache finto senza linkarsi libcurl. Questa lane le chiama comunque, da header. */

static void sleep_ms(int ms) {
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    select(0, NULL, NULL, NULL, &tv);
}

static FmlxHfStatus write_no_exist_marker(const char *repo, const char *sha, const char *relpath) {
    char path[FMLX_HF_PATH_MAX], full[FMLX_HF_PATH_MAX];
    if (fmlx_hf_pjoin(path, sizeof path, repo, ".no_exist") != FMLX_HF_OK) return FMLX_HF_ERR_BUF_SMALL;
    if (fmlx_hf_pjoin(path, sizeof path, path, sha) != FMLX_HF_OK) return FMLX_HF_ERR_BUF_SMALL;
    if (fmlx_hf_pjoin(full, sizeof full, path, relpath) != FMLX_HF_OK) return FMLX_HF_ERR_BUF_SMALL;
    if (fmlx_hf_ensure_parent(full) != 0)
        return fmlx_hf_fail(FMLX_HF_ERR_IO, "mkdir per %s: %s", full, strerror(errno));
    int fd = open(full, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) return fmlx_hf_fail(FMLX_HF_ERR_IO, "touch %s: %s", full, strerror(errno));
    close(fd);
    return FMLX_HF_OK;
}

/* Crea il symlink di uno snapshot: `relpath` con `d` barre richiede (2+d) "../". */
static FmlxHfStatus make_snapshot_link(const char *repo, const char *sha, const char *relpath,
                                       const char *blob_name) {
    char entry[FMLX_HF_PATH_MAX], full[FMLX_HF_PATH_MAX], link[FMLX_HF_PATH_MAX];
    if (fmlx_hf_pjoin(entry, sizeof entry, repo, "snapshots") != FMLX_HF_OK) return FMLX_HF_ERR_BUF_SMALL;
    if (fmlx_hf_pjoin(entry, sizeof entry, entry, sha) != FMLX_HF_OK) return FMLX_HF_ERR_BUF_SMALL;
    if (fmlx_hf_pjoin(full, sizeof full, entry, relpath) != FMLX_HF_OK) return FMLX_HF_ERR_BUF_SMALL;

    struct stat st;
    if (lstat(full, &st) == 0) {
        if (!S_ISLNK(st.st_mode))
            /* Un file regolare nello snapshot appartiene a chi l'ha messo lì (un altro tool):
             * sono gli unici byte di quel file che esistano. NON si cancella, mai. */
            return fmlx_hf_fail(FMLX_HF_ERR_IO, "%s e' gia' un file regolare: non lo sostituisco", full);
        if (unlink(full) != 0) /* symlink pendente o vecchio: è un puntatore, non dati */
            return fmlx_hf_fail(FMLX_HF_ERR_IO, "unlink %s: %s", full, strerror(errno));
    }
    if (fmlx_hf_ensure_parent(full) != 0)
        return fmlx_hf_fail(FMLX_HF_ERR_IO, "mkdir per %s: %s", full, strerror(errno));

    size_t depth = 2;
    for (const char *c = relpath; *c; c++)
        if (*c == '/') depth++;
    size_t o = 0;
    for (size_t i = 0; i < depth; i++) o += (size_t)snprintf(link + o, sizeof link - o, "../");
    o += (size_t)snprintf(link + o, sizeof link - o, "blobs/%s", blob_name);
    if (o >= sizeof link) return fmlx_hf_fail(FMLX_HF_ERR_BUF_SMALL, "target del symlink troppo lungo");
    if (symlink(link, full) != 0)
        return fmlx_hf_fail(FMLX_HF_ERR_IO, "symlink %s -> %s: %s", full, link, strerror(errno));
    return FMLX_HF_OK;
}

static int64_t file_size(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (int64_t)st.st_size : -1;
}

/* Confronta il NETLOC di due URL, senza distinguere maiuscole/minuscole (RFC 3986: l'host non le
 * distingue). È la decisione da cui dipende se mandarci il token: huggingface_hub fa lo stesso
 * confronto (`urlparse(url).netloc != urlparse(metadata.location).netloc`, :1764) e se divergono
 * TOGLIE l'authorization, perché il secondo host è un CDN con URL firmato. Una forma inattesa
 * (manca "://") conta come DIVERSA: la scelta prudente è non spedire la credenziale. */
static int same_authority(const char *a, const char *b) {
    const char *sa = strstr(a, "://"), *sb = strstr(b, "://");
    if (!sa || !sb) return 0;
    sa += 3; sb += 3;
    const char *ea = sa, *eb = sb;
    while (*ea && *ea != '/' && *ea != '?' && *ea != '#') ea++;
    while (*eb && *eb != '/' && *eb != '?' && *eb != '#') eb++;
    size_t la = (size_t)(ea - sa), lb = (size_t)(eb - sb);
    if (la != lb) return 0;
    for (size_t i = 0; i < la; i++)
        if (tolower((unsigned char)sa[i]) != tolower((unsigned char)sb[i])) return 0;
    return 1;
}

/* refs/<revision> = commit, scritto DOPO che i byte sono a posto. Non si tocca se dice già la
 * stessa cosa (stessa ragione di file_download.py:773: un cache in sola lettura non deve
 * diventare un errore). Il fallimento non è un fallimento del download, e qui non lo si finge:
 * la funzione non ritorna nulla perché il suo esito non cambia quello del download. */
static void write_ref_if_new(const char *repo, const char *revision, const char *sha) {
    char path[FMLX_HF_PATH_MAX], cur[FMLX_HF_PATH_MAX];
    FILE *f;
    size_t n;

    if (strcmp(revision, sha) == 0) return;          /* la revisione ERA già il commit */
    if (!fmlx_hf_name_is_safe(revision)) return;     /* non è un nome di ref scrivibile */
    if (fmlx_hf_pjoin(path, sizeof path, repo, "refs") != FMLX_HF_OK) return;
    if (fmlx_hf_pjoin(path, sizeof path, path, revision) != FMLX_HF_OK) return;

    f = fopen(path, "rb");
    if (f) {
        n = fread(cur, 1, sizeof cur - 1, f);
        cur[n] = '\0';
        fclose(f);
        if (strcmp(cur, sha) == 0) return;           /* già a posto */
    }
    if (fmlx_hf_ensure_parent(path) != 0) return;
    f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "%s", sha);
    fclose(f);
}

/* Il `pull` ha il commit da un'altra parte (il manifesto, non la HEAD di un file), quindi il
 * ref non lo scrive la lane download: `fetch_one` salta il passo 9 quando la revisione requestata
 * ERA già il commit. Questa è la stessa regola, esportata — non una seconda scrittura del ref,
 * perché due copie avrebbero potuto divergere su quando NON toccare un cache in sola lettura.
 * Ritorna void per lo stesso motivo di `write_ref_if_new`: un ref non scritto non rende falso un
 * download i cui byte sono già verificati e raggiungibili. */
void fmlx_hf_record_ref(const char *cache_root, const char *repo_id, const char *revision,
                        const char *commit) {
    char root[FMLX_HF_PATH_MAX], repo[FMLX_HF_PATH_MAX];

    if (!revision || !commit) return;
    if (cache_root) snprintf(root, sizeof root, "%s", cache_root);
    else if (fmlx_hf_cache_root(root, sizeof root) != FMLX_HF_OK) return;
    if (fmlx_hf_repo_dir(root, repo_id, repo, sizeof repo) != FMLX_HF_OK) return;
    write_ref_if_new(repo, revision, commit);
}

/* Un passetto della macchina: tutto ciò che decide DOVE finiscono i byte, con il transport
 * passato a mano. È statica perché il confine pubblico è uno solo per tipo:
 * `fmlx_hf_download` (curl di sistema) e `fmlx_hf_download_with` (transport del test). */
static FmlxHfStatus fetch_one(const char *repo_id, const char *revision, const char *relpath,
                              const FmlxHfFetchOpts *opts, const FmlxHfTransport *t,
                              FmlxHfRef *out) {
    /* Dichiarazioni tutte in testa: i `goto` di questo file non devono mai scavalcare una
     * dichiarazione (salto dentro uno scope è il genere di bug che -Wall non vede sempre). */
    FmlxHfFetchOpts d = {0};
    FmlxHfRef ref;
    FmlxHfMeta meta;
    FmlxHfStatus s;
    char root[FMLX_HF_PATH_MAX], repo[FMLX_HF_PATH_MAX], sha[64];
    char ep_buf[FMLX_HF_PATH_MAX], url[FMLX_HF_PATH_MAX * 2], blobs[FMLX_HF_PATH_MAX];
    char lock[FMLX_HF_PATH_MAX * 2], dn[FMLX_HF_PATH_MAX];
    char tmp[FMLX_HF_PATH_MAX * 2], blob[FMLX_HF_PATH_MAX * 2], hex[FMLX_HF_HEX_MAX];
    int repairing = 0, lock_fd = -1, fd = -1, waited = 0, sha_from_local = 1, commit_ok = 0;
    int64_t from = 0, written = 0;
    const char *ep, *token;

    memset(&ref, 0, sizeof ref);
    memset(&meta, 0, sizeof meta);
    meta.size = -1;

    if (opts) d = *opts;
    if (!t || !t->meta || !t->get) return fmlx_hf_fail(FMLX_HF_ERR_IO, "transport senza meta/get");
    /* Le opzioni della chiamata arrivano al transport UNA volta, prima di qualunque richiesta:
     * `stall_timeout_s` è una soglia di CHI scarica, non del transport (vedi il sigillo).
     * NULL = il transport finto dei test non ha bisogno di saperlo. */
    if (t->configure) t->configure(t->ctx, &d);
    if (fmlx_hf_check_relpath(relpath) != FMLX_HF_OK) return FMLX_HF_ERR_NAME;
    if (!repo_id || !*repo_id || !revision || !*revision)
        return fmlx_hf_fail(FMLX_HF_ERR_REPO_ID, "repo id o revisione vuoti");

    if (d.cache_root) snprintf(root, sizeof root, "%s", d.cache_root);
    else {
        s = fmlx_hf_cache_root(root, sizeof root);
        if (s != FMLX_HF_OK) return s;
    }
    s = fmlx_hf_repo_dir(root, repo_id, repo, sizeof repo);
    if (s != FMLX_HF_OK) return s;
    /* 1) da quale commit si parte a cercare in casa. refs/<revision> risponde quando c'è, e serve
     *    a due cose: provare il colpo di fortuna SULLE BISCIA (passo 2) senza toccare la rete, e
     *    fare da rifugio se il server non dice nulla. NON è la parola finale: il commit dove i
     *    byte FINIRANNO lo decide il server (X-Repo-Commit, passo 4), perché un ref fermo a ieri
     *    descriverebbe come "ieri" dei byte scaricati oggi. Un ref che NON c'è non è un
     *    fallimento: è un repo mai visto, e lì il commit può solo arrivare dal server. */
    sha_from_local = 1;
    s = fmlx_hf_resolve_ref(repo, revision, sha, sizeof sha);
    if (s == FMLX_HF_ERR_REF_MISSING || s == FMLX_HF_ERR_NO_REPO) sha_from_local = 0;
    else if (s != FMLX_HF_OK) return s; /* REF_CONTENT/REVISION/BUF_SMALL: si dicono, non si aggirano */

    /* 2) è già nel cache? */
    if (sha_from_local) {
        s = fmlx_hf_locate(root, repo_id, sha, relpath, &ref);
        if (s == FMLX_HF_OK) {
            FmlxHfStatus v = fmlx_hf_verify(&ref, NULL);
            /* NOT_VERIFIABLE qui è un successo: il nome del blob non promette nulla, quindi non c'è
             * nulla da confrontare. Non è una rinuncia: non esiste un'attesa da violare. */
            if (v == FMLX_HF_OK || v == FMLX_HF_ERR_NOT_VERIFIABLE) {
                if (out) *out = ref;
                return FMLX_HF_OK;
            }
            if (v == FMLX_HF_ERR_HASH_MISMATCH && d.repair) repairing = 1;
            else return v;
        } else if (s != FMLX_HF_ERR_NOT_CACHED && s != FMLX_HF_ERR_SNAPSHOT_MISSING &&
                   s != FMLX_HF_ERR_BLOB_MISSING && s != FMLX_HF_ERR_NO_REPO) {
            /* UN REPO MAI SCARICATO È IL CASO NORMALE di un download: "non è nel cache" non è un
             * fallimento, è la ragione per cui si sta per chiedere la rete. Le altre sono veri
             * fallimenti e si dicono: NOT_ON_REMOTE, NOT_A_FILE, LINK_FORM, NAME… */
            return s;
        }
    }

    /* 3) la rete è concessa? */
    if (d.offline || fmlx_hf_offline())
        return fmlx_hf_fail(FMLX_HF_ERR_OFFLINE, "%s:%s non nel cache e rete vietata", repo_id, relpath);
    if (fmlx_hf_mkdir_p(repo) != 0)
        return fmlx_hf_fail(FMLX_HF_ERR_IO, "mkdir %s: %s", repo, strerror(errno));

    token = d.token;
    if (!token || !*token) token = getenv("HF_TOKEN");
    if (token && !*token) token = NULL; /* una variabile vuota NON è un bearer token */
    ep = d.endpoint;
    if (!ep || !*ep) {
        s = fmlx_hf_endpoint(ep_buf, sizeof ep_buf);
        if (s != FMLX_HF_OK) return s;
        ep = ep_buf;
    }
    s = fmlx_hf_url_with(ep, repo_id, revision, relpath, 1, url, sizeof url);
    if (s != FMLX_HF_OK) return s;

    /* 4) cosa dice il server. Una HEAD sola: metadati e lunghezza, mai byte. */
    s = t->meta(t->ctx, url, token, &meta);
    if (s != FMLX_HF_OK) return s;
    /* Xet: il server può annunciare `X-Xet-Hash` (huggingface_hub lo legge in utils/_xet.py:47-63
     * e, SE hf_xet è installato, ci MANDA il download: file_download.py:1735 con
     * `is_xet_available()` di utils/_runtime.py:155-159, che è esattamente ciò che
     * HF_HUB_DISABLE_XET spegne — la stessa guardia che il fork Python paga col commit a47fb67c).
     * Qui il protocollo Xet non esiste e il backend classico è la scelta del piano §12.4, MA non è
     * un ripiegamento silenzioso: è una differenza di banda e di servidori che chi guarda il log ha
     * diritto di vedere. Una volta per processo, perché i file di un repo xet sono decine. */
    if (meta.xet_announced) {
        static int xet_said;
        if (!xet_said) {
            xet_said = 1;
            fprintf(stderr, "[fetch] Xet annunciato dal server: i byte arrivano dal backend "
                            "classico HTTP (il protocollo xet non e' implementato)\n");
        }
    }
    /* Il commit dove finiscono i byte è quello che il server ha detto per QUESTO URL, non quello
     * che il cache pensa del ramo: un refs/main fermo a ieri descriverebbe come "ieri" dei byte di
     * oggi. huggingface_hub fa allo stesso modo (`commit_hash = metadata.commit_hash`,
     * file_download.py:1733) e SE NON C'È È ERRORE (`:1734-1737`), non un ripiegamento sul ref:
     * qui siamo più morbidi perche' un ramo locale e' informazione reale, ma il fatto resta nel
     * messaggio, non in silenzio. */
    commit_ok = fmlx_hf_blob_hash_kind(meta.commit) == FMLX_HF_HASH_GIT_BLOB;
    if (meta.not_found) {
        /* 404/405: il marker va scritto nel commit del server se c'è, altrimenti in quello locale.
         * Scritto, chi chiede lo stesso file non rifà più la domanda alla rete. È anche qui che
         * HF scrive il ref nello stesso ramo del marker `.no_exist`: il nome vale anche se il file
         * non esiste. */
        if (commit_ok) snprintf(sha, sizeof sha, "%s", meta.commit);
        if (commit_ok || sha_from_local) {
            s = write_no_exist_marker(repo, sha, relpath);
            if (s != FMLX_HF_OK) return s;
            if (commit_ok) write_ref_if_new(repo, revision, sha);
        }
        return fmlx_hf_fail(FMLX_HF_ERR_NOT_ON_REMOTE, "il remote non ha %s:%s (HTTP %d)", repo_id,
                            relpath, meta.status);
    }
    if (!commit_ok && !sha_from_local)
        return fmlx_hf_fail(FMLX_HF_ERR_HTTP, "%s: né refs/%s né un X-Repo-Commit a 40 hex: non so in "
                                              "quale snapshot scrivere", repo_id, revision);
    if (commit_ok && strcmp(sha, meta.commit) != 0) {
        snprintf(sha, sizeof sha, "%s", meta.commit);
        sha_from_local = 0; /* il ref è stale: il passo 9 lo riscrive */
    }
    if (!meta.etag[0]) return fmlx_hf_fail(FMLX_HF_ERR_NO_ETAG, "nessun etag per %s", url);
    /* L'etag DIVENTA il nome del blob: è una stringa che arriva dal server, quindi passa dalla
     * STESSA regola del nome letto da un symlink. Un etag "../../x" non deve scegliere un path. */
    if (!fmlx_hf_name_is_safe(meta.etag))
        return fmlx_hf_fail(FMLX_HF_ERR_BAD_ETAG, "etag inaccettabile come nome: '%s'", meta.etag);
    /* Senza lunghezza dichiarata non c'è modo di provare un troncamento: huggingface_hub la esige
     * allo stesso modo (file_download.py:1752, "Distant resource does not have a Content-Length."). */
    if (meta.size < 0)
        return fmlx_hf_fail(FMLX_HF_ERR_NO_SIZE, "%s: il server non dichiara la lunghezza", relpath);
    if (d.max_bytes > 0 && meta.size > d.max_bytes)
        return fmlx_hf_fail(FMLX_HF_ERR_SIZE, "%s dichiara %lld byte, limite %lld", relpath,
                            (long long)meta.size, (long long)d.max_bytes);

    /* 5) lock. STESSO file e STESSA chiamata di sistema che usa chi condivide questo cache:
     *    huggingface_hub prende `<root>/.locks/<repo_folder_name>/<etag>.lock` (file_download.py
     *    :1058 (`locks_dir`) e :1203 (`lock_path`), misurato) e lo blocca con `fcntl.flock(fd, LOCK_EX|LOCK_NB)`
     *    (filelock/_unix.py:76). Da lì due conseguenze che cambiano il codice:
     *      - il file ESISTE per sempre, anche a download finito: `_release` fa solo LOCK_UN + close
     *        (:103-108), NON cancella nulla — e infatti il cache reale è pieno di .lock. "C'è" non
     *        vuol dire "è preso": lo dice solo flock. Un O_EXCL su questo path si sarebbe bloccato
     *        per sempre alla SECONDA esecuzione.
     *      - il fd si tiene aperto finché lavoro (chiudere rilascia) e NON si cancella: togliere
     *        un lock che un altro processo sta per riaprire è la corsa TOCTOU che _unix.py:88
     *        documenta dal lato opposto. */
    if (fmlx_hf_repo_dirname(repo_id, dn, sizeof dn) != FMLX_HF_OK) return FMLX_HF_ERR_REPO_ID;
    s = fmlx_hf_pjoin(lock, sizeof lock, root, ".locks");
    if (s != FMLX_HF_OK) return s;
    s = fmlx_hf_pjoin(lock, sizeof lock, lock, dn);
    if (s != FMLX_HF_OK) return s;
    if (fmlx_hf_pjoin(lock, sizeof lock, lock, meta.etag) != FMLX_HF_OK) return FMLX_HF_ERR_BUF_SMALL;
    {
        size_t ll = strlen(lock);
        if (ll + 6 >= sizeof lock) return fmlx_hf_fail(FMLX_HF_ERR_BUF_SMALL, "nome di lock troppo lungo");
        memcpy(lock + ll, ".lock", 6);
    }
    if (fmlx_hf_ensure_parent(lock) != 0)
        return fmlx_hf_fail(FMLX_HF_ERR_IO, "mkdir per %s: %s", lock, strerror(errno));
    for (;;) {
        int e;
        lock_fd = open(lock, O_CREAT | O_RDWR, 0666);
        if (lock_fd < 0)
            return fmlx_hf_fail(FMLX_HF_ERR_IO, "open %s: %s", lock, strerror(errno));
        if (flock(lock_fd, LOCK_EX | LOCK_NB) == 0) break;
        e = errno;
        close(lock_fd);
        lock_fd = -1;
        if (e != EWOULDBLOCK && e != EINTR)
            return fmlx_hf_fail(FMLX_HF_ERR_IO, "flock %s: %s", lock, strerror(e));
        if (waited >= d.wait_lock_ms)
            return fmlx_hf_fail(FMLX_HF_ERR_LOCKED, "%s trattenuto da un altro processo", lock);
        sleep_ms(50);
        waited += 50;
    }

    /* 6) dove stanno i byte: `blobs/<etag>` e, mentre scende, `blobs/<etag>.incomplete`. Il nome
     *    del temporaneo è DETERMINATO proprio perché la ripresa funzioni tra processi: un
     *    `.incomplete.<casuale>` non lo troverebbe più nessuno, e 30 GB di lavoro si ripeterebbero
     *    da zero. NON è una divergenza da huggingface_hub: è una SCELTA TRA DUE COPIE di "0.36.2"
     *    installate su questa macchina. La serie `requests` (`~/ai/env-vllmlx`) fa esattamente
     *    così — temp condiviso `:1176`, mantenuto: «By default, we will try to resume the download
     *    if possible» `:1700-1704`. La serie `httpx` (`~/ai/env-mflux`) è cambiata in
     *    `<etag>.<8hex>.incomplete` (`:1908`) **cancellato a ogni fallimento** (`:1944-1947`),
     *    perché su Lustre/GPFS/alcuni NFS `flock(2)` riesce silenziosamente a tutti (`:1900-1907`).
     *    Qui si paga il secondo rischio per avere il primo comportamento, che è ciò che serve a un
     *    terminale: il costo è scritto nel README e regge solo se il lock è preso prima di aprire
     *    il temporaneo — sempre. */
    s = fmlx_hf_pjoin(blobs, sizeof blobs, repo, "blobs");
    if (s != FMLX_HF_OK) goto done;
    if (fmlx_hf_mkdir_p(blobs) != 0) {
        s = fmlx_hf_fail(FMLX_HF_ERR_IO, "mkdir %s: %s", blobs, strerror(errno));
        goto done;
    }
    s = fmlx_hf_pjoin(blob, sizeof blob, blobs, meta.etag);
    if (s != FMLX_HF_OK) goto done;
    if (snprintf(tmp, sizeof tmp, "%s.incomplete", blob) >= (int)sizeof tmp) {
        s = fmlx_hf_fail(FMLX_HF_ERR_BUF_SMALL, "nome di temporaneo non sta in %zu byte", sizeof tmp);
        goto done;
    }
    if (repairing && unlink(tmp) != 0 && errno != ENOENT) { /* force_download non riprende */
        s = fmlx_hf_fail(FMLX_HF_ERR_IO, "unlink %s: %s", tmp, strerror(errno));
        goto done;
    }

    /* Dove si riparte: il .incomplete, se c'è, è lavoro già pagato. Se è PIÙ lungo di quanto il
     * server dichiara non è il nostro frammento: si riparte da zero. */
    from = file_size(tmp);
    if (from < 0) from = 0;
    if (from > meta.size) from = 0;

    /* GET: non si chiede MAI la testa quando il temporaneo è già completo (stessa guardia di
     * huggingface_hub, file_download.py:359-361) — il server risponderebbe 416. */
    if (from == meta.size) {
        written = 0;
    } else {
        const char *dl_url = meta.location[0] ? meta.location : url;
        const char *dl_token = token;
        int attempt = 0;
        /* Su un host DIVERSO dal hub (CDN) il token non si manda: la firma dell'URL è già
         * l'autorizzazione. È la regola di file_download.py:1760-1766, e senza di essa ogni
         * download LFS esporrebbe la credenziale a un terzo host. */
        if (token && !same_authority(url, dl_url)) dl_token = NULL;
        /* APERTURA: identica a prima che esistesse il ritentativo — `O_TRUNC` SOLO se si chiede
         * da zero, altrimenti un 200 che ignorava il Range avrebbe già azzerato il transport. */
        fd = open(tmp, O_CREAT | O_WRONLY | (from > 0 ? 0 : O_TRUNC), 0644);
        if (fd < 0) {
            s = fmlx_hf_fail(FMLX_HF_ERR_IO, "open %s: %s", tmp, strerror(errno));
            goto done;
        }
        if (from > 0 && lseek(fd, 0, SEEK_END) < 0) {
            s = fmlx_hf_fail(FMLX_HF_ERR_IO, "seek %s: %s", tmp, strerror(errno));
            close(fd);
            fd = -1;
            goto done;
        }
        for (;;) {
            /* Il limite È la lunghezza dichiarata dal server per il file intero, non "quanto manca":
             * se il server ignora il Range risponde con TUTTO il corpo, e fermarlo a metà sarebbe un
             * falso positivo. Resta una guardia vera: un server che dichiara 1 KB e ne streamma 1 TB
             * viene fermato al primo kilobyte oltre la promessa. */
            s = t->get(t->ctx, dl_url, dl_token, from, meta.size, fd, &written);
            close(fd);
            fd = -1;
            if (s == FMLX_HF_OK) break;
            if (s != FMLX_HF_ERR_STALL || attempt >= d.stall_retries) goto done;
            /* Solo LO STALL si ritenta: un 404, un hash che non torna o un lock in mano ad altri
             * ripetuti un secondo dopo ridicono la stessa cosa. Qui il punto è che i byte arrivati
             * SONO già pagati: si riparte dall'ultimo, non da zero — su 30 GB al 90% è la
             * differenza fra un resume e un riscaricamento. */
            attempt++;
            from = file_size(tmp);
            if (from < 0) from = 0;
            if (from > meta.size) from = 0;   /* il temporaneo è più lungo del promesso: si ricomincia */
            {
                struct timespec ts;
                long ms = 250;
                int i;
                for (i = 1; i < attempt && ms < 2000; i++) ms *= 2;   /* 250, 500, 1000, 2000, ... */
                ts.tv_sec = ms / 1000;
                ts.tv_nsec = (ms % 1000) * 1000000L;
                nanosleep(&ts, NULL);   /* il server che ha smesso di rispondere non va pestato */
            }
            fprintf(stderr, "[fetch] stream fermo: ripartenza da %lld byte su %lld, tentativo %d/%d\n",
                    (long long)from, (long long)meta.size, attempt, d.stall_retries);
            fd = open(tmp, O_CREAT | O_WRONLY | (from > 0 ? 0 : O_TRUNC), 0644);
            if (fd < 0) {
                s = fmlx_hf_fail(FMLX_HF_ERR_IO, "open %s: %s", tmp, strerror(errno));
                goto done;
            }
            if (from > 0 && lseek(fd, 0, SEEK_END) < 0) {
                s = fmlx_hf_fail(FMLX_HF_ERR_IO, "seek %s: %s", tmp, strerror(errno));
                close(fd);
                fd = -1;
                goto done;
            }
        }
    }

    /* La verifica è sui BYTE DEL FILE, non su un contatore: se il server ha ignorato il Range
     * (200 invece di 206) il transport ha già azzerato il file, e `stat` dice il vero. */
    {
        int64_t have = file_size(tmp);
        /* File vuoto dichiarato (es. i file in `.ipynb_checkpoints` nei repo mlx-community): il transport non scrive niente e il
         * temporaneo puo' non esistere. Un file da 0 byte e' valido: lo si crea, invece di rifiutare l'intero pull. */
        if (have < 0 && meta.size == 0) {
            int zfd = open(tmp, O_CREAT | O_WRONLY | O_TRUNC, 0644);
            if (zfd >= 0) close(zfd);
            have = file_size(tmp);
        }
        if (have != meta.size) {
            s = fmlx_hf_fail(FMLX_HF_ERR_SIZE, "%s: %lld byte contro %lld dichiarati (%s)", relpath,
                             (long long)have, (long long)meta.size, tmp);
            goto done;
        }
    }

    /* 7) verifica del temporaneo PRIMA di esporre il nome definitivo. */
    {
        int hfd = open(tmp, O_RDONLY);
        if (hfd < 0) {
            s = fmlx_hf_fail(FMLX_HF_ERR_IO, "open %s: %s", tmp, strerror(errno));
            goto done;
        }
        int64_t hashed = 0;
        FmlxHfHashKind kind = fmlx_hf_blob_hash_kind(meta.etag);
        if (kind == FMLX_HF_HASH_NONE) {
            close(hfd); /* nessun hash da confrontare: il nome non promette niente */
        } else {
            int64_t sz = file_size(tmp);
            int rc = fmlx_hf_hash_fd(hfd, kind, sz, hex, &hashed);
            close(hfd);
            if (rc != 0) {
                s = fmlx_hf_fail(FMLX_HF_ERR_IO, "hash di %s: %s", tmp, strerror(errno));
                goto done;
            }
            if (strcmp(hex, meta.etag) != 0) {
                /* Non rinominiamo: un blob scaricato male non deve mai prendere il nome di
                 * quello giusto. Il temp si butta, perché è nostro e lo abbiamo appena provato
                 * falso. */
                unlink(tmp);
                s = fmlx_hf_fail(FMLX_HF_ERR_HASH_MISMATCH, "%s: server=%s calcolato=%s (%lld byte)",
                                 tmp, meta.etag, hex, (long long)hashed);
                goto done;
            }
        }
    }
    /* 8) rename ATOMICO: il nome definitivo compare solo ora, e solo perché i byte hanno già
     *    passato l'hash che quel nome promette. */
    if (rename(tmp, blob) != 0) {
        s = fmlx_hf_fail(FMLX_HF_ERR_IO, "rename %s -> %s: %s", tmp, blob, strerror(errno));
        goto done;
    }
    s = make_snapshot_link(repo, sha, relpath, meta.etag);
    if (s != FMLX_HF_OK) goto done;
    s = fmlx_hf_locate(root, repo_id, sha, relpath, &ref);
    if (s != FMLX_HF_OK) goto done; /* i byte ci sono, ma non sono raggiungibili: è un fallimento */
    if (out) *out = ref;

    /* 9) refs/<revision> = commit, se la revisione era un nome e non il commit stesso. È il
     *    passo che rende il SECONDO colpo di fortuna risolvibile senza rete (la stessa cosa che
     *    huggingface_hub fa in _cache_commit_hash_for_specific_revision, file_download.py:763-778).
     *    Un suo fallimento NON fallisce il download: i byte sono già nel cache e raggiungibili;
     *    è il caso — documentato lì — del cache in sola lettura. */
    if (!sha_from_local) write_ref_if_new(repo, revision, sha);

done:
    /* Si chiude il fd, NON si cancella il file: è un lock che vive nel cache condiviso e che
     * huggingface_hub lascia dov'è per sempre. Chiuderlo basta a rilasciarlo. */
    if (lock_fd >= 0) close(lock_fd);
    return s;
}

FmlxHfStatus fmlx_hf_download(const char *repo_id, const char *revision, const char *relpath,
                              const FmlxHfFetchOpts *opts, FmlxHfRef *out) {
    return fmlx_hf_download_with(repo_id, revision, relpath, opts, fmlx_hf_transport_curl(), out);
}

FmlxHfStatus fmlx_hf_download_with(const char *repo_id, const char *revision, const char *relpath,
                                   const FmlxHfFetchOpts *opts, const FmlxHfTransport *t,
                                   FmlxHfRef *out) {
    if (!t) return fmlx_hf_fail(FMLX_HF_ERR_IO, "transport NULL: il download non ha un trasporto");
    return fetch_one(repo_id, revision, relpath, opts, t, out);
}
