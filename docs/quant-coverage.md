# Copertura quant GGUF: cosa carica MLX, cosa dequantizza FastLLM

> Aggiornato: 2026-09-23. Fonte: **codice letto su questa macchina**, non documentazione.
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

La lane GGUF di FastLLM copre **31** tipi (dal 2026-09-23: erano 13, poi 21 con i lotti A e B):
F32, F16, BF16, Q4_0, Q4_1, Q5_0, Q5_1, Q8_0, Q2_K, Q3_K, Q4_K, Q5_K, Q6_K, Q8_K, IQ2_XXS,
IQ2_XS, IQ2_S, IQ3_XXS, IQ3_S, IQ1_S, IQ1_M, IQ4_NL, IQ4_XS, TQ1_0, TQ2_0, MXFP4, NVFP4, Q1_0,
I8, I16, I32 — e l'audit del gate (`tools/quant_diff.c::coverage_audit`) verifica che
nessuno di quei 31 sia asserito senza almeno un testimone, non una lista scritta a mano.
Fuori, e **senza** possibilità di testimone: Q8_1 (ggml non ha `dequantize_row_q8_1`: è un tipo
di quantizzazione *intermedia*), Q4_2/Q4_3 (ritirati upstream), I64/F64 (non sono quantizzazioni).

## 4. Il testimone C: i 18 gap che apriva, oggi tutti colmati

`/Users/gaetano/llamacpp`: sorgente `ggml/src/ggml-quants.c` + libreria già costruita
`build/bin/libggml-base.0.15.3.dylib` (più recente del sorgente, stesso giorno; repo a
`4fc4ec554`). `nm -gU | grep -c dequantize_row` = **25**, e i 25 nomi sono esattamente le 25
definizioni nel sorgente. Firma: `void dequantize_row_X(const block_X *x, float *y, int64_t k)` con
`assert(k % QK_x == 0)` — **la stessa forma del nostro contratto** (riga di `n` elementi → F32), a
differenza di MLX che produce pesi impaccati + scale/bias separate.

Il gate li confronta tutti (`tools/quant_diff_ggml.c`). Esito del 2026-09-23, dopo i tre lotti:
**34 tipi confrontati sui bit, 42 forme confrontate con ggml, 0 incoerenze, 0 gap colmabili**,
`247 confronti` totali (7 GGUF reali + sintetico + sweep + esaustivo f16), di cui **0 nostri**
bit-diversi e **4 disaccordi fra testimoni** (A e B contro ggml sul payload degli NaN in tipo 1:
voce aperta nel vault GMBasic, `Bugs/(open) - GGUF-F16-Nan-Segnalandi-Non-Quietato.md`).

**Esito del 2026-09-24, dopo il testimone D — e il numero qui sopra è quello di IERI, non di oggi.**
Agganciata gguflib come quarta opinione (`tools/quant_diff_gguflib.c`), e corretto un difetto del
gate stesso (`main()` partiva da `argv[2]` e **saltava il primo file**: con un solo `.gguf` non
confrontava nessun tensore reale), i totali rimisurati sono **250 confronti** sui sette GGUF locali
(231 sul solo `--synthetic`), **0 nostri** bit-diversi, **23 disaccordi fra codici che vivono
altrove** (11 nel solo sintetico): i soliti 4 sull'NaN più quelli di gguflib sui bit e sulla
dimensione di blocco. Il perché l'ultima classe non sia nostra, con l'indice del primo peso
divergente su tre tensori reali, è in `docs/mlx-quant-bridge.md` §1 e §5.2.

* i **31** tipi coperti sono bit-exact anche contro C — non solo contro A e B — sui tensori reali
  dei 7 GGUF, su blocchi pseudo-casuali a seed fisso e, per le codebook, su **OGNI indice**;
* gli 18 tipi che nel lotto precedente erano "secondo codice pronto, nostra riga assente" sono
  entrati uno per uno, ciascuno con la SUA passata: Q4_1, Q5_0, Q5_1, Q3_K, Q8_K, IQ2_XS, IQ3_XXS,
  IQ1_S, IQ4_NL, IQ3_S, IQ2_S, IQ4_XS, IQ1_M, TQ1_0, TQ2_0, MXFP4, NVFP4, Q1_0;
* restano **senza** testimone: **Q8_1** (ggml non ha `dequantize_row_q8_1`, pur avendo il tipo e
  `QK8_1`: è un tipo di *quantizzazione intermedia*, non di archiviazione), Q4_2/Q4_3 (ritirati
  upstream), I64/F64 (non sono quantizzazioni).

Il caso che l'enumerazione MLX aveva fatto notare — **Q4_1, che MLX sa caricare e noi no** — è
chiuso: implementato, confrontato bit a bit, coperto.

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

## 6. Le codebook IQ: che grado di prova ha ogni tabella, e due lezioni misurate

Le griglie IQ **non sono formule**: sono codicebook *imparati* dalla calibrazione di un modello
reale, e nessuna derivazione matematica li produce. Dire "due copie concordano" non è una prova: sono due
copie dello stesso dato. Il grado di prova allora si dichiara, e per le griglie la prova vera è
**eseguita**: `grid_sweep()` in `tools/quant_diff.c` costruisce blocchi che exercano **ogni**
indice di **ognuna** delle 11 righe di codebook e batte i float contro la tabella compilata dentro
`libggml-base.dylib`.

| tabella | voci | grado di prova |
|---|---|---|
| `iq2xxs_grid`, `iq2_ksigns` | 256, 128 | **due AUTORI** distinti (ds4 + linea ggml dei duplicati GMBasic), **e** sweep esaustivo |
| `kvalues_iq4nl` | 16 | **due VERSIONI** di ggml (`~/llamacpp` e il vendor in GMBasic) + ricalcolo dalla tabella + sweep |
| `kvalues_fp4` (E2M1) | 16 | **derivata dai bit** della specifica OCP MX: non è una copia |
| `iq2xs_grid`, `iq3xxs_grid`, `iq3s_grid`, `iq2s_grid`, `iq1s_grid` | 512, 256, 512, 1024, 2048 | una sola sorgente (ggml): lo **sweep esaustivo è** la prova, non il confronto fra copie |

Esito dello sweep (2026-09-23): 256/256, 512/512, 256/256, 512/512, 1024/1024, 2048/2048 (tipo 19),
2048/2048 (tipo 29), 16/16 ×4 — **nessun bit divergente**.

**Lezione 1 — lo sweep serve, e l'ho misurato.** Scambiando due voci adiacenti dell'header generato
(`iq2s_grid[504]` ↔ `[505]`) il gate `gen_codebooks.py --check` dice *"tutte le copie concordano"*
exit 0 — perché confronta le **copie sorgente** e non rilegge mai l'header scritto — e passa anche
la forma (voci distinte, alfabeto dei byte: sono due valori *legali*, solo al posto sbagliato).
A rispondere è stato solo `grid_sweep`: `DIVERGENZA C (ggml) su "iq2s_grid"[160]: 11 vs 34.375`,
uscita 1. È la ragione per cui quello sweep esiste, ed è scritta in `tools/gen_codebooks.py` per
non far finta che la rete di forma copra più di quanto copra.

**Lezione 2 — il segno si applica negando il PRODOTTO.** La riga C di ggml è
`db * grid[j] * (segni & mask ? -1.f : 1.f)`; trascritta così com'è, sui valori finiti e su ±0 dà
gli stessi bit della dylib. Con `d` **NaN** no: su ARM64 `NaN * -1.0f` conserva il segno del NaN
(IEEE 754 non specifica il segno del risultato quando un operando è NaN, e l'AdvSIMD propaga
l'operando), mentre il codice compilato gira il bit 31. Misurato su un blocco IQ3_S con `d` NaN:
**135 elementi su 256 fuori di UN solo bit**, e **0** fuori costringendo `d` a 1.0f — la divergenza
era quello, non la formula. Da qui `fmlx_iq_signed(prod, neg)` in `fmlx_quant_internal.h`, usato da
tutti e sei i tipi IQ con i segni. Il caso non è teorico: un file corrotto, o un blocco che
moltiplica 0 per una scala infinita.
