/* fmlx_fetch.h — download da Hugging Face Hub verso il cache del formato di fmlx_hf.h.
 *
 * La logica di cache (temp, lock, verifica, rinomina atomica, symlink, marker .no_exist) è
 * SEPARATA dal trasporto per un motivo concreto: si può provare senza rete. `fmlx_hf_download`
 * usa il transport libcurl di sistema; `fmlx_hf_download_with` accetta uno qualsiasi, e i test
 * ne usano uno finto che risponde intestazioni e byte prestabiliti. Non è uno "stub": è il
 * confine del sistema (HTTP è il mondo esterno), ed è l'unico modo di dimostrare che un file
 * riparato finisce davvero nel punto giusto del cache.
 *
 * Le intestazioni lette sono quelle che huggingface_hub legge davvero (misurato in
 * `file_download.py:1478-1489`):
 *   commit  = X-Repo-Commit
 *   etag    = X-Linked-Etag, altrimenti ETag, "normalizzato" come lstrip("W/").strip('"')
 *             (`file_download.py:642-660`) — è il NOME del blob, quindi non è un dettaglio;
 *   size    = X-Linked-Size, altrimenti Content-Length.
 * REGEX_COMMIT_HASH `^[0-9a-f]{40}$` e REGEX_SHA256 `^[0-9a-f]{64}$` (file_download.py:81,84)
 * sono la STESSA regola che `fmlx_hf_blob_hash_kind` applica per lunghezza: due codici indipendenti
 * dicono la stessa cosa, e questa è la conferma che i 40 hex NON sono uno SHA1 nudo.
 *
 * Regole non negoziabili di questa lane:
 *   - MAI scrivere il blob finale direttamente: si scrive `<etag>.incomplete`, si VERIFICA, e solo
 *     allora `rename()` (stessa directory = atomica) e symlink. Un processo interrotto lascia solo
 *     un `.incomplete`, mai un blob mezzo col nome giusto. Il nome del temporaneo è DETERMINATO
 *     proprio perché un ALTRO processo — o lo stesso al riavvio — lo trovi e riprenda da dove era
 *     arrivato invece di riscaricare 30 GB (huggingface_hub file_download.py:1176).
 *   - Lock: `<root>/.locks/<repo-dir>/<etag>.lock`, trattenuto con `flock(LOCK_EX|LOCK_NB)` finché
 *     il fd resta aperto. Il file ESISTE anche quando è libero (è così nel cache reale, dove i lock
 *     non si cancellano mai), quindi "c'è" non vuol dire "è preso": lo dice solo flock. Scaduti
 *     `wait_lock_ms` è FMLX_HF_ERR_LOCKED.
 *   - Il commit dove finiscono i byte è X-Repo-Commit di QUESTA risposta, non refs/<ramo> di ieri;
 *     il ref si riscrive dopo, mai prima.
 *   - MAI sovrascrivere un blob che non verifica: serve `repair != 0` esplicito.
 *   - Il token non finisce MAI in un messaggio d'errore (le funzioni di questa lane non lo
 *     stampano e `fmlx_hf_last_error` non lo contiene mai).
 */
#ifndef FMLX_FETCH_H
#define FMLX_FETCH_H

#include "fmlx/fmlx_hf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Ciò che il server dice di un file. */
typedef struct {
    char commit[64];   /* X-Repo-Commit, 40 hex (può essere vuoto: non sempre presente) */
    char etag[128];    /* normalizzato: è il nome del blob. Vuoto => FMLX_HF_ERR_NO_ETAG */
    char location[FMLX_HF_PATH_MAX * 2]; /* dove GETtare davvero: il Location di HF verso il CDN */
    int64_t size;      /* dichiarato; -1 se il server non l'ha detto => FMLX_HF_ERR_NO_SIZE */
    int status;        /* codice HTTP osservato */
    int not_found;     /* 404/405 => il marker .no_exist va scritto */
} FmlxHfMeta;

typedef struct {
    void *ctx;
    /* HEAD dei metadati: DEVE compilare `out` senza scaricare il corpo. Due regole prese da
     * huggingface_hub, perché sono ciò che il server risponde in modo diverso:
     *  - `Accept-Encoding: identity`, altrimenti Content-Length racconterebbe il corpo compresso
     *    (constants in file_download.py:1470) e l'hash non combacerebbe MAI;
     *  - i redirect ASSOLUTI non si seguono (sono la risposta 302 con già negli header
     *    X-Linked-*: finiscono in `location`), quelli RELATIVI sì — HF li rifà a mano
     *    (`_request_wrapper`, file_download.py:282-302) per il caso di repo rinominato.
     */
    FmlxHfStatus (*meta)(void *ctx, const char *url, const char *token, FmlxHfMeta *out);
    /* GET con `Range: bytes=<from>-` se from > 0; APPEND su `fd`; scrive in *written i byte NUOVI
     * ricevuti. Deve onorare i redirect (compresi quelli relativi). `limit >= 0` è il MASSIMO di
     * byte nuovi accettati: un server che promette 1 KB e ne streamma 1 TB deve essere fermato
     * prima di aver riempito il disco. `limit` è la lunghezza dichiarata per il FILE INTERO, non
     * "quanto manca": se il server ignora il Range risponde con tutto il corpo, e fermarlo a metà
     * sarebbe un falso positivo che costringe a ricominciare. Se il server risponde 200 a una
     * richiesta con `from > 0` ha IGNORATO il Range: il transport DEVE azzerare il file prima di
     * scrivere (stessa guardia di file_download.py:429-433), altrimenti il temporaneo sarebbe un
     * corpo doppio. */
    FmlxHfStatus (*get)(void *ctx, const char *url, const char *token, int64_t from, int64_t limit,
                        int fd, int64_t *written);
} FmlxHfTransport;

typedef struct {
    const char *cache_root; /* NULL = fmlx_hf_cache_root() */
    const char *endpoint;   /* NULL = fmlx_hf_endpoint() */
    const char *token;      /* NULL = getenv("HF_TOKEN") */
    int offline;            /* 1 = non chiedere mai la rete */
    int64_t max_bytes;      /* 0 = nessun limite */
    int repair;             /* 1 = permesso di sostituire un blob che non verifica */
    int wait_lock_ms;       /* >0: attende il lock altrui per questo tempo, poi ERR_LOCKED */
} FmlxHfFetchOpts;

/* Stato del transport libcurl di sistema. Init/finalize NON sono richiesti: `acquire` crea
 * l'handle quando serve. Il transport è riusato tra chiamate (handle reuse = keepalive). */
const FmlxHfTransport *fmlx_hf_transport_curl(void);

/* Scarica (o ritrova) `relpath` di `repo_id` alla `revision`. Riempie `*out` come
 * `fmlx_hf_locate` farebbe dopo il successo. `opts == NULL` = tutto di default.
 * Esiti attesi e distinti:
 *   OK                 già nel cache e verificato, oppure scaricato e verificato
 *   ERR_OFFLINE        serve la rete ma l'ambiente (o opts) la vieta
 *   ERR_NOT_ON_REMOTE  il server ha detto 404 (e il marker è stato scritto)
 *   ERR_LOCKED         un altro processo sta scaricando lo stesso blob
 *   ERR_HASH_MISMATCH  il blob scaricato non corrisponde al suo nome
 *   ERR_SIZE           tronco o più lungo di quanto il server ha dichiarato */
FmlxHfStatus fmlx_hf_download(const char *repo_id, const char *revision, const char *relpath,
                              const FmlxHfFetchOpts *opts, FmlxHfRef *out);

FmlxHfStatus fmlx_hf_download_with(const char *repo_id, const char *revision, const char *relpath,
                                   const FmlxHfFetchOpts *opts, const FmlxHfTransport *t,
                                   FmlxHfRef *out);

/* Normalizzazione dell'etag esattamente come huggingface_hub (lstrip("W/") + strip('"')):
 * esportata perché è una decisione su un NAME, e i test la provano da sola. */
void fmlx_hf_normalize_etag(const char *raw, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* FMLX_FETCH_H */
