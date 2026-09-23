# Ponte GGUF → MLX: cosa fa davvero il loader, e con quali nomi d'API

> Letto il codice che **compiliamo noi**, non una descrizione di MLX recente. Ogni affermazione qui
> ha un `path:riga` e si può battere con il comando in fondo. Le righe `mlx/...` sono di
> `GMBasic/libs/mlx-deps/mlx-src/` = **MLX 0.32.0** (`mlx/version.h:7-9`), che è l'albero linkato da
> `libmlx.a`/`libmlxc.a` in `GMBasic/libs/mlx-deps/precompiled/darwin-arm64/`.

## 1. Il loader GGUF di MLX: due percorsi, e dove sta il buco reale

`mlx/io/gguf.cpp:226-237`: i tipi **`Q4_0`, `Q4_1`, `Q8_0`** vanno a
`gguf_load_quantized` (→ `uint32` + scale/bias `float16`, il formato di `quantized_matmul`);
**tutti gli altri** vanno a `extract_tensor_data` (`:11-40`), che fa due cose diverse:

- `gguf_type_to_dtype` (`:3-15`) mappa **solo** `F32 · F16 · I8 · I16 · I32` → copia byte;
- ogni altro tipo → `gguf_tensor_to_f16` della **gguflib** vendorizzata
  (`native/gmb_host_ai_mlx/build/_deps/gguflib-src/gguflib.c:955-984`), che **dequantizza davvero**
  `F32 · F16 · BF16 · Q8_0 · Q2_K · Q4_K · Q6_K · Q4_0 · Q4_1` e per il resto ritorna `NULL` con
  `errno = EINVAL` → `mlx` lancia `runtime_error("[load_gguf] gguf_tensor_to_f16 failed")`
  (`mlx/io/gguf.cpp:33-36`).

**Rettifica di una conclusione che avevo scritto sbagliata.** Prima di aprire `extract_tensor_data`
avevo dedotto dall'`else` di `:231-236` che i tipi fuori lista fossero «spazzatura silenziosa». **È
falso**: non c'è nessun percorso che legga male i byte — gli IQ, `Q3_K`, `Q5_K`, `Q2_S…Q8_K`, `F64`
**muoiono rumorosamente** con `EINVAL`. Il `else` non è un buco silenzioso, è un bivio che non
avevo letto fino in fondo (la stessa trappola di
`AiWiki/01-Projects/GMBasic/Hard-Lessons/Grep-Negativo-Dipendenza-Vendorizzata-Non-Prova-Assenza.md`:
osservazione vera, *quindi* falso).

**Il rischio vero, e non è il silenzio:** i tre k-quants che `load_gguf` **accetta**
(`Q2_K · Q4_K · Q6_K`) passano dalla **dequantizzazione di gguflib**, che è un **secondo motore
numerico** e di cui **non esiste una verifica di bit-esattezza contro ggml** in questo progetto. La
nostra lane (`src/lane_gguf/fmlx_quant*.c`) è bit-esatta battuta contro le tabelle compilate nella
dylib di ggml; gguflib no, mai confrontata. Prima di appoggiarci un carico di lavoro, va misurata —
§5.4.

**Conseguenza per la lane ①:** i tipi che gguflib non conosce (tutti gli **IQ**, che sono il cuore di
DS4) **non possono** entrare da `load_gguf`: lì la dequant la facciamo noi con i 31 tipi bit-esatti
e consegniamo a MLX tensori `f16`/`f32`, oppure i nostri `uint32` + scale/bias **solo** per i tre
tipi che MLX intende allo stesso modo (§3).


## 2. I nomi d'API: `group_size`/`mode`, non `frame_size`

Nell'albero 0.32 **la stringa `frame_size` non esiste** (`grep -rn frame_size mlx-src/mlx` → niente;
assente anche negli header di `mlx` 0.32.2 installato in `~/ai/env-mlxlm`, `env-qwen4ref`,
`env-mflux`, `env-vmlx`). I knob sono `group_size` + `bits` + `mode`:

```
mlx/ops.h:1548-1557  quantized_matmul(x, w, scales, biases, transpose, group_size, bits, mode)
mlx/ops.cpp:4519-4545 quantization_params_from_mode:
                     Affine → 64/4 · Nvfp4 → 16/4 · Mxfp4 → 32/4 · Mxfp8 → 32/8
```

Quindi il difetto descritto nella letteratura recente — «il `frame_size` di default è 64 e un
`Q4_0` ha scale+pesi ogni 32, quindi la mappatura 1:1 non c'è e serve un packing artificioso e
lossy» — qui **assume un'altra forma, e più benigna**: la forma è `group_size`, e un gruppo 32 non è
perso, è **il valore che il loader GGUF usa già per conto suo** (`gguf_quants.cpp:115`
`weights_per_block = 32`). La regola per noi è la stessa, enunciata in un altro modo: **mai passare
dal default** — `group_size` si dichiara esplicitamente a ogni `quantized_matmul`, perché il default
di `Affine` è 64 e un tensore con gruppi da 32 non ci sta.

## 3. L'aritmetica che il `group_size` deve soddisfare (e che NON è silenziosa)

```
mlx/ops.cpp:115   w.shape(-1) * 32 / bits  ==  scales.shape(-1) * group_size
```

Se non torna, `throw std::invalid_argument` (`:116-122`), idem per la dimensione interna del matmul
(`:130-150`). Con `bits = 4` un `uint32` contiene 8 pesi: 32 pesi = 4 `uint32` = 16 byte, che è
esattamente il `bytes_per_block` di `Q4_0` (18 = 2 di scala + 16) e di `Q4_1` (20 = 2 + 2 + 16) in
`gguf_quants.cpp:39,60-61`. **Questa è la parte buona**: per i tre tipi supportati il packing di MLX
è una *riorganizzazione* dei byte ggml, non una perdita — `unpack_32_4` (`gguf_quants.cpp:13-30`) è
l'interleave a nibbie che produce i 16 byte, e la scala è `float16` con bias `-8·s` (Q4_0, `:46`) o
bias esplicito (Q4_1, `:68`).

## 4. Dove il racconto "ripack" va corretto, col byte layout vero

| tipo ggml | cosa dice la letteratura MLX | cosa dice il layout **misurato da noi** |
|---|---|---|
| `Q4_0`, `Q4_1` | «repack del formato q4 di ggml, 2 byte di scale fp16 e 32 pesi 4-bit per blocco» | **confermato**: `dq_q4_0` legge `blk[0..1]`=scala, `blk[2..17]`=16 byte di nibbie, 32 pesi (`src/lane_gguf/fmlx_quant.c:105-111`), blocco da 18 byte |
| `Q2_K` | «i bit bassi nei primi 32 byte, i 2 bit alti impaccati nelle nibbie dei secondi 32» | **non è il layout ggml di Q2_K**: il blocco è da **84** byte = 16 byte di scale/min per i 16 sottogruppi (`x[g]`, nibbia bassa=scala, alta=min) + **64** byte di pesi a 2 bit con indici a salire di 32 ogni 8 gruppi e di 16 nei gruppi dispari + `d`/`dmin` fp16 a `x[80..83]` (`src/lane_gguf/fmlx_quant.c:117-138`, bit-esatto contro le tabelle di ggml). I bit «alti in una nibbia separata» sono la forma di **Q3_K** (`ql`+`qh`) e **Q5_K** (`qh` a `:140-142`), non di Q2_K: Q2_K non ha un terzo bit da nessuna parte |
| `IQ1_S`, `IQ1_M` | «codici a 8 bit separati dai segni, reinterpretati in word da 16 bit (codici + 8 mask di segno)» | plausibile come **descrivere il kernel ggml** (che legge 8 codici + 8 bit di segno per 32 pesi), ma qui **non lo affermiamo**: la nostra lane dequantizza questi tipi con la tabella codebook battuta contro `ggml` (`src/lane_gguf/fmlx_quant_iq.c`), e non abbiamo mai letto `mlx_i1` — perché in 0.32 **non esiste** |

## 5. Regole di progettazione che ne escono

1. **`load_gguf` non è il percorso per gli IQ** (e per `Q3_K`, `Q5_K`, `Q2_S…Q8_K`, `F64`): lì
   gguflib dice `EINVAL` e MLX lancia. Per i tre tipi impaccati (`Q4_0/Q4_1/Q8_0`) è invece **il**
   percorso, perché produce esattamente i `uint32` + scale/bias che `quantized_matmul` vuole.
2. **`Q2_K · Q4_K · Q6_K` sono una scelta da misurare, non da decidere oggi**: entrano, ma dalla
   numerica di gguflib. Il test è economico e sta nel repo già pronto: costruire blocchi casuali,
   battere `gguf_q*_to_float` di gguflib contro `fmlx_ggml_dequant_row` (nostro, bit-esatto) e contro
   ggml (`tools/quant_diff_ggml.c`) — **tre** opinioni, e se gguflib diverge il verdetto dice di chi
   è la colpa.
3. **Mai il default di `group_size`**: lo passiamo noi, e il valore viene dal **tipo ggml** (32 per
   Q4_0/Q4_1/Q8_0, non 64).
4. **Per i tipi k-quants e IQ la conversione è dequant → `f16`**, non ri-pack: costa banda di memoria
   ma è l'unico cammino che abbiamo **già** bit-esatto, e la decisione va presa da misurati (pesi in
   `f16` = 4× i byte di un Q4) e non da una promessa di velocità.
5. **Se la lane ① adotterà un MLX più recente**, il nome del knob cambia (`group_size` →
   `frame_size`) e il §2 va riletto lì: i default per modalità si leggono in un solo punto,
   `quantization_params_from_mode`.

## 6. Come si ribatte

```
cd /Users/gaetano/Documents/Projects/GMBasic/libs/mlx-deps
sed -n '226,237p' mlx-src/mlx/io/gguf.cpp       # i tre impaccati, e l'else
sed -n '3,15p' mlx-src/mlx/io/gguf.cpp          # gguf_type_to_dtype: SOLO i tipi non quantizzati
sed -n '11,40p' mlx-src/mlx/io/gguf.cpp         # extract_tensor_data: copia, o f16, o NULL
sed -n '955,984p' ../native/gmb_host_ai_mlx/build/_deps/gguflib-src/gguflib.c   # cosa conosce gguflib
sed -n '13,30p;105,125p' mlx-src/mlx/io/gguf_quants.cpp
sed -n '108,124p;4519,4545p' mlx-src/mlx/ops.cpp # l'invariante e i default per modalità
grep -rn frame_size mlx-src/mlx | head          # → niente (0.32)
cd ../../../FastMLX && ctest                    # la nostra dequant bit-esatta (31 tipi)
```
