/* test_codec.c — FastLLM CONSUMA il codec GGUF di LLMEnv (`llmcodec`).
 *
 * Non testa più un codec proprio: la lane `src/lane_gguf/` e l'ABI `mlxengine_*` sono state
 * eliminate. Qui si usa il writer e il parser di `LLMCodec`, la sua TASSONOMIA d'errore e il
 * suo dequant ggml — se il link a `llmcodec` si rompe, questo file non compila.
 */
#include "llmcodec_gguf_read.h"
#include "llmcodec_gguf_writer.h"
#include "llmcodec_gguf_status.h"
#include "llmcodec_ggml_quants.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(cond, ...) do {                                     \
    checks++;                                                     \
    if (!(cond)) { fails++;                                       \
        printf("  FAIL %s:%d  ", __FILE__, __LINE__);             \
        printf(__VA_ARGS__); printf("\n");                        \
    }                                                             \
} while (0)
#define OK(name) printf("  ok   %s\n", name)

static uint32_t bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static const char *tmp_path(void) {
    const char *d = getenv("TMPDIR");
    static char p[512];
    snprintf(p, sizeof p, "%s/fmlx_test_codec.gguf", (d && *d) ? d : "/tmp");
    return p;
}

static void test_write_read_roundtrip(void) {
    float f32[32];
    unsigned char q8[34];
    uint64_t dims[2] = { 32, 1 };
    LlmCodecGgufTensor ts[2];
    LlmCodecGgufMetaKV kv;
    for (int i = 0; i < 32; i++) f32[i] = (float)i * 0.5f;
    memset(q8, 0, sizeof q8);
    q8[0] = 0x00; q8[1] = 0x40;            /* d = 2.0 (f16 0x4000) */
    for (int i = 0; i < 32; i++) q8[2 + i] = (unsigned char)(i % 2 ? 0xFF : 0x01);

    memset(&kv, 0, sizeof kv);
    kv.key = "general.architecture"; kv.type = LLMCODEC_GGUF_MT_STRING; kv.str = "codec-test";
    memset(ts, 0, sizeof ts);
    ts[0].name = "w.f32"; ts[0].ggml_type = LLMCODEC_GGML_T_F32; ts[0].dims = dims; ts[0].ndim = 2; ts[0].data = f32;
    ts[1].name = "w.q8";  ts[1].ggml_type = LLMCODEC_GGML_T_Q8_0; ts[1].dims = dims; ts[1].ndim = 2; ts[1].data = q8;

    CHECK(llmcodec_gguf_write(tmp_path(), &kv, 1, ts, 2, 32) == 0, "llmcodec_gguf_write");

    LlmCodecGgufFileInfo info;
    LlmCodecGgufStatus st = llmcodec_gguf_read_info_status(tmp_path(), &info);
    CHECK(st == LLMCODEC_GGUF_STATUS_OK, "read_info: %s", llmcodec_gguf_status_str(st));
    CHECK(info.n_tensors == 2 && info.n_kv == 1, "conteggi %llu/%llu",
          (unsigned long long)info.n_tensors, (unsigned long long)info.n_kv);
    CHECK(info.tensors[0].ggml_type == LLMCODEC_GGML_T_F32, "tipo 0 = %d", info.tensors[0].ggml_type);
    CHECK(info.tensors[1].ggml_type == LLMCODEC_GGML_T_Q8_0, "tipo 1 = %d", info.tensors[1].ggml_type);
    CHECK(strcmp(info.tensors[1].name, "w.q8") == 0, "nome 1 = \"%s\"", info.tensors[1].name);
    llmcodec_gguf_free_info(&info);
    unlink(tmp_path());
    OK("writer + parser di LLMCodec (round-trip)");
}

static void test_dequant(void) {
    unsigned char q8[34];
    float y[32];
    int i;
    /* Q8_0: d = 2.0, qs alternati 1/-1 -> y[0]=+2, y[1]=-2 */
    memset(q8, 0, sizeof q8);
    q8[0] = 0x00; q8[1] = 0x40;
    for (i = 0; i < 32; i++) q8[2 + i] = (unsigned char)(i % 2 ? 0xFF : 0x01);
    CHECK(llmcodec_ggml_block_vals(LLMCODEC_GGML_T_Q8_0) == 32, "block_vals Q8_0");
    CHECK(llmcodec_ggml_dequant_row(LLMCODEC_GGML_T_Q8_0, q8, y, 32) == 0, "dequant Q8_0");
    CHECK(y[0] == 2.0f && y[1] == -2.0f && y[31] == -2.0f, "Q8_0: %g %g %g",
          (double)y[0], (double)y[1], (double)y[31]);

    /* F16 -> F32 */
    {
        unsigned char h[2]; float f;
        h[0] = 0x00; h[1] = 0x3C;   /* 1.0 */
        CHECK(llmcodec_ggml_dequant_row(LLMCODEC_GGML_T_F16, h, &f, 1) == 0, "dequant F16");
        CHECK(bits(f) == 0x3F800000u, "F16 1.0 -> 0x%08x", bits(f));
    }
    OK("dequant dei tipi ggml via LLMCodec");
}

static void test_taxonomy(void) {
    unsigned char b[24];
    LlmCodecGgufFileInfo info;
    memset(b, 0, sizeof b);
    /* magic sbagliata */
    b[0] = 0x44; b[1] = 0x33; b[2] = 0x22; b[3] = 0x11;
    CHECK(llmcodec_gguf_parse_mem_status(b, sizeof b, NULL, NULL, &info) ==
          LLMCODEC_GGUF_STATUS_NOT_GGUF, "magic diversa non distinta");
    /* troncato */
    memset(b, 0, sizeof b);
    b[0] = 0x47; b[1] = 0x47; b[2] = 0x55; b[3] = 0x46;   /* "GGUF" LE */
    b[4] = 1;   /* versione 1 */
    CHECK(llmcodec_gguf_parse_mem_status(b, sizeof b, NULL, NULL, &info) ==
          LLMCODEC_GGUF_STATUS_VERSION, "versione 1 non distinta");
    {
        int i;
        for (i = 0; i <= 25; i++)
            CHECK(llmcodec_gguf_status_str((LlmCodecGgufStatus)i) != NULL, "status_str(%d) NULL", i);
    }
    OK("tassonomia d'errore di LLMCodec");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("fastllm/codec (llmcodec):\n");
    test_write_read_roundtrip();
    test_dequant();
    test_taxonomy();
    printf("\n%d verifiche, %d fallite\n", checks, fails);
    return fails ? 1 : 0;
}
