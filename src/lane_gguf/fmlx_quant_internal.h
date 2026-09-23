/* fmlx_quant_internal.h — helper CONDIVISI tra i TU della dequant.
 *
 * Non è API pubblica: quello che un chiamante esterno può fare sta in
 * include/fmlx/fmlx_quant.h. Qui vivono solo i caricamenti non allineati che più file
 * `.c` della stessa lane devono fare allo stesso identico modo.
 *
 * Perché `memcpy` e non il cast del puntatore: l'offset di un tensore dentro il GGUF può
 * essere qualunque (rispetta l'allineamento dichiarato dal file, non quello del tipo C), e
 * un `(uint32_t *)p` su un indirizzo dispari è un crash su ARM — non un'ottimizzazione.
 * GGUF è little-endian per definizione, quindi il memcpy dà esattamente le cifre che ggml
 * legge dai suoi struct.
 */
#ifndef FMLX_QUANT_INTERNAL_H
#define FMLX_QUANT_INTERNAL_H

#include <stdint.h>
#include <string.h>

static inline uint16_t ld16(const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}

static inline uint32_t ld32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static inline float ld_f32(const uint8_t *p) {
    float v;
    memcpy(&v, p, 4);
    return v;
}

/* Ogni funzione scrive `nblk * VALORI_PER_BLOCCO` float in `y`, partendo da `p` che punta
 * il primo blocco. I nomi e gli arità sono quelli delle `dequantize_row_*` di ggml, così il
 * confronto differenziale di tools/quant_diff.c è riga-per-riga con la forma di riferimento. */

/* --- tipi da 32 (o multipli) con sola formula --- */
void fmlx_dq_q4_1(const uint8_t *p, uint64_t nblk, float *y);   /* tipo 3,  20 byte / 32  */
void fmlx_dq_q5_0(const uint8_t *p, uint64_t nblk, float *y);   /* tipo 6,  22 byte / 32  */
void fmlx_dq_q5_1(const uint8_t *p, uint64_t nblk, float *y);   /* tipo 7,  24 byte / 32  */
void fmlx_dq_q3_k(const uint8_t *p, uint64_t nblk, float *y);   /* tipo 11, 110 byte / 256 */
void fmlx_dq_q8_k(const uint8_t *p, uint64_t nblk, float *y);   /* tipo 15, 292 byte / 256 */
void fmlx_dq_q1_0(const uint8_t *p, uint64_t nblk, float *y);   /* tipo 41, 18 byte / 128 */

/* --- quantizzazione ternaria (trit, base 3) --- */
void fmlx_dq_tq1_0(const uint8_t *p, uint64_t nblk, float *y);  /* tipo 34, 54 byte / 256 */
void fmlx_dq_tq2_0(const uint8_t *p, uint64_t nblk, float *y);  /* tipo 35, 66 byte / 256 */

/* --- tipi a codebook: il valore esce da una TABELLA (fmlx_quant_iq.c) --- */
void fmlx_dq_iq4_nl(const uint8_t *p, uint64_t nblk, float *y);  /* tipo 20,  18 byte / 32  */
void fmlx_dq_iq4_xs(const uint8_t *p, uint64_t nblk, float *y);  /* tipo 23, 136 byte / 256 */

/* --- microscaling FP4 (tavola kvalues_fp4 + scale E8M0/UE4M3) --- */
void fmlx_dq_mxfp4(const uint8_t *p, uint64_t nblk, float *y);  /* tipo 39, 17 byte / 32  */
void fmlx_dq_nvfp4(const uint8_t *p, uint64_t nblk, float *y);  /* tipo 40, 36 byte / 64  */

#endif /* FMLX_QUANT_INTERNAL_H */
