/* test_hf_manifest.c — il manifesto dei file (piano §12.1) SENZA RETE.
 *
 * Qui non c'è nessun cache da preparare: `fmlx_hf_manifest_with` non tocca disco, e il fatto che
 * NON lo tocchi è una delle verifiche (meta/get contati e attesi a zero — un manifesto che parte
 * per la lane download scaricherebbe metà di un repo per sbaglio). Ciò che si prova è tutto su una
 * STRINGA di risposta: l'URL costruito, i campi accettati, e i rifiuti.
 *
 * Le risposte finte hanno la FORMA misurata su huggingface_hub (`hf_api.py:896-906`: `rfilename`
 * obbligatorio, `size`/`blobId`/`lfs` no) non immaginata: per questo "manca size" è un caso e non
 * un errore.
 */
#include "fmlx/fmlx_fetch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failures;

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

static const char *EP = "https://huggingface.co";
static const char *REPO = "org/repo";
/* Macro, non `const char *`: entrano nella CONCATENAZIONE dei letterali JSON qui sotto. */
#define SHA "1111111111111111111111111111111111111111"
#define BLOB_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
/* Una risposta minima ma LEGALE: la riusano i casi che interessano al RIFIUTO, dove ciò che
 * conta è il codice di ritorno e non il corpo. */
static const char *MIN1 = "{\"sha\":\"" SHA "\",\"siblings\":[{\"rfilename\":\"a.bin\"}]}";

typedef struct {
    const char *body;
    size_t len;
    FmlxHfStatus status; /* != OK: il transport fallisce COSI' prima di rispondere byte */
    int buf_calls, meta_calls, get_calls, configure_calls;
    int token_seen; /* 1 = e' arrivato un bearer NON VUOTO (mai stampato: potrebbe essere vero) */
    char url[FMLX_HF_PATH_MAX * 2];
} Fake;

static Fake *CUR;

/* Il contract di `get_buf` com'è scritto nell'header: o tutto il corpo, o ERR_BUF_SMALL. Un fake
 * che tronca farebbe passare un test sul corpo più lungo del buffer — il contrario di ciò che
 * questo file deve provare. */
static FmlxHfStatus fake_buf(void *ctx_, const char *url, const char *token, char *buf, size_t cap,
                             size_t *written) {
    Fake *f = ctx_ ? ctx_ : CUR;
    f->buf_calls++;
    snprintf(f->url, sizeof f->url, "%s", url);
    f->token_seen = token && *token;
    *written = 0;
    if (f->status != FMLX_HF_OK) return f->status;
    if (f->len > cap) return FMLX_HF_ERR_BUF_SMALL;
    memcpy(buf, f->body, f->len);
    *written = f->len;
    return FMLX_HF_OK;
}

static FmlxHfStatus fake_meta(void *ctx_, const char *u, const char *t, FmlxHfMeta *m) {
    (void)u;
    (void)t;
    (void)m;
    Fake *f = ctx_ ? ctx_ : CUR;
    f->meta_calls++;
    return FMLX_HF_ERR_IO; /* se la chiamata arriva qui, il caso è già sbagliato */
}

static FmlxHfStatus fake_get(void *ctx_, const char *u, const char *t, int64_t from, int64_t limit,
                             int fd, int64_t *written) {
    (void)u;
    (void)t;
    (void)from;
    (void)limit;
    (void)fd;
    (void)written;
    Fake *f = ctx_ ? ctx_ : CUR;
    f->get_calls++;
    return FMLX_HF_ERR_IO;
}

static void fake_configure(void *ctx_, const FmlxHfFetchOpts *o) {
    (void)o;
    Fake *f = ctx_ ? ctx_ : CUR;
    f->configure_calls++;
}

/* Ordine dei campi dell'header: ctx, meta, get, configure, get_buf. */
static const FmlxHfTransport VT = {NULL, fake_meta, fake_get, fake_configure, fake_buf};
static const FmlxHfTransport VT_NOBUF = {NULL, fake_meta, fake_get, fake_configure, NULL};

static void setup(Fake *f, const char *body) {
    memset(f, 0, sizeof *f);
    f->body = body;
    f->len = strlen(body);
    CUR = f;
}

static void opts_for(FmlxHfFetchOpts *o) {
    memset(o, 0, sizeof *o);
    o->endpoint = EP;
}

/* ---- i casi ---- */

/* Il caso per cui il manifesto esiste: i campi giusti, sottocartelle incluse. */
static void case_happy(void) {
    Fake f;
    FmlxHfFetchOpts o;
    FmlxHfManifest m;
    setup(&f, "{\"id\":\"org/repo\",\"sha\":\"" SHA "\",\"siblings\":["
              "{\"rfilename\":\"config.json\",\"blobId\":\"" BLOB_A "\",\"size\":123},"
              "{\"rfilename\":\"weights/0.bin\",\"size\":9999999},"
              "{\"rfilename\":\"README.md\"}]}");
    opts_for(&o);
    CK_ST(fmlx_hf_manifest_with(REPO, "main", &o, &VT, &m), FMLX_HF_OK);
    CK(strcmp(m.commit, SHA) == 0, "commit='%s'", m.commit);
    CK(m.n == 3, "n=%zu, atteso 3", m.n);
    if (m.n == 3 && m.files) {
        CK(strcmp(m.files[0].path, "config.json") == 0, "path[0]='%s'", m.files[0].path);
        CK(m.files[0].size == 123, "size[0]=%lld, atteso 123", (long long)m.files[0].size);
        CK(strcmp(m.files[0].git_blob_id, BLOB_A) == 0, "blobId[0]='%s'", m.files[0].git_blob_id);
        CK(m.files[0].is_lfs == 0, "is_lfs[0]=%d", m.files[0].is_lfs);
        /* Una sottocartella DEL REPO è legale (è così che stanno i pesi): il path del manifesto
         * non è un nome di blob, e `check_relpath` lo verifica come path. */
        CK(strcmp(m.files[1].path, "weights/0.bin") == 0, "path[1]='%s'", m.files[1].path);
        CK(m.files[1].size == 9999999, "size[1]=%lld", (long long)m.files[1].size);
        CK(m.files[1].git_blob_id[0] == '\0', "blobId[1]='%s', attendevo vuoto", m.files[1].git_blob_id);
        /* Nessun `size` e nessun `blobId`: il server può. Vale "non dichiarato" (-1), NON zero:
         * zero byte sarebbe un file vuoto, e un `pull` lo creerebbe vuoto. */
        CK(m.files[2].size == -1, "size[2]=%lld, atteso -1 (non dichiarato)",
           (long long)m.files[2].size);
    }
    /* Il manifesto è una richiesta di METADATI: la lane download non deve essere sfiorata. */
    CK(f.meta_calls == 0 && f.get_calls == 0, "meta=%d get=%d, attendevo 0 e 0", f.meta_calls,
       f.get_calls);
    CK(strcmp(f.url, "https://huggingface.co/api/models/org/repo/revision/main?blobs=true") == 0,
       "url='%s'", f.url);
    fmlx_hf_manifest_free(&m);
    CK(m.files == NULL && m.n == 0, "free non ha ripulito");
}

/* `lfs:{size}` quando il `size` in cima manca: è dove huggingface_hub legge la lunghezza per i
 * file grossi, e un `pull` che la perdesse perderebbe la guardia sulla dimensione. */
static void case_lfs_size(void) {
    Fake f;
    FmlxHfFetchOpts o;
    FmlxHfManifest m;
    setup(&f, "{\"sha\":\"" SHA "\",\"siblings\":[{\"rfilename\":\"m.safetensors\",\"lfs\":"
              "{\"sha256\":\"beef\",\"size\":4242}}]}");
    opts_for(&o);
    CK_ST(fmlx_hf_manifest_with(REPO, "main", &o, &VT, &m), FMLX_HF_OK);
    CK(m.n == 1 && m.files[0].is_lfs == 1, "n=%zu is_lfs=%d", m.n,
       m.n == 1 ? m.files[0].is_lfs : -1);
    CK(m.files[0].size == 4242, "size=%lld, atteso 4242 da lfs", (long long)m.files[0].size);
    fmlx_hf_manifest_free(&m);
}

/* Nessuna revisione: la rotta è quella del ramo di default, SENZA il tratto `/revision/`. */
static void case_no_revision(void) {
    Fake f;
    FmlxHfFetchOpts o;
    FmlxHfManifest m;
    setup(&f, "{\"sha\":\"" SHA "\",\"siblings\":[{\"rfilename\":\"a.bin\"}]}");
    opts_for(&o);
    CK_ST(fmlx_hf_manifest_with(REPO, NULL, &o, &VT, &m), FMLX_HF_OK);
    CK(strcmp(f.url, "https://huggingface.co/api/models/org/repo?blobs=true") == 0, "url='%s'",
       f.url);
    fmlx_hf_manifest_free(&m);
}

/* `quote(revision, safe="")` di Python: `/` diventa %2F e resta DENTRO il path. Un ramo chiamato
 * "refs/pr/7" non codificato sarebbe una richiesta a un ramo di tre segmenti. */
static void case_revision_encoded(void) {
    Fake f;
    FmlxHfFetchOpts o;
    FmlxHfManifest m;
    setup(&f, "{\"sha\":\"" SHA "\",\"siblings\":[{\"rfilename\":\"a.bin\"}]}");
    opts_for(&o);
    CK_ST(fmlx_hf_manifest_with(REPO, "refs/pr/7", &o, &VT, &m), FMLX_HF_OK);
    CK(strcmp(f.url, "https://huggingface.co/api/models/org/repo/revision/refs%2Fpr%2F7"
                     "?blobs=true") == 0,
       "url='%s'", f.url);
    fmlx_hf_manifest_free(&m);
}

/* Il bearer arriva al transport, e una variabile d'ambiente VUOTA non è un bearer. */
static void case_token(void) {
    Fake f;
    FmlxHfFetchOpts o;
    FmlxHfManifest m;
    setup(&f, "{\"sha\":\"" SHA "\",\"siblings\":[{\"rfilename\":\"a.bin\"}]}");
    opts_for(&o);
    o.token = "portato";
    CK_ST(fmlx_hf_manifest_with(REPO, "main", &o, &VT, &m), FMLX_HF_OK);
    CK(f.token_seen == 1, "il token non e' arrivato al transport");
    fmlx_hf_manifest_free(&m);

    /* `HF_TOKEN` vuoto deve valere "nessun token", come nella lane download. */
    setup(&f, MIN1);
    opts_for(&o);
    o.token = "";
    unsetenv("HF_TOKEN");
    CK_ST(fmlx_hf_manifest_with(REPO, "main", &o, &VT, &m), FMLX_HF_OK);
    CK(f.token_seen == 0, "un token vuoto e' stato passato come se autorizzasse");
    fmlx_hf_manifest_free(&m);
}

/* Un elenco vuoto NON è "un repo senza file": è la risposta inaffidabile che `_snapshot_download`
 * dichiara tale, e qui diventa un errore. */
static void case_empty_siblings(void) {
    Fake f;
    FmlxHfFetchOpts o;
    FmlxHfManifest m;
    setup(&f, "{\"sha\":\"" SHA "\",\"siblings\":[]}");
    opts_for(&o);
    CK_ST(fmlx_hf_manifest_with(REPO, "main", &o, &VT, &m), FMLX_HF_ERR_HTTP);
    CK(m.files == NULL && m.n == 0 && m.commit[0] == '\0', "out non e' azzerato");
}

static void case_no_siblings_key(void) {
    Fake f;
    FmlxHfFetchOpts o;
    FmlxHfManifest m;
    setup(&f, "{\"sha\":\"" SHA "\"}");
    opts_for(&o);
    CK_ST(fmlx_hf_manifest_with(REPO, "main", &o, &VT, &m), FMLX_HF_ERR_HTTP);
}

/* Uno `sha` che non è 40 hex non si "aggiusta" col ref locale: il ref locale puo' essere di ieri. */
static void case_bad_sha(void) {
    Fake f;
    FmlxHfFetchOpts o;
    FmlxHfManifest m;
    setup(&f, "{\"sha\":\"main\",\"siblings\":[{\"rfilename\":\"a.bin\"}]}");
    opts_for(&o);
    CK_ST(fmlx_hf_manifest_with(REPO, "main", &o, &VT, &m), FMLX_HF_ERR_HTTP);
    CK(m.commit[0] == '\0' && m.files == NULL && m.n == 0, "un commit illegale e' stato lasciato "
                                                           "in `out`");
}

static void case_sha_not_a_string(void) {
    Fake f;
    FmlxHfFetchOpts o;
    FmlxHfManifest m;
    setup(&f, "{\"sha\":42,\"siblings\":[{\"rfilename\":\"a.bin\"}]}");
    opts_for(&o);
    CK_ST(fmlx_hf_manifest_with(REPO, "main", &o, &VT, &m), FMLX_HF_ERR_HTTP);
}

/* Il `rfilename` arriva dal network: `../` deve restare un ERRORE, non un file da scaricare fuori
 * dal cache. E non si salta il caso: si rifiuta il manifesto intero. */
static void case_evil_rfilename(void) {
    Fake f;
    FmlxHfFetchOpts o;
    FmlxHfManifest m;
    setup(&f, "{\"sha\":\"" SHA "\",\"siblings\":[{\"rfilename\":\"ok.bin\"},"
              "{\"rfilename\":\"../scappa.bin\"}]}");
    opts_for(&o);
    CK_ST(fmlx_hf_manifest_with(REPO, "main", &o, &VT, &m), FMLX_HF_ERR_NAME);
    CK(m.files == NULL && m.n == 0, "lista a meta' lasciata in giro: n=%zu files=%p", m.n,
       (void *)m.files);
}

static void case_absolute_rfilename(void) {
    Fake f;
    FmlxHfFetchOpts o;
    FmlxHfManifest m;
    setup(&f, "{\"sha\":\"" SHA "\",\"siblings\":[{\"rfilename\":\"/etc/passwd\"}]}");
    opts_for(&o);
    CK_ST(fmlx_hf_manifest_with(REPO, "main", &o, &VT, &m), FMLX_HF_ERR_NAME);
}

/* Un nome oltre il buffer non si TRONCA: un path tagliato sarebbe un ALTRO file, e il `pull`
 * scaricherebbe qualcos'altro credendo di aver scaricato questo. */
static void case_rfilename_too_long(void) {
    Fake f;
    FmlxHfFetchOpts o;
    FmlxHfManifest m;
    size_t name_len = FMLX_HF_PATH_MAX + 16;
    size_t cap = name_len + 256, len;
    char *body = (char *)malloc(cap);
    CK(body != NULL, "malloc della fixture fallita");
    if (!body) return;
    len = (size_t)snprintf(body, cap, "{\"sha\":\"%s\",\"siblings\":[{\"rfilename\":\"", SHA);
    memset(body + len, 'a', name_len);
    len += name_len;
    memcpy(body + len, "\"}]}", 6); /* 5 byte + NUL: `setup` misura da qui */
    setup(&f, body);
    opts_for(&o);
    CK_ST(fmlx_hf_manifest_with(REPO, "main", &o, &VT, &m), FMLX_HF_ERR_NAME);
    free(body);
}

/* La pagina di errore di un proxy non è JSON: deve diventare un rifiuto, non "repo senza file". */
static void case_not_json(void) {
    Fake f;
    FmlxHfFetchOpts o;
    FmlxHfManifest m;
    setup(&f, "<html><body>502 Bad Gateway</body></html>");
    opts_for(&o);
    CK_ST(fmlx_hf_manifest_with(REPO, "main", &o, &VT, &m), FMLX_HF_ERR_HTTP);
}

/* Un transport che non sa fare la GET a memoria NON inventa un elenco: ERR_IO. */
static void case_transport_without_getbuf(void) {
    Fake f;
    FmlxHfFetchOpts o;
    FmlxHfManifest m;
    setup(&f, "{\"sha\":\"" SHA "\",\"siblings\":[{\"rfilename\":\"a.bin\"}]}");
    opts_for(&o);
    CK_ST(fmlx_hf_manifest_with(REPO, "main", &o, &VT_NOBUF, &m), FMLX_HF_ERR_IO);
    CK(f.buf_calls == 0, "chiamato lo stesso get_buf (che non c'e'): %d", f.buf_calls);
}

/* Il corpo troppo lungo lo dice il transport: qui dev'essere RIPORTATO, non interpretato come una
 * risposta JSON parziale. */
static void case_body_too_large(void) {
    Fake f;
    FmlxHfFetchOpts o;
    FmlxHfManifest m;
    setup(&f, "{\"sha\":\"" SHA "\",\"siblings\":[{\"rfilename\":\"a.bin\"}]}");
    f.status = FMLX_HF_ERR_BUF_SMALL;
    opts_for(&o);
    CK_ST(fmlx_hf_manifest_with(REPO, "main", &o, &VT, &m), FMLX_HF_ERR_BUF_SMALL);
}

/* Un 404 sul manifesto MISURATO sul server vero (`.../revision/refs%2Fpr%2F1?blobs=true` risponde
 * 404): il codice resta "non sul remote" — `pull` non deve ritentare — ma il messaggio non può
 * promettere un marker `.no_exist` che questa lane NON scrive, perché i manifesti non stanno in
 * cache. Qui si prova proprio quella parola. */
static void case_remote_404(void) {
    Fake f;
    FmlxHfFetchOpts o;
    FmlxHfManifest m;
    setup(&f, MIN1);
    f.status = FMLX_HF_ERR_NOT_ON_REMOTE;
    opts_for(&o);
    CK_ST(fmlx_hf_manifest_with(REPO, "refs/pr/1", &o, &VT, &m), FMLX_HF_ERR_NOT_ON_REMOTE);
    {
        const char *err = fmlx_hf_last_error();
        CK(err && strstr(err, "nessun marker") != NULL,
           "il 404 del manifesto racconta un marker che non viene scritto: '%s'",
           err ? err : "(nessun messaggio)");
        CK(err && strstr(err, "refs/pr/1") != NULL, "il messaggio non dice quale revisione: '%s'",
           err ? err : "(nessun messaggio)");
    }
}

/* `pull` con la rete spenta: un errore che dice "serve la rete", non una lista vuota che
 * significherebbe "non c'è niente da scaricare". */
static void case_offline(void) {
    Fake f;
    FmlxHfFetchOpts o;
    FmlxHfManifest m;
    setup(&f, "{\"sha\":\"" SHA "\",\"siblings\":[{\"rfilename\":\"a.bin\"}]}");
    opts_for(&o);
    o.offline = 1;
    CK_ST(fmlx_hf_manifest_with(REPO, "main", &o, &VT, &m), FMLX_HF_ERR_OFFLINE);
    CK(f.buf_calls == 0, "offline ha comunque chiamato il transport (%d volte)", f.buf_calls);

    setup(&f, MIN1);
    opts_for(&o);
    setenv("HF_HUB_OFFLINE", "1", 1);
    CK_ST(fmlx_hf_manifest_with(REPO, "main", &o, &VT, &m), FMLX_HF_ERR_OFFLINE);
    unsetenv("HF_HUB_OFFLINE");
}

/* Il numero di voci lo DECIDE il server, e ogni voce costa FMLX_HF_PATH_MAX byte: il tetto c'è
 * perché la nostra malloc non sia una funzione di una risposta. */
static void case_too_many_files(void) {
    Fake f;
    FmlxHfFetchOpts o;
    FmlxHfManifest m;
    size_t cap = 16u * 1024u * 1024u, len = 0;
    char *body = (char *)malloc(cap);
    int i;
    CK(body != NULL, "malloc della fixture fallita");
    if (!body) return;
    len = (size_t)snprintf(body, cap, "{\"sha\":\"%s\",\"siblings\":[", SHA);
    for (i = 0; i < 5000; i++)
        len += (size_t)snprintf(body + len, cap - len, "%s{\"rfilename\":\"f%d\"}", i ? "," : "", i);
    len += (size_t)snprintf(body + len, cap - len, "]}");
    setup(&f, body);
    f.len = len;
    opts_for(&o);
    CK_ST(fmlx_hf_manifest_with(REPO, "main", &o, &VT, &m), FMLX_HF_ERR_TOO_MANY);
    CK(m.files == NULL && m.n == 0, "tetto superato e lista lo stesso popolate");
    free(body);
}

/* Un `size` reale (1.5) non diventa 1 in silenzio: vale come "non dichiarato". */
static void case_size_is_real(void) {
    Fake f;
    FmlxHfFetchOpts o;
    FmlxHfManifest m;
    setup(&f, "{\"sha\":\"" SHA "\",\"siblings\":[{\"rfilename\":\"a.bin\",\"size\":1.5}]}");
    opts_for(&o);
    CK_ST(fmlx_hf_manifest_with(REPO, "main", &o, &VT, &m), FMLX_HF_OK);
    CK(m.n == 1 && m.files[0].size == -1, "size=%lld, attendevo -1",
       m.n == 1 ? (long long)m.files[0].size : -2);
    fmlx_hf_manifest_free(&m);
}

int main(void) {
    /* L'ambiente reale NON deve decidere l'esito: se la shell ha HF_HUB_OFFLINE o HF_ENDPOINT,
     * i casi "serve la rete" mentirebbero. */
    unsetenv("HF_HUB_OFFLINE");
    unsetenv("TRANSFORMERS_OFFLINE");
    unsetenv("HF_ENDPOINT");
    unsetenv("HF_TOKEN");

    case_happy();
    case_lfs_size();
    case_no_revision();
    case_revision_encoded();
    case_token();
    case_empty_siblings();
    case_no_siblings_key();
    case_bad_sha();
    case_sha_not_a_string();
    case_evil_rfilename();
    case_absolute_rfilename();
    case_rfilename_too_long();
    case_not_json();
    case_transport_without_getbuf();
    case_body_too_large();
    case_remote_404();
    case_offline();
    case_too_many_files();
    case_size_is_real();

    printf("test_hf_manifest: %d verifiche, %d fallimenti\n", checks, failures);
    return failures ? 1 : 0;
}
