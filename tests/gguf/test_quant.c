/* test_quant.c — test autocontenuti della lane di dequant (nessun riferimento esterno).
 *
 * Il confronto DIFFERENZIALE con le implementazioni già in esercizio è in
 * tools/quant_diff.c (volontariamente FUORI dalla build: richiede i sorgenti di un altro
 * repo). Qui si prova ciò che si può provare da soli: i valori IEEE noti, i blocchi
 * costruiti a mano, e — soprattutto — che un RIFIUTO non tocca `dst`: la regola del "mai un
 * risultato silenzioso" vale anche per i float.
 */
#include "fmlx/fmlx_gguf.h"
#include "fmlx/fmlx_quant.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;

#define CHECK(cond, ...) do {                                     \
    checks++;                                                     \
    if (!(cond)) { fails++;                                       \
        printf("  FAIL %s:%d  ", __FILE__, __LINE__);             \
        printf(__VA_ARGS__);                                      \
        printf("\n");                                             \
    }                                                             \
} while (0)

#define OK(name) printf("  ok   %s\n", name)

static uint32_t bits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static void st16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }

/* ------------------------------------------------------------------ f16 / bf16 */
static void test_half_conversion(void) {
    CHECK(bits(fmlx_ggml_f16_to_f32(0x3C00u)) == 0x3F800000u, "f16 1.0");
    CHECK(bits(fmlx_ggml_f16_to_f32(0xBC00u)) == 0xBF800000u, "f16 -1.0");
    CHECK(bits(fmlx_ggml_f16_to_f32(0x0000u)) == 0x00000000u, "f16 +0");
    CHECK(bits(fmlx_ggml_f16_to_f32(0x8000u)) == 0x80000000u, "f16 -0 (il segno si perde)");
    /* subnormale minimo: 2^-24 */
    CHECK(bits(fmlx_ggml_f16_to_f32(0x0001u)) == 0x33800000u, "f16 subnormale 0x0001 -> 0x%08x",
          bits(fmlx_ggml_f16_to_f32(0x0001u)));
    CHECK(bits(fmlx_ggml_f16_to_f32(0x7C00u)) == 0x7F800000u, "f16 +inf");
    CHECK((fmlx_ggml_f16_to_f32(0x7E00u) * 0.0f) != 0.0f, "f16 NaN deve restare NaN");
    CHECK(bits(fmlx_ggml_bf16_to_f32(0x3F80u)) == 0x3F800000u, "bf16 1.0");
    CHECK(bits(fmlx_ggml_bf16_to_f32(0xC080u)) == 0xC0800000u, "bf16 -4.0");
    OK("f16/bf16 -> f32 (normali, subnormali, inf, NaN)");
}

/* ------------------------------------------------------------------ blocchi a mano */
static void test_q4_0_by_hand(void) {
    uint8_t blk[18];
    float y[32];
    int i;
    memset(blk, 0, sizeof blk);
    st16(blk, 0x3C00u); /* d = 1.0 */
    for (i = 0; i < 16; i++) blk[2 + i] = 0x88; /* nibble 8 -> 8-8 = 0 */
    CHECK(fmlx_ggml_dequant_row(2, blk, 32, y) == FMLX_GGUF_OK, "Q4_0 rifiutato");
    for (i = 0; i < 32; i++) CHECK(y[i] == 0.0f, "Q4_0[%d] = %g, atteso 0", i, (double)y[i]);

    /* Il nibble BAESSO è l'elemento j, il nibble ALTO è l'elemento j+16 (forma ggml,
     * verificata bit a bit da tools/quant_diff.c contro gmb_moe_ggml.c). */
    blk[2] = 0x0F; /* elemento 0: nibble basso 15 -> +7 ; elemento 16: nibble alto 0 -> -8 */
    CHECK(fmlx_ggml_dequant_row(2, blk, 32, y) == FMLX_GGUF_OK, "Q4_0 2");
    CHECK(y[0] == 7.0f, "Q4_0[0] = %g, atteso +7", (double)y[0]);
    CHECK(y[16] == -8.0f, "Q4_0[16] = %g, atteso -8", (double)y[16]);
    OK("Q4_0 costruito a mano");
}

static void test_q8_0_by_hand(void) {
    uint8_t blk[34];
    float y[32];
    int i;
    st16(blk, 0x4000u); /* d = 2.0 */
    for (i = 0; i < 32; i++) blk[2 + i] = (uint8_t)(i % 2 ? -1 : 1);
    CHECK(fmlx_ggml_dequant_row(8, blk, 32, y) == FMLX_GGUF_OK, "Q8_0 rifiutato");
    CHECK(y[0] == 2.0f && y[1] == -2.0f && y[31] == -2.0f, "Q8_0: %g %g %g", (double)y[0],
          (double)y[1], (double)y[31]);
    OK("Q8_0 costruito a mano");
}

/* IQ2_XXS a mano: un blocco da 66 byte, costruito perché OGNI voce della grid usata sia
 * la numero 0 (otto valori = 8) e d = 1.0. Così 0.125*1*ls*8 = ls, e si vedono separatamente
 * la scala ls e il mask di segni — che è tutto ciò che questo tipo sa fare. */
static void test_iq2_xxs_by_hand(void) {
    uint8_t blk[66];
    float y[256];
    int i;
    memset(blk, 0, sizeof blk);
    st16(blk, 0x3C00u); /* d = 1.0 */
    CHECK(fmlx_ggml_dequant_row(16, blk, 256, y) == FMLX_GGUF_OK, "IQ2_XXS rifiutato");
    for (i = 0; i < 256; i++) CHECK(y[i] == 1.0f, "IQ2_XXS[%d] = %g, atteso 1", i,
                                    (double)y[i]);

    /* gruppo ib=1 (elementi 32..63), sottogruppo li=0: aux32[1] = byte 14..17 = 0x00000001
     * => sidx = 1 e ksigns[1] = 129 = 0b10000001 => segnano j=0 e j=7. */
    blk[2 + 8 * 1 + 4] = 0x01;
    CHECK(fmlx_ggml_dequant_row(16, blk, 256, y) == FMLX_GGUF_OK, "IQ2_XXS 2");
    CHECK(y[32] == -1.0f && y[39] == -1.0f, "segni j=0/j=7: %g %g", (double)y[32],
          (double)y[39]);
    CHECK(y[33] == 1.0f && y[38] == 1.0f, "segni j=1..6 non toccati: %g %g", (double)y[33],
          (double)y[38]);
    CHECK(y[40] == 1.0f && y[63] == 1.0f, "sottogruppo li=1 deve restare +1");

    /* gruppo ib=2: aux32[1] = byte 22..25, con 0x10000000 => ls = 2*1+1 = 3 e nessun segno
     * (0x10000000 >> 7*li & 127 vale 0 per li = 0..3). */
    blk[2 + 8 * 2 + 7] = 0x10;
    CHECK(fmlx_ggml_dequant_row(16, blk, 256, y) == FMLX_GGUF_OK, "IQ2_XXS 3");
    for (i = 64; i < 96; i++) CHECK(y[i] == 3.0f, "IQ2_XXS[%d] = %g, atteso ls = 3", i,
                                    (double)y[i]);
    OK("IQ2_XXS costruito a mano (grid 0, segni, scala ls)");
}

/* Il rifiuto non deve lasciare valori in `dst`: è il modo in cui i lettori di casa
 * producono pesi a zero senza dire niente. L'elenco da rifiutare NON è una lista a mano:
 * era `bad[] = {3, 4, 5, 11, 15, 17, ...}`, e quando i tipi 3/11/15 sono diventati coperti
 * il test è arancione da solo — il che va bene — ma la lista non diceva MAI a nessuno di
 * aggiornarla. Qui si iterà la tabella e si chiede a `is_covered` chi deve parlare. */
static void test_refusal_does_not_write(void) {
    uint8_t src[4096];
    float y[1024];
    int t;
    memset(src, 0x5A, sizeof src);
    for (t = 0; t <= FMLX_GGML_TYPE_MAX; t++) {
        uint64_t i;
        FmlxGgufStatus s;
        if (fmlx_ggml_type_is_covered(t) || !fmlx_ggml_type(t)) continue;
        for (i = 0; i < 1024; i++) y[i] = 7.5f;
        s = fmlx_ggml_dequant_row(t, src, 1024, y);
        CHECK(s != FMLX_GGUF_OK, "tipo %d: ritornato OK, il gap non doveva essere silenzioso",
              t);
        for (i = 0; i < 1024 && s != FMLX_GGUF_OK; i++) {
            if (bits(y[i]) != bits((float){ 7.5f })) break;
        }
        CHECK(i == 1024, "tipo %d: dst sporcata nonostante il rifiuto", t);
    }
    /* Il tipo 17 (IQ2_XS) è legittimo ma NON COPERTO: deve dirlo con il SUO codice. */
    {
        uint64_t i;
        for (i = 0; i < 1024; i++) y[i] = 7.5f;
        CHECK(fmlx_ggml_dequant_row(17, src, 1024, y) == FMLX_GGUF_ERR_NOT_COVERED,
              "IQ2_XS deve dare NOT_COVERED");
        CHECK(fmlx_ggml_dequant_row(4, src, 1024, y) == FMLX_GGUF_ERR_TYPE_REMOVED,
              "Q4_2 deve dare TYPE_REMOVED");
        CHECK(fmlx_ggml_dequant_row(999, src, 1024, y) == FMLX_GGUF_ERR_TYPE_UNKNOWN,
              "tipo 999 deve dare TYPE_UNKNOWN");
    }
    OK("i rifiuti non scrivono in dst e hanno codici distinti");
}

/* Ogni tipo coperto, su blocchi tutti zero, deve produrre zeri e niente di più. */
#define NELEM   2048u /* multipla di 1, 32 e 256: tutti i blocchi della tabella */
#define SRC_CAP 8192u /* il tipo più esigente a 2048 elementi è F32: 2048 * 4 byte */
static void test_zero_blocks(void) {
    uint8_t src[SRC_CAP];
    float y[NELEM];
    int t;
    memset(src, 0, sizeof src);
    for (t = 0; t <= FMLX_GGML_TYPE_MAX; t++) {
        const FmlxGgmlType *ty = fmlx_ggml_type(t);
        uint64_t bs = ty ? ty->block_vals : 1, rowb = 0, n, i;
        FmlxGgufStatus s;
        if (!fmlx_ggml_type_is_covered(t)) continue; /* il resto è nel test sopra */
        CHECK(ty != NULL, "tipo %d coperto ma assente dalla tabella", t);
        if (!ty) continue;
        n = NELEM - (NELEM % bs);
        /* Il contratto di dequant_row è che `src` contenga i byte della riga
         * (include/fmlx/fmlx_quant.h:26): se non ci sta, il test non è valido. */
        CHECK(fmlx_ggml_type_bytes(t, n, &rowb) == FMLX_GGUF_OK && rowb <= SRC_CAP,
              "tipo %d: riga da %llu byte oltre il buffer da %u", t, (unsigned long long)rowb,
              SRC_CAP);
        if (rowb > SRC_CAP) continue;
        for (i = 0; i < n; i++) y[i] = 1.0f;
        s = fmlx_ggml_dequant_row(t, src, n, y);
        CHECK(s == FMLX_GGUF_OK, "tipo %d sui blocchi zero: %s", t, fmlx_gguf_status_str(s));
        if (s != FMLX_GGUF_OK) continue;
        for (i = 0; i < n && y[i] == 0.0f; i++) {}
        CHECK(i == n, "tipo %d: valore non nullo all'elemento %llu", t,
              (unsigned long long)i);
    }
    /* e per tutti i tipi della tabella: is_covered == (dequant ritorna OK su dati validi) */
    {
        for (t = 0; t <= FMLX_GGML_TYPE_MAX; t++) {
            const FmlxGgmlType *ty = fmlx_ggml_type(t);
            uint64_t rowb = 0, n;
            FmlxGgufStatus s;
            if (!ty || !ty->usable) continue;
            n = NELEM - (NELEM % ty->block_vals);
            if (fmlx_ggml_type_bytes(t, n, &rowb) != FMLX_GGUF_OK || rowb > SRC_CAP) continue;
            s = fmlx_ggml_dequant_row(t, src, n, y);
            CHECK((s == FMLX_GGUF_OK) == (fmlx_ggml_type_is_covered(t) == 1),
                  "tipo %d: esito %d incoerente con is_covered(%d)", t, (int)s,
                  fmlx_ggml_type_is_covered(t));
        }
    }
    OK("blocchi zero: ogni tipo coperto dequantizza + coerenza is_covered su tutta la tabella");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("lane_gguf/quant:\n");
    test_half_conversion();
    test_q4_0_by_hand();
    test_q8_0_by_hand();
    test_iq2_xxs_by_hand();
    test_refusal_does_not_write();
    test_zero_blocks();
    printf("\n%d verifiche, %d fallite\n", checks, fails);
    return fails ? 1 : 0;
}
