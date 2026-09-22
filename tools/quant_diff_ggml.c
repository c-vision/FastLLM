/* quant_diff_ggml.c — llama.cpp/ggml come TESTIMONE C del gate differenziale.
 *
 * Perché esiste: la regola di copertura di `fmlx_ggml_dequant_row` dice che un tipo entra
 * solo se esiste un SECONDO codice indipendente con cui confrontarlo bit a bit. Fin qui i
 * secondi codici erano due, entrambi di casa (A `gmb_ggml.h`, B `gmb_moe_ggml.c`). C è un
 * codice che non condivide una riga con quei due e che gira in un altro mondo.
 *
 * Cosa è verificato qui DENTRO, non assunto fuori:
 *  - i `_Static_assert` sui valori di enum: la nostra tabella di 42 slot keyed per NUMERO
 *    (src/lane_gguf/fmlx_gguf.c) deve dare lo stesso numero di ggml per ogni tipo. Se uno
 *    dei due ha rinumerato, questa unità non compila e lo dice col nome del tipo;
 *  - `ggc_row_bytes` passa da `ggml_row_size`: la dimensione in byte del blocco è letta
 *    dalla TAVOLA DI GGML, non ricopiata qui — così il gate confronta anche i byte, non
 *    solo i float.
 *
 * Non si compila `ggml-quants.c` in questo Eseguibile: si linka la libreria già costruita
 * su questa macchina (`/Users/gaetano/llamacpp/build/bin/libggml-base.*.dylib`), che
 * esporta esattamente le 25 `dequantize_row_*` nominate qui sotto (`nm -gU | grep -c` = 25,
 * misurato) e resolve da sé i propri simboli.
 *
 * Build: vedi il blocco "gate differenziale" nel README (flag -I/-L/-l).
 */

#include "ggml-quants.h" /* dequantize_row_*, block_q*, QK* */
#include "ggml.h"        /* ggml_row_size */

#include <stdint.h>
#include <string.h>

int ggc_block_vals(int type);
int ggc_dequant_row(int type, const uint8_t *bytes, int64_t n, float *dst);
uint64_t ggc_row_bytes(int type, int64_t n);

/* I numeri sono quelli della NS. tabella (posizionale, riga = valore di enum). Se ggml
 * rinumerasse, qui si ferma la compilazione con il nome del tipo: mai un mismatch che
 * diventa "leggo i byte di Q4_K come se fossero Q5_K". */
#define GG_ASSERT_NUM(ggname, ourn) _Static_assert((int)(ggname) == (ourn), #ggname " != " #ourn)
GG_ASSERT_NUM(GGML_TYPE_Q4_0, 2);
GG_ASSERT_NUM(GGML_TYPE_Q4_1, 3);
GG_ASSERT_NUM(GGML_TYPE_Q5_0, 6);
GG_ASSERT_NUM(GGML_TYPE_Q5_1, 7);
GG_ASSERT_NUM(GGML_TYPE_Q8_0, 8);
GG_ASSERT_NUM(GGML_TYPE_Q2_K, 10);
GG_ASSERT_NUM(GGML_TYPE_Q3_K, 11);
GG_ASSERT_NUM(GGML_TYPE_Q4_K, 12);
GG_ASSERT_NUM(GGML_TYPE_Q5_K, 13);
GG_ASSERT_NUM(GGML_TYPE_Q6_K, 14);
GG_ASSERT_NUM(GGML_TYPE_Q8_K, 15);
GG_ASSERT_NUM(GGML_TYPE_IQ2_XXS, 16);
GG_ASSERT_NUM(GGML_TYPE_IQ2_XS, 17);
GG_ASSERT_NUM(GGML_TYPE_IQ3_XXS, 18);
GG_ASSERT_NUM(GGML_TYPE_IQ1_S, 19);
GG_ASSERT_NUM(GGML_TYPE_IQ4_NL, 20);
GG_ASSERT_NUM(GGML_TYPE_IQ3_S, 21);
GG_ASSERT_NUM(GGML_TYPE_IQ2_S, 22);
GG_ASSERT_NUM(GGML_TYPE_IQ4_XS, 23);
GG_ASSERT_NUM(GGML_TYPE_IQ1_M, 29);
GG_ASSERT_NUM(GGML_TYPE_TQ1_0, 34);
GG_ASSERT_NUM(GGML_TYPE_TQ2_0, 35);
GG_ASSERT_NUM(GGML_TYPE_MXFP4, 39);
GG_ASSERT_NUM(GGML_TYPE_NVFP4, 40);
GG_ASSERT_NUM(GGML_TYPE_Q1_0, 41);

/* Il numero di elementi per blocco SECONDO GGML: i suoi macro, non i nostri numeri. Il gate
 * lo confronta con `block_vals` della nostra tabella e, se divergono, NON chiama il dequant
 * (ogni `dequantize_row_*` fa `assert(k % qk == 0)`: lasciarlo abortire sarebbe un verdetto
 * peggiore che non darne uno). */
int ggc_block_vals(int type) {
    switch (type) {
    case 2:  return QK4_0;
    case 3:  return QK4_1;
    case 6:  return QK5_0;
    case 7:  return QK5_1;
    case 8:  return QK8_0;
    case 10: case 11: case 12: case 13: case 14: case 15:
    case 16: case 17: case 18: case 19:
    case 21: case 22: case 23: case 29: case 34: case 35: return QK_K;
    case 20: return QK4_NL;
    case 39: return QK_MXFP4;
    case 40: return QK_NVFP4;
    case 41: return QK1_0;
    default: return 0; /* nessun dequantize_row_* per questo valore */
    }
}

/* 1 = il testimone C conosce `type` e ha scritto `n` float in `dst`.
 * Ritorna 0 SENZA TOCCARE `dst` per ogni tipo che ggml non dequantizza (F32/F16/BF16/I*
 * sono copia diretta in ggml, non hanno un dequantize_row_*; Q8_1, Q4_2, Q4_3 idem). */
int ggc_dequant_row(int type, const uint8_t *bytes, int64_t n, float *dst) {
    if (n % ggc_block_vals(type)) return 0; /* 0 % ... = 0: tipo ignoto ricade qui */
    switch (type) {
    case 2:  dequantize_row_q4_0((const block_q4_0 *)bytes, dst, n); return 1;
    case 3:  dequantize_row_q4_1((const block_q4_1 *)bytes, dst, n); return 1;
    case 6:  dequantize_row_q5_0((const block_q5_0 *)bytes, dst, n); return 1;
    case 7:  dequantize_row_q5_1((const block_q5_1 *)bytes, dst, n); return 1;
    case 8:  dequantize_row_q8_0((const block_q8_0 *)bytes, dst, n); return 1;
    case 10: dequantize_row_q2_K((const block_q2_K *)bytes, dst, n); return 1;
    case 11: dequantize_row_q3_K((const block_q3_K *)bytes, dst, n); return 1;
    case 12: dequantize_row_q4_K((const block_q4_K *)bytes, dst, n); return 1;
    case 13: dequantize_row_q5_K((const block_q5_K *)bytes, dst, n); return 1;
    case 14: dequantize_row_q6_K((const block_q6_K *)bytes, dst, n); return 1;
    case 15: dequantize_row_q8_K((const block_q8_K *)bytes, dst, n); return 1;
    case 16: dequantize_row_iq2_xxs((const block_iq2_xxs *)bytes, dst, n); return 1;
    case 17: dequantize_row_iq2_xs((const block_iq2_xs *)bytes, dst, n); return 1;
    case 18: dequantize_row_iq3_xxs((const block_iq3_xxs *)bytes, dst, n); return 1;
    case 19: dequantize_row_iq1_s((const block_iq1_s *)bytes, dst, n); return 1;
    case 20: dequantize_row_iq4_nl((const block_iq4_nl *)bytes, dst, n); return 1;
    case 21: dequantize_row_iq3_s((const block_iq3_s *)bytes, dst, n); return 1;
    case 22: dequantize_row_iq2_s((const block_iq2_s *)bytes, dst, n); return 1;
    case 23: dequantize_row_iq4_xs((const block_iq4_xs *)bytes, dst, n); return 1;
    case 29: dequantize_row_iq1_m((const block_iq1_m *)bytes, dst, n); return 1;
    case 34: dequantize_row_tq1_0((const block_tq1_0 *)bytes, dst, n); return 1;
    case 35: dequantize_row_tq2_0((const block_tq2_0 *)bytes, dst, n); return 1;
    case 39: dequantize_row_mxfp4((const block_mxfp4 *)bytes, dst, n); return 1;
    case 40: dequantize_row_nvfp4((const block_nvfp4 *)bytes, dst, n); return 1;
    case 41: dequantize_row_q1_0((const block_q1_0 *)bytes, dst, n); return 1;
    default: return 0;
    }
}

/* Byte necessari a `n` elementi secondo la TAVOLA DI GGML (0 se il tipo non le è noto).
 * Serve a confrontare i byte, non solo i float: un blocco 66 vs 67 darebbe float diversi
 * comunque, ma con una diagnosi muta. */
uint64_t ggc_row_bytes(int type, int64_t n) {
    size_t r;
    if (ggc_block_vals(type) == 0) return 0;
    if (n % ggc_block_vals(type)) return 0;
    r = ggml_row_size((enum ggml_type)type, n);
    return (uint64_t)r;
}

/* Quanti valori di enum ggml conosce QUESTA libreria: serve all'audit per non fermarsi ai
 * tipi che hanno un `dequantize_row_*` — anche F32/F16/BF16/I8..I32 hanno una dimensione di
 * riga, ed è quella la prova che la ns. tabella da 42 righe non ha TRASCRITTO male i byte dei
 * tipi che nessuno ha mai confrontato bit a bit. */
int ggc_type_count(void) {
    return (int)GGML_TYPE_COUNT;
}

/* Byte per `n` elementi di un tipo QUALSIASI noto a ggml (0 se il valore non le è noto). */
uint64_t ggc_row_bytes_any(int type, int64_t n) {
    if (type < 0 || type >= (int)GGML_TYPE_COUNT || n <= 0) return 0;
    return (uint64_t)ggml_row_size((enum ggml_type)type, n);
}
