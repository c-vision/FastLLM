/* fmlx_http.h — il codec HTTP/1.1 di FastMLX: parse della richiesta, scrittura della
 * risposta, framing SSE.
 *
 * Questa è la superficie che tocca per prima i byte che arrivano da internet. Il contratto
 * qui non è "fare HTTP": è **non poter essere tratti in inganno** su dove finisce una
 * richiesta e dove comincia la successiva — che è la classe di bug (request smuggling) in
 * cui un parser permissivo e uno severo non sono lo stesso server.
 *
 * TRE REGOLE DI CASA, che valgono più della comodità:
 *  1. **Zero allocazioni**: il parser NON copia e NON mallocà. `FmlxHttpReq` punta nel
 *     buffer del chiamante, che DEVE restare valido e immutato finché il struct vive.
 *     Un body da 100 MB copiato due volte è il costo che non si vede nel benchmark.
 *  2. **Ogni rifiuto ha un codice proprio**, e nessun codice copre due cause diverse:
 *     `CHUNKED` e `CL_AMBIGUOUS` sono due decisioni diverse del server (una è "non
 *     implementato", l'altra è "sospetto attacco"). `NEED_MORE` NON è un errore: è
 *     l'unico modo di dire "ho letto, non basta" senza far perdere i byte già letti.
 *  3. **La risposta non esce a metà**: come il writer JSON, dopo qualunque errore ogni
 *     chiamata è un no-op e `body_off` non è affidabile. Un `Content-Length` che promette
 *     più byte di quelli inviati è una connessione appesa, non una risposta sbagliata.
 *
 * Nessuna dipendenza esterna (niente http-parser, niente curses). C17.
 */
#ifndef FMLX_HTTP_H
#define FMLX_HTTP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FMLX_HTTP_MAX_HEADERS 64u    /* oltre: TOO_MANY, non "gli ultimi entrano" */
#define FMLX_HTTP_MAX_TARGET 8192u   /* un path più lungo non è una richiesta API */
#define FMLX_HTTP_MAX_HEAD (128u << 10) /* testata richiesta completa (CRLFCRLF incluso) */

typedef enum {
    /* NON errori */
    FMLX_HTTP_OK = 0,
    FMLX_HTTP_NEED_MORE = 1,     /* testata non ancora completa: si legga ancora */

    /* La request-line */
    FMLX_HTTP_ERR_BAD_METHOD = 2,      /* caratteri che non sono `token` (RFC 9110 §5.6.2) */
    FMLX_HTTP_ERR_BAD_TARGET = 3,      /* vuota, con spazio/CTRL, o in una forma non ammessa */
    FMLX_HTTP_ERR_BAD_VERSION = 4,     /* non esattamente `HTTP/1.0` / `HTTP/1.1` */
    FMLX_HTTP_ERR_BAD_REQ_LINE = 5,    /* numero di campi ≠ 3, o CRLF mancante */

    /* I campi header */
    FMLX_HTTP_ERR_BAD_HEADER = 6,      /* nome vuoto, o `:` assente, o carattere non ammesso */
    FMLX_HTTP_ERR_BAD_HDR_VALUE = 7,   /* CTRL crudi nel valore (HTAB sì, il resto no) */
    FMLX_HTTP_ERR_FOLDING = 8,         /* obs-fold: RFC 9112 lo vieta, qui è un rifiuto */
    FMLX_HTTP_ERR_TOO_MANY_HDRS = 9,   /* > FMLX_HTTP_MAX_HEADERS */
    FMLX_HTTP_ERR_HEAD_TOO_LARGE = 10, /* > FMLX_HTTP_MAX_HEAD */
    FMLX_HTTP_ERR_LONE_CR = 11,        /* `\r` non seguito da `\n`: frammentazione ambigua */

    /* Il framing del body — la parte su cui un server sbaglia e resta vulnerabile */
    FMLX_HTTP_ERR_NO_CONTENT_LENGTH = 12, /* `Content-Length` assente ma il corpo c'è */
    FMLX_HTTP_ERR_BAD_CONTENT_LENGTH = 13, /* non cifra, con `+`, vuoto, o oltre uint64 */
    FMLX_HTTP_ERR_CL_AMBIGUOUS = 14,       /* due `Content-Length` con valori DIVERSI */
    FMLX_HTTP_ERR_CHUNKED = 15,            /* `Transfer-Encoding: chunked`: non supportato */
    FMLX_HTTP_ERR_TE_WITH_CL = 16,         /* TE e CL insieme: il classicissimo divergence */
    FMLX_HTTP_ERR_BAD_TE = 17,             /* TE con un token che non è chunked */

    /* Scrittura della risposta */
    FMLX_HTTP_ERR_BUF_SMALL = 18,
    FMLX_HTTP_ERR_BAD_ARG = 19,
    FMLX_HTTP_ERR_BAD_STATUS = 20 /* fuori 100..999, o reason/target con CTRL */
} FmlxHttpStatus;

const char *fmlx_http_status_str(FmlxHttpStatus s);

/* Una fetta del buffer del chiamante: `{ptr, len}`. `ptr` NON è NUL-terminata e il suo
 * `len` è parte del valore: una header-name troncata al `\0` interno leggerebbe oltre. */
typedef struct {
    const char *p;
    uint32_t n;
} FmlxSlice;

typedef enum {
    FMLX_HTTP_TARGET_ORIGIN = 0, /* `/v1/chat/completions` — la forma delle API */
    FMLX_HTTP_TARGET_STAR = 1,   /* `*` — OPTIONS */
    FMLX_HTTP_TARGET_ABSOLUTE = 2 /* `http://host/p` — richiesto da un proxy */
} FmlxHttpTargetForm;

typedef struct {
    FmlxSlice method;
    FmlxSlice target;
    FmlxHttpTargetForm target_form;
    int http10; /* 1 = `HTTP/1.0`, 0 = `HTTP/1.1` */

    /* Framing del body. `has_body` è ciò che la testata DICE, non ciò che è arrivato. */
    uint64_t content_length;
    int has_content_length;
    int has_body;

    size_t body_off; /* offset di `buf` dove comincia il corpo, subito dopo CRLFCRLF */
    size_t head_len; /* byte consumati dalla testata */

    uint32_t n_hdr;
    struct {
        FmlxSlice name;  /* come è arrivato; il CONFRONTO è case-insensitive */
        FmlxSlice value; /* senza gli spazi di OWS attorno */
    } hdr[FMLX_HTTP_MAX_HEADERS];

    FmlxSlice err_field; /* di quale header parliamo, quando il codice è un BAD_/AMBIGUOUS_ */
    const char *err_msg; /* in italiano, per i log: mai un rifiuto muto */
    size_t err_off;      /* byte in `buf` a cui è successo, o FMLX_HTTP_NO_OFF */
} FmlxHttpReq;

#define FMLX_HTTP_NO_OFF ((size_t)-1)

/* Inizializza a vuoto. OBBLIGATORIO: un `FmlxHttpReq` non inizializzato non è un valore,
 * è un `n_hdr` casuale che diventa un lettura fuori array. */
void fmlx_http_req_init(FmlxHttpReq *r);

/* Interpreta i primi `len` byte di `buf`. Ritorna OK, NEED_MORE, o un errore.
 * `buf` DEVE restare valido e immutato: `r` punta lì dentro.
 * Non tocca il corpo: chi lo legge è il trasporto. */
FmlxHttpStatus fmlx_http_parse_req(const char *buf, size_t len, FmlxHttpReq *r);

/* Accesso agli header. Il nome è confrontato case-insensitive come impone RFC 9112 §5,
 * e `NULL` significa "assente", non "presente ma vuoto": distinguere le due cose è
 * proprio ciò che serve per `Content-Length`. */
FmlxSlice fmlx_http_get_header(const FmlxHttpReq *r, const char *name);
int fmlx_http_has_header(const FmlxHttpReq *r, const char *name);

/* Se la connessione resta aperta: `HTTP/1.0` chiude di default, `1.1` no, e i token di
 * `Connection` vincevano su entrambi. I token si confrontano in una lista separata da
 * virgole, case-insensitive — un `keep-alive` dentro `close, keep-alive` c'è davvero. */
int fmlx_http_keep_alive(const FmlxHttpReq *r);

/* Il valore di `Expect: 100-continue`, se presente (il server DEVE rispondere 100 prima
 * di leggere un body grande, o il client si mette nei tempi morti). */
int fmlx_http_expects_continue(const FmlxHttpReq *r);

/* ---- Risposta ------------------------------------------------------------- */

/* Il writer della risposta scrive la SOLA testata; il corpo lo scrive il chiamante nello
 * stesso buffer o subito dopo. `Content-Length` è scritto da `body_len`: non esiste una
 * chiamata che lasci scegliere al chiamante un valore incoerente col corpo. */
typedef struct {
    char *buf;
    size_t cap, len;
    int chunked; /* 1 = si scrive `Transfer-Encoding: chunked` al posto di CL (SSE) */
    FmlxHttpStatus err;
} FmlxHttpRes;

void fmlx_http_res_init(FmlxHttpRes *w, char *buf, size_t cap);

/* Status-line + header fissi + `Content-Length: body_len` + CRLF finale. */
FmlxHttpStatus fmlx_http_write_head(FmlxHttpRes *w, int status, const char *content_type,
                                    uint64_t body_len, const char *extra_headers);

/* Un chunk SSE `data: ...\n\n` (o `event: ...\ndata: ...\n\n` se `event` non è NULL).
 * Un payload con `\n` DENTRO deve diventare PIÙ righe `data:`: è l'errore SSE più comune
 * e silenzioso, e qui è gestito dal writer, non dalla chiamata. */
FmlxHttpStatus fmlx_http_sse_write(FmlxHttpRes *w, const char *event, FmlxSlice data);

/* Chiude lo streaming: `0\r\n\r\n` se chunked, altrimenti nessun byte. */
FmlxHttpStatus fmlx_http_sse_end(FmlxHttpRes *w);

/* ---- Piccole utilities condivise (niente di più: sono il motivo per cui questo header
 * non dipende da una libreria di supporto) ---- */

/* Confronto case-insensitive ASCII di una slice con una C string (la slice NON è
 * terminata: `strlen` su una header name è una lettura oltre il fine). */
int fmlx_http_slice_ieq(FmlxSlice s, const char *lower);

/* Copia in `dst` (cap `dn`) returnnando i byte scritti, o `(size_t)-1` se non entrano. */
size_t fmlx_http_slice_copy(FmlxSlice s, char *dst, size_t dn);

#ifdef __cplusplus
}
#endif

#endif /* FMLX_HTTP_H */
