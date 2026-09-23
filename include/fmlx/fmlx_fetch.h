/* fmlx_fetch.h — download da Hugging Face Hub verso il cache del formato di fmlx_hf.h.
 *
 * La logica di cache (temp, lock, verifica, rinomina atomica, symlink, marker .no_exist) è
 * SEPARATA dal trasporto per un motivo concreto: si può provare senza rete. `fmlx_hf_download`
 * usa il transport libcurl di sistema; `fmlx_hf_download_with` accetta uno qualsiasi, e i test
 * ne usano uno finto che risponde intestazioni e byte prestabiliti. Non è uno "stub": è il
 * confine del sistema (HTTP è il mondo esterno), ed è l'unico modo di dimostrare che un file
 * riparato finisce davvero nel punto giusto del cache.
 *
 * Le intestazioni lette sono quelle che huggingface_hub legge davvero (copia `~/ai/env-mflux`,
 * `file_download.py:1623-1632`; la serie `requests` le dice a `:1478-1489` — stesse
 * regole, righe diverse: tabella delle due copie in `LICENSES.md`):
 *   commit  = X-Repo-Commit
 *   etag    = X-Linked-Etag, altrimenti ETag, "normalizzato" come lstrip("W/").strip('"')
 *             (`_normalize_etag`, file_download.py:596) — è il NOME del blob, quindi non è un dettaglio;
 *   size    = X-Linked-Size, altrimenti Content-Length.
 * REGEX_COMMIT_HASH `^[0-9a-f]{40}$` e REGEX_SHA256 `^[0-9a-f]{64}$` (file_download.py:70,73)
 * sono la STESSA regola che `fmlx_hf_blob_hash_kind` applica per lunghezza: due codici indipendenti
 * dicono la stessa cosa, e questa è la conferma che i 40 hex NON sono uno SHA1 nudo.
 *
 * Regole non negoziabili di questa lane:
 *   - MAI scrivere il blob finale direttamente: si scrive `<etag>.incomplete`, si VERIFICA, e solo
 *     allora `rename()` (stessa directory = atomica) e symlink. Un processo interrotto lascia solo
 *     un `.incomplete`, mai un blob mezzo col nome giusto. Il nome del temporaneo è DETERMINATO
 *     proprio perché un ALTRO processo — o lo stesso al riavvio — lo trovi e riprenda da dove era
 *     arrivato invece di riscaricare 30 GB. QUI SI DIVERGE da huggingface_hub, deliberatamente: lì
 *     il path logico è `<etag>.incomplete` (`:1237`) ma si scende in `<etag>.<8hex-uuid>.incomplete`
 *     (`:1908`) e il temporaneo VIEN CANCELLATO a ogni fallimento (`:1944-1947`), cioè il resume fra
 *     processi non c'è; il motivo (`:1900-1907`) è che su Lustre/GPFS/NFS `flock(2)` riesce
 *     silenziosamente a tutti. Un nome determinato è quindi più esposto a un lock non onorato:
 *     regge SOLO SE il lock è preso prima di aprire il temporaneo, sempre.
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
    /* Il server ha annunciato `X-Xet-Hash`, cioè che i byte stanno su Xet. Il backend classico è
     * una SCELTA di questa lane (il protocollo Xet non è implementato, piano §12.4), ma NON deve
     * essere silenziosa: chi scarica ha diritto di sapere che la strada percorsa è l'altra. */
    int xet_announced;
} FmlxHfMeta;

typedef struct FmlxHfFetchOpts FmlxHfFetchOpts;

typedef struct {
    void *ctx;
    /* HEAD dei metadati: DEVE compilare `out` senza scaricare il corpo. Due regole prese da
     * huggingface_hub, perché sono ciò che il server risponde in modo diverso:
     *  - `Accept-Encoding: identity` (file_download.py:1612), altrimenti Content-Length
     *    racconterebbe il corpo compresso e l'hash non combacerebbe MAI;
     *  - i redirect ASSOLUTI non si seguono (sono la risposta 302 con già negli header
     *    X-Linked-*: finiscono in `location`), quelli RELATIVI sì — HF li rifà a mano con
     *    `_httpx_follow_relative_redirects_with_backoff` (`:1615-1617`) e il `location` è la
     *    `Location` della risposta, non l'URL richiesto (`:1627-1629`: "we might have followed
     *    relative redirects") — è il caso del repo rinominato.
     */
    FmlxHfStatus (*meta)(void *ctx, const char *url, const char *token, FmlxHfMeta *out);
    /* GET con `Range: bytes=<from>-` se from > 0; APPEND su `fd`; scrive in *written i byte NUOVI
     * ricevuti. Deve onorare i redirect (compresi quelli relativi). `limit >= 0` è il MASSIMO di
     * byte nuovi accettati: un server che promette 1 KB e ne streamma 1 TB deve essere fermato
     * prima di aver riempito il disco. `limit` è la lunghezza dichiarata per il FILE INTERO, non
     * "quanto manca": se il server ignora il Range risponde con tutto il corpo, e fermarlo a metà
     * sarebbe un falso positivo che costringe a ricominciare. Se il server risponde 200 a una
     * richiesta con `from > 0` ha IGNORATO il Range: il transport DEVE azzerare il file prima di
     * scrivere (stessa guardia di file_download.py:384-396: `seek(0)` + `truncate()` +
     * `resume_size = 0`), altrimenti il temporaneo sarebbe un corpo doppio. Se il temporaneo ha già
     * `expected_size` byte, il GET non parte affatto (`:359-361`). */
    FmlxHfStatus (*get)(void *ctx, const char *url, const char *token, int64_t from, int64_t limit,
                        int fd, int64_t *written);
    /* NULL ammessa (il transport finto dei test non ne ha bisogno). Chiamata UNA volta prima del
     * primo `get`, con le opzioni effettive della chiamata: il watchdog dello stream NON può avere
     * una soglia cablata nel transport, perché è `--stall-timeout` della CLI la decisione. Portare
     * le opzioni qui dentro invece che aggiungere un parametro per capo tiene il sigillo a tre
     * funzioni (meta/get/configure) anche quando il piano §12.5 ci aggiunge il resto. */
    void (*configure)(void *ctx, const FmlxHfFetchOpts *opts);
} FmlxHfTransport;

struct FmlxHfFetchOpts {
    const char *cache_root; /* NULL = fmlx_hf_cache_root() */
    const char *endpoint;   /* NULL = fmlx_hf_endpoint() */
    const char *token;      /* NULL = getenv("HF_TOKEN") */
    int offline;            /* 1 = non chiedere mai la rete */
    int64_t max_bytes;      /* 0 = nessun limite */
    int repair;             /* 1 = permesso di sostituire un blob che non verifica */
    int wait_lock_ms;       /* >0: attende il lock altrui per questo tempo, poi ERR_LOCKED */
    /* Watchdog dello stream (piano §12.5). 0 = default: 60 s senza UN byte, e NESSUN ritentativo
     * (il comportamento di prima che queste opzioni esistessero — così chi non le tocca non vede
     * cambiare nulla). `stall_timeout_s` finisce su `CURLOPT_LOW_SPEED_TIME` con limite 1 byte/s,
     * che E' il watchdog di libcurl: niente thread, niente timer, niente segnali (AGENTS: il
     * processo che scarica non è quello che gestisce una UI).
     * `stall_retries` è il numero di RIPARTENZE dopo uno stall: ognuna chiede `Range: bytes=N-`
     * con N = byte già nel `.incomplete`, quindi un 30 GB fermi al 90% riparte dal 90%. */
    int stall_timeout_s;
    int stall_retries;
};

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
