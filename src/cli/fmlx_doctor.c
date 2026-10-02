/* fmlx_doctor.c — `fastllm doctor`: gli byte del cache SONO quelli che promettono (piano §10.2).
 *
 * È il verbo che manca a `models`. `models` dice «ho 9 file, 4.2 GB»; può dirlo anche se uno di
 * quei file è metà, se il blob è stato troncato da un `kill -9` o se un symlink punta il nome
 * sbagliato: guarda i NOMI. Qui si hashano i byte, con la STESSA verifica che usa il `pull`
 * (`fmlx_hf_verify`), e questo è il punto del file: un verdetto scritto a parte sarebbe verde qui e
 * rosso al download, e a quel punto nessuno dei due vale più niente.
 *
 * Tre scelte sull'uscita, tutte prese per lo stesso motivo — un checker che non può stare in uno
 * script non è un checker:
 *
 *  - **0 = niente corruzione**, 1 = c'è almeno un PROBLEMA (o un repo che non ho potuto audire),
 *    2 = argv sbagliato. Le NOTE non muovono l'uscita: un `.incomplete` lasciato da un download
 *    interrotto e un ref che punta un commit non scaricato sono fatti, non guasti, e far fallire
 *    `doctor && echo ok` per quelli insegnerebbe a ignorare il codice d'uscita.
 *  - `--quiet` stampa SOLO i PROBLEMA: niente intestazioni, niente riepilogo, niente note. È la
 *    forma `fastllm doctor --quiet || die` — e il silenzio continua a voler dire una cosa sola.
 *  - Zero match con un filtro è un FALLIMENTO, come in `models`: ha senso ancora di più qui, perché
 *    «ho controllato tutto e va bene» detto senza aver controllato niente è la bugia peggiore che
 *    un verificatore può dire.
 *
 * Il danno non si tocca: questo verbo NON cancella, NON ripara, NON tocca mtime. La cura di un
 * hash che non torna è `pull --repair`, che ha già il permesso esplicito di sostituire un blob —
 * due verbi che scrivono dove l'altro legge sarebbe un `rm` senza invocarlo.
 */
#include "fmlx/fmlx_cli.h"
#include "fmlx/fmlx_hf.h"
#include "fmlx_cli_internal.h" /* fmlx_cli_fmt_bytes + il filtro, gli STESSI di `models` */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/sysctl.h>
#include "llmengine_mlx.h"   /* B5: llm_ai_mlx_selfcheck (MLX/device/memoria, model-free) */

/* B5 — self-check d'ambiente (piano §B5): verifica che la componente nativa sia quella giusta e
 * che l'ambiente (OS/arch/device/RAM) sia compatibile, PRIMA di toccare i modelli. Non usa il
 * motore (il `doctor` non carica un modello): e' il check che puo' girare anche a macchina scarica. */
static void env_preamble(FILE *out) {
    char os[64] = "?"; size_t l = sizeof os; sysctlbyname("kern.osproductversion", os, &l, NULL, 0);
    char arch[32] = "?"; l = sizeof arch;      sysctlbyname("hw.machine", arch, &l, NULL, 0);
    uint64_t mem = 0;   l = sizeof mem;        sysctlbyname("hw.memsize", &mem, &l, NULL, 0);
    int ncpu = 0;       l = sizeof ncpu;       sysctlbyname("hw.ncpu", &ncpu, &l, NULL, 0);
    fprintf(out, "[env] macOS %s / %s | %d CPU | RAM %.1f GB\n", os, arch, ncpu,
            mem / 1073741824.0);
    { char sc[256] = ""; if (llm_ai_mlx_selfcheck(sc, (int)sizeof sc) == 0 && sc[0])
        fprintf(out, "[env] %s\n", sc); }
}

/* Il verbo, stampato a sinistra del messaggio: è ciò che distingue una riga di `doctor` da una di
 * `models` quando finiscono nello stesso log. */
static void line(FILE *out, int problem, FmlxHfStatus code, const char *rel, const char *rev,
                 const char *msg) {
    if (rev && *rev)
        fprintf(out, "  %-8s %-19s %s [%s]  %s\n", problem ? "PROBLEMA" : "nota",
                fmlx_hf_status_str(code), rel, rev, msg);
    else
        fprintf(out, "  %-8s %-19s %s  %s\n", problem ? "PROBLEMA" : "nota",
                fmlx_hf_status_str(code), rel, msg);
}

/* Una riga per finding trattenuto + la riga che dice quanti NON sono stati elencati. Il tetto è
 * della lane (32), e qui si NOMINA: quaranta problemi stampati in trentadue righe senza "altri 8"
 * è il modo in cui un report sembra completo. */
static void dump_findings(FILE *out, const FmlxHfAudit *a, int quiet) {
    size_t total = a->n_problems + a->n_notes;
    size_t i, lim = total < FMLX_HF_AUDIT_FINDING_MAX ? total : FMLX_HF_AUDIT_FINDING_MAX;

    for (i = 0; i < lim; i++) {
        const FmlxHfFinding *f = &a->findings[i];
        if (quiet && !f->is_problem) continue;
        line(out, f->is_problem, f->code, f->rel, f->rev, f->msg);
    }
    if (total > FMLX_HF_AUDIT_FINDING_MAX)
        fprintf(out, "  (%zu verdetti in più non elencati: %zu problemi, %zu note)\n",
                total - FMLX_HF_AUDIT_FINDING_MAX, a->n_problems, a->n_notes);
}

static void repo_summary(FILE *out, const FmlxHfAudit *a) {
    char dim[32];

    fmlx_cli_fmt_bytes(a->bytes_hashed, dim, sizeof dim);
    fprintf(out, "%s  %zu snapshot, %zu entry, %zu verificabili, %zu ok, %zu blob", a->repo_id,
            a->n_snaps, a->n_files, a->n_verifiable, a->n_verified, a->n_blobs);
    if (a->n_plain) fprintf(out, ", %zu file regolari", a->n_plain);
    if (a->n_partial) fprintf(out, ", %zu mezzo-download", a->n_partial);
    if (a->n_orphan_blobs) {
        char o[32];
        fmlx_cli_fmt_bytes(a->bytes_orphan, o, sizeof o);
        fprintf(out, ", %zu blob orfani (%s)", a->n_orphan_blobs, o);
    }
    if (a->n_lock) fprintf(out, ", %zu lock", a->n_lock);
    if (a->n_noexist) fprintf(out, ", %zu .no_exist", a->n_noexist);
    fprintf(out, "  — hashito %s\n", dim);
}

FmlxCliStatus fmlx_doctor_exec(const FmlxCliArgs *a, FILE *out, FILE *err) {
    FmlxHfRepo *v;
    FmlxHfAudit *aud;
    const char *given, *filt;
    char root[FMLX_HF_PATH_MAX] = "";
    FmlxHfStatus s;
    size_t i, n = 0, ignored = 0, kept = 0, total_problems = 0, total_notes = 0;
    size_t total_files = 0, total_verified = 0, hard_errors = 0;
    int quiet;

    if (!a) return FMLX_CLI_USAGE;
    if (!out) out = stdout;
    if (!err) err = stderr;
    quiet = a->quiet;
    if (!quiet) env_preamble(out);
    given = (a->pull.fetch.cache_root && *a->pull.fetch.cache_root) ? a->pull.fetch.cache_root : NULL;
    filt = a->repo[0] ? a->repo : NULL;

    /* La forma prima del disco: un filtro con la `*` in mezzo non è «nessun repo», è una domanda
     * che nessuno può leggere (stessa regola, stesso helper di `models`). */
    if (filt && !fmlx_cli_filter_shape_ok(filt)) {
        fprintf(err,
                "fastllm doctor: filtro '%s' non ammesso (le forme sono org/nome, nome, org/*)\n",
                filt);
        return FMLX_CLI_USAGE;
    }

    v = (FmlxHfRepo *)malloc(FMLX_HF_CATALOG_REPOS_MAX * sizeof *v);
    aud = (FmlxHfAudit *)malloc(sizeof *aud);
    if (!v || !aud) {
        fprintf(err, "fastllm doctor: memoria per il giro rifiutata\n");
        free(v);
        free(aud);
        return FMLX_CLI_FAILED;
    }
    s = fmlx_hf_catalog(given, v, FMLX_HF_CATALOG_REPOS_MAX, &n, &ignored);
    if (s != FMLX_HF_OK) {
        const char *m = fmlx_hf_last_error();
        fprintf(err, "fastllm doctor: %s [%s]\n", m ? m : fmlx_hf_status_str(s),
                fmlx_hf_status_str(s));
        if (s == FMLX_HF_ERR_NO_CACHE && !given)
            fprintf(err, "  (HF_HUB_CACHE / HF_HOME non puntano a una directory esistente)\n");
        free(v);
        free(aud);
        return FMLX_CLI_FAILED;
    }
    if (given) snprintf(root, sizeof root, "%s", given);
    else (void)fmlx_hf_cache_root(root, sizeof root);

    for (i = 0; i < n; i++) {
        FmlxHfStatus as;
        if (!fmlx_cli_filter_match(filt, v[i].repo_id)) continue;
        kept++;
        as = fmlx_hf_audit_repo(given, v[i].repo_id, aud);
        if (as != FMLX_HF_OK) {
            /* "Non ho potuto guardare" NON è "a posto": esce fallito con la sua riga. */
            const char *m = fmlx_hf_last_error();
            if (!quiet) fprintf(out, "%s  AUDIT IMPOSSIBILE\n", v[i].repo_id);
            line(err, 1, as, v[i].repo_id, "", m && *m ? m : fmlx_hf_status_str(as));
            hard_errors++;
            continue;
        }
        if (!quiet) repo_summary(out, aud);
        dump_findings(out, aud, quiet);
        total_problems += aud->n_problems;
        total_notes += aud->n_notes;
        total_files += aud->n_files;
        total_verified += aud->n_verified;
    }

    if (!quiet) {
        fprintf(out, "\ndoctor: %zu repo controllati, %zu entry, %zu verificati, %zu problemi, "
                     "%zu note",
                kept, total_files, total_verified, total_problems + hard_errors, total_notes);
        if (filt) fprintf(out, "  [filtro '%s', %zu repo nel cache]", filt, n);
        if (ignored && !filt) fprintf(out, "  (%zu entry che non sono un repo)", ignored);
        fprintf(out, "\n%s\n", root[0] ? root : "(radice della lane hf)");
    }
    /* Zero match CON un filtro: «non c'è» deve restare distinguibile da «ho guardato, va tutto
     * bene» anche in `--quiet`, dove dello schermo non arriva niente. Il messaggio è uno solo per
     * le due modalità, e il codice d'uscita è la parte che un script legge. */
    if (filt && kept == 0)
        fprintf(err, "fastllm doctor: nessun repo nel cache che matcha '%s'\n", filt);
    free(v);
    free(aud);
    if (filt && kept == 0) return FMLX_CLI_FAILED;
    return (total_problems || hard_errors) ? FMLX_CLI_FAILED : FMLX_CLI_OK;
}
