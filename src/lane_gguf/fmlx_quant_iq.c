/* fmlx_quant_iq.c — dequant dei tipi il cui valore passa da una TABELLA, non da una formula.
 *
 * Perché un file separato da fmlx_quant_ext.c: lì ogni tipo era SOLA aritmetica e la prova
 * stava nel confrontare i bit con un altro codice; qui c'è in più un pezzo di DATO — 16
 * valori che nessuno può dedurre dai principî (iq4nl) o che si possono dedurre ma solo se
 * si conosce la codifica (fp4). Un byte sbagliato in una di quelle due tabelle cambia UN
 * elemento per blocco e non si vede in nessun test di forma: per questo le tabelle arrivano
 * da `tools/gen_codebooks.py`, che le incrocia e rifiuta di scriverle se qualcosa diverge.
 *
 * `kvalues_fp4` è l'unica delle due che ha un testimone CHE NON È UNA COPIA: i suoi 16 valori
 * sono la codifica E2M1 della specifica OCP MX moltiplicata per 2 (la convenzione di ggml,
 * vedi ggml-common.h:1115 "e2m1 values (doubled)"), e lo script la ricalcola dai bit.
 *
 * Ogni funzione è la trascrizione della forma SCALARE di ggml di questa macchina, riga citata
 * sotto; il gate (`tools/quant_diff.c`) confronta poi i bit contro `dequantize_row_*` esportate
 * da libggml-base. Un tipo entra nella copertura solo se quel confronto dà zero divergenze.
 */
#include "fmlx/fmlx_quant.h"
#include "fmlx_quant_internal.h"
#include "fmlx_iq2_tables.h"

#include <math.h>   /* ldexpf: moltiplica per una potenza di due, nessun arrotondamento */

/* Le due scale microscaling NON sono half: sono ottetti opachi con semantica propria, e
 * nessuna delle due è "un float piccolo". `ggml-impl.h:477` e :502, riga per riga. */

/* E8M0: 8 bit di solo esponente, bias 127. La versione "_HALF" è quella che serve a MXFP4,
 * perché i kvalues sono gia` raddoppiati. I due rami sotto sono letterali di bit, non
 * approssimazioni: per x<2 il pattern è quello di un denormale (2^-128 e 2^-127), per x>=2
 * è un normal con esponente x-1. NaN (x=0xFF) NON è gestito, come in ggml: `(x-1)<<23`
 * con x=255 darebbe l'esponente di +inf, e un file che porta 0xFF non è un modello. */
static float e8m0_to_f32_half(uint8_t x) {
    uint32_t bits = (x < 2) ? (0x00200000u << x) : (uint32_t)((uint32_t)(x - 1) << 23);
    float v;
    memcpy(&v, &bits, 4);
    return v;
}

/* UE4M3: senza segno, 4 bit di esponente (bias 7), 3 di mantissa; ggml moltiplica per 0.5
 * "to match kvalues_mxfp4 convention". I due valori degeneri sono ESPRESSI: 0 e 0x7F (che
 * nel formato è NaN) valgono entrambi 0.0f. `ldexpf` qui non approssima mai: moltiplica per
 * una potenza di due, e 1 + man/8 è esatto in binario per man in [0,7]. */
static float ue4m3_to_f32(uint8_t x) {
    int exp, man;
    float raw;
    if (x == 0 || x == 0x7F) return 0.0f;
    exp = (x >> 3) & 0xF;
    man = x & 0x7;
    if (exp == 0) raw = ldexpf((float)man, -9);
    else raw = ldexpf(1.0f + (float)man / 8.0f, exp - 7);
    return raw * 0.5f;
}

/* ---- IQ4_NL (tipo 20) — ggml-quants.c:2653 ----
 * Blocco 18 byte: d f16 [0], qs 16 [2]. Trentadue elementi per blocco, due nibble per byte,
 * e la codebook NON è simmetrica: `kvalues_iq4nl` va da -127 a +113, quindi il nibble
 * basso e l'alto non possono condividere una formula "meno 8".
 * Nota d'ordine, perché è l'unica cosa facile da sbagliare: ggml scrive `y[j]` e
 * `y[j+QK4_NL/2]`, cioè i nibble bassi riempiono PRIMA i 16 elementi alti — non sono
 * interlacciati. */
void fmlx_dq_iq4_nl(const uint8_t *p, uint64_t nblk, float *y) {
    uint64_t b;
    for (b = 0; b < nblk; b++) {
        const uint8_t *x = p + b * 18;
        const uint8_t *qs = x + 2;
        float d = fmlx_ggml_f16_to_f32(ld16(x));
        float *out = y + b * 32;
        int j;
        for (j = 0; j < 16; j++) {
            out[j]      = d * (float)fmlx_kvalues_iq4nl[qs[j] & 0xF];
            out[j + 16] = d * (float)fmlx_kvalues_iq4nl[qs[j] >> 4];
        }
    }
}

/* ---- IQ4_XS (tipo 23) — ggml-quants.c:2671 ----
 * Blocco 136 byte: d f16 [0], scales_h u16 [2], scales_l 4 [4], qs 128 [8]. Sono otto
 * sottoblocchi da 32 con scala propria, e la scala ha 6 BIT spaccati in due campi: 4 nel
 * nibble di `scales_l` e 2 nei bit `2*ib` di `scales_h`, con offset -32. Ricavare "6 bit
 * contigui" darebbe lo stesso NUMERO di scale ma non le stesse coppie (ib, valore): la
 * permutazione è quella riga, copiata. */
void fmlx_dq_iq4_xs(const uint8_t *p, uint64_t nblk, float *y) {
    uint64_t b;
    for (b = 0; b < nblk; b++) {
        const uint8_t *x = p + b * 136;
        const uint8_t *qs = x + 8;
        float d = fmlx_ggml_f16_to_f32(ld16(x));
        uint16_t sh = ld16(x + 2);
        float *out = y + b * 256;
        int ib, j;
        for (ib = 0; ib < 8; ib++) {
            int ls = (int)(((x[4 + ib / 2] >> (4 * (ib % 2))) & 0xF) |
                           (((sh >> (2 * ib)) & 3) << 4));
            float dl = d * (float)(ls - 32);
            for (j = 0; j < 16; j++) {
                out[j]      = dl * (float)fmlx_kvalues_iq4nl[qs[j] & 0xF];
                out[j + 16] = dl * (float)fmlx_kvalues_iq4nl[qs[j] >> 4];
            }
            out += 32;
            qs += 16;
        }
    }
}

/* ---- MXFP4 (tipo 39) — ggml-quants.c:511 ----
 * Blocco 17 byte: e u8 [0] (E8M0), qs 16 [1]. Microscaling OCP: la scala è UNA potenza di
 * due per 32 elementi. I kvalues sono DOPPI, e la scala è MEZZA (`_HALF`): si annullano a
 * vicenda ed è il motivo per cui `0,1,2,3,4,6,8,12` non è un errore di scala. */
void fmlx_dq_mxfp4(const uint8_t *p, uint64_t nblk, float *y) {
    uint64_t b;
    for (b = 0; b < nblk; b++) {
        const uint8_t *x = p + b * 17;
        const uint8_t *qs = x + 1;
        float d = e8m0_to_f32_half(x[0]);
        float *out = y + b * 32;
        int j;
        for (j = 0; j < 16; j++) {
            out[j]      = (float)fmlx_kvalues_fp4[qs[j] & 0xF] * d;
            out[j + 16] = (float)fmlx_kvalues_fp4[qs[j] >> 4] * d;
        }
    }
}

/* ---- NVFP4 (tipo 40) — ggml-quants.c:531 ----
 * Blocco 36 byte: d 4 [0] (UE4M3, una per sottoblocco da 16), qs 32 [4]. Stessa codebook
 * di MXFP4 ma scala UE4M3 (che può avere mantissa, e i suoi due degeneri valgono 0) e
 * grana quattro volte più fine: 64 elementi per blocco, 4 scale, 16 valori l'una.
 * L'indice di `qs` è `s*(qk_sub/2) + j` = `s*8 + j`: i quattro sottoblocchi OCCUPANO BYTE
 * CONTIGUI, non sono interlacciati per j. */
void fmlx_dq_nvfp4(const uint8_t *p, uint64_t nblk, float *y) {
    uint64_t b;
    for (b = 0; b < nblk; b++) {
        const uint8_t *x = p + b * 36;
        const uint8_t *qs = x + 4;
        float *out = y + b * 64;
        int s, j;
        for (s = 0; s < 4; s++) {
            float d = ue4m3_to_f32(x[s]);
            float *yb = out + s * 16;
            const uint8_t *q = qs + s * 8;
            for (j = 0; j < 8; j++) {
                yb[j]     = (float)fmlx_kvalues_fp4[q[j] & 0xF] * d;
                yb[j + 8] = (float)fmlx_kvalues_fp4[q[j] >> 4] * d;
            }
        }
    }
}
