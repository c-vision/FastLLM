/* fmlx_hf.h — cache locale di Hugging Face Hub: lettura, percorsi, verifica (lane hf).
 *
 * Questa lane NON tocca la rete. Risolve, valida e verifica ciò che è già su disco, e
 * costruisce gli URL per chi scarica. La rete vive in `fmlx_hf_fetch.c`, separata proprio
 * perché qui ogni funzione è testabile in una macchina senza rete e senza credenziali.
 *
 * Il layout che si supporta è STATO MISURATO su questa macchina, non immaginato:
 *   <root>/CACHEDIR.TAG
 *   <root>/models--<org>--<name>/refs/<branch>            testo, 40-hex
 *   <root>/models--<org>--<name>/blobs/<etag>             i byte
 *   <root>/models--<org>--<name>/snapshots/<rev>/<rel>    symlink RELATIVO "../../blobs/<etag>"
 *   <root>/models--<org>--<name>/.no_exist/<rev>/<rel>    file vuoto = "il remote non ha questo file"
 *   <root>/models--<org>--<name>/.rapid-mlx-mirror/<rel>.lock  lock di un altro tool (non nostro)
 * (rilevato su `~/.cache/huggingface/hub/models--mlx-community--Qwen3-0.6B-4bit`).
 *
 * Due fatti sul nome dei blob, misurati (e uno dei due contro-intuitivo):
 *  - un nome di 64 esadecimali è il **SHA256 del contenuto**: verificato su 2/2 blob LFS
 *    locali, incluso `model.safetensors` da 335.450.584 byte;
 *  - un nome di 40 esadecimali **NON** è lo SHA1 del contenuto: 6/6 blob locali lo
 *    smentiscono. È il **git blob id**, cioè SHA1 di `"blob <len>\0" + contenuto`
 *    (3/3 confermati con `git hash-object` contro i symlink dello snapshot).
 *  Un verificatore che assumesse il secondo avrebbe scartato per "corrotto" ogni file
 *  non-LFS del cache: da qui `FmlxHfHashKind` per nome, e i vettori nel test.
 *
 * Regole volute, le stesse della lane gguf: nessun rifiuto silenzioso — ogni fallimento
 * ha il SUO codice (scaricabile vs inesistente vs symlink pendente sono casi diversi,
 * e solo il primo giustifica una richiesta di rete).
 */
#ifndef FMLX_HF_H
#define FMLX_HF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FMLX_HF_PATH_MAX 4096 /* PATH_MAX di macOS è 1024: oltre il nostro buffer => errore distintivo */
#define FMLX_HF_BLOB_NAME_MAX 80
#define FMLX_HF_ENDPOINT_DEFAULT "https://huggingface.co"

typedef enum {
    FMLX_HF_OK = 0,
    FMLX_HF_ERR_BUF_SMALL = 1,       /* un path non sta nel buffer: mai troncare in silenzio */
    FMLX_HF_ERR_NO_HOME = 2,         /* né HF_HUB_CACHE né HF_HOME né HOME: radice non derivabile */
    FMLX_HF_ERR_REPO_ID = 3,         /* repo id malformato (non "<org>/<nome>", segmenti vietati) */
    FMLX_HF_ERR_NAME = 4,            /* path del file nel repo inaccettabile (assoluto, "..", vuoto) */
    FMLX_HF_ERR_REVISION = 5,        /* revision vuota, con spazi o con "/" */
    FMLX_HF_ERR_REF_MISSING = 6,     /* refs/<branch> non esiste: la revisione è un nome, non uno SHA */
    FMLX_HF_ERR_REF_CONTENT = 7,     /* refs/<branch> esiste ma non contiene un 40-hex */
    FMLX_HF_ERR_NO_REPO = 8,         /* la cartella models--<org>--<name> non esiste */
    FMLX_HF_ERR_SNAPSHOT_MISSING = 9,/* snapshots/<rev>/ non esiste (la rev non è mai stata scaricata) */
    FMLX_HF_ERR_NOT_CACHED = 10,     /* entry assente ma il remote potrebbe averla: SCARICABILE */
    FMLX_HF_ERR_NOT_ON_REMOTE = 11,  /* marker .no_exist: NON scaricare, il file non esiste */
    FMLX_HF_ERR_LINK_FORM = 12,      /* symlink che non ha la forma ../../blobs/<nome>: rifiutato */
    FMLX_HF_ERR_BLOB_MISSING = 13,   /* symlink pendente: il blob puntato non c'è */
    FMLX_HF_ERR_IO = 14,             /* errore di sistema non classificato (errno nel messaggio) */
    FMLX_HF_ERR_HASH_MISMATCH = 15,  /* il contenuto NON corrisponde al nome del blob */
    FMLX_HF_ERR_NOT_VERIFIABLE = 16, /* entry non verificabile per nome (file regolare, o nome non-hex) */
    /* 17: la voce richiesta è una DIRECTORY dello snapshot, non un file. HF la scrive davvero
     * (misurato: models--bert-base-uncased/snapshots/<rev>/coreml/ contiene i file con i loro
     * symlink a 5 livelli di "../"). Non è un errore di formato: chi chiama deve passare il
     * path del file dentro la directory, che questa lane risolve allo stesso modo. */
    FMLX_HF_ERR_NOT_A_FILE = 17,
    FMLX_HF_ERR_OFFLINE = 18,   /* l'ambiente vieta la rete e il file non c'è: mai fingere */
    FMLX_HF_ERR_HTTP = 19,      /* risposta non 2xx/3xx, o transport fallito (codice nel messaggio) */
    FMLX_HF_ERR_LOCKED = 20,    /* un altro processo tiene <blob>.lock: non si ruba il lock */
    FMLX_HF_ERR_NO_ETAG = 21,   /* il server non ha detto quale hash deve avere il blob */
    FMLX_HF_ERR_SIZE = 22,      /* byte ricevuti diversi da X-Linked-Size/Content-Length */
    /* 23: ciò che il server ha dato come etag NON è accettabile come NOME DI FILE (contiene
     * "/", "..", caratteri di controllo...). Un etag arriva dal mondo esterno e diventa il nome
     * di un blob nel cache: non validarlo vorrebbe dire lasciare che la risposta scelga un path. */
    FMLX_HF_ERR_BAD_ETAG = 23,
    /* 24: il server non ha dichiarato MAI una lunghezza. Non è il caso "byte diversi": qui non
     * esiste nemmeno un'attesa da confrontare, quindi un troncamento sarebbe invisibile. */
    FMLX_HF_ERR_NO_SIZE = 24,
    /* 25: lo STREAM si è fermato — nessun byte per `stall_timeout_s`. NON è un errore HTTP: la
     * connessione è viva e il server non ha detto niente, quindi il corpo parziale è valido e il
     * `.incomplete` resta. È l'unica classe di fallimento che conviene RITENTARE con lo stesso
     * `Range` (la replica del `--stall-timeout`/`--stall-retries` di rapid-mlx); le altre si
     * ritentano solo a scaldare il server. */
    FMLX_HF_ERR_STALL = 25,
    /* 26: una risposta LEGITTIMA che però chiede più di quanto abbiamo deciso di tenere. La usa il
     * manifesto (§12.1): `siblings` con più del massimo di voci che questa lane accetta. NON è
     * ERR_SIZE, che parla di byte ricevuti contro byte dichiarati — qui la promessa del server è
     * mantenuta, è il NOSTRO tetto sulla quantità che dice "no". Codice distinto perché il chiamante
     * fa una cosa diversa: un `pull` con un filtro, non una ritentativa. */
    FMLX_HF_ERR_TOO_MANY = 26,
    /* 27: la RADICE del cache non esiste (o non è una directory). Non è ERR_NO_HOME, che è "non so
     * nemmeno dove cercarla", e non è ERR_NO_REPO, che è "l'ho cercata e quel repo non c'è": qui il
     * path è derivato e corretto, è il disco che non lo ha. La distinzione serve al `doctor`, perché
     * "HF_HOME punta nel posto sbagliato" e "non hai ancora scaricato niente" sono due consigli
     * diversi — e un `models` che stampasse l'elenco vuoto per la seconda ragione quando vale la
     * prima sarebbe il risultato silenzioso che il piano vieta. */
    FMLX_HF_ERR_NO_CACHE = 27
} FmlxHfStatus;

const char *fmlx_hf_status_str(FmlxHfStatus s);

/* ---- hash, per tipo di nome-blob ---- */
typedef enum {
    FMLX_HF_HASH_NONE = 0,    /* il nome non promette un hash: nulla da confrontare */
    FMLX_HF_HASH_GIT_BLOB = 1,/* 40 hex = SHA1("blob <len>\0" + contenuto) */
    FMLX_HF_HASH_SHA256 = 2   /* 64 hex = SHA256(contenuto) */
} FmlxHfHashKind;

/* Classifica un nome di blob SOLO dalla sua forma. Esportato perché è la decisione
 * che comanda la verifica; mai dedurla da un'altra parte. */
FmlxHfHashKind fmlx_hf_blob_hash_kind(const char *blob_name);

/* ---- ambiente ---- */

/* Radice del cache, nell'ordine MISURATO in huggingface_hub/constants.py:166-189 (copia
 * `~/ai/env-mflux`; l'altra copia 0.36.2 di questa macchina ha le stesse regole a righe
 * diverse, vedi LICENSES.md):
 * HF_HUB_CACHE, poi HUGGINGFACE_HUB_CACHE (legacy: è il DEFAULT di HF_HUB_CACHE, quindi viene
 * PRIMA di HF_HOME), poi $HF_HOME/hub, poi $XDG_CACHE_HOME/huggingface/hub, poi
 * $HOME/.cache/huggingface/hub. `~` e `$VAR` sono espansi nell'ordine di Python (tilde prima);
 * lo slash finale va tolto, come fa `os.path.join`. NON crea nulla; se non c'è nessuna fonte
 * da cui derivare la radice (nemmeno HOME) è FMLX_HF_ERR_NO_HOME, mai una radice inventata. */
FmlxHfStatus fmlx_hf_cache_root(char *out, size_t out_len);

/* 1 = l'ambiente chiede il solo-offline: HF_HUB_OFFLINE o TRANSFORMERS_OFFLINE, veri i valori
 * "1","ON","YES","TRUE" (case-insensitive), misurato in constants.py:11 e :202. */
int fmlx_hf_offline(void);

/* Endpoint: HF_ENDPOINT senza "/" finale, altrimenti quello di default. */
FmlxHfStatus fmlx_hf_endpoint(char *out, size_t out_len);

/* ---- percorsi (tutti puri: nessuna I/O) ---- */

/* "<org>/<nome>" -> "<root>/models--<org>--<nome>". L'id è validato come UNO o DUE segmenti:
 * sono vietati il segmento vuoto, "." e "..", i caratteri di controllo, il backslash e — per
 * i segmenti — l'inizio/fine con "." o spazio. NON è una whitelist alfanumerica: il cache
 * reale contiene id senza namespace (`models--roberta-base`) e nomi con spazi e UTF-8. */
FmlxHfStatus fmlx_hf_repo_dir(const char *cache_root, const char *repo_id, char *out, size_t out_len);

/* Nome di directory per un repo id (SENZA radice): "models--<org>--<nome>", con "/" rimpiazzata da
 * "--". È il `repo_folder_name` di HF, e non è solo la cartella del cache: è ANCHE il nome della
 * sottodirectory che huggingface_hub crea sotto `.locks/` (file_download.py:1203, misurato).
 * Ambiguità ereditata da HF: "a--b/c" e "a/b--c" producono lo STESSO nome di directory. */
FmlxHfStatus fmlx_hf_repo_dirname(const char *repo_id, char *out, size_t out_len);

/* Valida un path relativo dentro il repo (es. "weights/model.safetensors"). */
FmlxHfStatus fmlx_hf_check_relpath(const char *relpath);

/* Unisce due path con "/", o FMLX_HF_ERR_BUF_SMALL (e azzera `out` in quel caso).
 * È pubblico da quando il `pull` costruisce path suo (il `--dest`): la regola «componente + '/' +
 * componente, e se non sta NON troncare» è UNA, e due copie sarebbero due comportamenti diversi
 * sullo stesso cache — lo stesso motivo per cui `quote` non è stata riscritta nel manifesto. */
FmlxHfStatus fmlx_hf_pjoin(char *out, size_t n, const char *a, const char *b);

/* `mkdir -p` sul path, per livelli: EEXIST non è un errore. Ritorna 0 o -1 con `errno`.
 * Pubblico per la stessa ragione di `fmlx_hf_pjoin`: chi specchia un modello fuori dal cache deve
 * creare la stessa forma di albero, non una seconda versione della regola. */
int fmlx_hf_mkdir_p(const char *path);

/* SOLO il genitore di `path`: `fmlx_hf_mkdir_p(dirname(path))`. Il nome dice esattamente ciò che
 * fa, perché chi deve scrivere IN una directory (i `blobs/`, il repo stesso) deve chiamare
 * `fmlx_hf_mkdir_p` su QUELLA directory: confondere i due è il bug che ha reso impossibile il
 * primo download, e la distinzione vale anche fuori dal cache. 0 o -1 con `errno`. */
int fmlx_hf_ensure_parent(const char *path);

/* Revisione -> commit SHA a 40 hex. Se `revision` è già 40-hex la usa così com'è (a
 * condizione che lo snapshot esista: qui non si guarda il disco), altrimenti legge
 * `<repo_dir>/refs/<revision>` e valida il contenuto. */
FmlxHfStatus fmlx_hf_resolve_ref(const char *repo_dir, const char *revision, char *sha_out,
                                 size_t sha_len);

/* URL di download: <endpoint>/<repo_id>/resolve/<revision>/<relpath>.
 * `use_resolve=0` produce l'URL API dei metadati (<endpoint>/api/models/.../revision/<rev>). */
FmlxHfStatus fmlx_hf_url(const char *repo_id, const char *revision, const char *relpath,
                         int use_resolve, char *out, size_t out_len);

/* ---- locate: dove sono i byte ---- */

typedef struct {
    char path[FMLX_HF_PATH_MAX];  /* path assoluto dei byte */
    char blob_name[FMLX_HF_BLOB_NAME_MAX]; /* nome del blob, "" se entry file-regolare */
    FmlxHfHashKind hash_kind;     /* cosa promette quel nome */
    int is_blob_link;             /* 1 = symlink in blobs/, 0 = file regolare nello snapshot */
    int64_t size;                 /* byte dichiarati dal filesystem */
} FmlxHfRef;

/* Risolve repo/revision/file al path reale. `cache_root == NULL` usa `fmlx_hf_cache_root()`.
 * `*out` viene AZZERATA sempre, anche quando la funzione fallisce: un chiamante non deve mai
 * leggere il residuo della chiamata precedente.
 * Segue SOLO symlink nella forma esatta "../../blobs/<nome>": un symlink che punta fuori
 * da quella forma è FMLX_HF_ERR_LINK_FORM, non un percorso da seguire. */
FmlxHfStatus fmlx_hf_locate(const char *cache_root, const char *repo_id, const char *revision,
                            const char *relpath, FmlxHfRef *out);

/* Verifica i byte contro ciò che il nome promette. FMLX_HF_ERR_NOT_VERIFIABLE se il nome
 * non promette nulla (entry file-regolare o nome non-hex): NON è un successo. */
FmlxHfStatus fmlx_hf_verify(const FmlxHfRef *ref, int64_t *bytes_hashed);

/* ---- catalogo locale: cosa c'è già sul disco (piano §10.2, i verbi `models`/`list`) ---- */

/* I tre limiti sono NUMERI, non ipotesi: `NAME_MAX` su APFS è 255 (un componente di path non può
 * superarli), e una *revisione* è al più 40 hex — 64 sta largo a un `refs/pr/12` lungo. Il terzo
 * (8 ref) è una scelta, e per questo `n_refs` resta il numero VERO: se ce n'erano 11, la lista ne
 * mostra 8 e `n_refs` dice 11. Truncare in silenzio sarebbe il risultato silenzioso che il piano
 * vieta; qui la truncation è un campo, non un'omissione. */
#define FMLX_HF_CATALOG_NAME_MAX 256
#define FMLX_HF_CATALOG_REF_MAX 64
#define FMLX_HF_CATALOG_REFS_MAX 8
#define FMLX_HF_CATALOG_REPOS_MAX 256

typedef struct {
    /* Ricostruito dal nome della directory con la regola di huggingface_hub
     * (`_cache_manager.py:742-744`, copia 1.31.0): il prefisso fino al primo "--" è il TIPO, il
     * resto è l'id con ogni "--" trasformato in "/". La regola è PERDENTE per costruzione — un
     * repo il cui nome contiene "--" arriva a un id con un "/" in più — e `ambiguous_name` è
     * lì per dirlo, non per nasconderlo: è ciò che rende il dubbio visibile invece che silenzioso. */
    char repo_id[FMLX_HF_CATALOG_NAME_MAX];
    char dirname[FMLX_HF_CATALOG_NAME_MAX]; /* com'è scritta su disco: mai ricostruire a caso */
    char kind[8];              /* "model" | "dataset" | "space" */
    char refs[FMLX_HF_CATALOG_REFS_MAX][FMLX_HF_CATALOG_REF_MAX]; /* i NOMI in refs/, non i commit */
    size_t n_refs;             /* il numero VERO di ref, anche se `refs[]` non li contiene tutti */
    size_t n_blobs;            /* file DISTINCT (inode) in blobs/: due hardlink = uno */
    int64_t bytes;             /* somma dei byte di quei blob: l'INGOMBRO, non la somma degli symlink */
    int64_t mtime;             /* il più recente fra i blob; 0 se non c'è nessun blob */
    int refs_only;             /* 1 = ha `refs/` ma zero blob: un puntatore, non un modello */
    int ambiguous_name;        /* 1 = più di un "--" dopo il tipo: l'id potrebbe non essere quello vero */
} FmlxHfRepo;

/* Elenca i repo del cache. `cache_root == NULL` = `fmlx_hf_cache_root()`. Ordine per `repo_id`:
 * readdir non è deterministico e un test che dipende dall'ordine del filesystem è un test che
 * fallisce su un altro disco. `*n_out` è scritto SEMPRE, anche sull'errore.
 * `FMLX_HF_ERR_TOO_MANY` se i repo sono più di `cap`: `*n_out` dice Quanti, così il chiamante può
 * ingrandire l'array invece di stampare un elenco monco.
 * Cosa NON è un repo: qualunque entry che non sia una directory con un prefisso di tipo noto — le
 * directory `xet/`, i file `version.txt`, `CACHEDIR.TAG`. Vengono ignorate, e `n_ignored_out`
 * (opzionale) le conta: "il cache ha 23 repo e 2 cose che non riconosco" è una frase diversa da
 * "il cache ha 23 repo". */
FmlxHfStatus fmlx_hf_catalog(const char *cache_root, FmlxHfRepo *out, size_t cap, size_t *n_out,
                             size_t *n_ignored_out);

/* Un solo repo, per il `doctor` e per chi deve confermare un id dopo un `pull`. Stesse regole di
 * conteggio. `FMLX_HF_ERR_NO_REPO` se la directory non c'è: un repo che NON abbiamo non è un repo da
 * zero byte, e i due casi devono arrivare al chiamante distinti (l'uno è "scaricalo", l'altro è "è
 * rotto"). */
FmlxHfStatus fmlx_hf_catalog_repo(const char *cache_root, const char *repo_id, FmlxHfRepo *out);

/* Messaggio dell'ultimo fallimento di questa lane (NULL se nessuno). Posseduto dal caller?
 * No: valida fino alla prossima chiamata di questa lane. */
const char *fmlx_hf_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* FMLX_HF_H */
