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

**Il rischio vero, e non è il silenzio — era vero, ed è ora MISURATO:** i tre k-quants che
`load_gguf` **accetta** (`Q2_K · Q4_K · Q6_K`) passano dalla **dequantizzazione di gguflib**, che è un
**secondo motore numerico**. Da questa pagina è nata la quarta testa del gate differenziale
(`tools/quant_diff_gguflib.c`, §5.2) e il verdetto è che **quel motore ha due difetti, entrambi non
nostri**:

* **Q4_K dequantizzato male in modo sistematico.** `gguf_q4_k_to_float`
  (`gguflib.c:568`) calcola le 8 scale e gli 8 minimi dei sotto-blocchi, ma nel ciclo che estrae i
  pesi usa sempre `scales[b]`/`mins[b]` per **entrambe** le metà del gruppo da 64 e non legge mai
  `scales[b+1]`/`mins[b+1]`: **128 pesi su 256 di ogni super-blocco** prendono scala e minimo del
  vicino. ggml fa l'opposto, e lo si vede in quattro righe (`ggml-quants.c:1483-1488`: `d1/m1` sui
  nibble bassi, `d2/m2` su quelli alti). Sui tensori **reali** dei nostri GGUF la divergenza comincia
  dove scatta il primo sotto-blocco dispari con scala diversa: **indice 32** su `token_embd.weight`
  di qwen2.5-omni Q4_K_M (max|diff| **0.0521**) e su `blk.37.ffn_gate_exps.weight` di
  `~/ds4/ds4flash.gguf` (**0.0716**), indice **2080** su `token_embd.weight` di olmoe q4_k_m
  (**0.0355**). Ciò che i tre indici hanno in comune è `idx % 64 == 32`, cioè **l'inizio del
  sotto-blocco dispari di un gruppo da 64**: è la firma che il meccanismo predice (dove le due scale
  coincidono, o dove i nibble alti non la sentono, la riga non compare prima). Che per olmoe le
  scale dispari dei primi 32 gruppi siano eguali alle pari **non è misurato qui**, è solo la
  conseguenza possibile dell'indice; la prova del meccanismo resta quella delle due righe sulla
  copia, che rende il tipo 12 bit-exact
  contro A, B, C e D.
* **Tre voci della tavola delle dimensioni di blocco sono sbagliate** (`gguflib.c:33-62`): `q2_k` 82
  (dove ggml, la tavola di DS4 `~/ds4/gguf-tools/quants.c:48` e la ns. dicono **84**), `iq1_s` 110
  (50), `iq4_nl` 50 (144 = 32 pesi × 18 byte). Non riguarda solo chi dequantizza:
  `gguflib.c:309-310` calcola `bsize` di OGNI tensore da quei numeri, quindi il data section si
  sposta per tutto ciò che segue nello stesso file. E la libreria **si contraddice da sola**: il suo
  `gguf_q2_k_to_float` avanza di `16+64+4` = 84 (`gguflib.c:797`).

Bug aperti dove il codice è vendorizzato: `AiWiki/01-Projects/GMBasic/Bugs/(open) -
GGUF-Gguflib-Q4K-ScalaPerduta-e-TavolaBlocchi.md`. **DS4 non usa gguflib** (verificato: zero
riferimenti a `gguflib.h` nell'albero, ha un proprio `gguf-tools/quants.c` *di sola scrittura* con la
tavola giusta) — ma i suoi GGUF sono Q4_K/Q2_K nel nome del file, quindi qualunque percorso
MLX+gguflib su di essi passa da quelle due funzioni.

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
2. **`Q2_K · Q4_K · Q6_K` NON si prendono da gguflib, e ora c'è il perché misurato**: entrano dal
   `load_gguf`, ma dalla numerica di gguflib, che su Q4_K sbaglia metà dei pesi di ogni super-blocco
   (§1). Il test che lo ha trovato è **dentro il repo** ed è diventato la quarta testa del gate:
   `tools/quant_diff_gguflib.c` batte `gguf_q*_to_float` contro `fmlx_ggml_dequant_row` (nostro,
   bit-esatto) e contro ggml, **quattro** opinioni, con l'attribuzione che segue l'autorità (se ggml
   coincide con noi l'outlier è l'altro e il rosso non è nostro). La tavola dei byte è interpellata
   **su tutti i tipi**, non sui soli sei con un dequant accanto: è `bsize` di ogni tensore che ne
   dipende. Regola operativa per la lane ①: **i passi di blocco si leggono dalla ns. tabella, mai da
   una tavola di lettori** — e se un giorno dovessimo camminiare il data section con la loro
   arithmetic, il gate va rilanciato (comandi in fondo).
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
# le due righe del §1, viste sul posto (non ricordate):
sed -n '620,650p' ../native/gmb_host_ai_mlx/build/_deps/gguflib-src/gguflib.c   # scales[b] e basta
sed -n '1483,1488p' /Users/gaetano/llamacpp/ggml/src/ggml-quants.c              # d1/m1 e d2/m2
sed -n '33,62p;309,310p;795,798p' ../native/gmb_host_ai_mlx/build/_deps/gguflib-src/gguflib.c
sed -n '40,60p' /Users/gaetano/ds4/gguf-tools/quants.c   # la TERZA tavola: q2_k 84, iq1_s 50
grep -rn "gguflib.h" /Users/gaetano/ds4 | head -3        # → niente: DS4 non lo usa
cd ../../../FastMLX && ctest                    # la nostra dequant bit-esatta (31 tipi)
# e il gate a QUATTRO teste (build completa nel README §gate differenziale):
build/quant_diff --synthetic ~/ai/Models/gguf/*.gguf ~/ds4/ds4flash.gguf | tail -6
```
