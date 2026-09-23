/* fmlx_quant.c — dequant ggml -> F32. Vedi il contratto in include/fmlx/fmlx_quant.h.
 *
 * Le formule dei blocchi sono la stessa forma intera delle due implementazioni già in
 * esercizio su GMBasic (`gmb_host_ai_mlx/gmb_ggml.h:169-212` e
 * `gmb_moe_runtime/src/gmb_moe_ggml.c:121-251`); `tools/quant_diff.c` le confronta bit a
 * bit sui tensori REALI dei GGUF di questa macchina. Un tipo su cui quel confronto non è
 * possibile resta NOT_COVERED: errore esplicito, mai valori inventati.
 */
#include "fmlx/fmlx_quant.h"
#include "fmlx_quant_internal.h"
#include "fmlx_iq2_tables.h" /* GENERATO: vedi tools/gen_codebooks.py */

#include <string.h>

float fmlx_ggml_f16_to_f32(uint16_t h) {
    uint32_t s = (uint32_t)(h & 0x8000u) << 16;
    uint32_t e = (h >> 10) & 0x1Fu;
    uint32_t m = h & 0x3FFu;
    uint32_t u;
    if (e == 0) {
        if (m == 0) {
            u = s;
        } else { /* subnormale: normalizza la mantissa */
            e = 127 - 15 + 1;
            while (!(m & 0x400u)) {
                m <<= 1;
                e--;
            }
            m &= 0x3FFu;
            u = s | (e << 23) | (m << 13);
        }
    } else if (e == 31) {
        /* Inf (m == 0) va copiato così com'è; un NaN SEGNALATO (bit 10 della mantissa
         * spento) va QUIETATO: `ggml_fp16_to_fp32` arriva a un moltiplicatore che imposta
         * il bit più alto della mantissa, e il gate esaustivo sui 65536 pattern misura la
         * divergenza esattamente in 1022 = 2 segni × 511 mantisse con bit 10 spento.
         * Fuori da quell'insieme i due codici non divergono di un bit. */
        if (m && !(m & 0x200u)) m |= 0x200u;
        u = s | 0x7F800000u | (m << 13); /* inf / NaN */
    } else {
        u = s | ((e - 15 + 127) << 23) | (m << 13);
    }
    {
        float f;
        memcpy(&f, &u, 4);
        return f;
    }
}

float fmlx_ggml_bf16_to_f32(uint16_t b) {
    float f;
    uint32_t u = (uint32_t)b << 16;
    memcpy(&f, &u, 4);
    return f;
}

/* Scala e min dei sottoblocchi da 32 di un Q4_K/Q5_K: 8 gruppi in 12 bit (ggml). */
static void scale_min_k4(int j, const uint8_t *q, uint8_t *d, uint8_t *m) {
    if (j < 4) {
        *d = (uint8_t)(q[j] & 63);
        *m = (uint8_t)(q[j + 4] & 63);
    } else {
        *d = (uint8_t)((q[j + 4] & 0xF) | ((q[j - 4] >> 6) << 4));
        *m = (uint8_t)((q[j + 4] >> 4) | ((q[j] >> 6) << 4));
    }
}

int fmlx_ggml_type_is_covered(int type) {
    switch (type) {
    case 0:  /* F32  */ case 1:  /* F16  */ case 30: /* BF16 */
    case 2:  /* Q4_0 */ case 8:  /* Q8_0 */
    case 3:  /* Q4_1 */ case 6:  /* Q5_0 */ case 7:  /* Q5_1 */
    case 10: /* Q2_K */ case 11: /* Q3_K */ case 12: /* Q4_K */ case 13: /* Q5_K */
    case 14: /* Q6_K */ case 15: /* Q8_K */
    case 16: /* IQ2_XXS */
    case 24: /* I8   */ case 25: /* I16  */ case 26: /* I32  */
    case 20: /* IQ4_NL */ case 23: /* IQ4_XS */
    case 34: /* TQ1_0 */ case 35: /* TQ2_0 */
    case 39: /* MXFP4 */ case 40: /* NVFP4 */
    case 41: /* Q1_0  */
        return 1;
    default:
        return 0;
    }
}

/* ---- blocchi da 32 (Q4_0, Q8_0) ---- */

static void dq_q8_0(const uint8_t *p, uint64_t nblk, float *y) {
    uint64_t b;
    for (b = 0; b < nblk; b++) {
        const uint8_t *blk = p + b * 34;
        float d = fmlx_ggml_f16_to_f32(ld16(blk));
        int j;
        for (j = 0; j < 32; j++) y[b * 32 + j] = d * (float)(int8_t)blk[2 + j];
    }
}

static void dq_q4_0(const uint8_t *p, uint64_t nblk, float *y) {
    uint64_t b;
    for (b = 0; b < nblk; b++) {
        const uint8_t *blk = p + b * 18;
        float d = fmlx_ggml_f16_to_f32(ld16(blk));
        int j;
        for (j = 0; j < 16; j++) {
            y[b * 32 + j] = d * (float)((int)(blk[2 + j] & 0xF) - 8);
            y[b * 32 + j + 16] = d * (float)((int)(blk[2 + j] >> 4) - 8);
        }
    }
}

/* ---- K-quant (blocchi da 256) ---- */

static void dq_q2_k(const uint8_t *p, uint64_t nblk, float *y) {
    uint64_t b;
    for (b = 0; b < nblk; b++) {
        const uint8_t *x = p + b * 84;
        float d = fmlx_ggml_f16_to_f32(ld16(x + 80));
        float dmin = fmlx_ggml_f16_to_f32(ld16(x + 82));
        int g;
        for (g = 0; g < 16; g++) {
            /* L'indiccizzazione di qs è quella ggml: salta di 32 ogni 8 gruppi e di 16
             * nei gruppi dispari; 2 bit per valore, shift a coppie di gruppi. */
            uint32_t q_base = 32u * (uint32_t)(g / 8) + 16u * (uint32_t)(g & 1);
            uint32_t shift = 2u * (uint32_t)((g / 2) & 3);
            uint32_t sc = x[g];
            float sd = d * (float)(sc & 0xF), sm = dmin * (float)(sc >> 4);
            int l;
            for (l = 0; l < 16; l++) {
                uint32_t q = ((uint32_t)x[16 + q_base + (uint32_t)l] >> shift) & 3u;
                y[b * 256 + (uint64_t)g * 16 + (uint64_t)l] = sd * (float)q - sm;
            }
        }
    }
}

/* Q4_K (144) e Q5_K (176) condividono tutto tranne i 32 byte qh di Q5_K, che portano il
 * quinto/sesto bit e si frappongono tra scales e qs. */
static void dq_qk(const uint8_t *p, uint64_t nblk, float *y, int is5) {
    const uint64_t bsz = is5 ? 176 : 144;
    uint64_t b;
    for (b = 0; b < nblk; b++) {
        const uint8_t *x = p + b * bsz;
        float d = fmlx_ggml_f16_to_f32(ld16(x));
        float dmin = fmlx_ggml_f16_to_f32(ld16(x + 2));
        const uint8_t *scales = x + 4;
        const uint8_t *qh = is5 ? x + 16 : NULL;
        const uint8_t *qs = is5 ? x + 48 : x + 16;
        float *out = y + b * 256;
        int is = 0;
        unsigned u1 = 1, u2 = 2;
        int j;
        for (j = 0; j < 256; j += 64) {
            uint8_t s1, m1, s2, m2;
            scale_min_k4(is, scales, &s1, &m1);
            scale_min_k4(is + 1, scales, &s2, &m2);
            {
                float d1 = d * (float)s1, mn1 = dmin * (float)m1;
                float d2 = d * (float)s2, mn2 = dmin * (float)m2;
                int l;
                for (l = 0; l < 32; l++) {
                    int q = (int)(qs[l] & 0xF);
                    if (is5 && (qh[l] & u1)) q += 16;
                    out[l] = d1 * (float)q - mn1;
                }
                for (l = 0; l < 32; l++) {
                    int q = (int)(qs[l] >> 4);
                    if (is5 && (qh[l] & u2)) q += 16;
                    out[l + 32] = d2 * (float)q - mn2;
                }
            }
            out += 64;
            qs += 32;
            is += 2;
            if (is5) {
                u1 <<= 2;
                u2 <<= 2;
            }
        }
    }
}

static void dq_q6_k(const uint8_t *p, uint64_t nblk, float *y) {
    uint64_t b;
    for (b = 0; b < nblk; b++) {
        const uint8_t *ql = p + b * 210;
        const uint8_t *qh = ql + 128;
        const int8_t *sc = (const int8_t *)(qh + 64);
        float d = fmlx_ggml_f16_to_f32(ld16(qh + 64 + 16));
        float *out = y + b * 256;
        int n;
        for (n = 0; n < 256; n += 128) {
            int l;
            for (l = 0; l < 32; l++) {
                int is = l / 16;
                int q1 = (int)((ql[l] & 0xF) | (((qh[l] >> 0) & 3) << 4)) - 32;
                int q2 = (int)((ql[l + 32] & 0xF) | (((qh[l] >> 2) & 3) << 4)) - 32;
                int q3 = (int)((ql[l] >> 4) | (((qh[l] >> 4) & 3) << 4)) - 32;
                int q4 = (int)((ql[l + 32] >> 4) | (((qh[l] >> 6) & 3) << 4)) - 32;
                out[l] = d * (float)sc[is + 0] * (float)q1;
                out[l + 32] = d * (float)sc[is + 2] * (float)q2;
                out[l + 64] = d * (float)sc[is + 4] * (float)q3;
                out[l + 96] = d * (float)sc[is + 6] * (float)q4;
            }
            out += 128;
            ql += 64;
            qh += 32;
            sc += 8;
        }
    }
}

/* ---- IQ (codebook: i valori non sono una formula, sono 256 voci di tabella) ----
 *
 * Layout del blocco IQ2_XXS (66 byte, 256 elementi): d f16 a [0], poi 8 gruppi da 8 byte
 * (2 uint32) all'offset 2. Ogni gruppo copre 32 elementi come 4 sottogruppi da 8:
 * aux8[0..3] sono INDICI della grid (una voce = 8 valori da un byte), e in aux32[1]
 * stanno ls (bit 28..31) e quattro indici di segno da 7 bit (bit 7*li..7*li+6).
 *
 * `aux32` si legge con memcpy e NON con un cast a uint32_t*: l'offset del tensore nel file
 * può essere dispari, e un cast allineato sarebbe un crash su un GGUF legittimo.
 * GGUF è little-endian: il memcpy dà esattamente le 4 cifre che ggml legge. */
static void dq_iq2_xxs(const uint8_t *p, uint64_t nblk, float *y) {
    uint64_t b;
    for (b = 0; b < nblk; b++) {
        const uint8_t *blk = p + b * 66;
        float d = fmlx_ggml_f16_to_f32(ld16(blk));
        float *out = y + b * 256;
        uint32_t ib;
        for (ib = 0; ib < 8; ib++) {
            uint32_t aux32[2];
            const uint8_t *aux8 = (const uint8_t *)aux32;
            float ls;
            uint32_t li;
            memcpy(aux32, blk + 2 + 8 * ib, 8);
            ls = (float)(2u * (aux32[1] >> 28) + 1u);
            for (li = 0; li < 4; li++) {
                const uint8_t *grid = (const uint8_t *)(fmlx_iq2xxs_grid + aux8[li]);
                uint32_t sidx = (aux32[1] >> (7 * li)) & 127u;
                uint32_t sg = fmlx_iq2_ksigns[sidx];
                uint32_t j;
                for (j = 0; j < 8; j++) {
                    float v = (float)grid[j];
                    if (sg & (1u << j)) v = -v;
                    out[ib * 32 + li * 8 + j] = 0.125f * d * ls * v;
                }
            }
        }
    }
}

FmlxGgufStatus fmlx_ggml_dequant_row(int type, const uint8_t *src, uint64_t n, float *dst) {
    const FmlxGgmlType *ty;
    uint64_t nblk;

    if (!src || !dst) return FMLX_GGUF_ERR_IO;
    ty = fmlx_ggml_type(type);
    if (!ty) return FMLX_GGUF_ERR_TYPE_UNKNOWN;
    if (!ty->usable) return FMLX_GGUF_ERR_TYPE_REMOVED;
    if (ty->block_vals == 0 || n % ty->block_vals) return FMLX_GGUF_ERR_NUMEL_BLOCK;
    nblk = n / ty->block_vals;

    switch (type) {
    case 0:
        memcpy(dst, src, (size_t)n * 4);
        return FMLX_GGUF_OK;
    case 1: {
        uint64_t i;
        for (i = 0; i < n; i++) dst[i] = fmlx_ggml_f16_to_f32(ld16(src + 2 * i));
        return FMLX_GGUF_OK;
    }
    case 30: {
        uint64_t i;
        for (i = 0; i < n; i++) dst[i] = fmlx_ggml_bf16_to_f32(ld16(src + 2 * i));
        return FMLX_GGUF_OK;
    }
    case 2:
        dq_q4_0(src, nblk, dst);
        return FMLX_GGUF_OK;
    case 3:
        fmlx_dq_q4_1(src, nblk, dst);
        return FMLX_GGUF_OK;
    case 6:
        fmlx_dq_q5_0(src, nblk, dst);
        return FMLX_GGUF_OK;
    case 7:
        fmlx_dq_q5_1(src, nblk, dst);
        return FMLX_GGUF_OK;
    case 8:
        dq_q8_0(src, nblk, dst);
        return FMLX_GGUF_OK;
    case 10:
        dq_q2_k(src, nblk, dst);
        return FMLX_GGUF_OK;
    case 11:
        fmlx_dq_q3_k(src, nblk, dst);
        return FMLX_GGUF_OK;
    case 12:
        dq_qk(src, nblk, dst, 0);
        return FMLX_GGUF_OK;
    case 13:
        dq_qk(src, nblk, dst, 1);
        return FMLX_GGUF_OK;
    case 14:
        dq_q6_k(src, nblk, dst);
        return FMLX_GGUF_OK;
    case 15:
        fmlx_dq_q8_k(src, nblk, dst);
        return FMLX_GGUF_OK;
    case 16:
        dq_iq2_xxs(src, nblk, dst);
        return FMLX_GGUF_OK;
    case 24: {
        uint64_t i;
        for (i = 0; i < n; i++) dst[i] = (float)(int8_t)src[i];
        return FMLX_GGUF_OK;
    }
    case 25: {
        uint64_t i;
        for (i = 0; i < n; i++) {
            int16_t v;
            memcpy(&v, src + 2 * i, 2);
            dst[i] = (float)v;
        }
        return FMLX_GGUF_OK;
    }
    case 26: {
        uint64_t i;
        for (i = 0; i < n; i++) {
            int32_t v;
            memcpy(&v, src + 4 * i, 4);
            dst[i] = (float)v;
        }
        return FMLX_GGUF_OK;
    }
    case 20:
        fmlx_dq_iq4_nl(src, nblk, dst);
        return FMLX_GGUF_OK;
    case 23:
        fmlx_dq_iq4_xs(src, nblk, dst);
        return FMLX_GGUF_OK;
    case 34:
        fmlx_dq_tq1_0(src, nblk, dst);
        return FMLX_GGUF_OK;
    case 35:
        fmlx_dq_tq2_0(src, nblk, dst);
        return FMLX_GGUF_OK;
    case 39:
        fmlx_dq_mxfp4(src, nblk, dst);
        return FMLX_GGUF_OK;
    case 40:
        fmlx_dq_nvfp4(src, nblk, dst);
        return FMLX_GGUF_OK;
    case 41:
        fmlx_dq_q1_0(src, nblk, dst);
        return FMLX_GGUF_OK;
    default:
        return FMLX_GGUF_ERR_NOT_COVERED;
    }
}
