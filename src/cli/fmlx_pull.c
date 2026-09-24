/* fmlx_pull.c — un repo INTERO, a UN commit, e (se viene chiesto) lo specchio fuori dal cache.
 *
 * Il file è piccolo perché le decisioni vere sono già prese altrove: il manifesto dice QUALI file,
 * la lane download decide DOVE finiscono i byte, il transport decide COME arrivano. Qui c'è l'ordine,
 * che però è una decisione:
 *
 *   1. un manifesto, UN commit (`sha` della risposta) per tutti i file. Chiedere la HEAD file per
 *      file vorrebbe dire che una pubblicazione a metà pull produce uno snapshot misto: metà file di
 *      ieri e metà di oggi, tutti sotto lo stesso nome di commit, perché ciascun HEAD scrive il
 *      proprio. È anche il motivo per cui il ref del ramo lo scriviamo NOI alla fine: alla lane
 *      download passiamo il commit, non il nome, e lei salta di proposito quel passo.
 *   2. specchio DOPO che tutto il cache è a posto. Un `--dest` riempito a metà sarebbe un modello
 *      che sembra pronto e non lo è: la directory deve raccontare ciò che contiene.
 *   3. hardlink, mai symlink. Il symlink è la trappola classica (un file da 132 byte che punta
 *      altrove, e il cache che si sposta sotto i piedi); la copia da 30 GB sarebbe il modello due
 *      volte sul disco. L'hardlink sono gli STESSI byte con un secondo nome: `EXDEV` (volume
 *      diverso) è l'unico caso in cui si copia davvero, e si copia su un temporaneo + `rename`.
 *
 * Regola che vale tutto il file: nessun esito silenzioso. Ogni fallimento finisce nel report con il
 * codice, il file e il messaggio; `n_ok` dice dove si è arrivati.
 */
#include "fmlx/fmlx_cli.h"
#include "fmlx_cli_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define COPY_CHUNK 65536

/* Il messaggio viene dalla lane hf quando è la lane hf ad aver fallito: copiarlo qui vuol dire non
 * inventarsi una seconda descrizione dello stesso rifiuto. `fmlx_hf_last_error` resta valida fino
 * alla prossima chiamata di quella lane, quindi si copia SUBITO. */
static void fail_lane(FmlxPullReport *rep, FmlxHfStatus s, const char *file) {
    const char *m = fmlx_hf_last_error();
    rep->error = s;
    if (file) snprintf(rep->error_file, sizeof rep->error_file, "%s", file);
    snprintf(rep->error_msg, sizeof rep->error_msg, "%s", m ? m : fmlx_hf_status_str(s));
}

static void fail_own(FmlxPullReport *rep, FmlxHfStatus s, const char *file, const char *fmt, ...) {
    va_list ap;
    rep->error = s;
    if (file) snprintf(rep->error_file, sizeof rep->error_file, "%s", file);
    va_start(ap, fmt);
    vsnprintf(rep->error_msg, sizeof rep->error_msg, fmt, ap);
    va_end(ap);
}

/* L'ultimo segmento di un repo id: il nome della directory di `--dest`. Qui non c'è una seconda
 * validazione, perché `fmlx_hf_repo_dir` — chiamata prima — ha già stabilito che ogni segmento è un
 * nome di directory accettabile; rivalidarlo vorrebbe dire tenere due regole divergenti. */
static const char *repo_leaf(const char *repo_id) {
    const char *slash = strrchr(repo_id, '/');
    return slash ? slash + 1 : repo_id;
}

/* Copia `src` su `dst` passando da un temporaneo: serve SOLO dove `link()` non può funzionare
 * (volume diverso), perché è l'unico caso in cui i byte diventano due copie e quindi vale la stessa
 * atomicità che il cache si impone. */
static FmlxHfStatus copy_stream(const char *src, const char *dst) {
    char tmp[FMLX_HF_PATH_MAX * 2];
    unsigned char buf[COPY_CHUNK];
    FmlxHfStatus s = FMLX_HF_OK;
    int in, out;
    ssize_t r;

    if (snprintf(tmp, sizeof tmp, "%s.fmlx-part", dst) >= (int)sizeof tmp) return FMLX_HF_ERR_BUF_SMALL;
    in = open(src, O_RDONLY);
    if (in < 0) return FMLX_HF_ERR_IO;
    out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        close(in);
        return FMLX_HF_ERR_IO;
    }
    while ((r = read(in, buf, sizeof buf)) > 0) {
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = write(out, buf + off, (size_t)(r - off));
            if (w < 0) {
                if (errno == EINTR) continue;
                s = FMLX_HF_ERR_IO;
                break;
            }
            off += w;
        }
        if (s != FMLX_HF_OK) break;
    }
    if (r < 0) s = FMLX_HF_ERR_IO;
    if (close(in) != 0 && s == FMLX_HF_OK) s = FMLX_HF_ERR_IO;
    if (close(out) != 0 && s == FMLX_HF_OK) s = FMLX_HF_ERR_IO;
    if (s != FMLX_HF_OK) {
        unlink(tmp);
        return s;
    }
    if (rename(tmp, dst) != 0) {
        unlink(tmp);
        return FMLX_HF_ERR_IO;
    }
    return FMLX_HF_OK;
}

/* Un file nello specchio: se `dst` è già LO STESSO inode dei byte nel cache non c'è nulla da fare;
 * negli altri casi la directory di destinazione prende i byte del cache (hardlink dove si può,
 * copia dove non si può). È il significato di `--dest`: quel path deve contenere il modello. */
static FmlxHfStatus mirror_one(const char *src, const char *dst) {
    struct stat a, b;
    int saved;

    if (stat(src, &a) != 0) return FMLX_HF_ERR_IO;
    if (stat(dst, &b) == 0) {
        if (a.st_dev == b.st_dev && a.st_ino == b.st_ino) return FMLX_HF_OK; /* già lo stesso file */
        if (unlink(dst) != 0) return FMLX_HF_ERR_IO;
    }
    if (link(src, dst) == 0) return FMLX_HF_OK;
    saved = errno;
    /* EXDEV = volumi diversi (un disco esterno accanto al cache di casa): l'unica copia vera.
     * EPERM/EOPNOTSUPP = il filesystem non sa che farsene di un hardlink (FAT, alcuni mount di
     * rete): stessa strada. Tutto il resto è un errore da dire, non da aggirare. */
    if (saved != EXDEV && saved != EPERM && saved != EOPNOTSUPP && saved != ENOTSUP)
        return FMLX_HF_ERR_IO;
    return copy_stream(src, dst);
}

/* Il transport chiama QUI, e l'orologio si prende QUI e non dentro il renderer: il renderer deve
 * poter essere provato con un tempo FINTO, o «un ridisegno al secondo» si prova solo dormendo — che
 * non è un test, è un rituale. */
static void pull_on_bytes(void *pctx, int64_t done, int64_t total_hint) {
    fmlx_pull_progress_bytes((FmlxPullProgress *)pctx, done, total_hint, (int64_t)time(NULL));
}

static FmlxHfStatus pull_go(const char *repo_id, const char *revision, const FmlxPullOpts *opts,
                            const FmlxHfTransport *t, FmlxPullReport *rep_out, FILE *log) {
    FmlxPullReport scratch;
    FmlxPullOpts d;
    FmlxHfFetchOpts fo;
    FmlxHfManifest m;
    FmlxPullProgress prog;
    FmlxHfStatus s = FMLX_HF_OK;
    char root[FMLX_HF_PATH_MAX], repo_dir[FMLX_HF_PATH_MAX], snap[FMLX_HF_PATH_MAX];
    char dst_root[FMLX_HF_PATH_MAX], src[FMLX_HF_PATH_MAX], dst[FMLX_HF_PATH_MAX];
    FmlxPullReport *rep = rep_out;
    size_t i;
    int mirroring;

    memset(&d, 0, sizeof d);
    memset(&m, 0, sizeof m);
    if (opts) d = *opts;
    if (!rep) {
        memset(&scratch, 0, sizeof scratch);
        rep = &scratch; /* chi non vuole il report vuole comunque un codice di ritorno */
    }
    memset(rep, 0, sizeof *rep);
    fo = d.fetch;
    /* Su un terminale la riga di avanzamento SOSTITUISCE quella per-file; su tutto il resto (pipe,
     * log, `$( )`) non c'è nessun `\r` e resta il comportamento di sempre. `log == NULL` è il
     * `--quiet`, che non è un terminale: è il silenzio. */
    fmlx_pull_progress_init(&prog, log, log ? isatty(fileno(log)) : 0);
    if (fmlx_pull_progress_inplace(&prog)) {
        fo.on_bytes = pull_on_bytes;
        fo.bytes_ctx = &prog;
    }
    if (!t) t = fmlx_hf_transport_curl(); /* NULL = il transport di sistema, non "nessun transport" */

    if (!repo_id || !*repo_id) {
        fail_own(rep, FMLX_HF_ERR_REPO_ID, NULL, "pull: repo id vuoto");
        return rep->error;
    }
    if (!revision || !*revision) {
        fail_own(rep, FMLX_HF_ERR_REVISION, NULL,
                 "pull di %s: revisione vuota (il default 'main' lo decide la CLI, non qui)", repo_id);
        return rep->error;
    }

    if (fo.cache_root) snprintf(root, sizeof root, "%s", fo.cache_root);
    else {
        s = fmlx_hf_cache_root(root, sizeof root);
        if (s != FMLX_HF_OK) {
            fail_lane(rep, s, NULL);
            return s;
        }
    }

    /* 1) quali file, e a quale commit. */
    s = fmlx_hf_manifest_with(repo_id, revision, &fo, t, &m);
    if (s != FMLX_HF_OK) {
        fail_lane(rep, s, NULL);
        return s;
    }
    snprintf(rep->commit, sizeof rep->commit, "%s", m.commit);
    rep->n_files = m.n;

    s = fmlx_hf_repo_dir(root, repo_id, repo_dir, sizeof repo_dir);
    if (s != FMLX_HF_OK) {
        fail_lane(rep, s, NULL);
        fmlx_hf_manifest_free(&m);
        return s;
    }
    if (fmlx_hf_pjoin(snap, sizeof snap, repo_dir, "snapshots") != FMLX_HF_OK ||
        fmlx_hf_pjoin(rep->snapshot_dir, sizeof rep->snapshot_dir, snap, m.commit) != FMLX_HF_OK) {
        fmlx_hf_manifest_free(&m);
        fail_own(rep, FMLX_HF_ERR_BUF_SMALL, NULL, "pull: il path dello snapshot non sta nel buffer");
        return rep->error;
    }
    if (log) fprintf(log, "pull %s@%s -> commit %.12s…  %zu file\n", repo_id, revision, m.commit, m.n);

    /* 2) un file alla volta, tutti allo STESSO commit. */
    for (i = 0; i < m.n; i++) {
        FmlxHfRef ref;
        const char *rel = m.files[i].path;

        memset(&ref, 0, sizeof ref);
        if (m.files[i].size >= 0) rep->bytes_declared += m.files[i].size;
        fmlx_pull_progress_begin(&prog, i + 1, m.n, rel, m.files[i].size, (int64_t)time(NULL));
        s = fmlx_hf_download_with(repo_id, m.commit, rel, &fo, t, &ref);
        if (s != FMLX_HF_OK) {
            /* La riga aperta va chiusa PRIMA del messaggio: un errore che compare in mezzo a una
             * barra a metà schermo è illeggibile, e su un terminale resterebbe scritta. */
            fmlx_pull_progress_abort(&prog);
            fail_lane(rep, s, rel);
            break;
        }
        rep->n_ok++;
        if (fmlx_pull_progress_inplace(&prog))
            fmlx_pull_progress_end(&prog, ref.size, (int64_t)time(NULL));
        else if (log)
            fprintf(log, "  [%zu/%zu] %s  %" PRId64 " byte\n", i + 1, m.n, rel, ref.size);
    }

    /* 3) il ramo, scritto solo quando tutto il commit è a posto: `refs/<revision>` è ciò che rende
     *    il SECONDO pull risolvibile senza rete. Ritorna void di proposito: un ref non scritto non
     *    rende falso un download i cui byte sono già verificati (stessa regola del passo 9 della
     *    lane download). */
    if (s == FMLX_HF_OK) fmlx_hf_record_ref(root, repo_id, revision, m.commit);

    /* 4) lo specchio, solo se tutto il resto è a posto. */
    mirroring = (s == FMLX_HF_OK && d.dest && *d.dest);
    if (mirroring) {
        if (fmlx_hf_pjoin(dst_root, sizeof dst_root, d.dest, repo_leaf(repo_id)) != FMLX_HF_OK) {
            fail_own(rep, FMLX_HF_ERR_BUF_SMALL, NULL, "pull: il path di --dest non sta nel buffer");
            s = rep->error;
            mirroring = 0;
        } else if (fmlx_hf_mkdir_p(dst_root) != 0) {
            fail_own(rep, FMLX_HF_ERR_IO, NULL, "pull: mkdir %s: %s", dst_root, strerror(errno));
            s = rep->error;
            mirroring = 0;
        }
    }
    if (mirroring) {
        snprintf(rep->dest_dir, sizeof rep->dest_dir, "%s", dst_root);
        for (i = 0; i < m.n; i++) {
            FmlxHfRef ref;
            const char *rel = m.files[i].path;

            memset(&ref, 0, sizeof ref);
            s = fmlx_hf_locate(root, repo_id, m.commit, rel, &ref);
            if (s != FMLX_HF_OK) {
                fail_lane(rep, s, rel);
                break;
            }
            if (fmlx_hf_pjoin(dst, sizeof dst, dst_root, rel) != FMLX_HF_OK) {
                fail_own(rep, FMLX_HF_ERR_BUF_SMALL, rel, "pull: il path specchiato non sta nel buffer");
                s = rep->error;
                break;
            }
            if (fmlx_hf_ensure_parent(dst) != 0) {
                fail_own(rep, FMLX_HF_ERR_IO, rel, "pull: mkdir per %s: %s", dst, strerror(errno));
                s = rep->error;
                break;
            }
            snprintf(src, sizeof src, "%s", ref.path);
            s = mirror_one(src, dst);
            if (s != FMLX_HF_OK) {
                fail_own(rep, s, rel, "pull: specchio %s -> %s: %s", src, dst, strerror(errno));
                s = rep->error;
                break;
            }
            rep->n_mirrored++;
            if (log) fprintf(log, "  specchio %s\n", dst);
        }
    }

    if (log) {
        if (s == FMLX_HF_OK) {
            fprintf(log, "cache: %s\n", rep->snapshot_dir);
            if (rep->dest_dir[0]) fprintf(log, "dest:  %s  (%zu file)\n", rep->dest_dir, rep->n_mirrored);
        } else {
            fprintf(log, "pull FERMO a %zu/%zu file", rep->n_ok, rep->n_files);
            if (rep->error_file[0]) fprintf(log, " (%s)", rep->error_file);
            fprintf(log, ": %s\n", rep->error_msg);
        }
    }

    fmlx_hf_manifest_free(&m);
    return s;
}

FmlxHfStatus fmlx_pull_with(const char *repo_id, const char *revision, const FmlxPullOpts *opts,
                            const FmlxHfTransport *t, FmlxPullReport *rep, FILE *log) {
    return pull_go(repo_id, revision, opts, t, rep, log);
}

FmlxHfStatus fmlx_pull(const char *repo_id, const char *revision, const FmlxPullOpts *opts,
                       FmlxPullReport *rep, FILE *log) {
    return pull_go(repo_id, revision, opts, fmlx_hf_transport_curl(), rep, log);
}
