# Modelli gestiti — comandi di download con `fastllm`

Tutti i comandi di questo file usano **solo `fastllm`**: non serve avere il progetto
[ds4](https://github.com/antirez/ds4) installato, né il suo `download_model.sh`. I modelli vivono su
Hugging Face e `fastllm pull` li scarica nel cache HF, in modo **riprendibile** e **verificato**
(SHA-256 del blob).

> **Regola dei repo grandi.** I repo di ds4 contengono **molte varianti** dello stesso modello
> (Q2 / Q4 / MXFP4 / vision / DSpark), da ~80 a ~430 GiB l'una. Un `pull` senza filtro le scarica
> **tutte**. Usate sempre `--file '<nome esatto>'` (o un glob) per prendere **un** modello.

```sh
# un file preciso
fastllm pull antirez/deepseek-v4-gguf --file 'DeepSeek-V4-Flash-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-chat-v2-imatrix-0731.gguf'

# tutte le parti di un modello spezzato
fastllm pull antirez/deepseek-v4.1-flash-gguf --file 'DeepSeek-V4.1-Flash-1017-MXFP4-experts-*-of-00009.gguf'
```

Poi si esegue indicando il file dello snapshot:

```sh
SNAP=$(ls -d ~/.cache/huggingface/hub/models--antirez--deepseek-v4-gguf/snapshots/* | head -1)
fastllm test "$SNAP/DeepSeek-V4-Flash-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-chat-v2-imatrix-0731.gguf"
fastllm run  "$SNAP/DeepSeek-V4-Flash-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-chat-v2-imatrix-0731.gguf" "Ciao"
```

oppure lo si registra una volta e lo si chiama per nome:

```sh
fastllm add ds4flash "$SNAP/DeepSeek-V4-Flash-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-chat-v2-imatrix-0731.gguf"
fastllm run ds4flash "Ciao"
```

`--dest <dir>` specchia il modello fuori dal cache (hardlink), utile per dischi esterni.
`fastllm doctor` verifica a posteriori che i byte siano quelli promessi; `fastllm pull --repair` li
sostituisce se non lo sono.

---

## 1. Quale motore esegue cosa (stato al 2026-10-30)

`fastllm` sceglie il motore dalla **firma del file** (`general.architecture` del GGUF, o la
`config.json` per una dir MLX). Il download è indipendente da questo; l'esecuzione no.

| architettura | motore in `auto` | esito |
|---|---|---|
| `deepseek4` | ds4 **nativo** (`LLMEngine/mlx/ds4`) | funziona; con `--opt ds4_engine=vendor` va sul vendor (per misurare) |
| `qwen4exp` | ds4 **vendorizzato** (`ds4n`) | funziona |
| `glm-dsa` (GLM 5.2, GLM 5.3 full) | llama.cpp | funziona; con `--opt ds4_engine=vendor` va sul vendor |
| `deepseek41` (DeepSeek V4.1) | ds4 **vendorizzato** (`ds4n`) | instradato dal 2026-10-30; prova end-to-end ancora da fare |
| `glm5-next` (GLM 5.3 **Flash**) | ds4 **vendorizzato** (`ds4n`) | instradato dal 2026-10-30; prova end-to-end ancora da fare |
| altri GGUF | llama.cpp | dipende dall'architettura |
| dir MLX (safetensors) | MLX | funziona |

I modelli sotto sono **tutti scaricabili** oggi.  La scelta fra nativo e vendor si forza con
`fastllm --opt ds4_engine=auto|native|vendor run <modello>`; `auto` è il default e per `deepseek4`
significa nativo. È così che si misura quale dei due è più veloce sullo stesso file.

---

## 2. DeepSeek V4 Flash — repo `antirez/deepseek-v4-gguf`

| variante | file | GiB |
|---|---|---|
| **Flash Q2** (consigliata 96–128 GB) | `DeepSeek-V4-Flash-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-chat-v2-imatrix-0731.gguf` | 80,8 |
| Flash misto Q2/Q4 (128 GB, più qualità) | `DeepSeek-V4-Flash-Layers37-42Q4KExperts-OtherExpertLayersIQ2XXSGateUp-Q2KDown-AProjQ8-SExpQ8-OutQ8-chat-v2-imatrix-fixed-0731.gguf` | 90,9 |
| Flash Q4 (≥256 GB) | `DeepSeek-V4-Flash-Q4KExperts-F16HC-F16Compressor-F16Indexer-Q8Attn-Q8Shared-Q8Out-chat-v2-imatrix-0731.gguf` | 153,3 |
| Flash MXFP4 (Metal/CUDA) | `DeepSeek-V4-Flash-MXFP4Experts-F16HC-F16Compressor-F16Indexer-Q8Attn-Q8Shared-Q8Out-chat-v2-mxfp4-0731.gguf` | 145,3 |
| DSpark (speculazione, opzionale) | `DeepSeek-V4-Flash-DSpark-support-0731.gguf` | 5,6 |
| MTP (opzionale) | `DeepSeek-V4-Flash-MTP-Q4K-Q8_0-F32.gguf` | 3,5 |

```sh
fastllm pull antirez/deepseek-v4-gguf --file 'DeepSeek-V4-Flash-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-chat-v2-imatrix-0731.gguf'
fastllm pull antirez/deepseek-v4-gguf --file 'DeepSeek-V4-Flash-Layers37-42Q4KExperts-*-imatrix-fixed-0731.gguf'
fastllm pull antirez/deepseek-v4-gguf --file 'DeepSeek-V4-Flash-Q4KExperts-*-imatrix-0731.gguf'
fastllm pull antirez/deepseek-v4-gguf --file 'DeepSeek-V4-Flash-MXFP4Experts-*-mxfp4-0731.gguf'
fastllm pull antirez/deepseek-v4-gguf --file 'DeepSeek-V4-Flash-DSpark-support-0731.gguf'

# DSpark, una volta scaricato il draft (motore vendor; richiede anche l'opzione ds4_mtp)
fastllm --opt ds4_mtp="<percorso>/DeepSeek-V4-Flash-DSpark-support-0731.gguf" --opt ds4_dspark=1 \
    run "<percorso>/DeepSeek-V4-Flash-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-chat-v2-imatrix-0731.gguf"
```

Varianti **vision** (stesso repo; richiedono l'encoder):

```sh
fastllm pull antirez/deepseek-v4-gguf --file 'DeepSeek-V4-Flash-Vision-Encoder.gguf'
fastllm pull antirez/deepseek-v4-gguf --file 'DeepSeek-V4-Flash-Vision-Exp-IQ2XXS-*.gguf'          # 80,8
fastllm pull antirez/deepseek-v4-gguf --file 'DeepSeek-V4-Flash-Vision-Exp-Layers37-42Q4K*.gguf'    # 90,9
fastllm pull antirez/deepseek-v4-gguf --file 'DeepSeek-V4-Flash-Vision-Exp-MXFP4*.gguf'            # 145,3
fastllm pull antirez/deepseek-v4-gguf --file 'DeepSeek-V4-Flash-Vision-Exp-DSpark-support.gguf'     # 5,6
```

---

## 3. DeepSeek V4 PRO — stesso repo `antirez/deepseek-v4-gguf`

| variante | file | GiB |
|---|---|---|
| PRO Q2 imatrix (512 GB residente, o streaming) | `DeepSeek-V4-Pro-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-Instruct-imatrix-0813.gguf` | 432,7 |
| PRO Q4, prima metà (pipeline) | `DeepSeek-V4-Pro-Q4K-Layers00-30.gguf` | 426,1 |
| PRO Q4, seconda metà (pipeline) | `DeepSeek-V4-Pro-Q4K-Layers-31-output.gguf` | 411,6 |

```sh
fastllm pull antirez/deepseek-v4-gguf --file 'DeepSeek-V4-Pro-IQ2XXS-*-imatrix-0813.gguf'
# oppure le due metà, per l'esecuzione in pipeline (non si concatenano: sono due modelli-parte)
fastllm pull antirez/deepseek-v4-gguf --file 'DeepSeek-V4-Pro-Q4K-Layers00-30.gguf' \
                                   --file 'DeepSeek-V4-Pro-Q4K-Layers-31-output.gguf'
```

---

## 4. DeepSeek V4.1 Flash — repo `antirez/deepseek-v4.1-flash-gguf`

| variante | file | GiB |
|---|---|---|
| **V4.1 Q2** (consigliata: file unico) | `DeepSeek-V4.1-Flash-1017-IQ2_XXS-Q2_K-imatrix.gguf` | 340,6 |
| V4.1 Q4 (due parti da unire) | `DeepSeek-V4.1-Flash-Q4.gguf.part1` + `.part2` | 447,0 + 35,9 |
| V4.1 MXFP4 (nove shard) | `DeepSeek-V4.1-Flash-1017-MXFP4-experts-0000N-of-00009.gguf` | 467,1 totali |
| V4.1 Vision (encoder) | `DeepSeek-V4.1-Flash-Vision.gguf` | 0,9 |

```sh
# Q2: un solo file, il percorso più semplice
fastllm pull antirez/deepseek-v4.1-flash-gguf --file 'DeepSeek-V4.1-Flash-1017-IQ2_XXS-Q2_K-imatrix.gguf'

# Q4: due parti; vanno unite IN ORDINE in un unico .gguf (come fa il downloader di ds4).
# Servono ~2x lo spazio: le parti + il file finale (~483 GiB in tutto).
fastllm pull antirez/deepseek-v4.1-flash-gguf --file 'DeepSeek-V4.1-Flash-Q4.gguf.part*'
SNAP=$(ls -d ~/.cache/huggingface/hub/models--antirez--deepseek-v4.1-flash-gguf/snapshots/* | head -1)
cat "$SNAP/DeepSeek-V4.1-Flash-Q4.gguf.part1" "$SNAP/DeepSeek-V4.1-Flash-Q4.gguf.part2" \
    > ~/ai/Models/DeepSeek-V4.1-Flash-Q4.gguf

# MXFP4: nove shard, si scaricano tutti
fastllm pull antirez/deepseek-v4.1-flash-gguf --file 'DeepSeek-V4.1-Flash-1017-MXFP4-experts-*-of-00009.gguf'

# Vision encoder
fastllm pull antirez/deepseek-v4.1-flash-gguf --file 'DeepSeek-V4.1-Flash-Vision.gguf'
```

> **Nota onesta.** Il Q4 e l'MXFP4 di V4.1 sono archiviati **a pezzi**: l'unione/uso degli shard non
> è ancora provato da `fastllm` (nessun test end-to-end su questi file). Il Q2 è un file singolo ed è
> la scelta sicura. Il routing di `deepseek41` verso il vendor è cablato dal 2026-10-30 (§1), ma la
> prova con il modello vero è ancora da fare.

---

## 5. GLM

### 5.2 — repo `antirez/GLM-5.2-GGUF`

| variante | file | GiB |
|---|---|---|
| IQ2_XXS (routed) | `GLM-5.2-UD-IQ2_XXS_RoutedIQ2XXS_blk78Q2K.gguf` | 196,6 |
| Q2_K (routed) | `GLM-5.2-UD-Q2_K_RoutedQ2K.gguf` | 244,0 |
| Q4_K (routed) | `GLM-5.2-UD-Q4_K_RoutedQ4K.gguf` | 404,4 |

```sh
fastllm pull antirez/GLM-5.2-GGUF --file 'GLM-5.2-UD-IQ2_XXS_RoutedIQ2XXS_blk78Q2K.gguf'
fastllm pull antirez/GLM-5.2-GGUF --file 'GLM-5.2-UD-Q2_K_RoutedQ2K.gguf'
fastllm pull antirez/GLM-5.2-GGUF --file 'GLM-5.2-UD-Q4_K_RoutedQ4K.gguf'
```

### 5.3 Flash — repo `antirez/glm-5.3-flash-gguf`

| variante | file | GiB |
|---|---|---|
| **Q2** | `GLM-5.3-Flash-Q2.gguf` | 89,9 |
| Q4_K | `GLM-5.3-Flash-Q4_K.gguf` | 177,8 |
| FP8 | `GLM-5.3-Flash-FP8.gguf` | 304,7 |
| Vision encoder | `GLM-5.3-Flash-Vision-Encoder.gguf` | 1,0 |

```sh
fastllm pull antirez/glm-5.3-flash-gguf --file 'GLM-5.3-Flash-Q2.gguf'
fastllm pull antirez/glm-5.3-flash-gguf --file 'GLM-5.3-Flash-Q4_K.gguf'
fastllm pull antirez/glm-5.3-flash-gguf --file 'GLM-5.3-Flash-FP8.gguf'
fastllm pull antirez/glm-5.3-flash-gguf --file 'GLM-5.3-Flash-Vision-Encoder.gguf'
```

### 5.3 (peso pieno) — repo `antirez/glm-5.3-gguf`

| variante | file | GiB |
|---|---|---|
| IQ2_XXS (routed) | `GLM-5.3-UD-IQ2_XXS_RoutedIQ2XXS_blk78Q2K.gguf` | 196,6 |

```sh
fastllm pull antirez/glm-5.3-gguf --file 'GLM-5.3-UD-IQ2_XXS_RoutedIQ2XXS_blk78Q2K.gguf'
```

---

## 6. Qwen3.8 Flash Next — repo `antirez/qwen3.8-flash-next-gguf`

| variante | file | GiB |
|---|---|---|
| **Q2** | `Qwen3.8-Flash-Next-Q2.gguf` | 137,1 |
| Q4 | `Qwen3.8-Flash-Next-Q4.gguf` | 165,1 |

```sh
fastllm pull antirez/qwen3.8-flash-next-gguf --file 'Qwen3.8-Flash-Next-Q2.gguf'
fastllm pull antirez/qwen3.8-flash-next-gguf --file 'Qwen3.8-Flash-Next-Q4.gguf'
```

**Vision / OCR** (proiettore multimodale) dal repo `ggml-org/Qwen3.8-Flash-Next-GGUF`:

```sh
fastllm pull ggml-org/Qwen3.8-Flash-Next-GGUF --file 'mmproj-Qwen3.8-Flash-Next-Q8_0.gguf'   # 0,6 GiB
fastllm add qwen38-vl "<percorso-Q2.gguf>" --modality vlm \
    --mmproj "$(ls -d ~/.cache/huggingface/hub/models--ggml-org--Qwen3.8-Flash-Next-GGUF/snapshots/* | head -1)/mmproj-Qwen3.8-Flash-Next-Q8_0.gguf"
```

Opzionali dallo stesso repo (speculazione): `dflash-Qwen3.8-Flash-Next-{Q4_0,Q8_0,BF16}.gguf`,
`mtp-Qwen3.8-Flash-Next-{Q4_0,Q8_0,BF16}.gguf`.

---

## 7. Altri modelli

### MLX (safetensors) — esempi

```sh
fastllm pull mlx-community/Qwen3-8B-4bit
fastllm run mlx-community/Qwen3-8B-4bit "Ciao"
```

### GGUF generico (llama.cpp)

```sh
fastllm pull ggml-org/Qwen3-8B-GGUF --file '*Q4_K_M.gguf'
fastllm run "$(ls -d ~/.cache/huggingface/hub/models--ggml-org--Qwen3-8B-GGUF/snapshots/* | head -1)"/Qwen3-8B-Q4_K_M.gguf "Ciao"
```

### OCR — PaddleOCR-VL (`PaddlePaddle/PaddleOCR-VL-1.6-GGUF`)

```sh
fastllm pull PaddlePaddle/PaddleOCR-VL-1.6-GGUF
fastllm add ocr "<snapshot>/PaddleOCR-VL-1.6-GGUF.gguf" --modality ocr \
    --mmproj "<snapshot>/PaddleOCR-VL-1.6-GGUF-mmproj.gguf"
fastllm run ocr pagina.png --system "OCR:"
```

### ASR — Qwen3-ASR (`Qwen/Qwen3-ASR-0.6B`, `Qwen/Qwen3-ASR-1.7B`)

```sh
fastllm pull Qwen/Qwen3-ASR-0.6B
fastllm asr "$(ls -d ~/.cache/huggingface/hub/models--Qwen--Qwen3-ASR-0.6B/snapshots/* | head -1)" discorso.wav
```

Parakeet-TDT: `fastllm asr` accetta un **GGUF** Parakeet, ma Hugging Face non ha un repo GGUF
pubblico verificato per `nvidia/parakeet-tdt-0.6b-v2/v3` (i repo NVIDIA sono `.nemo`/ONNX): il GGUF
va convertito. Non è un modello che `fastllm` possa scaricare così com'è.

---

## 8. Spazio su disco

| modello | GiB | nota |
|---|---|---|
| DS4 Flash Q2 | 80,8 | il più piccolo dei DeepSeek |
| DS4 Flash Q2/Q4 | 90,9 | |
| DS4 Flash Q4 / MXFP4 | 153,3 / 145,3 | |
| DS4.1 Q2 | 340,6 | |
| DS4.1 Q4 (parti) | 482,9 | + il file unito = ~966 GiB durante l'unione |
| DS4 PRO Q2 | 432,7 | residente 512 GB o streaming |
| GLM 5.3 Flash Q2 | 89,9 | |
| GLM 5.3 Flash Q4_K | 177,8 | |
| GLM 5.2 IQ2_XXS | 196,6 | |
| Qwen3.8 Q2 / Q4 | 137,1 / 165,1 | |

Il download **riprende** da dove si era fermato (i byte parziali restano in `<etag>.incomplete`), e i
blob sono verificati con SHA-256: un'interruzione non corrompe il cache.

---

## 9. Dopo il download

```sh
fastllm models                  # cosa c'è nel cache, senza rete
fastllm test <file|alias>       # completo e non corrotto? (senza caricare)
fastllm doctor                  # i byte sono quelli promessi?
fastllm add <alias> <file>      # registra per nome
fastllm run <alias> "Ciao"
fastllm serve --cache ~/.cache/huggingface
```

Se il modello supera metà della memoria libera+reclamabile, `run`/`chat` propongono
`--offload-experts` (streaming degli esperti da disco): è la strada per i DeepSeek da 80–430 GiB su
128 GB di RAM, al prezzo della velocità.
