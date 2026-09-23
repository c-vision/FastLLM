/* fmlx_hf_manifest.c — la lista dei file di un repo (piano §12.1): ciò che serve a `pull` per
 * sapere COSA scaricare, prima di scaricare.
 *
 * Perché un file separato: il download (fmlx_hf_fetch.c) parla di UN file e tocca il cache; questo
 * parla di TUTTI i file e del cache non sa nulla. Condividono l'endpoint, il token e il sigillo
 * `FmlxHfTransport`, che è il confine giusto: anche qui i test girano senza rete.
 *
 * La forma della risposta è MISURATA sul client di riferimento, non immaginata:
 *  - URL: `{endpoint}/api/models/{repo_id}` più `/revision/{quote(revision)}` quando la revisione
 *    c'è, e `?blobs=true` per avere `size`/`blobId` — `hf_api.py:2648-2657`. La revisione vive nel
 *    PATH, non nella query: chi indovina l'URL chiede un ramo chiamato "None" invece del commit.
 *    `repo_id` NON è codificato, perché il client non lo codifica (`f"...api/models/{repo_id}"`):
 *    la stessa richiesta di chi condivide il cache con noi, non la nostra idea di URL corretto.
 *  - campi: `sha` e `siblings[].rfilename|size|blobId|lfs` — `hf_api.py:896-906`, dove `rfilename`
 *    è l'UNICO obbligatorio (gli altri tre sono `.get()`, cioè possono mancare).
 *  - `_snapshot_download.py:259-264` usa ESATTAMENTE questa lista per decidere cosa scaricare, e
 *    dichiara inaffidabile un `siblings` vuoto: anche qui un elenco vuoto è un ERRORE, non un repo
 *    da zero file ("nessun risultato silenzioso").
 */
#include "fmlx/fmlx_fetch.h"
#include "fmlx/fmlx_json.h"
#include "fmlx_hf_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 1 MiB di JSON sul corpo della risposta: è un tetto nostro su quello che il server può farci
 * mallocare, non una dimensione attesa di un manifesto (quelli misurati fanno ~1 KiB). */
#define MANIFEST_BODY_MAX (1024u * 1024u)
/* Il tetto sulle voci, e il PERCHÉ esiste: `FmlxHfFile` costa ~4.2 KiB (il path è inline a
 * FMLX_HF_PATH_MAX perché `pull` lo passa tale e quale a `fmlx_hf_download`), e 1 MiB di JSON può
 * nominare ~61.000 file (`{"rfilename":"x"}` = 17 byte): senza questo tetto la dimensione della
 * nostra malloc la deciderebbe una risposta.
 * MISURATO sul server vero in questa sessione: bert-base-uncased = 16 file,
 * mlx-community/Meta-Llama-3.1-8B-Instruct-4bit = 8. 4096 non è "il massimo osservato" — non ho
 * contato i repo più grossi che esistano — è un margine SCELTO, e SUPERARLO è un errore distinto:
 * un `pull` che salta file in silenzio è peggio di un `pull` che fallisce. */
#define MANIFEST_FILES_MAX 4096u

/* Lo `sha` della risposta è il NOME della directory di snapshot dove i file verranno: la stessa
 * regola di 40 hex che vale per `refs/<ramo>` e per `X-Repo-Commit`, letta da una funzione sola. */
static int sha_is_commit(const char *s) {
    return fmlx_hf_blob_hash_kind(s) == FMLX_HF_HASH_GIT_BLOB;
}

/* Il `size` del server: solo un intero. Un reale (1.5) NON diventa 1 in silenzio — è la stessa
 * regola che il parser JSON impone alle richieste — ma qui non è una colpa della risposta: è un
 * valore che non sappiamo usare, e vale come "non dichiarato". */
static int64_t json_size(const FmlxJsonValue *v) {
    int64_t n = -1;
    if (!v || !fmlx_json_as_i64(v, &n) || n < 0) return -1;
    return n;
}

/* Una stringa della risposta in un buffer a dimensione fissa. Assente = vuoto (il server può non
 * dare `blobId`); presente e lunga troppo = ERRORE, non troncamento: un path troncato a 255 byte
 * sarebbe un file diverso da quello chiesto, e il download lo creerebbe comunque. */
static FmlxHfStatus copy_str(const FmlxJsonValue *v, char *out, size_t out_len, const char *what,
                             const char *ctx) {
    const char *s = NULL;
    size_t len = 0;
    if (!v) return FMLX_HF_OK;
    if (!fmlx_json_as_str(v, &s, &len))
        return fmlx_hf_fail(FMLX_HF_ERR_HTTP, "manifesto: '%s' non e' una stringa (%s)", what,
                            ctx ? ctx : "?");
    if (len >= out_len)
        return fmlx_hf_fail(FMLX_HF_ERR_NAME, "manifesto: '%s' di %zu byte troppo lungo (%s)", what,
                            len, ctx ? ctx : "?");
    memcpy(out, s, len);
    out[len] = '\0';
    return FMLX_HF_OK;
}

/* UN elemento di `siblings`. Un `rfilename` che non è un path legale NON si salta: saltare un file
 * di un repo che si sta scaricando intero è esattamente il risultato silenzioso che il piano vieta,
 * e il nome arriva dal network. */
static FmlxHfStatus one_sibling(const FmlxJsonValue *obj, FmlxHfFile *out) {
    const FmlxJsonValue *lfs;
    FmlxHfStatus s;

    memset(out, 0, sizeof *out);
    out->size = -1;
    if (!obj) return fmlx_hf_fail(FMLX_HF_ERR_HTTP, "manifesto: un elemento di siblings non e' un oggetto");
    s = copy_str(fmlx_json_obj_get(obj, "rfilename"), out->path, sizeof out->path, "rfilename", NULL);
    if (s != FMLX_HF_OK) return s;
    if (!out->path[0]) return fmlx_hf_fail(FMLX_HF_ERR_NAME, "manifesto: rfilename vuoto");
    if (fmlx_hf_check_relpath(out->path) != FMLX_HF_OK)
        return fmlx_hf_fail(FMLX_HF_ERR_NAME, "manifesto: rfilename inaccettabile '%s'", out->path);
    s = copy_str(fmlx_json_obj_get(obj, "blobId"), out->git_blob_id, sizeof out->git_blob_id,
                 "blobId", out->path);
    if (s != FMLX_HF_OK) return s;
    out->size = json_size(fmlx_json_obj_get(obj, "size"));
    lfs = fmlx_json_obj_get(obj, "lfs");
    if (lfs) {
        /* `lfs` presente = nel cache il blob si chiama con lo SHA256 (64 hex), non con il git blob
         * id: misurato su `model.safetensors` (blobId 40 hex, `X-Linked-ETag` 64 hex). È il motivo
         * per cui `pull` deve chiedere il NOME al download lane e non dedurlo da `git_blob_id`. */
        out->is_lfs = 1;
        if (out->size < 0) out->size = json_size(fmlx_json_obj_get(lfs, "size"));
    }
    return FMLX_HF_OK;
}

FmlxHfStatus fmlx_hf_manifest_with(const char *repo_id, const char *revision,
                                   const FmlxHfFetchOpts *opts, const FmlxHfTransport *t,
                                   FmlxHfManifest *out) {
    FmlxHfFetchOpts d;
    char ep[FMLX_HF_PATH_MAX], url[FMLX_HF_PATH_MAX * 2], rev[FMLX_HF_PATH_MAX * 3];
    const char *token, *endpoint;
    char *body;
    size_t got = 0, n, i;
    FmlxJsonDoc doc;
    const FmlxJsonValue *root, *sibs;
    FmlxHfStatus s;

    memset(out, 0, sizeof *out);
    if (opts) d = *opts;
    else memset(&d, 0, sizeof d);
    if (!t || !t->get_buf)
        return fmlx_hf_fail(FMLX_HF_ERR_IO, "il transport non sa fare una GET a memoria: il "
                                            "manifesto non e' disponibile");
    if (!repo_id || !*repo_id) return fmlx_hf_fail(FMLX_HF_ERR_REPO_ID, "repo id vuoto");
    /* Non esiste un manifesto nel cache (huggingface_hub non lo scrive): offline qui vuol dire
     * RIFIUTARE, mai "lista vuota", che sarebbe il repo senza file. */
    if (d.offline || fmlx_hf_offline())
        return fmlx_hf_fail(FMLX_HF_ERR_OFFLINE, "manifesto di %s: serve la rete e l'ambiente la "
                                                 "vieta", repo_id);

    endpoint = (d.endpoint && *d.endpoint) ? d.endpoint : NULL;
    if (!endpoint) {
        s = fmlx_hf_endpoint(ep, sizeof ep);
        if (s != FMLX_HF_OK) return s;
        endpoint = ep;
    }
    token = d.token;
    if (!token || !*token) token = getenv("HF_TOKEN");
    if (token && !*token) token = NULL; /* una variabile vuota non è un bearer token */

    if (revision && *revision) {
        if (fmlx_hf_pct_encode(revision, "", rev, sizeof rev) != 0)
            return fmlx_hf_fail(FMLX_HF_ERR_BUF_SMALL, "revisione codificata non sta in buffer");
        if (snprintf(url, sizeof url, "%s/api/models/%s/revision/%s?blobs=true", endpoint, repo_id,
                     rev) >= (int)sizeof url)
            return fmlx_hf_fail(FMLX_HF_ERR_BUF_SMALL, "URL del manifesto troppo lungo");
    } else if (snprintf(url, sizeof url, "%s/api/models/%s?blobs=true", endpoint, repo_id) >=
               (int)sizeof url) {
        return fmlx_hf_fail(FMLX_HF_ERR_BUF_SMALL, "URL del manifesto troppo lungo");
    }

    /* +1 byte: è dove il parser può trovare il NUL senza che nessuno conti su di esso. Il corpo
     * più lungo del limite lo tronca il transport con ERR_BUF_SMALL (vedi curl_get_buf). */
    body = (char *)malloc(MANIFEST_BODY_MAX + 1);
    if (!body) return fmlx_hf_fail(FMLX_HF_ERR_IO, "malloc del manifesto (%u byte) rifiutata",
                                   MANIFEST_BODY_MAX);
    s = t->get_buf(t->ctx, url, token, body, MANIFEST_BODY_MAX, &got);
    if (s != FMLX_HF_OK) {
        if (s == FMLX_HF_ERR_NOT_ON_REMOTE) {
            /* 404 su una richiesta di MANIFESTO: per un FILE vuol dire "scrivi il marker .no_exist",
             * ma qui il marker non esiste (huggingface_hub non lo scrive per l'API, e misurato sul
             * server: `.../revision/refs%2Fpr%2F1?blobs=true` risponde 404 e basta). Stesso codice
             * — `pull` non deve ritentare — messaggio che non promette una cosa mai fatta. */
            s = fmlx_hf_fail(FMLX_HF_ERR_NOT_ON_REMOTE,
                             "manifesto di %s@%s: il server non conosce questo repo o questa "
                             "revisione (HTTP 404, nessun marker scritto: i manifesti non stanno "
                             "nel cache)",
                             repo_id, (revision && *revision) ? revision : "(ramo di default)");
        }
        goto free_body; /* HTTP, BUF_SMALL o stall: il motivo lo dice il transport */
    }
    body[got] = '\0';

    fmlx_json_doc_init(&doc);
    /* `max_nodes = 0`: è il codec che DECIDE il tetto dai byte (`fmlx_json_node_bound`, con il suo
     * tetto di processo), non noi. Una stima nostra sarebbe una SECONDA regola sulla stessa cosa —
     * e sbagliata: la forma più densa che JSON produce (`[1,1,1...]`) fa un nodo ogni DUE byte, non
     * uno ogni sette come avevo supposto. Un corpo che il parser rifiuta per Too Big torna comunque
     * qui come errore distinto, mai come lista monca. */
    if (fmlx_json_parse(body, got, 0, &doc) != FMLX_JSON_OK) {
        s = fmlx_hf_fail(FMLX_HF_ERR_HTTP, "manifesto di %s: la risposta non e' JSON (%s, offset %zu)",
                         repo_id, doc.err_msg, doc.err_off);
        fmlx_json_doc_free(&doc);
        goto free_body;
    }
    root = fmlx_json_root(&doc);
    if (!root) {
        s = fmlx_hf_fail(FMLX_HF_ERR_HTTP, "manifesto di %s: documento senza radice", repo_id);
        fmlx_json_doc_free(&doc);
        goto free_body;
    }
    s = copy_str(fmlx_json_obj_get(root, "sha"), out->commit, sizeof out->commit, "sha", repo_id);
    if (s != FMLX_HF_OK) {
        fmlx_json_doc_free(&doc);
        goto free_body;
    }
    /* Un `sha` che non è 40 hex non è "mi arreglo col ref locale": il ref locale può essere di
     * ieri, e descriverebbe come "ieri" dei byte letti oggi. Si rifiuta, con la causa. */
    if (!sha_is_commit(out->commit)) {
        s = fmlx_hf_fail(FMLX_HF_ERR_HTTP, "manifesto di %s: lo sha del server ('%s') non e' un "
                                           "commit a 40 hex", repo_id, out->commit);
        fmlx_json_doc_free(&doc);
        goto free_body;
    }
    sibs = fmlx_json_obj_get(root, "siblings");
    n = fmlx_json_arr_n(sibs);
    if (!sibs || n == 0) {
        s = fmlx_hf_fail(FMLX_HF_ERR_HTTP, "manifesto di %s: nessun file in siblings (il server "
                                           "ha risposto in modo inaffidabile)", repo_id);
        fmlx_json_doc_free(&doc);
        goto free_body;
    }
    /* Il tetto sulle voci: `n` arriva dal network e moltiplica 4 KiB. SUPERARLO è un errore
     * distinto, non un "prendiamo i primi n": un `pull` che salta file in silenzio è peggio di
     * un `pull` che fallisce. */
    if (n > MANIFEST_FILES_MAX) {
        s = fmlx_hf_fail(FMLX_HF_ERR_TOO_MANY, "manifesto di %s: %zu file in siblings, oltre il "
                                               "limite di %u (serve un filtro sul path)", repo_id, n,
                         MANIFEST_FILES_MAX);
        fmlx_json_doc_free(&doc);
        goto free_body;
    }
    out->files = (FmlxHfFile *)calloc(n, sizeof *out->files);
    if (!out->files) {
        s = fmlx_hf_fail(FMLX_HF_ERR_IO, "calloc di %zu voci del manifesto rifiutata", n);
        fmlx_json_doc_free(&doc);
        goto free_body;
    }
    for (i = 0; i < n; i++) {
        s = one_sibling(fmlx_json_arr_at(sibs, i), &out->files[i]);
        if (s != FMLX_HF_OK) {
            fmlx_json_doc_free(&doc);
            fmlx_hf_manifest_free(out); /* `out` torna azzerata: chi non vede OK non deve poter usare una lista metà */
            goto free_body;
        }
    }
    out->n = n;
    fmlx_json_doc_free(&doc);
    free(body);
    return FMLX_HF_OK;

free_body:
    /* Chi non vede OK non deve poter leggere NULLA: lo `sha` è stato copiato prima di essere
     * validato, e un `commit` mezzo valido in mano a `pull` varrebbe uno snapshot nel posto
     * sbagliato. (I `files` già liberati da `fmlx_hf_manifest_free` sul percorso che li aveva
     * allocati: qui l'azzeramento è solo la conferma, non la pulizia.) */
    memset(out, 0, sizeof *out);
    free(body);
    return s;
}

FmlxHfStatus fmlx_hf_manifest(const char *repo_id, const char *revision,
                              const FmlxHfFetchOpts *opts, FmlxHfManifest *out) {
    const FmlxHfTransport *t = fmlx_hf_transport_curl();
    if (!t) return fmlx_hf_fail(FMLX_HF_ERR_IO, "transport libcurl non disponibile");
    return fmlx_hf_manifest_with(repo_id, revision, opts, t, out);
}

void fmlx_hf_manifest_free(FmlxHfManifest *m) {
    if (!m) return;
    free(m->files);
    memset(m, 0, sizeof *m);
}
