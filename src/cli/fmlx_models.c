/* fmlx_models.c — `fastmlx models` / `list`: cosa c'è GIÀ sul disco (piano §10.2, §13).
 *
 * Perché un file suo e non un pezzo di `fmlx_cli.c`: `fmlx_cli.c` è il dispatcher e la grammatica
 * dell'argv (il core dispatcher, per la regola sulle dimensioni dei file); la TABELLA che questo
 * verbo stampa — colonne, unità di misura, marker — è una cosa che cresce per conto suo ed è l'unica
 * che un test può confrontare stringa per stringa.
 *
 * La forma dei dati NON è decisa qui: viene da `fmlx_hf_catalog`, che è la lane che sa cosa è un blob
 * e cosa è uno symlink. Questo file decide solo come si LEGGE una riga, e su quello c'è una scelta
 * presa deliberatamente:
 *
 *  - la dimensione è dei BLOB distinti, non dello snapshot. Un `models` che contasse anche i link
 *    raddoppierebbe ogni modello, e chi ha 40 GB liberi si fiderebbe di quel numero;
 *  - un repo con solo `refs/` è in lista, marcato: è il caso MISURATO su questa macchina
 *    (`models--mlx-community--Mixtral-8x22B-4bit`), e nasconderlo significherebbe dire "non ce l'hai"
 *    a chi ce l'ha puntato; nascondere il fatto che non ci sono byte significherebbe dire "occupa
 *    0" a chi deve fare spazio;
 *  - `--quiet` stampa SOLO gli id, una per riga: è la forma per `$(fastmlx models --quiet)`, e i
 *    numeri formattati non si parsano.
 *
 * Il filtro posizionale (`models <x>`) segue tre forme, e nessuna è «sottostringa»:
 *
 *   `org/nome`   confronto **esatto** (case-insensitive) con l'`repo_id`;
 *   `nome`       confronto **esatto** con la parte dopo l'ultimo `/` — può matchare più org, e la
 *                tabella li mostra entrambi: è il filtro che dice «non è uno solo», non un messaggio;
 *   `org/` + `*` tutti i repo di quell'org.
 *
 * Una `*` che non è l'ultimo carattere è **rifiutata** (`USAGE`), non interpretata: ammettere oggi
 * una forma a metà e domani matchare qualcosa di diverso è il modo in cui un filtro diventa una
 * bugia. E **zero match con un filtro è un FALLIMENTO**, non un elenco vuoto con exit 0: chi chiede
 * `models mm/uno` ha chiesto una cosa specifica, e `grep` insegna che "non c'è" deve potersi
 * distinguere da "ho guardato e va tutto bene" senza leggere lo stdout. Senza filtro, un cache vuoto
 * resta un successo (è un altro fatto).
 *
 * Le unità sono DECIMALI (1 kB = 1000 byte) perché è così che Hugging Face pubblica le dimensioni:
 * mescolare le due convenzioni nello stesso schermo è il modo per far sembrare un modello più piccolo
 * di quello che si è scaricati.
 */
#include "fmlx/fmlx_cli.h"
#include "fmlx/fmlx_hf.h"
#include "fmlx_cli_internal.h" /* fmlx_cli_fmt_bytes: la formattazione è UNA, chi la usa sono due */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp / strncasecmp: il filtro non distingue il maiuscolo, come gli
                      * id HF nel cache (la directory è minuscola per regola della lane) */
#include <time.h>

#define MARKER_REFS_ONLY " [solo ref]"
#define MARKER_AMBIGUOUS " [?]"
#define NAME_MIN_WIDTH 24

void fmlx_cli_fmt_bytes(int64_t b, char *out, size_t n) {
    static const char *const unit[] = {"byte", "kB", "MB", "GB", "TB", "PB"};
    size_t k = 0;
    double v;

    if (b < 0) b = 0; /* un conteggio negativo viene da un orologio o da un inode saltato: non è
                      * una dimensione, e stamparlo sarebbe peggio di stampare 0. */
    v = (double)b;
    while (v >= 1000.0 && k + 1 < sizeof unit / sizeof unit[0]) {
        v /= 1000.0;
        k++;
    }
    if (k == 0) snprintf(out, n, "%lld byte", (long long)b);
    else if (v >= 100.0) snprintf(out, n, "%.0f %s", v, unit[k]);
    else snprintf(out, n, "%.1f %s", v, unit[k]);
}

/* L'età del BLOB più recente, non quella della directory: `mtime` della directory cambia quando si
 * aggiunge un ref, che non è un byte scaricato. */
static void fmt_age(int64_t mtime, int64_t now, char *out, size_t n) {
    int64_t d;

    if (mtime <= 0) {
        snprintf(out, n, "-"); /* nessun blob: non c'è una data da dire */
        return;
    }
    d = now - mtime;
    if (d < 0) d = 0; /* un clock di sistema più vecchio del cache: non è "tra X giorni" */
    if (d < 60) snprintf(out, n, "adesso");
    else if (d < 3600) snprintf(out, n, "%lld min fa", (long long)(d / 60));
    else if (d < 86400) snprintf(out, n, "%lld ore fa", (long long)(d / 3600));
    else if (d < 86400 * 30) snprintf(out, n, "%lld giorni fa", (long long)(d / 86400));
    else if (d < 86400 * 365) snprintf(out, n, "%lld mesi fa", (long long)(d / (86400 * 30)));
    else snprintf(out, n, "%lld anni fa", (long long)(d / (86400 * 365)));
}

/* Gli slot davvero riempiti: sono `n_refs` o meno? Il conteggio VERO è `n_refs`, la lista può essere
 * più corta e il `*` in colonna è il modo di dirlo senza stampare 8 nomi a caso. */
static size_t refs_shown(const FmlxHfRepo *r) {
    size_t i, k = 0;
    for (i = 0; i < FMLX_HF_CATALOG_REFS_MAX; i++)
        if (r->refs[i][0]) k++;
    return k;
}

/* Il filtro è BEN FORMATO? Una `*` è ammessa SOLO come ultimo carattere, e solo dopo `org/` con
 * l'org non vuoto; una domanda su tutto il cache ha già una forma — nessun filtro. (Qui dentro non
 * si può scrivere la sequenza `org/` + asterisco letterale: aprirebbe un commento nel commento, e
 * `-Wcomment` la blocca — il che è il modo gentile in cui il compilatore avvisa che la forma è
 * ambigua anche per un lettore di carne.) */
int fmlx_cli_filter_shape_ok(const char *f) {
    const char *star = strchr(f, '*');
    size_t len;

    if (!star) return 1;
    len = strlen(f);
    if (star != f + len - 1) return 0;
    if (len < 3 || f[len - 2] != '/' || f[len - 3] == '/') return 0;
    return 1;
}

/* `nome` senza `/` si confronta con la parte dopo l'ultimo `/`; `org/nome` con l'id intero.
 * `f == NULL` (nessun filtro) matcha tutto, e il check sta PRIMA di `strlen`: la prima versione
 * leggeva `strlen(f)` sulla riga sopra e andava in segfault sul caso più comune — quello senza
 * filtro. Lo ha trovato il test, non la lettura. */
int fmlx_cli_filter_match(const char *f, const char *id) {
    const char *tail;
    size_t len;

    if (!f || !*f) return 1;
    len = strlen(f);
    if (f[len - 1] == '*') return strncasecmp(f, id, len - 1) == 0;
    if (strchr(f, '/')) return strcasecmp(f, id) == 0;
    tail = strrchr(id, '/');
    return strcasecmp(f, tail ? tail + 1 : id) == 0;
}

static void print_row(const FmlxHfRepo *r, int64_t now, size_t wname, FILE *out) {
    char name[FMLX_HF_CATALOG_NAME_MAX + sizeof MARKER_REFS_ONLY + sizeof MARKER_AMBIGUOUS + 8];
    char dim[32], age[32], refcol[24];
    size_t shown = refs_shown(r);

    snprintf(name, sizeof name, "%s%s%s", r->repo_id,
             r->refs_only ? MARKER_REFS_ONLY : "", r->ambiguous_name ? MARKER_AMBIGUOUS : "");
    fmlx_cli_fmt_bytes(r->bytes, dim, sizeof dim);
    fmt_age(r->mtime, now, age, sizeof age);
    if (r->n_refs > shown) snprintf(refcol, sizeof refcol, "%zu*", r->n_refs);
    else snprintf(refcol, sizeof refcol, "%zu", r->n_refs);
    fprintf(out, "%-*s  %-7s %9s %5zu %4s  %s\n", (int)wname, name, r->kind, dim, r->n_blobs, refcol,
            age);
}

FmlxCliStatus fmlx_models_exec(const FmlxCliArgs *a, FILE *out, FILE *err) {
    FmlxHfRepo *v;
    const char *given, *filt;
    char root[FMLX_HF_PATH_MAX] = "";
    size_t i, n = 0, ignored = 0, wname = NAME_MIN_WIDTH, kept = 0;
    int64_t total = 0;
    int any_refs_only = 0, any_ambiguous = 0;
    FmlxHfStatus s;

    if (!a) return FMLX_CLI_USAGE;
    if (!out) out = stdout;
    if (!err) err = stderr;
    given = (a->pull.fetch.cache_root && *a->pull.fetch.cache_root) ? a->pull.fetch.cache_root : NULL;
    filt = a->repo[0] ? a->repo : NULL;
    /* La forma si valuta PRIMA di leggere il disco: un filtro con la `*` in mezzo non è "nessun
     * repo", è un filtro che nessuno può leggere, e dirlo dopo aver scansito il cache farebbe
     * perdere anche il tempo. */
    if (filt && !fmlx_cli_filter_shape_ok(filt)) {
        fprintf(err,
                "fastmlx models: filtro '%s' non ammesso (le forme sono org/nome, nome, org/*)\n",
                filt);
        return FMLX_CLI_USAGE;
    }

    v = (FmlxHfRepo *)malloc(FMLX_HF_CATALOG_REPOS_MAX * sizeof *v);
    if (!v) {
        fprintf(err, "fastmlx models: memoria per %d repo rifiutata\n", FMLX_HF_CATALOG_REPOS_MAX);
        return FMLX_CLI_FAILED;
    }
    /* `given == NULL` lascia la scelta della radice alla lane: la regola degli ambiente
     * (HF_HUB_CACHE, HF_HOME, il default) vive in `fmlx_hf_cache_root` e qui NON viene ripetuta. La
     * si richiama una sola volta, DOPO, e solo per scrivere il path nella riga finale: se la
     * prossima volta che qualcuno aggiunge una variabile d'ambiente cambierà la regola, cambierà per
     * tutte e due i giri perché è la STESSA funzione. */
    s = fmlx_hf_catalog(given, v, FMLX_HF_CATALOG_REPOS_MAX, &n, &ignored);
    if (s != FMLX_HF_OK) {
        const char *m = fmlx_hf_last_error();
        fprintf(err, "fastmlx models: %s [%s]\n", m ? m : fmlx_hf_status_str(s), fmlx_hf_status_str(s));
        if (s == FMLX_HF_ERR_NO_CACHE && !given)
            /* Il caso reale di chi non ha mai scaricato niente con HF: "non esiste" senza il path
             * cercato fa perdere più tempo del path sbagliato. */
            fprintf(err, "  (HF_HUB_CACHE / HF_HOME non puntano a una directory esistente)\n");
        free(v);
        return FMLX_CLI_FAILED;
    }
    if (given) snprintf(root, sizeof root, "%s", given);
    else (void)fmlx_hf_cache_root(root, sizeof root);

    if (a->quiet) { /* una riga per repo: niente da parsare */
        for (i = 0; i < n; i++) {
            if (!fmlx_cli_filter_match(filt, v[i].repo_id)) continue;
            fprintf(out, "%s\n", v[i].repo_id);
            kept++;
        }
        if (filt && kept == 0)
            fprintf(err, "fastmlx models: nessun repo nel cache che matcha '%s'\n", filt);
        free(v);
        return (filt && kept == 0) ? FMLX_CLI_FAILED : FMLX_CLI_OK;
    }

    for (i = 0; i < n; i++) {
        size_t len;
        if (!fmlx_cli_filter_match(filt, v[i].repo_id)) continue;
        kept++;
        len = strlen(v[i].repo_id);
        if (v[i].refs_only) {
            len += sizeof MARKER_REFS_ONLY - 1;
            any_refs_only = 1;
        }
        if (v[i].ambiguous_name) {
            len += sizeof MARKER_AMBIGUOUS - 1;
            any_ambiguous = 1;
        }
        if (len > wname) wname = len;
        total += v[i].bytes;
    }
    /* Zero match CON un filtro: nessuna tabella, nessun totale, e un codice che lo dice. Stampare
     * l'intestazione con sotto "(nessun repo)" farebbe sembrare completo un risposta che è un
     * fallimento, ed è esattamente il caso che `grep` risolve con il suo exit 1. */
    if (filt && kept == 0) {
        fprintf(err, "fastmlx models: nessun repo nel cache che matcha '%s'\n", filt);
        free(v);
        return FMLX_CLI_FAILED;
    }
    fprintf(out, "%-*s  %-7s %9s %5s %4s  %s\n", (int)wname, "NOME", "TIPO", "DIM", "BLOB", "REF",
            "MODIFICATO");
    for (i = 0; i < n; i++) {
        if (!fmlx_cli_filter_match(filt, v[i].repo_id)) continue;
        print_row(&v[i], (int64_t)time(NULL), wname, out);
    }
    if (n == 0) fprintf(out, "(nessun repo)\n");

    {
        char t[32];
        fmlx_cli_fmt_bytes(total, t, sizeof t);
        /* Il conteggio della riga finale è quello dei **match**, non quello del cache: dire "3 repo,
         * 1.5 kB" sotto una tabella di una riga sola sarebbe il modo in cui un filtro si spaccia per
         * l'elenco completo. E il filtro si NOMINA, perché "1 repo" senza dire 1 di cosa non è
         * un'informazione. */
        fprintf(out, "\n%zu repo%s, %s in %s", kept, filt ? " che matchano" : "", t,
                root[0] ? root : "(radice della lane hf)");
        if (filt) fprintf(out, "  [filtro '%s', %zu repo nel cache]", filt, n);
        /* I non-repo si NOMINANO: `version.txt` e `xet/` nella radice ci sono davvero, e un elenco
         * che tace su ciò che salta è il modo in cui "non ho trovato niente" diventa
         * "non c'è niente". */
        if (ignored) fprintf(out, "  (%zu entry che non sono un repo)", ignored);
        fprintf(out, "\n");
    }
    if (any_refs_only || any_ambiguous) {
        fprintf(out, "note:");
        if (any_refs_only) fprintf(out, " [solo ref] = solo il puntamento al commit, nessun byte");
        if (any_ambiguous) fprintf(out, "%s [?] = il nome contiene \"--\": l'id puo' non essere invertibile",
                                   any_refs_only ? "," : "");
        fprintf(out, "\n");
    }
    free(v);
    return FMLX_CLI_OK;
}
