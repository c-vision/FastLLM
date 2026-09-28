/* fmlx_tools.h — riconoscimento delle chiamate di tool nel TESTO generato dal modello.
 *
 * PERCHÉ QUI. Il modello non "ritorna" un tool: lo SCRIVE, nella convenzione della sua
 * famiglia (Qwen/ChatML `<tool_call>{"name":...,"arguments":{...}}</tool_call>`, Hermes lo
 * stesso, Llama-3 nativo un oggetto JSON nudo `{"name":...,"parameters":{...}}`, i modelli
 * più vecchi `<|python_tag|>{...}` o `<function=name>{...}</function>`). Il motore produce
 * testo; questo modulo lo RILEGGE e ne estrae le chiamate, senza sapere nulla di HTTP né di
 * quale dialetto le vestirà (OpenAI `tool_calls` o Anthropic `tool_use`).
 *
 * DUE LIVELLI.
 *  - `fmlx_tools_parse` è PURO: dato un blocco di testo, ritorna le chiamate che contiene.
 *    È il pezzo verificabile senza motore e senza rete.
 *  - `FmlxToolStream` è lo STATO per il decode token-live: gli si alimentano i delta di testo
 *    e lui restituisce, nell'ordine, i pezzi di testo e le chiamate. Trattiene la coda che
 *    può ancora essere l'inizio di un marcatore, così un `<tool_call>` spezzato fra due token
 *    non esce mai come testo.
 *
 * REGOLA D'ONESTÀ. Un blocco tool con JSON invalido NON viene emesso come chiamata: si
 * scarta. Chi veste l'evento (l'adapter) è tenuto a non dichiarare `finish_reason:"tool_calls"`
 * se le chiamate non validano (piano:505) — qui l'argomento resta la STRINGA JSON così è
 * arrivata, e la validazione è dell'adapter.
 */
#ifndef FMLX_TOOLS_H
#define FMLX_TOOLS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FMLX_TOOL_MAX_CALLS 8u
#define FMLX_TOOL_MAX_NAME 96u

typedef struct {
    char name[FMLX_TOOL_MAX_NAME];
    char *args;      /* JSON come STRINGA (oggetto); mai NULL: "{}" se il modello non ne dà */
    size_t args_len;
} FmlxToolCall;

typedef struct {
    FmlxToolCall call[FMLX_TOOL_MAX_CALLS];
    int n;
} FmlxToolCalls;

void fmlx_tools_init(FmlxToolCalls *tc);
void fmlx_tools_free(FmlxToolCalls *tc);

/* Estrae le chiamate da un blocco di testo, riconoscendo in un colpo solo tutti i formati
 * sopra. Ritorna il numero di chiamate (0..FMLX_TOOL_MAX_CALLS). I nomi vuoti o fuori
 * formato si SCARTANO, non si inventano. */
int fmlx_tools_parse(const char *s, size_t n, FmlxToolCalls *out);

/* Parse del SOLO corpo di un `<tool_call>`/`<function=…>`: uno o più oggetti JSON
 * `{"name":…,"arguments"|"parameters":…}` e la forma XML `<function=NAME><parameter=K>V…`.
 * Esposto perché lo stream lo usa sui blocchi già delimitati e il test lo prova a parte. */
int fmlx_tools_parse_body(const char *s, size_t n, FmlxToolCalls *out);

/* ---- stream token-live ---- */

typedef struct FmlxToolStream FmlxToolStream;

/* `allow_bare` = 1 abilita il riconoscimento dell'oggetto JSON nudo (famiglia Llama-3
 * nativa, che NON usa marcatori): in quel caso un `{…}` completo con `name` e
 * `parameters`/`arguments` diventa una chiamata, e finché è incompleto viene trattenuto.
 * Per le famiglie a marcatore (Qwen/ChatML) si passa 0. */
FmlxToolStream *fmlx_toolstream_new(int allow_bare);
void fmlx_toolstream_free(FmlxToolStream *t);

/* Alimenta un delta di testo decodificato. Il testo può essere trattenuto in coda. */
void fmlx_toolstream_feed(FmlxToolStream *t, const char *s, size_t n);
/* Chiude: libera la coda indecisa (testo che non è mai diventato un marcatore o un blocco
 * completo) come testo, e i blocchi tool incompleti come chiamate best-effort. */
void fmlx_toolstream_finish(FmlxToolStream *t);

typedef enum { FMLX_TS_TEXT = 0, FMLX_TS_TOOL_NAME = 1, FMLX_TS_TOOL_ARGS = 2 } FmlxToolOutKind;

typedef struct {
    FmlxToolOutKind kind;
    const char *text; /* TEXT/ARGS: NON terminato, `n` byte */
    size_t n;
    const char *name; /* TOOL_NAME */
    int tool_index;   /* TOOL_NAME/ARGS: indice della chiamata */
} FmlxToolOut;

/* Estrae il prossimo pezzo. 1 = scritto in `*out`, 0 = coda vuota. I puntatori valgono fino
 * alla prossima chiamata di pop/feed/free. */
int fmlx_toolstream_pop(FmlxToolStream *t, FmlxToolOut *out);

/* Quante chiamate sono state emesse finora (per il `finish_reason`). */
int fmlx_toolstream_ntools(const FmlxToolStream *t);

#ifdef __cplusplus
}
#endif
#endif /* FMLX_TOOLS_H */
