/* fmlx_quant.h — dequant dei tipi ggml in F32 (lane_gguf).
 *
 * Contratto (le regole sono nel commento di fmlx_gguf.h, qui si applicano ai valori):
 *  - nessun valore di ritorno "silenzioso": ogni tipo non coperto dà un CODICE, e `dst`
 *    non viene toccata quando il codice non è OK;
 *  - `n` (elementi) deve essere multiplo del blocco del tipo, altrimenti
 *    FMLX_GGUF_ERR_NUMEL_BLOCK — non si tronca e non si sballa;
 *  - le formule sono le stesse di ggml (float di scala f16 + indici interi). Le abbiamo
 *    confrontate bit a bit contro TRE implementazioni indipendenti — le due già in uso su
 *    GMBasic (gmb_ggml.h e gmb_moe_ggml.c) più `dequantize_row_*` di llama.cpp/ggml — sui
 *    tensori REALI dei file GGUF di questa macchina: vedi tools/quant_diff.c. Un tipo senza
 *    quella conferma è NOT_COVERED, non "circa". `fmlx_ggml_type_is_covered` asserisce quindi
 *    solo ciò che almeno un SECONDO codice sostiene: un tipo entra con un testimone che dice
 *    lo stesso bit, non entra perché "sembra giusto" (e quali gap sono già colmabili li
 *    enumera il gate, non questa riga di commento).
 *  - le codebook degli IQ sono DATI, non formule: arrivano da
 *    src/lane_gguf/fmlx_iq2_tables.h, generato e incrociato da tools/gen_codebooks.py.
 */
#ifndef FMLX_QUANT_H
#define FMLX_QUANT_H

#include "fmlx/fmlx_gguf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* True se questa lane dequantizza `type` (il tipo può essere comunque knowable: serve a
 * dire prima se il lavoro è possibile, senza copiare byte). */
int fmlx_ggml_type_is_covered(int type);

/* Dequant di `n` elementi da `src` (che deve contenere fmlx_ggml_type_bytes(type,n) byte)
 * a `dst` (n float). Ritorna OK o il codice del rifiuto. */
FmlxGgufStatus fmlx_ggml_dequant_row(int type, const uint8_t *src, uint64_t n, float *dst);

/* F16/BF16 -> F32 (IEEE half / upper 16 bit). */
float fmlx_ggml_f16_to_f32(uint16_t h);
float fmlx_ggml_bf16_to_f32(uint16_t b);

#ifdef __cplusplus
}
#endif

#endif /* FMLX_QUANT_H */
