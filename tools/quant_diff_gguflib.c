/* quant_diff_gguflib.c — gguflib come TESTIMONE D del gate differenziale.
 *
 * Perché esiste: `load_gguf` di MLX (0.32.0, l'albero che linkiamo in `libs/mlx-deps/mlx-src`)
 * per i tipi quantizzati che non sa impacchettare chiama `gguf_tensor_to_f16` della gguflib
 * vendorizzata (`mlx/io/gguf.cpp:11-40` → `_deps/gguflib-src/gguflib.c:955-984`). È un SECONDO
 * motore numerico che dequantizza `Q2_K · Q4_K · Q6_K` — gli stessi tipi che la ns. lane asserisce
 * di coprire bit a bit — e che nessuno ha mai confrontato con ggml. Questo modulo non aggiunge una
 * opinione estetica: chiude il buco "il percorso che il motore userà è mai stato misurato?".
 *
 * Cosa è verificato DENTRO, non assunto:
 *  - i `_Static_assert` sui valori di enum: i NUMERI che il gate usa (quelli di `ggml.h`) devono
 *    essere i numeri di gguflib per OGNI tipo che quel lettore nomina, altrimenti questo file non
 *    compila e nomina il tipo. Non è pignoleria: il gate interpella la LORO tavola degli
 *    `items_per_block`/`bytes_per_block` con un indice ggml, e una tavola interpretata all'indice
 *    sbagliato produrrebbe accuse inventate;
 *  - `count` è il numero di PESI, e con la callback `NULL` gguflib scrive `f32` diretto
 *    (`gguflib.c:541-560`): nessuna conversione nostra in mezzo, che sarebbe una terza opinione
 *    spacciata per la seconda.
 *
 * NON si linka qui `gguflib.c` per intero: si compila quel file (uno solo, senza dipendenze) con
 * questo. Serve il path via `-I`/argomento di build; il gate non è un target CMake di proposito
 * (vedi README §gate differenziale).
 */

#include <stddef.h> /* gguflib.h usa `size_t` senza includerlo: prima questo, poi quello */
#include <stdint.h>
#include <string.h>

#include "gguflib.h"

typedef void (*ggu_store_float_callback)(void *dst, uint64_t idx, float f);

void gguf_q8_0_to_float(void *w, void *dst, uint64_t count, ggu_store_float_callback cb);
void gguf_q4_k_to_float(void *w, void *dst, uint64_t count, ggu_store_float_callback cb);
void gguf_q6_k_to_float(void *w, void *dst, uint64_t count, ggu_store_float_callback cb);
void gguf_q2_k_to_float(void *w, void *dst, uint64_t count, ggu_store_float_callback cb);
void gguf_q4_0_to_float(void *w, void *dst, uint64_t count, ggu_store_float_callback cb);
void gguf_q4_1_to_float(void *w, void *dst, uint64_t count, ggu_store_float_callback cb);

/* Se gguflib rinumerasse l'enum, la compilazione si ferma col nome del tipo in mano.
 * I valori letterali NON sono i ns. numeri remembered: sono quelli di `ggml/include/ggml.h:390-420`
 * (F32=0 … BF16=30, IQ1_S=19, IQ4_NL=20, Q2_K=10, Q4_K=12, Q6_K=14), che è anche l'indice con cui
 * il gate chiama `ggml_row_size`. Sono quindi DUE cose, non una:
 *  - i sei tipi con un dequant devono avere quel numero (altrimenti D direbbe la sua opinione sul
 *    tipo sbagliato);
 *  - la TAVOLA degli `items_per_block`/`bytes_per_block` deve essere riga-per-riga allineata
 *    all'enum di ggml, perché il gate la interpella per TIPO ggml. Senza questi assert il
 *    confronto dei byte sarebbe una accusa costruita su indici non verificati. */
#define GGU_ASSERT_NUM(ourv, ggname) _Static_assert((int)(ggname) == (ourv), #ggname " != nostro")
GGU_ASSERT_NUM(0, GGUF_TYPE_F32);
GGU_ASSERT_NUM(1, GGUF_TYPE_F16);
GGU_ASSERT_NUM(2, GGUF_TYPE_Q4_0);
GGU_ASSERT_NUM(3, GGUF_TYPE_Q4_1);
GGU_ASSERT_NUM(6, GGUF_TYPE_Q5_0);
GGU_ASSERT_NUM(7, GGUF_TYPE_Q5_1);
GGU_ASSERT_NUM(8, GGUF_TYPE_Q8_0);
GGU_ASSERT_NUM(9, GGUF_TYPE_Q8_1);
GGU_ASSERT_NUM(10, GGUF_TYPE_Q2_K);
GGU_ASSERT_NUM(11, GGUF_TYPE_Q3_K);
GGU_ASSERT_NUM(12, GGUF_TYPE_Q4_K);
GGU_ASSERT_NUM(13, GGUF_TYPE_Q5_K);
GGU_ASSERT_NUM(14, GGUF_TYPE_Q6_K);
GGU_ASSERT_NUM(15, GGUF_TYPE_Q8_K);
GGU_ASSERT_NUM(16, GGUF_TYPE_IQ2_XXS);
GGU_ASSERT_NUM(17, GGUF_TYPE_IQ2_XS);
GGU_ASSERT_NUM(18, GGUF_TYPE_IQ3_XXS);
GGU_ASSERT_NUM(19, GGUF_TYPE_IQ1_S);
GGU_ASSERT_NUM(20, GGUF_TYPE_IQ4_NL);
GGU_ASSERT_NUM(21, GGUF_TYPE_IQ3_S);
GGU_ASSERT_NUM(22, GGUF_TYPE_IQ2_S);
GGU_ASSERT_NUM(23, GGUF_TYPE_IQ4_XS);
GGU_ASSERT_NUM(24, GGUF_TYPE_I8);
GGU_ASSERT_NUM(25, GGUF_TYPE_I16);
GGU_ASSERT_NUM(26, GGUF_TYPE_I32);
GGU_ASSERT_NUM(27, GGUF_TYPE_I64);
GGU_ASSERT_NUM(28, GGUF_TYPE_F64);
GGU_ASSERT_NUM(29, GGUF_TYPE_IQ1_M);
GGU_ASSERT_NUM(30, GGUF_TYPE_BF16);

/* 1 = questo testimone conosce il tipo. Il set è LETTO da `gguflib.c:958-982`, non ricordato: è
 * l'elenco esatto di ciò che `load_gguf` di MLX può dequantizzare passando da qui. */
int ggu_covers(int type) {
    switch (type) {
        case 2: case 3: case 8: case 10: case 12: case 14: return 1;
        default: return 0;
    }
}

/* 0 = non coperto (NON un fallimento: è "questo codice non ha un'opinione"). 1 = buffer scritto. */
int ggu_dequant_row(int type, const uint8_t *bytes, int64_t n, float *dst) {
    if (!ggu_covers(type) || n <= 0) return 0;
    switch (type) {
        case 2:  gguf_q4_0_to_float((void *)bytes, dst, (uint64_t)n, NULL); return 1;
        case 3:  gguf_q4_1_to_float((void *)bytes, dst, (uint64_t)n, NULL); return 1;
        case 8:  gguf_q8_0_to_float((void *)bytes, dst, (uint64_t)n, NULL); return 1;
        case 10: gguf_q2_k_to_float((void *)bytes, dst, (uint64_t)n, NULL); return 1;
        case 12: gguf_q4_k_to_float((void *)bytes, dst, (uint64_t)n, NULL); return 1;
        case 14: gguf_q6_k_to_float((void *)bytes, dst, (uint64_t)n, NULL); return 1;
        default: return 0;
    }
}

/* I byte che gguflib si aspetta per `n` pesi, letti dalla TAVOLA DEL LETTORE STESSO
 * (`gguflib.c:26-46`: nome, pesi per blocco, byte per blocco). La struct vive nel `.c` che
 * compiliamo insieme e non è nell'header, quindi è **ridichiarata qui con i campi nello stesso
 * ordine**: se gguflib cambia forma alla tabella noi leggiamo byte assurdi, e lo scontro con
 * `ggml_row_size` nel gate lo dice — non un assert che si fida.
 *
 * QUI NON VALE IL FILTRO `ggu_covers`: la tabella è usata per calcolare `bsize` di OGNI tensore
 * (`gguflib.c:309-310`), anche di quelli che il lettore non sa dequantizzare. Una voce sbagliata
 * sposta il data section di TUTTI i tensori che seguono, quindi l'interrogazione onesta è su
 * tutte le voci, non sui sei tipi con un dequant accanto. */
struct ggu_type_features {
    char *name;
    uint32_t items_per_block;
    uint32_t bytes_per_block;
};
/* L'accesso è la FUNZIONE di gguflib, non un nostro indice: `gguflib.c:78-81` confronta il tipo
 * con il `sizeof` VERO della tavola e rende NULL fuori gamma. Ridichiararla qui contro la ns.
 * struct è il rischio calcolato (se loro cambiano i campi leggiamo byte assurdi, e lo scontro con
 * ggml lo dice); indexare la tavola da fuori sarebbe peggio: un overrun che sembra un loro bug. */
extern struct ggu_type_features *gguf_get_tensor_type_features(uint32_t type);

uint64_t ggu_row_bytes(int type, int64_t n);
uint64_t ggu_row_bytes(int type, int64_t n) {
    struct ggu_type_features *tf;
    int64_t blk;
    if (n <= 0 || type < 0) return 0;
    tf = gguf_get_tensor_type_features((uint32_t)type);
    if (!tf || !tf->items_per_block || !tf->bytes_per_block) return 0;
    blk = (n + (int64_t)tf->items_per_block - 1) / (int64_t)tf->items_per_block;
    return (uint64_t)blk * (uint64_t)tf->bytes_per_block;
}

/* Il nome che gguflib stesso dà al tipo: serve perché il messaggio del gate dica quale voce della
 * ns. tabella posizionale corrisponde a quale voce della loro, senza che nessuno dei due debba
 * fidarsi dell'indice. 0 = fuori gamma. */
const char *ggu_type_name(int type) {
    struct ggu_type_features *tf;
    if (type < 0) return 0;
    tf = gguf_get_tensor_type_features((uint32_t)type);
    return tf ? tf->name : 0;
}
