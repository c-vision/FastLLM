/* fmlx_serve_metrics.c — i contatori del server e la rotta `GET /metrics` (formato Prometheus).
 *
 * PERCHÉ UN FILE SUO. I contatori non sono né il dispatch di una richiesta né il loop del socket:
 * sono lo STATO OSSERVABILE del server, scritto da più punti (il responder per le richieste, il
 * ponte verso il motore per i token e la cache). Tenerli qui, con un solo punto di lettura, è ciò
 * che impedisce a due punti di incrementare la stessa cosa (o di dimenticarsene) — ed è la stessa
 * ragione per cui `/metrics` è una rotta a sé e non un ramo di `/health`.
 *
 * ONESTÀ DEI NUMERI (la regola del progetto): ogni valore qui è un FATTO già accaduto.
 *  - `requests_total` / `..._by_endpoint_total`: incrementati quando la richiesta arriva al
 *    responder (dopo auth e routing);
 *  - `tokens_total`: dal DONE della generazione, cioè dai conteggi VERI del motore, mai stimati;
 *  - `prefix_cache_hits_total` / `..._misses_total`: dal valore vero di `append_ids_prefix`
 *    ("ho riusato la KV" vs "ho fatto il prefill"), non da un'euristica sui tempi;
 *  - `models_loaded` è letto DALLA cache residente del motore, non da un contatore nostro.
 * Il server è a un solo thread (una connessione per volta, `fmlx_serve.c`), quindi non serve
 * atomica; il giorno in cui il decode diventa concorrente questo è l'unico file da toccare.
 */
#include "fmlx_serve_internal.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    unsigned long long requests_total;
    unsigned long long by_endpoint[FMLX_API_ACT_EMBEDDINGS + 1];
    unsigned long long tokens_prompt;
    unsigned long long tokens_completion;
    unsigned long long prefix_hits;
    unsigned long long prefix_misses;
    long long inflight;
    double duration_sum;
    unsigned long long duration_count;
    /* KV dell'ULTIMA generazione completata: `used`/`cap` vengono da `kv_stats` (ABI v2) sulla
     * sessione. La sessione vive quanto la richiesta, quindi il gauge è "l'ultima foto", non
     * un valore corrente — ed è esattamente ciò che `kv_stats` può dire senza inventare. */
    int kv_used;
    int kv_cap;
} FmlxMetrics;

static FmlxMetrics g_m;

void fmlx_metrics_request(void) { g_m.requests_total++; }

void fmlx_metrics_route(FmlxApiAction act) {
    if (act >= 0 && act <= FMLX_API_ACT_EMBEDDINGS) g_m.by_endpoint[act]++;
}

void fmlx_metrics_inflight(int delta) {
    g_m.inflight += delta;
    if (g_m.inflight < 0) g_m.inflight = 0; /* un gauge non mente mai negativo */
}

void fmlx_metrics_tokens(uint64_t prompt, uint64_t completion) {
    g_m.tokens_prompt += prompt;
    g_m.tokens_completion += completion;
}

void fmlx_metrics_cache(int hit) {
    if (hit) g_m.prefix_hits++;
    else g_m.prefix_misses++;
}

void fmlx_metrics_kv(int used, int capacity) {
    if (used < 0) used = 0;
    if (capacity < 0) capacity = 0;
    g_m.kv_used = used;
    g_m.kv_cap = capacity;
}

void fmlx_metrics_duration(double seconds) {
    if (seconds < 0) seconds = 0;
    g_m.duration_sum += seconds;
    g_m.duration_count++;
}

/* Una etichetta stabile per endpoint: è il nome del ROUTER, non del path grezzo (un query string
 * non deve creare una serie nuova a ogni richiesta). */
static const char *endpoint_label(FmlxApiAction a) {
    switch (a) {
    case FMLX_API_ACT_CHAT: return "chat";
    case FMLX_API_ACT_COMPLETIONS: return "completions";
    case FMLX_API_ACT_MESSAGES: return "messages";
    case FMLX_API_ACT_COUNT_TOKENS: return "count_tokens";
    case FMLX_API_ACT_EMBEDDINGS: return "embeddings";
    case FMLX_API_ACT_RESPONSES: return "responses";
    case FMLX_API_ACT_LOAD: return "models_load";
    case FMLX_API_ACT_UNLOAD: return "models_unload";
    case FMLX_API_ACT_MODELS_STATUS: return "models_status";
    case FMLX_API_ACT_MODELS: return "models";
    case FMLX_API_ACT_HEALTH: return "health";
    case FMLX_API_ACT_METRICS: return "metrics";
    case FMLX_API_ACT_NONE: break;
    }
    return "unknown";
}

/* Appende con `vsnprintf`; -1 se non c'è posto (mai un corpo a metà: chi chiama risponde 500). */
static int mp_append(char *dst, size_t cap, size_t *len, const char *fmt, ...) {
    va_list ap;
    int n;

    if (*len >= cap) return -1;
    va_start(ap, fmt);
    n = vsnprintf(dst + *len, cap - *len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *len) return -1;
    *len += (size_t)n;
    return 0;
}

#define MP(...)                                                                                   \
    do {                                                                                          \
        if (mp_append(body, cap, &len, __VA_ARGS__) != 0) {                                       \
            return fmlx_serve_refuse_simple(buf, FMLX_API_INTERNAL, 0, NULL,                      \
                                            "the metrics body does not fit this server's buffer",  \
                                            out_len);                                             \
        }                                                                                         \
    } while (0)

FmlxServeSt fmlx_serve_body_metrics(FmlxServeBuf *buf, int ka, size_t *out_len) {
    char *body;
    size_t cap, len = 0;

    if (!buf || !buf->resp || buf->resp_cap <= FMLX_SERVE_RESERVE) return FMLX_SERVE_BUF_SMALL;
    body = buf->resp + FMLX_SERVE_RESERVE;
    cap = buf->resp_cap - FMLX_SERVE_RESERVE;

    MP("# HELP fastllm_requests_total HTTP requests handled by this server.\n");
    MP("# TYPE fastllm_requests_total counter\n");
    MP("fastllm_requests_total %llu\n", g_m.requests_total);

    MP("# HELP fastllm_requests_by_endpoint_total Requests per routed endpoint.\n");
    MP("# TYPE fastllm_requests_by_endpoint_total counter\n");
    for (int a = 0; a <= FMLX_API_ACT_EMBEDDINGS; a++)
        MP("fastllm_requests_by_endpoint_total{endpoint=\"%s\"} %llu\n", endpoint_label((FmlxApiAction)a),
           g_m.by_endpoint[a]);

    MP("# HELP fastllm_requests_in_flight Generation requests currently running.\n");
    MP("# TYPE fastllm_requests_in_flight gauge\n");
    MP("fastllm_requests_in_flight %lld\n", g_m.inflight);

    MP("# HELP fastllm_tokens_total Tokens processed by the engine.\n");
    MP("# TYPE fastllm_tokens_total counter\n");
    MP("fastllm_tokens_total{kind=\"prompt\"} %llu\n", g_m.tokens_prompt);
    MP("fastllm_tokens_total{kind=\"completion\"} %llu\n", g_m.tokens_completion);

    MP("# HELP fastllm_generation_duration_seconds Wall time of completed generation requests.\n");
    MP("# TYPE fastllm_generation_duration_seconds summary\n");
    MP("fastllm_generation_duration_seconds_sum %.6f\n", g_m.duration_sum);
    MP("fastllm_generation_duration_seconds_count %llu\n", g_m.duration_count);

    MP("# HELP fastllm_prefix_cache_hits_total Prompts whose prefix was reused from the KV cache.\n");
    MP("# TYPE fastllm_prefix_cache_hits_total counter\n");
    MP("fastllm_prefix_cache_hits_total %llu\n", g_m.prefix_hits);

    MP("# HELP fastllm_prefix_cache_misses_total Prompts that required a full prefill.\n");
    MP("# TYPE fastllm_prefix_cache_misses_total counter\n");
    MP("fastllm_prefix_cache_misses_total %llu\n", g_m.prefix_misses);

    MP("# HELP fastllm_models_loaded Models currently resident in memory.\n");
    MP("# TYPE fastllm_models_loaded gauge\n");
    MP("fastllm_models_loaded %d\n", fmlx_engine_resident_count());

    /* KV reale (ABI v2 `kv_stats`): token usati e capacità dell'ULTIMA generazione completata.
     * 0/0 prima della prima generazione: un gauge non inventa una capacità che non ha letto. */
    MP("# HELP fastllm_kv_tokens_used KV positions used by the last completed generation.\n");
    MP("# TYPE fastllm_kv_tokens_used gauge\n");
    MP("fastllm_kv_tokens_used %d\n", g_m.kv_used);

    MP("# HELP fastllm_kv_tokens_capacity KV capacity (positions) reported by the engine.\n");
    MP("# TYPE fastllm_kv_tokens_capacity gauge\n");
    MP("fastllm_kv_tokens_capacity %d\n", g_m.kv_cap);

    return fmlx_serve_finish_ok(buf, 200, "text/plain; version=0.0.4; charset=utf-8", len, ka, out_len);
}
