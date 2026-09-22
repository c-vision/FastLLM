# Copertura quant GGUF: cosa carica MLX, cosa dequantizza FastMLX

> Aggiornato: 2026-09-22. Fonte: **codice letto su questa macchina**, non documentazione.
> Ogni affermazione ha il suo `file:riga`. Dove il dato non è verificabile localmente è
> scritto **non verificabile**, mai spacciato per numero.

Percorso MLX ispezionato (copia **non modificata** da GMBasic — `git log` su quel path mostra
un solo commit, quello di vendoring):
`GMBasic/libs/mlx-deps/mlx-src/mlx/io/{gguf.cpp,gguf_quants.cpp}`.

## 1. Cosa dequantizza MLX: **3 tipi**, e non in F32

`gguf_quants.cpp` (166 righe) tratta esattamente:

| tipo | byte/blocco | dove |
|---|---|---|
| Q4_0 | 18 | `gguf_quants.cpp:39` |
| Q4_1 | 20 | `gguf_quants.cpp:60-61` |
| Q8_0 | 34 | `gguf_quants.cpp:83` |

Il **contratto non è "riga di F32"**: `gguf_load_quantized` produce tre array separati —
pesi `int8` impaccati in `uint32` (`:131`), `.scales` e `.biases` `float16` (`:138-139`), con
`bias = -8*scale` per Q4_0 (`:46`) e `-128*scale` per Q8_0 (`:91`): la moltiplicazione quantizzata
la fa un kernel dopo. Ne segue che **MLX non è un testimone bit-exact possibile** per
`fmlx_ggml_dequant_row` (che produce F32): confrontare le due cose avrebbe senso solo su
`w*scale+bias` rifatto a mano, cioè su una terza implementazione inventata qui. Per questo la
regola di copertura di `src/lane_gguf/fmlx_quant.c` ammette solo i due dequant-a-F32 di casa
(`gmb_host_ai_mlx` e `gmb_moe_runtime`).

Il resto dei tipi passa da `extract_tensor_data` (`gguf.cpp:59-86`): copia diretta per
**F32, F16, I8, I16, I32** (`gguf_type_to_dtype`, `:32-45`), altrimenti `gguf_tensor_to_f16`
(`:79`) con il `// TODO: Add other dequantization options.` (`:78`) ancora aperto.

**Non verificabile qui**: `gguf_tensor_to_f16` viene da `gguflib` (antirez), che MLX scarica e
che **non esiste su questa macchina** (`find /Users/gaetano -maxdepth 7 -name 'gguflib.*'` →
nulla; dentro `mlx-src` il simbolo è solo *chiamato*, mai definito). Quindi l'elenco completo di
ciò che MLX *può* caricare non è determinabile localmente; ciò che determinabile è il **punto 1**
sopra e il cancello del chiamante.

Il cancello: `gguf.cpp:228-230` — `if (Q4_0 || Q4_1 || Q8_0) gguf_load_quantized(...) else ...`.

## 2. Due difetti latenti in `gguf_quants.cpp` (upstream Apple, **non** GMBasic)

1. **`:105-110`**: `weights_per_byte` vale 2 per Q4_0/Q4_1, **1 nell'`else`** commentato
   `// tensor.type == GGUF_TYPE_Q8_0`. È un'assunzione sul chiamante, non un controllo.
2. **`:140-146`**: la catena `if/else if/else if` non ha `else`. Un tipo che arrivasse fin lì
   senza matchere nessuno lascerebbe `weights`/`scales`/`biases` alla **malloc non inizializzata**
   di `:131` e li `emplace`erebbe comunque nella mappa (`:148`, `:162-163`): spazzatura silenziosa,
   nessun errore. Oggi è irraggiungibile *solo* perché il cancello `:228` lo impedisce — i due
   elenchi (`:228` e `:140-146`) sono la stessa informazione scritta due volte, e la seconda non
   verifica la prima.

Non è un bug dell'implementazione GMBasic (il file è copia non modificata di upstream Apple, §0
sopra), quindi **non** ho aperto una voce nel vault GMBasic: aprirei una falsa asserzione. Se e
quando la lane `MLXEngine` porterà questo codice dentro ciò che costruiamo, i due punti diventano
nostri e la regola del vault scatta.

## 3. Il numero che conta oggi

**BF16 (30) non è in nessuno dei percorsi locali di MLX** (né i 3 quantizzati, né i 5
"a copia diretta"). Su questa macchina BF16 è già **164 tensori** in
`PaddleOCR-VL-1.6-GGUF-mmproj.gguf` (164 tensori BF16 su 443) e **128** in
`PaddleOCR-VL-1.6-GGUF.gguf` (istogrammi del nostro `fmlx_gguf_inspect`): caricarli con MLX dipende
dal codice di §1 "non verificabile qui". `fmlx_ggml_dequant_row` lo copre, con testimone bit-exact.

La lane GGUF di FastMLX copre **13** tipi: F32, F16, BF16, Q4_0, Q8_0, Q2_K, Q4_K, Q5_K, Q6_K,
IQ2_XXS, I8, I16, I32 — e l'audit del gate (`tools/quant_diff.c::coverage_audit`) verifica che
nessuno di quei 13 sia asserito senza almeno un testimone, non una lista scritta a mano.

## 4. Il testimone C, e i 18 gap che apre (misurato, non proposto)

`/Users/gaetano/llamacpp`: sorgente `ggml/src/ggml-quants.c` + libreria già costruita
`build/bin/libggml-base.0.15.3.dylib` (più recente del sorgente, stesso giorno; repo a
`4fc4ec554`). `nm -gU | grep -c dequantize_row` = **25**, e i 25 nomi sono esattamente le 25
definizioni nel sorgente. Firma: `void dequantize_row_X(const block_X *x, float *y, int64_t k)` con
`assert(k % QK_x == 0)` — **la stessa forma del nostro contratto** (riga di `n` elementi → F32), a
differenza di MLX che produce pesi impaccati + scale/bias separate.

Il gate li confronta tutti (`tools/quant_diff_ggml.c`). Esito di oggi: **34 tipi confrontati sui
bit, 42 forme confrontate con ggml, 0 incoerenze, 18 gap colmabili**.

* i nostri **13** tipi sono bit-exact anche contro C — non solo contro A e B — sui tensori reali
  dei 7 GGUF e su blocchi pseudo-casuali;
* **18** tipi hanno già un secondo codice pronto: Q4_1, Q5_0, Q5_1, Q3_K, Q8_K, IQ2_XS, IQ3_XXS,
  IQ1_S, IQ4_NL, IQ3_S, IQ2_S, IQ4_XS, IQ1_M, TQ1_0, TQ2_0, MXFP4, NVFP4, Q1_0. Entrano uno per
  uno, quando la LORO riga passa — non "quando avremo implementato la famiglia";
* restano **senza** testimone: **Q8_1** (ggml non ha `dequantize_row_q8_1`, pur avendo il tipo e
  `QK8_1`: è un tipo di *quantizzazione intermedia*, non di archiviazione), Q4_2/Q4_3 (ritirati
  upstream), I64/F64 (non sono quantizzazioni).

Il caso che l'enumerazione MLX aveva fatto notare — **Q4_1, che MLX sa caricare e noi no** — ha ora
un secondo codice: entra quando lo implemento e lo confronto, non prima.

## 5. L'incidente: la NS. tabella aveva cinque tipi spostati di uno

Ciò che il testimone C ha scoperto il primo giorno in cui l'ho agganciato — **non** un difetto dei
lettori di casa, un difetto **nostro**, e vale la pena raccontarlo per come è nato.

`ds4/gguf-tools/quants.h:48-52` e `ggml.h:424-431` dicono, entrambi:

| tipo | valore VERO | valore che avevamo scritto noi |
|---|---|---|
| TQ1_0 | 34 | 33 |
| TQ2_0 | 35 | 34 |
| MXFP4 | 39 | 38 |
| NVFP4 | 40 | 39 |
| Q1_0 | 41 | 40 |

ds4 usa inizializzatori designati (`[DS4Q_TYPE_TQ1_0] = { "tq1_0", QK_K, 54, ... }`): la forma
*keyed by value* che questo progetto adotta. Io ho trascritto quell'enum in un array
**posizionale** in C e, nel farlo, ho compresso i buchi 31/32/33 (`Q4_0_4_4`, `Q4_0_4_8`,
`Q4_0_8_8` ritirati) e 36/37/38 (`IQ4_NL_*`), shiftando tutto ciò che veniva dopo.

Perché nessuno se n'era accorto:
* `test_gguf.c` aveva una lista `REF[]` di valori "di riferimento" — che era **la mia stessa
  trascrizione**. Due copie dello stesso errore concordano: era un test di coerenza interna, non di
  verità, e il suo nome ("parità contro una sorgente indipendente") asseriva il contrario;
* nessun file GGUF su questa macchina contiene tensori TQ*/MXFP4/NVFP4/Q1_0 (istogrammi misurati),
  quindi nessun confronto bit-a-bit li avrebbe mai sfiorati;
* la conseguenza non era "float sbagliati" ma **byte sbagliati**: un tensore MXFP4 (39) etichettato
  NVFP4 avrebbe avuto una dimensione di riga errata, spostando la `data_usata` di ogni tensore
  successivo nel file.

Cosa c'è adesso, perché non succeda di nuovo (in ordine di efficacia):
1. **`_Static_assert`** contro i macro di `ggml.h` nel testimone C: se un numero diverge, la
   compilazione si ferma col nome del tipo. Non è un test che "gira": è un test che non si può
   saltare;
2. **asse della forma** nell'audit: 42 valori × (elementi/blocco, byte/riga) contro
   `ggml_row_size` — copre anche F32/F16/BF16/I8..I32, cioè i tipi che nessun dequant bit-a-bit
   tocca mai;
3. `REF_HOLES[]` in `test_gguf.c`: se 31/32/33/36/37/38 riprendono un nome, il test lo dice con la
   parola "compattata".

I lettori GMBasic **non** hanno questo difetto: `gmb_ggml.h` si ferma a `BF16 = 30` e non nomina
affatto 31..41 (il loro gap è un altro, ed è già una voce aperta nel vault — `type_size` che
ritorna 0 fuori range). **Nessuna nuova voce GMBasic aperta da questo incidente**, e nessuna
modifica a codice GMBasic.
