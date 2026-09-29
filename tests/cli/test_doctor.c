/* test_doctor.c — il verbo `doctor`: cosa stampa, su quale canale, e con che codice d'uscita.
 *
 * La lane (`src/hf/fmlx_hf_audit.c`) è provata da `test_hf_audit.c` su fixture con hash VERI; qui
 * si prova l'altro pezzo, ed è un pezzo diverso: il verbo deve dire la STESSA cosa in tre forme
 * (schermo, `--quiet`, pipe) e deve uscire con il codice che un script può leggere. Le cose che
 * solo questo file può prendere in fallo:
 *
 *  1. il silenzio bugiardo: `--quiet` su un cache pulito NON deve scrivere niente, e su uno
 *     corrotto deve scrivere il PROBLEMA (se tacesse anche lì, `doctor --quiet || die` sarebbe
 *     peggio di niente — darebbe ragione a chi l'ha scritto);
 *  2. i canali: l'elenco e i verdetti su `out`, l'impossibilità di guardare su `err`. Un problema
 *     sul canale sbagliato finisce in un `$()` e sparisce;
 *  3. le NOTE non muovono l'uscita, i PROBLEMI sì, e lo zero match è un FALLIMENTO: tre casi che un
 *     refactor pigro scambia volentieri fra loro;
 *  4. il tetto dei finding: 40 problemi (più la nota che ne deriva) devono dire che i verdetti
 *     oltre le 32 righe NON sono elencati, non fingere di averli detti tutti;
 *  5. la forma del filtro è controllata PRIMA di toccare il disco (un filtro illeggibile non è
 *     "zero repo", è USAGE);
 *  6. le colonne condizionali del riepilogo (orfani, mezzo-download, file regolari, lock,
 *     `.no_exist`): la lane prova i CONTATORI, qui si prova che la riga li STAMPA — un `%zu` che
 *     legge il campo sbagliato è invisibile a chi guarda solo i numeri della struttura.
 *
 * Il `--cache` che punta nel niente è l'unico caso in cui il verbo fallisce SENZA che il cache sia
 * rotto: la riga deve dirlo, e il dettaglio che conta è che il consiglio sull'ambiente appaia solo
 * quando l'ambiente è di mezzo (con `--cache` dato, consigliare HF_HOME è rumore).
 */
#include "fmlx/fmlx_cli.h"
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

/* I vettori, gli STESSI di `test_hf_audit.c` (git blob id di "a", sha256 di "abc"): copiati e non
 * presi dalla lane, perché un test che prende l'attesa dal codice sotto prova non prova niente. */
#define GIT_A "2e65efe2a145dda7ee51d1741299f848e5bf752e"
#define SHA_ABC "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
#define REV "0123456789abcdef0123456789abcdef01234567"

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

#define HAS(hay, needle) ((hay) && strstr((hay), (needle)) != NULL)

/* ---- i mattoni ---- */

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

/* UN SOLO buffer statico: due `at()` nella stessa espressione passerebbero lo STESSO path. */
static const char *at(const char *rel) {
    static char buf[FMLX_HF_PATH_MAX * 3];
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

static void link_to(const char *rel, const char *target) {
    const char *p = at(rel);
    if (fmlx_hf_ensure_parent(p) != 0) {
        printf("  FIXTURE: genitore di %s: %s\n", rel, strerror(errno));
        failures++;
        return;
    }
    if (symlink(target, p) != 0) {
        printf("  FIXTURE: symlink %s: %s\n", rel, strerror(errno));
        failures++;
    }
}

/* Un cache finto con il repo `org/<nome>`, due blob e due entry. `bad` = 1 corrompe il contenuto
 * del primo blob: il nome resta la promessa giusta, i byte no. È il caso del `kill -9` a metà
 * scrittura, o del blob toccato a mano. */
static void build_repo(const char *short_name, const char *org, int bad, int extra_note) {
    char d[FMLX_HF_PATH_MAX];
    char rel[FMLX_HF_PATH_MAX * 2];

    snprintf(d, sizeof d, "hub/models--%s--%s", org, short_name);
    snprintf(rel, sizeof rel, "%s/refs", d);
    dir(rel);
    snprintf(rel, sizeof rel, "%s/blobs", d);
    dir(rel);
    snprintf(rel, sizeof rel, "%s/refs/main", d);
    put(rel, REV "\n");
    snprintf(rel, sizeof rel, "%s/blobs/%s", d, GIT_A);
    put(rel, bad ? "Z" : "a");
    snprintf(rel, sizeof rel, "%s/blobs/%s", d, SHA_ABC);
    put(rel, "abc");
    snprintf(rel, sizeof rel, "%s/snapshots/%s/config.json", d, REV);
    link_to(rel, "../../blobs/" GIT_A);
    snprintf(rel, sizeof rel, "%s/snapshots/%s/weights/model.safetensors", d, REV);
    link_to(rel, "../../../blobs/" SHA_ABC);
    if (extra_note) { /* il mezzo-download: una NOTA, non un problema */
        snprintf(rel, sizeof rel, "%s/blobs/%s.incomplete", d, SHA_ABC);
        put(rel, "metà");
    }
}

/* I quattro «fatti» che `repo_summary` stampa SOLO se il contatore è diverso da zero: un blob che
 * nessuno richiama, un `.lock` residuo, un marker `.no_exist` e un file regolare in uno snapshot.
 * Il fixture di sopra li fa tutti e quattro ZERO, e una colonna che non scatta mai non è mai stata
 * letta da nessuno: se il `%zu` sbaglia campo, o se una di quelle condizioni è `&&` invece di `||`,
 * qui deve diventare rosso — non lo diventa nel `case_servizio` della lane, che prova i CONTATORI e
 * non la riga che li stampa. */
static void add_facts(const char *short_name, const char *org) {
    char d[FMLX_HF_PATH_MAX];
    char rel[FMLX_HF_PATH_MAX * 2];

    snprintf(d, sizeof d, "hub/models--%s--%s", org, short_name);
    snprintf(rel, sizeof rel, "%s/blobs/orfano.bin", d);
    put(rel, "nessuno lo richiama"); /* 19 byte: è la cifra che la riga deve stampare */
    snprintf(rel, sizeof rel, "%s/.no_exist/%s/manchera.txt", d, REV);
    put(rel, ""); /* il marker È vuoto: è un fatto, non un dato */
    snprintf(rel, sizeof rel, "%s/snapshots/%s/README.md", d, REV);
    put(rel, "regolare\n"); /* non è un symlink: si conta fra i file regolari */
    /* I lock NON stanno nel repo: stanno in `<root>/.locks/<repo_folder>/`, e chi scrive il path a
     * memoria guarda il posto sbagliato. DUE, non uno: la colonna deve contare, non dire "sì". */
    snprintf(rel, sizeof rel, "hub/.locks/models--%s--%s/pezzo.lock", org, short_name);
    put(rel, "");
    snprintf(rel, sizeof rel, "hub/.locks/models--%s--%s/altro.lock", org, short_name);
    put(rel, "");
}

/* ---- il giro: parse + exec, con i due canali catturati ---- */

typedef struct {
    char *out, *err;
    size_t outn, errn;
    char pmsg[256]; /* il testo di un parse rifiutato: i canali non esistono ancora */
} Cap;

static FmlxCliStatus run(Cap *c, FmlxCliArgs *a, int argc, char **av) {
    char msg[256];
    FILE *o, *e;
    FmlxCliStatus s;

    c->out = c->err = NULL;
    c->outn = c->errn = 0;
    c->pmsg[0] = '\0';
    if (fmlx_cli_parse(argc, av, a, msg, sizeof msg) != FMLX_CLI_OK) {
        /* L'argv si ferma PRIMA dei file: `fmlx_cli_exec_with` non è nemmeno chiamato, e pretesa
         * una prova su `err` qui si finirebbe per asserire su un NULL. Il `main` stampa questo
         * testo su stderr; nei test si legge dal buffer di parse. */
        snprintf(c->pmsg, sizeof c->pmsg, "%s", msg);
        return FMLX_CLI_USAGE;
    }
    o = open_memstream(&c->out, &c->outn);
    e = open_memstream(&c->err, &c->errn);
    s = fmlx_cli_exec_with(a, NULL, o, e);
    fclose(o);
    fclose(e); /* il buffer di `open_memstream` vale solo dopo la chiusura */
    return s;
}

static void cap_free(Cap *c) {
    free(c->out);
    free(c->err);
}

/* ---- i casi ---- */

static void case_pulito(void) {
    FmlxCliArgs a;
    Cap c;
    FmlxCliStatus s;

    build_repo("buono", "org", 0, 0);
    s = run(&c, &a, 4, (char *[]){"fastllm", "doctor", "--cache", (char *)at("hub")});
    CK(s == FMLX_CLI_OK, "un cache pulito esce %s [%s]", fmlx_cli_status_str(s), c.err);
    CK(HAS(c.out, "org/buono"), "il repo non è nominato: %s", c.out);
    CK(HAS(c.out, "2 ok"), "non dice due verificati: %s", c.out);
    CK(HAS(c.out, "0 problemi"), "non dice zero problemi: %s", c.out);
    CK(!HAS(c.out, "PROBLEMA"), "c'è un PROBLEMA dove non c'è: %s", c.out);
    CK(c.errn == 0, "ha scritto su err su un cache pulito (%zu byte): %s", c.errn, c.err);
    /* La radice che ha guardato: senza quella riga un `doctor` verde non dice QUALE disco ha
     * letto, e su una macchina con due cache è l'unica informazione che conta. */
    CK(HAS(c.out, "hub"), "la radice non è stampata: %s", c.out);
    cap_free(&c);
}

static void case_corrotto(void) {
    FmlxCliArgs a;
    Cap c;
    FmlxCliStatus s;

    build_repo("rotto", "org", 1, 0);
    s = run(&c, &a, 4, (char *[]){"fastllm", "doctor", "--cache", (char *)at("hub")});
    CK(s == FMLX_CLI_FAILED, "un hash che non torna esce %s, atteso fallito",
       fmlx_cli_status_str(s));
    CK(HAS(c.out, "PROBLEMA"), "non marchia il problema: %s", c.out);
    CK(HAS(c.out, "config.json"), "non nomina l'entry: %s", c.out);
    CK(HAS(c.out, "1 problemi"), "il riepilogo non conta 1: %s", c.out);
    CK(c.errn == 0, "il verdetto è su err (%zu byte): %s", c.errn, c.err);
    cap_free(&c);
}

/* `--quiet`: il caso per cui il flag esiste. */
static void case_quiet(void) {
    FmlxCliArgs a;
    Cap c;

    wipe(at("hub"));
    build_repo("zitto", "org", 0, 1); /* pulito, ma con la nota del mezzo-download */
    (void)run(&c, &a, 5,
              (char *[]){"fastllm", "doctor", "--cache", (char *)at("hub"), "--quiet"});
    CK(c.outn == 0, "`--quiet` su un cache senza problemi scrive %zu byte: %s", c.outn, c.out);
    CK(c.errn == 0, "`--quiet` scrive anche su err: %s", c.err);
    cap_free(&c);

    wipe(at("hub"));
    build_repo("zittorotto", "org", 1, 0);
    (void)run(&c, &a, 5,
              (char *[]){"fastllm", "doctor", "--cache", (char *)at("hub"), "--quiet"});
    CK(HAS(c.out, "PROBLEMA"), "`--quiet` nasconde un problema: %s", c.out);
    CK(!HAS(c.out, "nota"), "`--quiet` stampa le note: %s", c.out);
    CK(!HAS(c.out, "doctor:"), "`--quiet` stampa il riepilogo: %s", c.out);
    cap_free(&c);
}

/* Una NOTA non fallisce il comando: è il confine fra «è un fatto» e «è rotto», e se lo si perde il
 * verbo diventa quello che tutti disattivano. */
static void case_nota_non_fallisce(void) {
    FmlxCliArgs a;
    Cap c;
    FmlxCliStatus s;

    wipe(at("hub"));
    build_repo("annotato", "org", 0, 1);
    add_facts("annotato", "org");
    s = run(&c, &a, 4, (char *[]){"fastllm", "doctor", "--cache", (char *)at("hub")});
    CK(s == FMLX_CLI_OK, "una nota esce %s, atteso ok: %s", fmlx_cli_status_str(s), c.out);
    CK(HAS(c.out, "nota"), "la nota non è nominata: %s", c.out);
    CK(HAS(c.out, "mezzo-download"), "la nota non dice cosa: %s", c.out);
    /* Le quattro colonne condizionali della riga di riepilogo, tutte e quattro INSIEME a un'uscita
     * che resta 0: «ci sono cose nel cache» e «qualcuna è rotta» sono due frasi diverse, e se il
     * verbo le confonde il `doctor && echo ok` di chi ha un lock residuo diventa falso in rosso. */
    CK(HAS(c.out, "1 blob orfani (19 byte)"), "la riga non dice gli orfani (e quanti byte): %s",
       c.out);
    CK(HAS(c.out, "1 file regolari"), "la riga non dice i file regolari: %s", c.out);
    CK(HAS(c.out, "2 lock"), "la riga non conta i due lock: %s", c.out);
    CK(HAS(c.out, "1 .no_exist"), "la riga non dice il marker: %s", c.out);
    cap_free(&c);
}

/* ---- il repo che il filesystem rifiuta ---- */

/* Il ramo che, prima di questa correzione, NON POTEVA accendersi: `fmlx_hf_catalog_repo` rispondeva
 * `ERR_NO_REPO` anche per `EACCES`, e il `doctor` lo leggeva come «non scaricato» — il consiglio
 * `pull` dato su una directory che di `pull` non ha bisogno. Ora i due codici sono distinti e
 * `AUDIT IMPOSSIBILE` è la forma stampata di quella distinzione: un PROBLEMA che muove l'uscita.
 *
 * Due cose che solo questo caso prova. La prima è il canale: l'impossibilità di guardare finisce su
 * `err` (non è un verdetto sul cache, è il verbale di chi non ha potuto guardarlo) mentre il nome
 * del repo resta nell'elenco su `out`. La seconda è `--quiet`, che deve togliere l'elenco e NON il
 * PROBLEMA: `doctor --quiet || die` continua a dover dire perché è toccato. */
static void case_impossibile(void) {
    FmlxCliArgs a;
    Cap c;
    FmlxCliStatus s;
    char mdir[FMLX_HF_PATH_MAX * 2];

    if (geteuid() == 0) {
        printf("  (root: nessun chmod frena nessuno: caso del repo murato saltato)\n");
        return;
    }
    wipe(at("hub"));
    build_repo("buono", "org", 0, 0);
    build_repo("murato", "org", 0, 0);
    /* Il path da murare NON si scrive a memoria: `fmlx_hf_repo_dir` è LA regola (id -> directory) di
     * questa lane, e un `models--org--murato` ricopiato qui sarebbe il modo in cui il caso murerebbe
     * una directory che non esiste — verde, e senza aver provato niente. */
    if (fmlx_hf_repo_dir(at("hub"), "org/murato", mdir, sizeof mdir) != FMLX_HF_OK) {
        printf("  FIXTURE: la regola non compone il path di org/murato\n");
        failures++;
        return;
    }
    if (chmod(mdir, 0) != 0) {
        printf("  FIXTURE: chmod su %s: %s\n", mdir, strerror(errno));
        failures++;
        return;
    }
    s = run(&c, &a, 4, (char *[]){"fastllm", "doctor", "--cache", (char *)at("hub")});
    /* La directory resta murata FINO A DOPO il secondo giro: riaprirla qui avrebbe fatto passare il
     * `--quiet` su un cache sano — verde, e senza aver provato la cosa che serviva. */

    /* «Non ho potuto guardare» NON è «va tutto bene»: esce fallito. */
    CK(s == FMLX_CLI_FAILED, "un repo che non apre esce %s, atteso fallito",
       fmlx_cli_status_str(s));
    CK(HAS(c.out, "org/murato  AUDIT IMPOSSIBILE"), "non nomina il repo non aperto: %s", c.out);
    CK(HAS(c.err, "PROBLEMA"), "il PROBLEMA non è sul canale d'errore: %s", c.err);
    CK(HAS(c.err, "non si è potuto aprire"), "consiglia il download invece dei permessi: %s", c.err);
    CK(!HAS(c.err, "non è nel cache"), "un `EACCES` chiamato «non c'è»: %s", c.err);
    CK(HAS(c.out, "1 problemi"), "il riepilogo non lo conta: %s", c.out);
    /* L'altro repo DEVE essere stato audito lo stesso: era questo il difetto, in forma di verbo —
     * un diritto murato che cancellava il lavoro fatto sugli altri ventidue. */
    CK(HAS(c.out, "org/buono"), "il repo leggibile non è stato controllato: %s", c.out);
    CK(HAS(c.out, "2 ok"), "il repo leggibile non ha hashato i suoi due blob: %s", c.out);
    cap_free(&c);

    /* `--quiet`: via l'elenco, resta il PROBLEMA (sull'`err`, dove stava). */
    (void)run(&c, &a, 5,
              (char *[]){"fastllm", "doctor", "--cache", (char *)at("hub"), "--quiet"});
    chmod(mdir, 0755); /* il secondo giro è finito: si riapre, un fixture murato non si wipea */
    CK(!HAS(c.out, "AUDIT IMPOSSIBILE"), "`--quiet` stampa l'elenco: %s", c.out);
    CK(!HAS(c.out, "org/buono"), "`--quiet` nomina i repo: %s", c.out);
    CK(HAS(c.err, "PROBLEMA"), "`--quiet` nasconde il problema: %s", c.err);
    CK(!HAS(c.err, "AUDIT IMPOSSIBILE"), "`--quiet` sposta l'elenco sull'`err`: %s", c.err);
    cap_free(&c);
}

static void case_filtro(void) {
    FmlxCliArgs a;
    Cap c;
    FmlxCliStatus s;

    wipe(at("hub"));
    build_repo("due", "org", 0, 0);
    build_repo("tre", "altro", 1, 0);

    s = run(&c, &a, 5, (char *[]){"fastllm", "doctor", "--cache", (char *)at("hub"), "org/due"});
    CK(s == FMLX_CLI_OK, "filtro che matcha esce %s", fmlx_cli_status_str(s));
    CK(HAS(c.out, "org/due"), "il filtro non ha lasciato il suo repo: %s", c.out);
    CK(!HAS(c.out, "altro/tre"), "il filtro ha passato anche l'altro: %s", c.out);
    /* Il riepilogo deve dire che il cache ne ha di più: «2 repo controllati» quando ne è stato
     * filtrato uno è il modo di far sembrare completo un giro parziale. */
    CK(HAS(c.out, "1 repo controllati"), "non conta UNO controllato: %s", c.out);
    CK(HAS(c.out, "2 repo nel cache"), "non dice il totale del cache: %s", c.out);
    cap_free(&c);

    /* Il filtro che non prende niente è un FALLIMENTO, e lo dice su err. */
    s = run(&c, &a, 5,
            (char *[]){"fastllm", "doctor", "--cache", (char *)at("hub"), "nn/inesistente"});
    CK(s == FMLX_CLI_FAILED, "zero match esce %s, atteso fallito", fmlx_cli_status_str(s));
    CK(HAS(c.err, "nn/inesistente"), "zero match non nomina il filtro: %s", c.err);
    cap_free(&c);

    /* ... anche in `--quiet`, dove dello schermo non arriva niente e il codice è l'unica cosa che
     * un script legge. */
    s = run(&c, &a, 6,
            (char *[]){"fastllm", "doctor", "--cache", (char *)at("hub"), "nn/inesistente",
                       "--quiet"});
    CK(s == FMLX_CLI_FAILED, "zero match in `--quiet` esce %s", fmlx_cli_status_str(s));
    CK(c.outn == 0, "zero match in `--quiet` scrive su out: %s", c.out);
    cap_free(&c);

    /* La forma illeggibile è USAGE, e lo è PRIMA di aprire il disco: la prova è che non nomina la
     * radice e non dice «nessun repo». */
    s = run(&c, &a, 5,
            (char *[]){"fastllm", "doctor", "--cache", (char *)at("hub"), "no/such/star*"});
    CK(s == FMLX_CLI_USAGE, "filtro malformato esce %s, atteso uso", fmlx_cli_status_str(s));
    CK(HAS(c.err, "non ammesso"), "non dice che la forma è vietata: %s", c.err);
    CK(!HAS(c.err, "hub"), "ha parlato del disco prima di dire «uso»: %s", c.err);
    cap_free(&c);
}

/* Trenta e più problemi nello STESSO repo: il tetto della lane trattiene 32 verdetti, e il verbo
 * deve dirlo a parole. */
static void build_many_problems(const char *short_name) {
    char d[FMLX_HF_PATH_MAX];
    char rel[FMLX_HF_PATH_MAX * 2];
    int i;

    snprintf(d, sizeof d, "hub/models--org--%s", short_name);
    snprintf(rel, sizeof rel, "%s/refs", d);
    dir(rel);
    snprintf(rel, sizeof rel, "%s/refs/main", d);
    put(rel, REV "\n");
    for (i = 0; i < 40; i++) {
        char name[32];
        snprintf(name, sizeof name, "peso%02d.dat", i);
        snprintf(rel, sizeof rel, "%s/snapshots/%s/%s", d, REV, name);
        link_to(rel, "../../blobs/" GIT_A);
    }
}

/* Il tetto: 40 problemi in 32 righe, e la frase che ammette quelle mancanti. */
static void case_tetto(void) {
    FmlxCliArgs a;
    Cap c;
    FmlxCliStatus s;
    int rows = 0;
    const char *p;

    wipe(at("hub"));
    build_many_problems("molti");
    s = run(&c, &a, 4, (char *[]){"fastllm", "doctor", "--cache", (char *)at("hub")});
    CK(s == FMLX_CLI_FAILED, "quaranta problemi escono %s", fmlx_cli_status_str(s));
    for (p = c.out; (p = strstr(p, "PROBLEMA")) != NULL; p++) rows++;
    CK(rows == 32, "%d righe PROBLEMA, attese 32 (il tetto della lane)", rows);
    /* NOVE, non otto: il tetto conta i verdetti TOTALI, e questo repo ne produce 41 — i 40 problemi
     * più la nota «NULLA di verificabile», che nasce dagli stessi 40. 41 - 32 = 9, ed è il numero
     * che il test deve SAPERE, non quello che il testatore si aspetta. */
    CK(HAS(c.out, "9 verdetti in più non elencati"), "non ammette il taglio: %s", c.out);
    CK(HAS(c.out, "40 problemi"), "il riepilogo non conta 40: %s", c.out);
    CK(HAS(c.out, "1 note"), "il riepilogo non conta la nota: %s", c.out);
    cap_free(&c);
}

/* La radice che non c'è: fallisce, e il consiglio sull'ambiente compare SOLO quando l'ambiente è
 * di mezzo. Con `--cache` dato, dire «HF_HUB_CACHE non punta» è una frase su un flag che l'utente
 * ha appena usato. */
static void case_senza_cache(void) {
    FmlxCliArgs a;
    Cap c;
    FmlxCliStatus s;
    char ghost[FMLX_HF_PATH_MAX * 2];

    snprintf(ghost, sizeof ghost, "%s/hub-che-non-c-e", ROOT);
    s = run(&c, &a, 4, (char *[]){"fastllm", "doctor", "--cache", ghost});
    CK(s == FMLX_CLI_FAILED, "radice assente esce %s", fmlx_cli_status_str(s));
    CK(HAS(c.err, "non esiste"), "non dice che non esiste: %s", c.err);
    CK(!HAS(c.err, "HF_HUB_CACHE"), "consiglia l'ambiente con --cache dato: %s", c.err);
    cap_free(&c);

    /* Senza `--cache` e con l'ambiente puntato nel niente, la stessa riga DEVE dare il consiglio. */
    {
        const char *env = getenv("HF_HUB_CACHE");
        char keep[FMLX_HF_PATH_MAX * 2];
        snprintf(keep, sizeof keep, "%s", env ? env : "");
        setenv("HF_HUB_CACHE", ghost, 1);
        s = run(&c, &a, 2, (char *[]){"fastllm", "doctor"});
        CK(s == FMLX_CLI_FAILED, "ambiente rotto esce %s", fmlx_cli_status_str(s));
        CK(HAS(c.err, "HF_HUB_CACHE"), "senza --cache non nomina l'ambiente: %s", c.err);
        cap_free(&c);
        if (env)
            setenv("HF_HUB_CACHE", keep, 1);
        else
            unsetenv("HF_HUB_CACHE");
    }
}

/* L'argv che il verbo non legge: regola condivisa con gli altri verbi, e `doctor` è l'ultimo
 * entrato nella lista — è esattamente quello che si dimentica di metterci. */
static void case_bandiere_rifiutate(void) {
    FmlxCliArgs a;
    Cap c;

    (void)run(&c, &a, 4, (char *[]){"fastllm", "doctor", "o/r", "--repair"});
    CK(HAS(c.pmsg, "--repair"), "non nomina la bandiera rifiutata: %s", c.pmsg);
    CK(HAS(c.pmsg, "doctor"), "non nomina il verbo che la rifiuta: %s", c.pmsg);
    cap_free(&c);
    (void)run(&c, &a, 4, (char *[]){"fastllm", "doctor", "o/r", "o/t"});
    CK(HAS(c.pmsg, "posizionali") && HAS(c.pmsg, "o/t"),
       "il secondo argomento è passato in silenzio: %s", c.pmsg);
    cap_free(&c);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("uso: %s <directory-di-lavoro>\n", argv[0]);
        return 2;
    }
    snprintf(ROOT, sizeof ROOT, "%s", argv[1]);
    wipe(ROOT);
    dir("hub");

    case_pulito();
    case_corrotto();
    case_impossibile();
    case_quiet();
    case_nota_non_fallisce();
    case_filtro();
    case_tetto();
    case_senza_cache();
    case_bandiere_rifiutate();

    printf("test_doctor: %d verifiche, %d fallimenti\n", checks, failures);
    return failures ? 1 : 0;
}
