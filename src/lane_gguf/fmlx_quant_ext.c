/* fmlx_quant_ext.c — dequant dei tipi ggml la cui forma è SOLA formula (nessuna codebook).
 *
 * Ogni funzione qui è la trascrizione della forma SCALARE di ggml letta nel sorgente di
 * questa macchina (`/Users/gaetano/llamacpp/ggml/src/ggml-quants.c`, numero di riga citato
 * sotto ogni funzione), non una riscoperta: `tools/quant_diff.c` confronta poi i BIT del
 * risultato contro ggml stesso e contro i due dequant gemelli di GMBasic. Un tipo entra
 * solo se quel confronto dà zero bit divergenti (vedi il contratto in
 * include/fmlx/fmlx_quant.h): la trascrizione da sola non basta, è l'ipotesi; il gate è la
 * prova.
 *
 * Dove il C di ggml troncava, si tronca allo stesso modo. Non è pedanteria:
 * `(uint8_t)(q * 3)` e `q * 3` danno valori diversi per q > 85, e sui trit riempiti a caso
 * questo cambia i bit del risultato.
 */
#include "fmlx/fmlx_quant.h"
#include "fmlx_quant_internal.h"

/* ---- Q4_1 (tipo 3) — ggml-quants.c:421 ----
 * Come Q4_0 ma con il MIN separato e la scala non centrata: nessun -8.
 * Blocco 20 byte: d f16 [0], m f16 [2], qs 16 [4]. */
void fmlx_dq_q4_1(const uint8_t *p, uint64_t nblk, float *y) {
    uint64_t b;
    for (b = 0; b < nblk; b++) {
        const uint8_t *x = p + b * 20;
        float d = fmlx_ggml_f16_to_f32(ld16(x));
        float m = fmlx_ggml_f16_to_f32(ld16(x + 2));
        float *out = y + b * 32;
        int j;
        for (j = 0; j < 16; j++) {
            out[j] = (float)(x[4 + j] & 0xF) * d + m;
            out[j + 16] = (float)(x[4 + j] >> 4) * d + m;
        }
    }
}

/* ---- Q5_0 (tipo 6) e Q5_1 (tipo 7) — ggml-quants.c:442 e :468 ----
 * Il quinto bit di ogni quant sta in un uint32 `qh` letto con memcpy (l'offset del tensore
 * nel file può non essere allineato a 4). L'estrazione di ggml va replicata tale e quale:
 * `((qh >> j) << 4) & 0x10` NON è `(qh >> (j - 4)) & 0x10` — per j < 4 lo shift a sinistra
 * spinge il bit fuori dal registro e la posizione utile cambia.
 * Q5_0: 22 byte (d [0], qh [2], qs [6]); Q5_1: 24 byte (d [0], m [2], qh [4], qs [8]). */
static void dq_q5(const uint8_t *p, uint64_t nblk, float *y, int has_min) {
    const uint64_t bsz = has_min ? 24 : 22;
    const int qs_off = has_min ? 8 : 6;
    const int qh_off = has_min ? 4 : 2;
    uint64_t b;
    for (b = 0; b < nblk; b++) {
        const uint8_t *x = p + b * bsz;
        float d = fmlx_ggml_f16_to_f32(ld16(x));
        float m = has_min ? fmlx_ggml_f16_to_f32(ld16(x + 2)) : 0.0f;
        uint32_t qh = ld32(x + qh_off);
        float *out = y + b * 32;
        int j;
        for (j = 0; j < 16; j++) {
            int x0 = (int)(x[qs_off + j] & 0xF) | (int)(((qh >> (j + 0)) << 4) & 0x10u);
            int x1 = (int)(x[qs_off + j] >> 4) | (int)((qh >> (j + 12)) & 0x10u);
            if (has_min) {
                out[j] = (float)x0 * d + m;
                out[j + 16] = (float)x1 * d + m;
            } else {
                out[j] = (float)(x0 - 16) * d;
                out[j + 16] = (float)(x1 - 16) * d;
            }
        }
    }
}

void fmlx_dq_q5_0(const uint8_t *p, uint64_t nblk, float *y) { dq_q5(p, nblk, y, 0); }
void fmlx_dq_q5_1(const uint8_t *p, uint64_t nblk, float *y) { dq_q5(p, nblk, y, 1); }

/* ---- Q3_K (tipo 11) — ggml-quants.c:1247 ----
 * Blocco 110 byte: hmask 32 [0], qs 64 [32], scales 12 [96], d f16 [108].
 *
 * I 12 byte di scale NON sono 12 scale: sono 16 scale da 6 bit imballate in 96 bit, e ggml
 * le sposta con quattro maschere su uint32 (kmask1/kmask2) prima di usarle. Estrarre "6 bit
 * alla volta" darebbe gli stessi valori matematici ma NON le stesse `int8_t` nelle stesse
 * posizioni di `scales[]`: qui la permutazione è copiata bit per bit.
 *
 * L'ordine dei cicli è quello di ggml: `is` avanza di 2 a ogni giro di `j`, `q` avanza di
 * 32 solo al passaggio del blocco da 128, mentre `hm` e la maschera `m` proseguono attraverso
 * entrambi i giri di `n` (m vale 1,2,4,8 nel primo e 16,32,64,128 nel secondo). */
void fmlx_dq_q3_k(const uint8_t *p, uint64_t nblk, float *y) {
    const uint32_t kmask1 = 0x03030303u;
    const uint32_t kmask2 = 0x0f0f0f0fu;
    uint64_t b;
    for (b = 0; b < nblk; b++) {
        const uint8_t *x = p + b * 110;
        const uint8_t *q = x + 32;
        const uint8_t *hm = x;
        float d_all = fmlx_ggml_f16_to_f32(ld16(x + 108));
        float *out = y + b * 256;
        uint32_t aux[4], a0, a1, a2;
        const int8_t *sc;
        int n, j, l, is = 0;
        uint8_t m = 1;

        memcpy(aux, x + 96, 12);
        a0 = aux[0];
        a1 = aux[1];
        a2 = aux[2];
        aux[2] = ((a0 >> 4) & kmask2) | (((a2 >> 4) & kmask1) << 4);
        aux[3] = ((a1 >> 4) & kmask2) | (((a2 >> 6) & kmask1) << 4);
        aux[0] = (a0 & kmask2) | (((a2 >> 0) & kmask1) << 4);
        aux[1] = (a1 & kmask2) | (((a2 >> 2) & kmask1) << 4);
        sc = (const int8_t *)aux;

        for (n = 0; n < 256; n += 128) {
            int shift = 0;
            for (j = 0; j < 4; j++) {
                float dl = d_all * (float)(sc[is++] - 32);
                for (l = 0; l < 16; l++) {
                    out[l] = dl * (float)((int8_t)((q[l] >> shift) & 3u) -
                                          ((hm[l] & m) ? 0 : 4));
                }
                dl = d_all * (float)(sc[is++] - 32);
                for (l = 0; l < 16; l++) {
                    out[16 + l] = dl * (float)((int8_t)((q[16 + l] >> shift) & 3u) -
                                               ((hm[16 + l] & m) ? 0 : 4));
                }
                shift += 2;
                m = (uint8_t)(m << 1);
                out += 32;
            }
            q += 32;
        }
    }
}

/* ---- Q8_K (tipo 15) — ggml-quants.c:2735 ----
 * Blocco 292 byte: d f32 [0], qs 256 [4], bsums 32 [260]. È il tipo dei K-quanti
 * intermedi: l'unica cosa delicata è che la scala è un float32 LETTO DAL FILE, quindi va
 * copiato bit-for-bit (ld_f32) e non riconvertito da un half. I bsums non entrano nel
 * dequant: sono un'accelerazione per i dot, non dati del tensore. */
void fmlx_dq_q8_k(const uint8_t *p, uint64_t nblk, float *y) {
    uint64_t b;
    for (b = 0; b < nblk; b++) {
        const uint8_t *x = p + b * 292;
        float d = ld_f32(x);
        float *out = y + b * 256;
        int j;
        for (j = 0; j < 256; j++) out[j] = d * (float)(int8_t)x[4 + j];
    }
}

/* ---- Q1_0 (tipo 41) — ggml-quants.c:381 ----
 * Blocco 18 byte: d f16 [0], qs 16 [2] = un bit per elemento su 128 elementi.
 * `neg_d` è calcolato UNA volta come `-d`: se d è +0.0 il lato negativo è -0.0, e scrivere
 * `(bit ? d : -d)` in linea darebbe +0.0 dove ggml dà -0.0. Bit diversi. */
void fmlx_dq_q1_0(const uint8_t *p, uint64_t nblk, float *y) {
    uint64_t b;
    for (b = 0; b < nblk; b++) {
        const uint8_t *x = p + b * 18;
        float d = fmlx_ggml_f16_to_f32(ld16(x));
        float neg_d = -d;
        float *out = y + b * 128;
        int j;
        for (j = 0; j < 128; j++) {
            uint8_t bit = (uint8_t)((x[2 + j / 8] >> (j % 8)) & 1u);
            out[j] = bit ? d : neg_d;
        }
    }
}

/* ---- TQ1_0 (tipo 34) — ggml-quants.c:2356 ----
 * Quantizzazione "true" ternaria: cinque trit per byte (3^5 = 243 < 256) decodificati
 * moltiplicando per 3^n e prendendo gli alti di (q*3)>>8. NON è una divisione per 3, e il
 * prodotto intermedio TRONCA a uint8_t prima di essere moltiplicato per 3: entrambe le cose
 * cambiano i bit, quindi sono replicate.
 * Layout (54 byte): qs 48 [0], qh 4 [48], d f16 [52]. I 48 byte di qs coprono 240 trit e
 * sono letti in due pezzi (32 e 16) perché ggml così fa; gli ultimi 16 trit stanno in qh. */
void fmlx_dq_tq1_0(const uint8_t *p, uint64_t nblk, float *y) {
    static const uint8_t pow3[6] = { 1, 3, 9, 27, 81, 243 };
    uint64_t b;
    for (b = 0; b < nblk; b++) {
        const uint8_t *x = p + b * 54;
        float d = fmlx_ggml_f16_to_f32(ld16(x + 52));
        float *out = y + b * 256;
        int j, n, m;
        for (n = 0; n < 5; n++) {
            for (m = 0; m < 32; m++) {
                uint8_t q = (uint8_t)(x[m] * pow3[n]);
                int16_t xi = (int16_t)(((uint16_t)q * 3u) >> 8);
                *out++ = (float)(xi - 1) * d;
            }
        }
        for (n = 0; n < 5; n++) {
            for (m = 0; m < 16; m++) {
                uint8_t q = (uint8_t)(x[32 + m] * pow3[n]);
                int16_t xi = (int16_t)(((uint16_t)q * 3u) >> 8);
                *out++ = (float)(xi - 1) * d;
            }
        }
        for (n = 0; n < 4; n++) {
            for (j = 0; j < 4; j++) {
                uint8_t q = (uint8_t)(x[48 + j] * pow3[n]);
                int16_t xi = (int16_t)(((uint16_t)q * 3u) >> 8);
                *out++ = (float)(xi - 1) * d;
            }
        }
    }
}

/* ---- TQ2_0 (tipo 35) — ggml-quants.c:2395 ----
 * Layout (66 byte): qs 64 [0], d f16 [64]. Due bit per elemento, centrati su 1:
 * i quattro piani di bit escono a blocchi di 32, non interlacciati. */
void fmlx_dq_tq2_0(const uint8_t *p, uint64_t nblk, float *y) {
    uint64_t b;
    for (b = 0; b < nblk; b++) {
        const uint8_t *x = p + b * 66;
        float d = fmlx_ggml_f16_to_f32(ld16(x + 64));
        float *out = y + b * 256;
        int j, l, m;
        for (j = 0; j < 64; j += 32) {
            for (l = 0; l < 4; l++) {
                for (m = 0; m < 32; m++) {
                    int8_t q = (int8_t)((x[j + m] >> (l * 2)) & 3u);
                    *out++ = (float)(q - 1) * d;
                }
            }
        }
    }
}
