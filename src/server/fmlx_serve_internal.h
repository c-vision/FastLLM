/* fmlx_serve_internal.h — superfici condivise fra i due file del layer ③ (`fmlx_serve_respond.c`
 * e `fmlx_serve_admin.c`). NON è API della libreria: sta in `src` apposta. L'unica cosa che
 * questi file condividono è la MESSA IN FORMA di una risposta (testata già pronta + corpo) e
 * gli handler di amministrazione modelli, che non appartengono al dispatch delle chat. */
#ifndef FMLX_SERVE_INTERNAL_H
#define FMLX_SERVE_INTERNAL_H

#include "fmlx/fmlx_serve.h"

#include "engine/fmlx_engine.h"

#include <stdint.h>

/* Byte lasciati liberi in testa al buffer per la testata scritta poi da `fmlx_http_write_head`. */
#define FMLX_SERVE_RESERVE 256u

/* Testata (status + content-type + framing) + corpo già scritto a `resp + FMLX_SERVE_RESERVE`. */
FmlxServeSt fmlx_serve_finish_ok(FmlxServeBuf *buf, int status, const char *content_type,
                                 size_t body_len, int ka, size_t *out_len);

/* Errore del catalogo, renderizzato nel dialetto giusto. */
FmlxServeSt fmlx_serve_refuse_simple(FmlxServeBuf *buf, FmlxApiErr err, int anthropic,
                                     const char *code, const char *msg, size_t *out_len);

/* Errore del motore reso nel dialetto giusto e con un codice macchina-leggibile. */
FmlxServeSt fmlx_serve_engine_fail(FmlxServeBuf *buf, FmlxApiErr err, const char *code,
                                   const char *msg, int anthropic, size_t *out_len);

/* `/v1/models/status|load|unload`. */
FmlxServeSt fmlx_serve_body_models_status(FmlxServeBuf *buf, int ka, size_t *out_len);
FmlxServeSt fmlx_serve_body_model_load(FmlxServeBuf *buf, const char *body, size_t body_len,
                                       const FmlxServeOpts *o, int ka, size_t *out_len);
FmlxServeSt fmlx_serve_body_model_unload(FmlxServeBuf *buf, const char *body, size_t body_len,
                                         int ka, size_t *out_len);

/* ---- contatori del server e `GET /metrics` (fmlx_serve_metrics.c) ----
 * Scritti dal responder e dal ponte verso il motore; letti SOLO da `fmlx_serve_body_metrics`. */
void fmlx_metrics_request(void);                              /* una richiesta arrivata al responder */
void fmlx_metrics_route(FmlxApiAction act);                   /* +1 sulla serie dell'endpoint */
void fmlx_metrics_inflight(int delta);                        /* gauge richieste di generazione attive */
void fmlx_metrics_tokens(uint64_t prompt, uint64_t completion); /* dal DONE del motore */
void fmlx_metrics_cache(int hit);                             /* 1 = prefisso riusato, 0 = prefill */
void fmlx_metrics_duration(double seconds);                   /* durata di UNA generazione completata */
/* GET /metrics: testo Prometheus. */
FmlxServeSt fmlx_serve_body_metrics(FmlxServeBuf *buf, int ka, size_t *out_len);

#endif /* FMLX_SERVE_INTERNAL_H */
