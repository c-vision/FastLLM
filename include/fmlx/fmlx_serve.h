/* fmlx_serve.h — il pezzo ③ di FastLLM: il server TCP sopra il codec (http) e il layer ②
 * (api), SENZA il motore.
 *
 * Cosa è questo file e cosa NON è. Il codec sa incorniciare i byte, il layer ② sa tradurre
 * un evento in due dialetti. Qui c'è il filo che li tiene insieme: accettare, leggere,
 * rispondere, richiudere. È il primo pezzo del progetto che sta in piedi da solo — e il
 * primo che un client reale può colpire — quindi la domanda di design non è "come facciamo
 * a generare" ma **cosa rispondiamo finché il motore non c'è**.
 *
 * LA REGOLA CHE COMANDA TUTTO QUI: **mai una risposta che sembri ciò che non è.**
 *  - `/v1/models` risponde DAVVERO: i byte ci sono, vengono dal catalogo locale (`fmlx_hf_catalog`),
 *    e non serve il motore per sapere cosa c'è su disco.
 *  - `/health` risponde DAVVERO, e dice `engine:"not_wired"` in faccia a un orchestratore.
 *  - Ogni rotta che ha bisogno di generare — `chat/completions`, `completions`, `responses`,
 *    `messages`, `models/load|unload|status`, `embeddings`, `metrics` — risponde
 *    `FMLX_API_NOT_SUPPORTED` (501) col PERCHÉ, renderizzato dall'adapter del dialetto giusto.
 *    Un `chat.completion` con `"content": ""` e `finish_reason:"stop"` sarebbe la risposta
 *    peggiore possibile: coerente, vuota, e falsa (piano §11.2, e il divieto del risultato
 *    silenzioso che corre tutto il progetto).
 *  - La validazione che NON richiede il motore viene fatta LO STESSO: JSON del corpo,
 *    `anthropic-version`, framing. Un client che manda byte malformati riceve 400 oggi e
 *    riceverà 400 anche quando il motore arriverà — il comportamento non cambia all'arrivo
 *    del motore, perché non era del motore che era colpa.
 *
 * due metà, e il confine è il test. `fmlx_serve_respond()` è pura: prende una richiesta già
 * parseata e scrive una risposta completa in un buffer del chiamante — zero socket, zero
 * orologio, zero thread, quindi provabile byte a byte. `fmlx_serve()` è il loop (bind, accept,
 * read, write, keep-alive): la parte che in-process non si prova, e che per questo è sottile
 * quanto basta. Le allocazioni stanno TUTTE nel loop; il responder non mallocà, come il codec.
 *
 * AUTENTICAZIONE (piano §14, §18.3.3 "gated-401"): `FASTMLX_API_KEY` vuota o assente =
 * nessuna auth — che è il default, perché questo server ascolta su 127.0.0.1 e un key
 * obbligatorio renderebbe impossibile provarlo. Quando è messa, OGNI rotta (comprese
 * `/health` e il 404) la esige: sapere chi è autorizzato non dipende da cosa chiede. La
 * chiave NON ha una bandiera, per la stessa ragione di `HF_TOKEN`: argv è leggibile da ogni
 * processo dell'utente e finisce nella cronologia della shell.
 */
#ifndef FMLX_SERVE_H
#define FMLX_SERVE_H

#include "fmlx/fmlx_api.h"
#include "fmlx/fmlx_hf.h"

#include <signal.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* I valori di default sono quelli del piano §14, non una comodità. Porta 8000 e loopback:
 * `0.0.0.0` si mette per iscritto con `--host`, perché "ascolto su tutte le interfacce" non
 * è mai un default che deve capitare. */
#define FMLX_SERVE_HOST_DEFAULT "127.0.0.1"
#define FMLX_SERVE_PORT_DEFAULT 8000
/* 16 MiB: una richiesta con immagini in base64 le passa entrambi i numeri (un PNG da 4 MB
 * sono ~5.3 MB di testo). È un TETTO, non una dimensione attesa: oltre, 400. */
#define FMLX_SERVE_BODY_MAX (16u << 20)
#define FMLX_SERVE_RESP_MAX (1u << 20)
#define FMLX_SERVE_LISTEN_BACKLOG 16

/* ---- opzioni e memoria ---- */

typedef struct {
    const char *cache_root; /* NULL = la regola d'ambiente della lane hf */
    const char *host;       /* NULL = FMLX_SERVE_HOST_DEFAULT; "0.0.0.0" = ogni interfaccia */
    int port;               /* 0 = FMLX_SERVE_PORT_DEFAULT */
    const char *api_key;    /* NULL o "" = nessuna autenticazione */
    size_t max_body;        /* 0 = FMLX_SERVE_BODY_MAX */
    unsigned max_conns;     /* 0 = nessun limite: si ferma col segnale */
} FmlxServeOpts;

/* La memoria del server. Il `resp` è DOVE SIAMO DECISI di fallire: un `/v1/models` con più
 * byte di `resp_cap` NON viene tagliato, diventa un 500 (vedi il comment nel .c). */
typedef struct {
    char *resp;
    size_t resp_cap;
    FmlxHfRepo *repos;
    size_t repos_cap;
} FmlxServeBuf;

/* Mallocà i buffer. `repos_cap` 0 = FMLX_HF_CATALOG_REPOS_MAX. Ritorna 0, o -1 con `errno`.
 * `fmlx_serve` la chiama da sé se `buf == NULL`: esiste perché un test vuole buffer PICCOLI
 * (provare il 500 è il motivo) e perché il loop ha una sola finestra di memoria per connessione. */
int fmlx_serve_buf_init(FmlxServeBuf *b, size_t resp_cap, size_t repos_cap);
void fmlx_serve_buf_free(FmlxServeBuf *b);

/* ---- esito ---- */

typedef enum {
    FMLX_SERVE_OK = 0,
    FMLX_SERVE_BUF_SMALL = 1, /* il buffer della risposta non bastava: NON è una risposta a metà */
    FMLX_SERVE_BAD_ARG = 2, /* uso scorretto dell'API (NULL, porta fuori range, host che non si
                             * risolve): la correzione sta in chi chiama */
    /* I tre che seguono è il SO che rifiuta un `bind`, non l'uso scorretto. Prima stavano tutti
     * in BAD_ARG, che è una diagnosi SPOSTATA: dice a chi ha lanciato `fastmlx serve` di aver
     * passato un argomento sbagliato quando l'argomento era giusto e la porta era occupata.
     * Un codice per causa, perché il da-fare è diverso: cambiare porta, o alzare i privilegi, o
     * accorgersi che l'interfaccia nominata da `--host` su questa macchina non c'è. */
    FMLX_SERVE_ADDR_IN_USE = 3, /* EADDRINUSE: qualcun altro ascolta già su quella porta */
    FMLX_SERVE_NO_PERM = 4,     /* EACCES: porta < 1024 senza privilegi (o sandbox) */
    FMLX_SERVE_BIND_FAILED = 5  /* ogni altro diniego di bind/listen: `errno` è nel log */
} FmlxServeSt;

const char *fmlx_serve_status_str(FmlxServeSt s);

/* ---- le decisioni host/port/key (piano §14: flag > ambiente > default) ---- */

/* Applica `FASTMLX_HOST`/`FASTMLX_PORT`/`FASTMLX_API_KEY` a `o` LASCIANDO stare ciò che è già
 * posto (il flag ha già vinto). Una `FASTMLX_PORT` illeggibile — non cifre, o fuori 1..65535 —
 * è un ERRORE con il motivo in `msg`, non un ritorno silenzioso al default: un typo che sposta
 * il server sulla porta di casa è il genere di bug che si scopre dal log di un'altro processo.
 * Un `FASTMLX_API_KEY` vuoto È un valore: «nessuna auth», scritta per iscritto.
 * Ritorna FMLX_SERVE_OK o FMLX_SERVE_BAD_ARG. */
FmlxServeSt fmlx_serve_apply_env(FmlxServeOpts *o, char *msg, size_t msg_len);

/* 1 = l'ambiente fissa una chiave. Esposto perché il responder e il `doctor` di domani devono
 * dire la STESSA cosa su "auth attiva o no": due letture di `getenv` divergenti sarebbero due
 * server diversi. */
int fmlx_serve_key_from_env(const char **out);

/* ---- la metà pura: richiesta già parseata -> risposta completa ---- */

/* Scrive in `buf->resp` la risposta INTEGRA (testata + corpo) e `*out_len` i byte.
 * `*keep_alive` dice se il filo può restare aperto: lo decide QUI, non il loop, perché è qui
 * che è qui che si sa se la richiesta era incorniciata in modo affidabile (una `Transfer-Encoding`
 * sospetta non merita un secondo request sulla stessa connessione).
 * `req->err_*` non sono letti: la richiesta è già stata accettata dal codec.
 * Ritorna FMLX_SERVE_OK con `*out_len` valida; su BUF_SMALL/BAD_ARG NON c'è nulla da inviare.
 *
 * `body`/`body_len` sono il corpo già letto (NULL/0 per una `GET`). */
FmlxServeSt fmlx_serve_respond(const FmlxHttpReq *req, const char *body, size_t body_len,
                               const FmlxServeOpts *o, const FmlxServeBuf *buf, size_t *out_len,
                               int *keep_alive);

/* Il code machine-readable per i rifiuti: è UNO per causa, come nel resto del progetto, e un
 * test lo asserisce invece di confrontare frasi che possono cambiare. */
#define FMLX_SERVE_CODE_ENGINE "engine_not_wired"
#define FMLX_SERVE_CODE_AUTH "missing_api_key"
#define FMLX_SERVE_CODE_VERSION "unsupported_anthropic_version"
#define FMLX_SERVE_CODE_BODY_LARGE "body_too_large"
#define FMLX_SERVE_CODE_BODY_INVALID "invalid_body"
#define FMLX_SERVE_CODE_FRAMING "invalid_framing"
#define FMLX_SERVE_CODE_CHUNKED "chunked_not_supported"
#define FMLX_SERVE_CODE_CATALOG "catalog_unreadable"

/* Il tetto sul corpo, RISOLTO in un punto solo. Il loop deve rifiutare PRIMA di leggere (non si
 * mettono 4 GB in una finestra) e il responder deve poter dire la STESSA cosa su ciò che ha in
 * mano: due letture divergenti del campo `max_body` farebbero chiudere una connessione che
 * dall'altra parte sembrava buona. `0` = FMLX_SERVE_BODY_MAX. */
size_t fmlx_serve_body_limit(const FmlxServeOpts *o);

/* Rende la testata di una risposta GIÀ serializzata d'accordo con ciò che il loop sta per fare:
 * ecoa la versione del client (`HTTP/1.0` a chi ha chiesto 1.0 — la stessa lunghezza, nessuno
 * spostamento) e, quando `ka` è 0, SCRIVE `Connection: close`. Il `close` taciuto è il client che
 * fa pipelining e vede la seconda richiesta sparire: la connessione si chiude lo stesso, ma la
 * testata aveva promesso il contrario.
 *
 * `fmlx_serve_respond` la chiama da sé sul suo unico punto di uscita; esiste pubblicata perché il
 * loop ha due rifiuti (framing, corpo troppo grande) che il responder non ha mai visto, e li
 * chiude allo stesso modo. La regola sta in UN posto perché due copie divergono.
 * Ritorna BUF_SMALL se i 19 byte della riga non entrano, e allora azzera `*out_len`: NON si manda
 * una testata modificata a metà. */
FmlxServeSt fmlx_serve_agree_head(int http10, FmlxServeBuf *buf, size_t *out_len, int ka);

/* ---- la metà sporca: il loop ---- */
/* Bind, accept, e il giro delle connessioni. `log` (può essere NULL) riceve una riga per
 * richiesta in ITALIANO — sono log, non risposte al client: gli errori di bind e le note
 * d'ascolto vanno lì, e l'unica cosa che finisce sullo stdout del chiamante è ciò che ha chiesto.
 * `stop` non-NULL: il loop la guarda fra una connessione e l'altra (e durante un `accept`
 * interrotto) e ritorna FMLX_SERVE_OK. NULL: il loop installa e rimuove i propri handler di
 * SIGINT/SIGTERM, così chi chiama da `main` non deve sapere niente dei segnali.
 * Ritorna BAD_ARG sugli argomenti che non possono funzionare PRIMA di toccare il network stack
 * (porta fuori range, host che `getaddrinfo` non risolve), e BUF_SMALL se è la memoria a essere
 * negata. Un `bind` rifiutato NON è BAD_ARG: esce ADDR_IN_USE / NO_PERM / BIND_FAILED secondo
 * `errno`, perché quelle tre righe si correggono in tre modi diversi e un unico "argomento non
 * valido" mandava l'operatore a rileggere i flag invece di guardarsi la porta.
 *
 * Un solo thread, una connessione alla volta: è UNA scelta, non un limite ereditato — il
 * decode serializzato del piano §15 rende inutile un accept concorrente finché non c'è il
 * motore, e i contatori finta-paralleli sono peggio di un `busy` visibile. Il costo è detto:
 * una `/v1/models` su un cache grande tiene gli altri in coda.
 *
 * `served` (opzionale) conta le richieste SERVITE — non le connessioni aperte: è il numero che
 * il terminale stampa alla fine, ed è ciò che rende `--once` verificabile da un test. */
FmlxServeSt fmlx_serve(const FmlxServeOpts *o, FmlxServeBuf *buf, FILE *log,
                       volatile sig_atomic_t *stop, unsigned *served);

#ifdef __cplusplus
}
#endif

#endif /* FMLX_SERVE_H */
