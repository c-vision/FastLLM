/* fmlx_quant_iq.c — dequant dei tipi il cui valore passa da una TABELLA, non da una formula.
 *
 * Perché un file separato da fmlx_quant_ext.c: lì ogni tipo era SOLA aritmetica e la prova
 * stava nel confrontare i bit con un altro codice; qui c'è in più un pezzo di DATO — 16
 * valori che nessuno può dedurre dai principî (iq4nl) o che si possono dedurre ma solo se
 * si conosce la codifica (fp4). Un byte sbagliato in una di quelle due tabelle cambia UN
 * elemento per blocco e non si vede in nessun test di forma: per questo le tabelle arrivano
 * da `tools/gen_codebooks.py`, che le incrocia e rifiuta di scriverle se qualcosa diverge.
 *
 * `kvalues_fp4` è l'unica delle tabelle di questo file ad avere un testimone CHE NON È UNA
 * COPIA: i suoi 16 valori sono la codifica E2M1 della specifica OCP MX moltiplicata per 2
 * (la convenzione di ggml, vedi ggml-common.h:1115 "e2m1 values (doubled)"), e lo script la
 * ricalcola dai bit. Per le sei griglie del blocco sotto (IQ2_XS, IQ3_XXS, IQ1_S, IQ3_S,
 * IQ2_S, IQ1_M) quel testimone NON ESISTE: sono codebook allenati, e la prova è eseguita —
 * grid_sweep() di tools/quant_diff.c batte ogni singolo indice di griglia contro la copia
 * compilata dentro la dylib di ggml.
 *
 * Ogni funzione è la trascrizione della forma SCALARE di ggml di questa macchina, riga citata
 * sotto; il gate (`tools/quant_diff.c`) confronta poi i bit contro `dequantize_row_*` esportate
 * da libggml-base. Un tipo entra nella copertura solo se quel confronto dà zero divergenze.
 * L'ordine dei fattori è copiato DA GGML, non "equivalente": con i byte pseudo-casuali del
 * gate capitano `d` infiniti e NaN, e `(x*0)*-1.f` non è lo stesso float di `-(x*0)` per
 * quanto riguarda gli zero con segno — che nei bit si vede. Il SEGNO in particolare si
 * applica negando il prodotto (`fmlx_iq_signed`), non moltiplicando per -1.0f: su ARM64
 * `NaN * -1.f` lascia il segno dov'è, e il gate lo ha misurato su un blocco IQ3_S con `d`
 * NaN — 135 elementi su 256 fuori di UN bit, e 0 fuori costringendo `d` a 1.0f.
 */
#include "fmlx/fmlx_quant.h"
#include "fmlx_quant_internal.h"
#include "fmlx_iq2_tables.h"
#include "fmlx_iqgrid_tables.h"
#include "fmlx_iq1s_tables.h"

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

/* ================== le sei griglie (batch C) ==================
 *
 * Il segno NON è mai nella griglia per i tipi IQ2/IQ3: `kmask_iq2xs[j]` è 1<<j e le tabelle
 * `fmlx_iq2xs_grid`/`fmlx_iq2s_grid`/`fmlx_iq3xxs_grid`/`fmlx_iq3s_grid` contengono sole
 * grandezze (infatti i loro byte sono 8/25/43 e i dispari 1..15, mai 0x80|...). Per IQ1_S e
 * IQ1_M è il contrario: `fmlx_iq1s_grid` si legge come int8 e porta il segno già dentro
 * (0/+1/-1), e il `delta` è un termine aggiunto FUORI dalla tabella, non un suo byte.
 */
#define IQ1S_DELTA 0.125f   /* ggml-common.h:1122 — segno scelto dal bit 15 di qh (IQ1_S) */

/* ---- IQ2_XS (tipo 17) — ggml-quants.c:2444 ----
 * Blocco 74 byte: d f16 [0], qs 32 uint16 [2..65], scales 8 [66..73]. Ogni qs è DUE campi
 * in una parola sola: bit 0..8 = indice di griglia (512 voci), bit 9..15 = indice di segno
 * (128 voci). `q >> 9` non ha bisogno di maschera perché qs è uint16 e i bit sopra il 15 non
 * esistono — aggiungere `& 127` sarebbe un secondo posto dove sbagliare. */
void fmlx_dq_iq2_xs(const uint8_t *p, uint64_t nblk, float *y) {
    uint64_t b;
    for (b = 0; b < nblk; b++) {
        const uint8_t *x = p + b * 74;
        const uint8_t *scales = x + 66;
        float d = fmlx_ggml_f16_to_f32(ld16(x));
        float *out = y + b * 256;
        int ib, l, j;
        for (ib = 0; ib < 8; ib++) {
            float db[2];
            db[0] = d * (0.5f + (float)(scales[ib] & 0xF)) * 0.25f;
            db[1] = d * (0.5f + (float)(scales[ib] >> 4)) * 0.25f;
            for (l = 0; l < 4; l++) {
                uint16_t q = ld16(x + 2 + 2 * (4 * ib + l));
                const uint8_t *grid = (const uint8_t *)&fmlx_iq2xs_grid[q & 511u];
                uint8_t signs = fmlx_iq2_ksigns[q >> 9];
                float dl = db[l / 2];
                for (j = 0; j < 8; j++) {
                    out[j] = fmlx_iq_signed(dl * (float)grid[j], signs & (1u << j));
                }
                out += 8;
            }
        }
    }
}

/* ---- IQ2_S (tipo 22) — ggml-quants.c:2471 ----
 * Blocco 82 byte: d f16 [0], qs 32 [2..33], signs 32 [34..65], qh 8 [66..73], scales 8
 * [74..81]. L'insidia è una sola, ed è il motivo del commento in ggml: ggml scrive
 * `signs = qs + QK_K/8`, cioè i 32 byte dei SEGNI stanno dentro l'array `qs[QK_K/4]`, a
 * metà. `qs` quindi sono i primi 32 byte DOPO d, non 64: leggerli come 64 significherebbe
 * usare i segni come indici. Indice = 8 bit di qs + 2 bit di qh, e i due bit di qh si
 * prendono con lo shift dipendente da l (`8-2l`), non contigui. */
void fmlx_dq_iq2_s(const uint8_t *p, uint64_t nblk, float *y) {
    uint64_t b;
    for (b = 0; b < nblk; b++) {
        const uint8_t *x = p + b * 82;
        const uint8_t *qs = x + 2;
        const uint8_t *signs = x + 34;
        const uint8_t *qh = x + 66;
        const uint8_t *scales = x + 74;
        float d = fmlx_ggml_f16_to_f32(ld16(x));
        float *out = y + b * 256;
        int ib, l, j;
        for (ib = 0; ib < 8; ib++) {
            float db[2];
            db[0] = d * (0.5f + (float)(scales[ib] & 0xF)) * 0.25f;
            db[1] = d * (0.5f + (float)(scales[ib] >> 4)) * 0.25f;
            for (l = 0; l < 4; l++) {
                const uint8_t *grid =
                    (const uint8_t *)&fmlx_iq2s_grid[qs[l] | ((qh[ib] << (8 - 2 * l)) & 0x300)];
                for (j = 0; j < 8; j++) {
                    out[j] = fmlx_iq_signed(db[l / 2] * (float)grid[j], signs[l] & (1u << j));
                }
                out += 8;
            }
            qs += 4;
            signs += 4;
        }
    }
}

/* ---- IQ3_XXS (tipo 18) — ggml-quants.c:2503 ----
 * Blocco 98 byte: d f16 [0], qs 64 [2..65], scales_and_signs 32 [66..97].Qui `qs` SONO 64
 * byte veri (3*QK_K/8), e in ogni parola da 4 byte dei campi successivi stanno UNA scala a 4
 * bit (bit 28..31) e QUATTRO indici di segno da 7 bit (bit 7l..7l+6): i due nibble della
 * scala non ci sono. Ogni indice di griglia copre 4 elementi, non 8 — la griglia è uint32,
 * quattro byte. */
void fmlx_dq_iq3_xxs(const uint8_t *p, uint64_t nblk, float *y) {
    uint64_t b;
    for (b = 0; b < nblk; b++) {
        const uint8_t *x = p + b * 98;
        const uint8_t *qs = x + 2;
        const uint8_t *sss = x + 66;
        float d = fmlx_ggml_f16_to_f32(ld16(x));
        float *out = y + b * 256;
        int ib, l, j;
        for (ib = 0; ib < 8; ib++) {
            uint32_t aux32 = ld32(sss + 4 * ib);
            float db = d * (0.5f + (float)(aux32 >> 28)) * 0.5f;
            for (l = 0; l < 4; l++) {
                uint8_t signs = fmlx_iq2_ksigns[(aux32 >> (7 * l)) & 127u];
                const uint8_t *g1 = (const uint8_t *)&fmlx_iq3xxs_grid[qs[2 * l]];
                const uint8_t *g2 = (const uint8_t *)&fmlx_iq3xxs_grid[qs[2 * l + 1]];
                for (j = 0; j < 4; j++) {
                    out[j]     = fmlx_iq_signed(db * (float)g1[j], signs & (1u << j));
                    out[j + 4] = fmlx_iq_signed(db * (float)g2[j], signs & (1u << (j + 4)));
                }
                out += 8;
            }
            qs += 8;
        }
    }
}

/* ---- IQ3_S (tipo 21) — ggml-quants.c:2535 ----
 * Blocco 110 byte: d f16 [0], qs 64 [2..65], qh 8 [66..73], signs 32 [74..105], scales 4
 * [106..109]. Il giro esterno avanza di DUE sottoblocchi (`ib32 += 2`): db1/db2 vengono
 * dallo stesso byte di scala e qh[0]/qh[1] valgono uno per metà. La scala qui è intera, non
 * frazionaria: `d * (1 + 2*nibble)`, e l'indice di griglia è 8 bit di qs + UN bit di qh
 * (512 voci da 4 byte). */
void fmlx_dq_iq3_s(const uint8_t *p, uint64_t nblk, float *y) {
    uint64_t b;
    for (b = 0; b < nblk; b++) {
        const uint8_t *x = p + b * 110;
        const uint8_t *qs = x + 2;
        const uint8_t *qh = x + 66;
        const uint8_t *sg = x + 74;
        const uint8_t *scales = x + 106;
        float d = fmlx_ggml_f16_to_f32(ld16(x));
        float *out = y + b * 256;
        int h, l, j;
        for (h = 0; h < 4; h++) {
            const uint8_t *q = qs;
            float db2 = d * (float)(1 + 2 * (scales[h] >> 4));
            int half;
            for (half = 0; half < 2; half++) {
                uint8_t qh_b = qh[half];
                float db = half ? db2 : d * (float)(1 + 2 * (scales[h] & 0xF));
                const uint8_t *s = sg + 4 * half;
                for (l = 0; l < 4; l++) {
                    const uint8_t *g1 =
                        (const uint8_t *)&fmlx_iq3s_grid[q[2 * l] | ((qh_b << (8 - 2 * l)) & 256)];
                    const uint8_t *g2 =
                        (const uint8_t *)&fmlx_iq3s_grid[q[2 * l + 1] | ((qh_b << (7 - 2 * l)) & 256)];
                    for (j = 0; j < 4; j++) {
                        out[j]     = fmlx_iq_signed(db * (float)g1[j], s[l] & (1u << j));
                        out[j + 4] = fmlx_iq_signed(db * (float)g2[j], s[l] & (1u << (j + 4)));
                    }
                    out += 8;
                }
                q += 8;
            }
            qs += 16;
            sg += 8;
            qh += 2;
        }
    }
}

/* ---- IQ1_S (tipo 19) — ggml-quants.c:2578 ----
 * Blocco 50 byte: d f16 [0], qs 32 [2..33], qh 8 uint16 [34..49]. qh non è solo indice: bit
 * 0..11 sono i 4 indici da 3 bit che completano qs (11 bit = 2048 voci), bit 12..14 scelgono
 * il MOLTIPLICATORE `d*(2*s+1)` e bit 15 il SEGNO del delta. La griglia è letta come int8 e
 * porta già il segno, e il delta si SOMMA prima di moltiplicare: `dl * (grid[j] + delta)`. */
void fmlx_dq_iq1_s(const uint8_t *p, uint64_t nblk, float *y) {
    uint64_t b;
    for (b = 0; b < nblk; b++) {
        const uint8_t *x = p + b * 50;
        const uint8_t *qs = x + 2;
        float d = fmlx_ggml_f16_to_f32(ld16(x));
        float *out = y + b * 256;
        int ib, l, j;
        for (ib = 0; ib < 8; ib++) {
            uint16_t qh = ld16(x + 34 + 2 * ib);
            float dl = d * (float)(2 * ((qh >> 12) & 7) + 1);
            float delta = (qh & 0x8000u) ? -IQ1S_DELTA : IQ1S_DELTA;
            for (l = 0; l < 4; l++) {
                const int8_t *grid = (const int8_t *)&fmlx_iq1s_grid[qs[l] |
                                                                    (((qh >> (3 * l)) & 7u) << 8)];
                for (j = 0; j < 8; j++) {
                    out[j] = dl * ((float)grid[j] + delta);
                }
                out += 8;
            }
            qs += 4;
        }
    }
}

/* ---- IQ1_M (tipo 29) — ggml-quants.c:2603 ----
 * Blocco 56 byte: qs 32 [0..31], qh 16 [32..47], scales 8 [48..55]. Qui NON c'è un `d` nel
 * blocco: la scala è ricomposta da 4 uint16 di `scales` (12 bit usati per parola su 16) in un
 * half da 16 bit, e SOLO DOPO si converte in float — convertire le quattro parole e moltiplicare
 * sarebbe un'altra aritmetica. Stessa forma di IQ1_S per la griglia (11 bit, int8 con segno), ma
 * il delta è per sottoblocco da 8 e viene da due bit diversi per ogni coppia di qh. */
void fmlx_dq_iq1_m(const uint8_t *p, uint64_t nblk, float *y) {
    uint64_t b;
    for (b = 0; b < nblk; b++) {
        const uint8_t *x = p + b * 56;
        const uint8_t *qs = x;
        const uint8_t *qh = x + 32;
        uint16_t sc[4];
        uint16_t su;
        float d;
        float *out = y + b * 256;
        int ib, l, j;
        sc[0] = ld16(x + 48);
        sc[1] = ld16(x + 50);
        sc[2] = ld16(x + 52);
        sc[3] = ld16(x + 54);
        su = (uint16_t)((sc[0] >> 12) | ((sc[1] >> 8) & 0x00F0u) | ((sc[2] >> 4) & 0x0F00u) |
                        (sc[3] & 0xF000u));
        d = fmlx_ggml_f16_to_f32(su);
        for (ib = 0; ib < 8; ib++) {
            float delta[4];
            uint16_t idx[4];
            float dl[4];
            dl[0] = d * (float)(2 * ((sc[ib / 2] >> (6 * (ib % 2) + 0)) & 0x7) + 1);
            dl[1] = dl[0];
            dl[2] = d * (float)(2 * ((sc[ib / 2] >> (6 * (ib % 2) + 3)) & 0x7) + 1);
            dl[3] = dl[2];
            idx[0] = (uint16_t)(qs[0] | ((qh[0] << 8) & 0x700));
            idx[1] = (uint16_t)(qs[1] | ((qh[0] << 4) & 0x700));
            idx[2] = (uint16_t)(qs[2] | ((qh[1] << 8) & 0x700));
            idx[3] = (uint16_t)(qs[3] | ((qh[1] << 4) & 0x700));
            delta[0] = qh[0] & 0x08 ? -IQ1S_DELTA : IQ1S_DELTA;
            delta[1] = qh[0] & 0x80 ? -IQ1S_DELTA : IQ1S_DELTA;
            delta[2] = qh[1] & 0x08 ? -IQ1S_DELTA : IQ1S_DELTA;
            delta[3] = qh[1] & 0x80 ? -IQ1S_DELTA : IQ1S_DELTA;
            for (l = 0; l < 4; l++) {
                const int8_t *grid = (const int8_t *)&fmlx_iq1s_grid[idx[l]];
                for (j = 0; j < 8; j++) {
                    out[j] = dl[l] * ((float)grid[j] + delta[l]);
                }
                out += 8;
            }
            qs += 4;
            qh += 2;
        }
    }
}
