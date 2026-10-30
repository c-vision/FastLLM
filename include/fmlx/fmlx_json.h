/* fmlx_json.h — parser e serializzatore JSON di FastLLM (lane json).

 * Perché riscriverlo quando esistono cjson/jansson: la lane json è l'unica superficie che
 * tocca OGNI byte che entra ed esce dalle API OpenAI/Anthropic, e qui il requisito non è
 * "reggere il JSON", è **rifiutare in modo distintivo** ciò che un client manda e che un
 * altro parser ingoia in silenzio. Due esempi che decidono il design:
 *  - `max_tokens: 1.5`: `json.loads` di Python lo accetta come float. Se qui diventasse 1
 *    per troncamento, il limite del client sarebbe mentito (piano §11.4: mai un valore
 *    normalizzato in silenzio). `fmlx_json_as_i64` su un reale fallisce: lo decide il
 *    chiamante, con l'errore `invalid_request` giusto.
 *  - `"a":1, "a":2`: chiave duplicata. Non è un errore di sintassi (RFC 8259 tace), ma
 *     rispondere a due valori diversi è peggio. Il parser le SEGNA (`had_dup_keys`) e
 *     `obj_get` prende la PRIMA, sempre: comportamento definito e documentato.
 *
 * Nessuna dipendenza, nessuna ricorsione illimitata (profondità max FMLX_JSON_MAX_DEPTH),
 * nessuna allocazione non dichiarata: il DOM sta in TRE blocchi mallocati dal parser e
 * liberati da `fmlx_json_doc_free`, e i limiti di nodi li dà il chiamante.
 *
 * `fmlx_json_parse` libera qualunque contenuto precedente di `out`: il documento deve
 * venire da `fmlx_json_doc_init` (o da una `parse` precedente), mai da memoria a caso.
 *
 * Il parser è confrontato con un oracolo indipendente — `json.loads` di Python 3.14 — su
 * un corpus di input ostili (`tests/tools/json_diff.c`; il confronto con `json.loads` di Python, tools/json_oracle.py, e' stato rimosso). Dove
 * divergiamo è scritto in docs/json-contract.md con la misura accanto.
 */
#ifndef FMLX_JSON_H
#define FMLX_JSON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FMLX_JSON_MAX_DEPTH 64u
#define FMLX_JSON_ERR_MSG_MAX 160

typedef enum {
    FMLX_JSON_OK = 0,
    FMLX_JSON_ERR_EMPTY = 1,        /* input vuoto o solo whitespace: non è un documento */
    FMLX_JSON_ERR_SYNTAX = 2,       /* forma non JSON (`err_off` dice l'offset del byte) */
    FMLX_JSON_ERR_TRAILING = 3,     /* c'è contenuto dopo il valore radice */
    FMLX_JSON_ERR_TOO_DEEP = 4,     /* oltre FMLX_JSON_MAX_DEPTH */
    FMLX_JSON_ERR_TOO_BIG = 5,      /* superato il limite di nodi dato dal chiamante */
    /* FMLX_JSON_ERR_NUMBER: numero NON RFC 8259. Accettiamo solo `-?(0|[1-9][0-9]*)`
     * seguito da `(.[0-9]+)?` e `([eE][-+]?[0-9]+)?`. Quindi `01`, `.5`, `5.`, `+1`,
     * `NaN`, `Infinity`, `1e` sono errori con il LORO codice, non "0" o "5". */
    FMLX_JSON_ERR_NUMBER = 6,
    /* FMLX_JSON_ERR_UTF8: byte non-ASCII che non formano UTF-8 valido nella stringa.
     * Scelta severa e voluta: una chat che fa da server non deve rimettere nel mondo
     * byte che non possono essere serializzati di nuovo. Un client che manda U+D800
     * scappolata da sola o in coppia spezzata riceve questo, non un replacement char. */
    FMLX_JSON_ERR_UTF8 = 7,
    /* FMLX_JSON_ERR_CONTROL: carattere di controllo (< 0x20) nudo in una stringa.
     * `\n` in un messaggio può arrivare davvero da un client che non fa l'escape: qui
     * è comunque errore, perché "accettarlo" e "normalizzarlo" sono due cose diverse e
     * la seconda cambierebbe il contenuto. Il server risponde 400 con l'offset. */
    FMLX_JSON_ERR_CONTROL = 8,
    FMLX_JSON_ERR_NO_MEM = 9,       /* malloc rifiutata (i limiti del body sono un'altra via) */
    FMLX_JSON_ERR_BAD_ARG = 10,     /* uso scorretto dell'API da parte del chiamante */
    FMLX_JSON_ERR_BUF_SMALL = 11    /* nel writer: il limite di capienza è stato superato */
} FmlxJsonStatus;

const char *fmlx_json_status_str(FmlxJsonStatus s);

typedef enum {
    FMLX_JSON_NULL = 0,
    FMLX_JSON_BOOL,
    FMLX_JSON_INT,   /* solo interi che stanno in int64; fuori range -> FMLX_JSON_ERR_NUMBER */
    FMLX_JSON_REAL,
    FMLX_JSON_STR,
    FMLX_JSON_ARR,
    FMLX_JSON_OBJ,
    FMLX_JSON_KEY    /* nodo-chiave di un membro: non è mai un valore esposto */
} FmlxJsonType;

typedef struct FmlxJsonDoc FmlxJsonDoc;
typedef struct FmlxJsonValue FmlxJsonValue;

struct FmlxJsonValue {
    const FmlxJsonDoc *d; /* il documento possiede arena e tabella dei figli */
    FmlxJsonType type;
    uint32_t child;     /* offset nella tabella dei figli (ARR/OBJ) */
    uint32_t n;         /* ARR: numero di elementi. OBJ: numero di ELEMENTI DELLA TABELLA = 2 * membri */
    uint32_t str_off;   /* STR: offset nel byte-arena del documento */
    uint32_t str_len;   /* STR: byte UTF-8 già unescapeati */
    union {
        int64_t i;
        double f;  /* `f`, non `d`: `v->d` è il documento, e i due convivono nello stesso nodo */
        int b;
    } v;
};

/* Il TAG deve esserci: `FmlxJsonValue` contiene un puntatore al documento, quindi il tipo
 * va dichiarato prima e DEFINITO dopo con lo stesso tag. Scrivere `typedef struct { ... }
 * FmlxJsonDoc` qui sotto significherebbe ridefinire il nome su un tipo anonimo, e il
 * compilatore si lamenta che `struct FmlxJsonDoc` resta incompleto. */
struct FmlxJsonDoc {
    FmlxJsonValue *nodes;
    uint32_t *kids;        /* per gli OBJ: [chiave, valore] alternati; per gli ARR: solo valori */
    char *arena;           /* byte delle stringhe */
    size_t nnodes, cap_nodes;
    size_t nkids, cap_kids;
    size_t narena, cap_arena;
    uint32_t root;         /* indice del nodo radice */
    int had_dup_keys;      /* almeno un OBJ del documento aveva chiavi ripetute */
    size_t err_off;        /* offset del byte colpevole (parser) */
    char err_msg[FMLX_JSON_ERR_MSG_MAX];
};

/* Azzera sempre, anche in caso di fallimento: un documento "sporco" non deve mai essere
 * passato a `doc_free` per errore e nemmeno letto. */
void fmlx_json_doc_init(FmlxJsonDoc *d);
void fmlx_json_doc_free(FmlxJsonDoc *d);

/* Limiti ragionevoli dal corpo della richiesta: ogni nodo costa almeno un byte di
 * sorgente, quindi `len` è un tetto onesto; `+2` per radice e sentinelle. */
size_t fmlx_json_node_bound(const char *src, size_t len);

/* Parse. `max_nodes` 0 = `fmlx_json_node_bound(src,len)`. Un corpo ostile di 8 MB con
 * `[[[[...` è fermato da `FMLX_JSON_ERR_TOO_DEEP` prima che dal limite di nodi. */
FmlxJsonStatus fmlx_json_parse(const char *src, size_t len, size_t max_nodes, FmlxJsonDoc *out);

const FmlxJsonValue *fmlx_json_root(const FmlxJsonDoc *d);
/* ATTENZIONE: il puntatore NON è terminato. L'arena conserva i byte della stringa e basta
 * (`narena = off + len`), quindi `%s` leggerebbe nel byte successivo. Si usa sempre
 * insieme a `fmlx_json_str_len`. Le KEY rispondono come le STR: il nodo viene da
 * `obj_key_at` e il chiamante sa cosa ha in mano (a differenza di `as_str`, che è
 * l'accessorio che *valida* e una chiave non la accetta). */
const char *fmlx_json_str(const FmlxJsonValue *v);   /* NULL se non STR/KEY */
size_t fmlx_json_str_len(const FmlxJsonValue *v);

/* I test d'accesso NON convertono mai: `as_i64` su un reale fallisce anche se il reale è
 * intero (4.0), perché la distinzione appartiene al protocollo, non al parser. */
int fmlx_json_is_null(const FmlxJsonValue *v);
int fmlx_json_as_bool(const FmlxJsonValue *v, int *out);
int fmlx_json_as_i64(const FmlxJsonValue *v, int64_t *out);
int fmlx_json_as_f64(const FmlxJsonValue *v, double *out);   /* INT e REAL entrambi ok */
int fmlx_json_as_str(const FmlxJsonValue *v, const char **p, size_t *len);

size_t fmlx_json_arr_n(const FmlxJsonValue *v);                       /* 0 se non ARR */
const FmlxJsonValue *fmlx_json_arr_at(const FmlxJsonValue *v, size_t i);
size_t fmlx_json_obj_n(const FmlxJsonValue *v);                       /* membri, 0 se non OBJ */
const FmlxJsonValue *fmlx_json_obj_get(const FmlxJsonValue *obj, const char *key);
const FmlxJsonValue *fmlx_json_obj_value_at(const FmlxJsonValue *obj, size_t i);
const FmlxJsonValue *fmlx_json_obj_key_at(const FmlxJsonValue *obj, size_t i);

/* ---- writer ----
 *
 * Due modi. Con `mem != NULL` il writer scrive nella memoria del chiamante e NON CRESCERÀ
 * MAI: oltre la capienza fornita arriva `FMLX_JSON_ERR_BUF_SMALL` (fare `realloc` di un array
 * dello stack è il crash che sembra un caso raro). Con `mem == NULL` cresce da sé con
 * realloc fino a `max`, e `fmlx_jsonw_free` è obbligatoria.
 * In entrambi i modi, oltre `max` entra in errore e OGNI chiamata successiva è un no-op:
 * `finish` restituisce l'errore e il buffer non va mai inviato. Un `Content-Length` scritto
 * a metà è il bug che non deve esistere.
 */
typedef struct {
    char *buf;
    char *base;        /* la memoria data dal chiamante: se `buf != base` il writer ha mallocato */
    size_t len, cap, max;
    uint32_t depth;
    /* I tre array sono lunghi FMLX_JSON_MAX_DEPTH: LA STESSA profondità del parser, non un
     * numero a parte. Stanno a 32 mentre il parser ne ammette 64, un documento legale da 33
     * a 64 livelli round-trippava `ERR_TOO_DEEP` dal writer — e il corpus l'ha misurato. */
    uint8_t need_comma[FMLX_JSON_MAX_DEPTH]; /* per livello: 1 = serve una virgola prima del prossimo */
    uint8_t in_obj[FMLX_JSON_MAX_DEPTH];     /* 1 = il livello corrente è un oggetto */
    uint8_t key_pending[FMLX_JSON_MAX_DEPTH]; /* 1 = l'oggetto attende il VALORE del membro in corso */
    /* Il valore radice è stato emesso. NON si può dedurre da `need_comma[0]`: quel flag è
     * anche lo stato del contenitore di livello 0, e `{}` chiuderebbe con "radice mai
     * emessa" — oltre a permettere un SECONDO valore radice dopo un oggetto chiuso. */
    uint8_t root_done;
    FmlxJsonStatus err;
} FmlxJsonW;

void fmlx_jsonw_init(FmlxJsonW *w, char *mem, size_t cap, size_t max);
/* Libera SOLO ciò che il writer ha mallocato: con `mem` del chiamante, `buf == base` e
 * nulla viene toccato. `finish` non va perso di vista: è l'unico punto che dice se la
 * risposta è completa, e il buffer è illeggibile finché non ritorna OK. */
void fmlx_jsonw_free(FmlxJsonW *w);
FmlxJsonStatus fmlx_jsonw_finish(FmlxJsonW *w, size_t *out_len);

/* Ogni `key` DEVE precedere il suo valore e solo in un oggetto; ogni `*_begin` va
 * chiuso. Usarli fuori posto porta `FMLX_JSON_ERR_BAD_ARG`, non output corrotto.
 * La forma `keyn` esiste perché una CHIAVE può contenere NUL (viene da `"a\u0000b"`):
 * una `strlen` lì dentro taglierebbe la chiave in due silenziosamente. */
FmlxJsonStatus fmlx_jsonw_key(FmlxJsonW *w, const char *key);
FmlxJsonStatus fmlx_jsonw_keyn(FmlxJsonW *w, const char *key, size_t len);
FmlxJsonStatus fmlx_jsonw_arr_begin(FmlxJsonW *w);
FmlxJsonStatus fmlx_jsonw_obj_begin(FmlxJsonW *w);
FmlxJsonStatus fmlx_jsonw_arr_end(FmlxJsonW *w);
FmlxJsonStatus fmlx_jsonw_obj_end(FmlxJsonW *w);

FmlxJsonStatus fmlx_jsonw_null(FmlxJsonW *w);
FmlxJsonStatus fmlx_jsonw_bool(FmlxJsonW *w, int b);
FmlxJsonStatus fmlx_jsonw_i64(FmlxJsonW *w, int64_t x);
FmlxJsonStatus fmlx_jsonw_u64(FmlxJsonW *w, uint64_t x);
FmlxJsonStatus fmlx_jsonw_f64(FmlxJsonW *w, double x);
FmlxJsonStatus fmlx_jsonw_str(FmlxJsonW *w, const char *s, size_t len);
FmlxJsonStatus fmlx_jsonw_cstr(FmlxJsonW *w, const char *s);
/* Valore già JSON: viene VALIDATO col parser prima di essere incorporato, perché il
 * piano (§11.4) vieta di emettere un `tool_use.input` parziale. Se non è JSON, errore. */
FmlxJsonStatus fmlx_jsonw_raw(FmlxJsonW *w, const char *json, size_t len);

/* Ricopia un sottoalbero del DOM in JSON nel writer. È il pezzo che rende possibile il
 * passaggio `arguments`(stringa OpenAI) ↔ `input`(oggetto Anthropic) senza stringhe
 * manipolate a mano: si parsea una volta e si renderizza nell'altra forma. */
FmlxJsonStatus fmlx_json_render(const FmlxJsonValue *v, FmlxJsonW *w);

/* Esportati perché il server deve poter misurare una stringa prima di scriverla e perché
 * il test del round-trip li usa direttamente. */
size_t fmlx_json_esc_len(const char *s, size_t len);   /* include i delimitatori "..." */
size_t fmlx_json_esc_into(const char *s, size_t len, char *out, size_t cap);

/* 1 = `s` è UTF-8 ben formato (niente surrogate codificati, niente sovrallungamenti,
 * niente sequenze tronche). Il writer la applica da solo; è esportata perché il SERVER
 * dello streaming ne ha bisogno PRIMA: un tokenizzatore può spezzare un codepoint fra due
 * chunk, e la risposta giusta è tenere da parte i byte incompleti e richiederla, non
 * mandare a `fmlx_jsonw_str` un frammento che verrebbe rifiutato. */
int fmlx_json_utf8_ok(const char *s, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* FMLX_JSON_H */
