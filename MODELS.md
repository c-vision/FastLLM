# Supported models — download commands with `fastllm`

All the commands in this file use **only `fastllm`**: you do not need the
[ds4](https://github.com/antirez/ds4) project installed, nor its `download_model.sh`. The models live
on Hugging Face and `fastllm pull` downloads them into the HF cache, **resumably** and **verified**
(SHA-256 of the blob).

> **Rule for big repositories.** The ds4 repositories contain **many variants** of the same model
> (Q2 / Q4 / MXFP4 / vision / DSpark), from ~80 to ~430 GiB each. A `pull` without a filter downloads
> **all** of them. Always use `--file '<exact name>'` (or a glob) to take **one** model.

```sh
# one exact file
fastllm pull antirez/deepseek-v4-gguf --file 'DeepSeek-V4-Flash-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-chat-v2-imatrix-0731.gguf'

# all the parts of a split model
fastllm pull antirez/deepseek-v4.1-flash-gguf --file 'DeepSeek-V4.1-Flash-1017-MXFP4-experts-*-of-00009.gguf'
```

Then you run it by giving the snapshot file:

```sh
SNAP=$(ls -d ~/.cache/huggingface/hub/models--antirez--deepseek-v4-gguf/snapshots/* | head -1)
fastllm test "$SNAP/DeepSeek-V4-Flash-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-chat-v2-imatrix-0731.gguf"
fastllm run  "$SNAP/DeepSeek-V4-Flash-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-chat-v2-imatrix-0731.gguf" "Hello"
```

or you register it once and call it by name:

```sh
fastllm add ds4flash "$SNAP/DeepSeek-V4-Flash-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-chat-v2-imatrix-0731.gguf"
fastllm run ds4flash "Hello"
```

`--dest <dir>` mirrors the model outside the cache (hardlink), useful for external disks.
`fastllm doctor` checks afterwards that the bytes are the promised ones; `fastllm pull --repair`
replaces them if they are not.

---

## 1. Which engine runs what (state at 2026-10-30)

`fastllm` chooses the engine from the **signature of the file** (`general.architecture` of the GGUF,
or `config.json` for an MLX directory). The download is independent of this; execution is not.

| architecture | engine in `auto` | outcome |
|---|---|---|
| `deepseek4` | **vendored** ds4 (`LLMEngine/ds4`) | works (native engine retired on 2026-10-30: slower and twice the RAM) |
| `qwen4exp` | **vendored** ds4 | works |
| `glm-dsa` (GLM 5.2, GLM 5.3 full) | llama.cpp | works; with `--opt ds4_engine=vendor` it goes to the vendor |
| `deepseek41` (DeepSeek V4.1) | **vendored** ds4 | routed since 2026-10-30; end-to-end test still to do |
| `glm5-next` (GLM 5.3 **Flash**) | **vendored** ds4 | routed since 2026-10-30; end-to-end test still to do |
| other GGUF | llama.cpp | depends on the architecture |
| MLX directory (safetensors) | MLX | works |

All the models below are **downloadable** today. `fastllm --opt ds4_engine=auto|native|vendor` is
still accepted for compatibility, but there is **one** DS4 engine (the vendor); `native` warns that
the native engine is retired.

---

## 2. DeepSeek V4 Flash — repository `antirez/deepseek-v4-gguf`

| variant | file | GiB |
|---|---|---|
| **Flash Q2** (recommended for 96–128 GB) | `DeepSeek-V4-Flash-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-chat-v2-imatrix-0731.gguf` | 80.8 |
| Flash mixed Q2/Q4 (128 GB, more quality) | `DeepSeek-V4-Flash-Layers37-42Q4KExperts-OtherExpertLayersIQ2XXSGateUp-Q2KDown-AProjQ8-SExpQ8-OutQ8-chat-v2-imatrix-fixed-0731.gguf` | 90.9 |
| Flash Q4 (≥256 GB) | `DeepSeek-V4-Flash-Q4KExperts-F16HC-F16Compressor-F16Indexer-Q8Attn-Q8Shared-Q8Out-chat-v2-imatrix-0731.gguf` | 153.3 |
| Flash MXFP4 (Metal/CUDA) | `DeepSeek-V4-Flash-MXFP4Experts-F16HC-F16Compressor-F16Indexer-Q8Attn-Q8Shared-Q8Out-chat-v2-mxfp4-0731.gguf` | 145.3 |
| DSpark (speculation, optional) | `DeepSeek-V4-Flash-DSpark-support-0731.gguf` | 5.6 |
| MTP (optional) | `DeepSeek-V4-Flash-MTP-Q4K-Q8_0-F32.gguf` | 3.5 |

```sh
fastllm pull antirez/deepseek-v4-gguf --file 'DeepSeek-V4-Flash-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-chat-v2-imatrix-0731.gguf'
fastllm pull antirez/deepseek-v4-gguf --file 'DeepSeek-V4-Flash-Layers37-42Q4KExperts-*-imatrix-fixed-0731.gguf'
fastllm pull antirez/deepseek-v4-gguf --file 'DeepSeek-V4-Flash-Q4KExperts-*-imatrix-0731.gguf'
fastllm pull antirez/deepseek-v4-gguf --file 'DeepSeek-V4-Flash-MXFP4Experts-*-mxfp4-0731.gguf'
fastllm pull antirez/deepseek-v4-gguf --file 'DeepSeek-V4-Flash-DSpark-support-0731.gguf'

# DSpark, once the draft is downloaded (vendor engine; also needs the ds4_mtp option)
fastllm --opt ds4_mtp="<path>/DeepSeek-V4-Flash-DSpark-support-0731.gguf" --opt ds4_dspark=1 \
    run "<path>/DeepSeek-V4-Flash-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-chat-v2-imatrix-0731.gguf"
```

**Vision** variants (same repository; they need the encoder):

```sh
fastllm pull antirez/deepseek-v4-gguf --file 'DeepSeek-V4-Flash-Vision-Encoder.gguf'
fastllm pull antirez/deepseek-v4-gguf --file 'DeepSeek-V4-Flash-Vision-Exp-IQ2XXS-*.gguf'          # 80.8
fastllm pull antirez/deepseek-v4-gguf --file 'DeepSeek-V4-Flash-Vision-Exp-Layers37-42Q4K*.gguf'    # 90.9
fastllm pull antirez/deepseek-v4-gguf --file 'DeepSeek-V4-Flash-Vision-Exp-MXFP4*.gguf'            # 145.3
fastllm pull antirez/deepseek-v4-gguf --file 'DeepSeek-V4-Flash-Vision-Exp-DSpark-support.gguf'     # 5.6
```

---

## 3. DeepSeek V4 PRO — same repository `antirez/deepseek-v4-gguf`

| variant | file | GiB |
|---|---|---|
| PRO Q2 imatrix (512 GB resident, or streaming) | `DeepSeek-V4-Pro-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-Instruct-imatrix-0813.gguf` | 432.7 |
| PRO Q4, first half (pipeline) | `DeepSeek-V4-Pro-Q4K-Layers00-30.gguf` | 426.1 |
| PRO Q4, second half (pipeline) | `DeepSeek-V4-Pro-Q4K-Layers-31-output.gguf` | 411.6 |

```sh
fastllm pull antirez/deepseek-v4-gguf --file 'DeepSeek-V4-Pro-IQ2XXS-*-imatrix-0813.gguf'
# or the two halves, for pipeline execution (they are not concatenated: they are two part-models)
fastllm pull antirez/deepseek-v4-gguf --file 'DeepSeek-V4-Pro-Q4K-Layers00-30.gguf' \
                                   --file 'DeepSeek-V4-Pro-Q4K-Layers-31-output.gguf'
```

---

## 4. DeepSeek V4.1 Flash — repository `antirez/deepseek-v4.1-flash-gguf`

| variant | file | GiB |
|---|---|---|
| **V4.1 Q2** (recommended: single file) | `DeepSeek-V4.1-Flash-1017-IQ2_XXS-Q2_K-imatrix.gguf` | 340.6 |
| V4.1 Q4 (two parts to join) | `DeepSeek-V4.1-Flash-Q4.gguf.part1` + `.part2` | 447.0 + 35.9 |
| V4.1 MXFP4 (nine shards) | `DeepSeek-V4.1-Flash-1017-MXFP4-experts-0000N-of-00009.gguf` | 467.1 total |
| V4.1 Vision (encoder) | `DeepSeek-V4.1-Flash-Vision.gguf` | 0.9 |

```sh
# Q2: a single file, the simplest path
fastllm pull antirez/deepseek-v4.1-flash-gguf --file 'DeepSeek-V4.1-Flash-1017-IQ2_XXS-Q2_K-imatrix.gguf'

# Q4: two parts; they must be joined IN ORDER into a single .gguf (as the ds4 downloader does).
# You need ~2x the space: the parts + the final file (~483 GiB in all).
fastllm pull antirez/deepseek-v4.1-flash-gguf --file 'DeepSeek-V4.1-Flash-Q4.gguf.part*'
SNAP=$(ls -d ~/.cache/huggingface/hub/models--antirez--deepseek-v4.1-flash-gguf/snapshots/* | head -1)
cat "$SNAP/DeepSeek-V4.1-Flash-Q4.gguf.part1" "$SNAP/DeepSeek-V4.1-Flash-Q4.gguf.part2" \
    > ~/ai/Models/DeepSeek-V4.1-Flash-Q4.gguf

# MXFP4: nine shards, download all of them
fastllm pull antirez/deepseek-v4.1-flash-gguf --file 'DeepSeek-V4.1-Flash-1017-MXFP4-experts-*-of-00009.gguf'

# Vision encoder
fastllm pull antirez/deepseek-v4.1-flash-gguf --file 'DeepSeek-V4.1-Flash-Vision.gguf'
```

> **Caveat.** The V4.1 Q4 and MXFP4 are stored **in pieces**: joining and using the shards is not yet
> proven by `fastllm` (no end-to-end test on these files). Q2 is a single file and is the safe
> choice. The routing of `deepseek41` to the vendor has been wired since 2026-10-30 (§1), but the
> test with the real model is still to do.

---

## 5. GLM

### 5.2 — repository `antirez/GLM-5.2-GGUF`

| variant | file | GiB |
|---|---|---|
| IQ2_XXS (routed) | `GLM-5.2-UD-IQ2_XXS_RoutedIQ2XXS_blk78Q2K.gguf` | 196.6 |
| Q2_K (routed) | `GLM-5.2-UD-Q2_K_RoutedQ2K.gguf` | 244.0 |
| Q4_K (routed) | `GLM-5.2-UD-Q4_K_RoutedQ4K.gguf` | 404.4 |

```sh
fastllm pull antirez/GLM-5.2-GGUF --file 'GLM-5.2-UD-IQ2_XXS_RoutedIQ2XXS_blk78Q2K.gguf'
fastllm pull antirez/GLM-5.2-GGUF --file 'GLM-5.2-UD-Q2_K_RoutedQ2K.gguf'
fastllm pull antirez/GLM-5.2-GGUF --file 'GLM-5.2-UD-Q4_K_RoutedQ4K.gguf'
```

### 5.3 Flash — repository `antirez/glm-5.3-flash-gguf`

| variant | file | GiB |
|---|---|---|
| **Q2** | `GLM-5.3-Flash-Q2.gguf` | 89.9 |
| Q4_K | `GLM-5.3-Flash-Q4_K.gguf` | 177.8 |
| FP8 | `GLM-5.3-Flash-FP8.gguf` | 304.7 |
| Vision encoder | `GLM-5.3-Flash-Vision-Encoder.gguf` | 1.0 |

```sh
fastllm pull antirez/glm-5.3-flash-gguf --file 'GLM-5.3-Flash-Q2.gguf'
fastllm pull antirez/glm-5.3-flash-gguf --file 'GLM-5.3-Flash-Q4_K.gguf'
fastllm pull antirez/glm-5.3-flash-gguf --file 'GLM-5.3-Flash-FP8.gguf'
fastllm pull antirez/glm-5.3-flash-gguf --file 'GLM-5.3-Flash-Vision-Encoder.gguf'
```

### 5.3 (full weights) — repository `antirez/glm-5.3-gguf`

| variant | file | GiB |
|---|---|---|
| IQ2_XXS (routed) | `GLM-5.3-UD-IQ2_XXS_RoutedIQ2XXS_blk78Q2K.gguf` | 196.6 |

```sh
fastllm pull antirez/glm-5.3-gguf --file 'GLM-5.3-UD-IQ2_XXS_RoutedIQ2XXS_blk78Q2K.gguf'
```

---

## 6. Qwen3.8 Flash Next — repository `antirez/qwen3.8-flash-next-gguf`

| variant | file | GiB |
|---|---|---|
| **Q2** | `Qwen3.8-Flash-Next-Q2.gguf` | 137.1 |
| Q4 | `Qwen3.8-Flash-Next-Q4.gguf` | 165.1 |

```sh
fastllm pull antirez/qwen3.8-flash-next-gguf --file 'Qwen3.8-Flash-Next-Q2.gguf'
fastllm pull antirez/qwen3.8-flash-next-gguf --file 'Qwen3.8-Flash-Next-Q4.gguf'
```

**Vision / OCR** (multimodal projector) from the repository `ggml-org/Qwen3.8-Flash-Next-GGUF`:

```sh
fastllm pull ggml-org/Qwen3.8-Flash-Next-GGUF --file 'mmproj-Qwen3.8-Flash-Next-Q8_0.gguf'   # 0.6 GiB
fastllm add qwen38-vl "<path-to-Q2.gguf>" --modality vlm \
    --mmproj "$(ls -d ~/.cache/huggingface/hub/models--ggml-org--Qwen3.8-Flash-Next-GGUF/snapshots/* | head -1)/mmproj-Qwen3.8-Flash-Next-Q8_0.gguf"
```

Optional, from the same repository (speculation): `dflash-Qwen3.8-Flash-Next-{Q4_0,Q8_0,BF16}.gguf`,
`mtp-Qwen3.8-Flash-Next-{Q4_0,Q8_0,BF16}.gguf`.

---

## 7. Other models

### MLX (safetensors) — examples

```sh
fastllm pull mlx-community/Qwen3-8B-4bit
fastllm run mlx-community/Qwen3-8B-4bit "Hello"
```

### Generic GGUF (llama.cpp)

```sh
fastllm pull ggml-org/Qwen3-8B-GGUF --file '*Q4_K_M.gguf'
fastllm run "$(ls -d ~/.cache/huggingface/hub/models--ggml-org--Qwen3-8B-GGUF/snapshots/* | head -1)"/Qwen3-8B-Q4_K_M.gguf "Hello"
```

### OCR — PaddleOCR-VL (`PaddlePaddle/PaddleOCR-VL-1.6-GGUF`)

```sh
fastllm pull PaddlePaddle/PaddleOCR-VL-1.6-GGUF
fastllm add ocr "<snapshot>/PaddleOCR-VL-1.6-GGUF.gguf" --modality ocr \
    --mmproj "<snapshot>/PaddleOCR-VL-1.6-GGUF-mmproj.gguf"
fastllm run ocr page.png --system "OCR:"
```

### ASR — Qwen3-ASR (`Qwen/Qwen3-ASR-0.6B`, `Qwen/Qwen3-ASR-1.7B`)

```sh
fastllm pull Qwen/Qwen3-ASR-0.6B
fastllm asr "$(ls -d ~/.cache/huggingface/hub/models--Qwen--Qwen3-ASR-0.6B/snapshots/* | head -1)" speech.wav
```

Parakeet-TDT: `fastllm asr` accepts a Parakeet **GGUF**, but Hugging Face has no verified public GGUF
repository for `nvidia/parakeet-tdt-0.6b-v2/v3` (the NVIDIA repositories are `.nemo`/ONNX): the GGUF
has to be converted. It is not a model that `fastllm` can download as it is.

---

## 8. Disk space

| model | GiB | note |
|---|---|---|
| DS4 Flash Q2 | 80.8 | the smallest of the DeepSeek models |
| DS4 Flash Q2/Q4 | 90.9 | |
| DS4 Flash Q4 / MXFP4 | 153.3 / 145.3 | |
| DS4.1 Q2 | 340.6 | |
| DS4.1 Q4 (parts) | 482.9 | + the joined file = ~966 GiB while joining |
| DS4 PRO Q2 | 432.7 | 512 GB resident or streaming |
| GLM 5.3 Flash Q2 | 89.9 | |
| GLM 5.3 Flash Q4_K | 177.8 | |
| GLM 5.2 IQ2_XXS | 196.6 | |
| Qwen3.8 Q2 / Q4 | 137.1 / 165.1 | |

The download **resumes** where it stopped (partial bytes stay in `<etag>.incomplete`), and the
blobs are verified with SHA-256: an interruption does not corrupt the cache.

---

## 9. After the download

```sh
fastllm models                  # what is in the cache, no network
fastllm test <file|alias>       # complete and not corrupted? (without loading)
fastllm doctor                  # are the bytes the promised ones?
fastllm add <alias> <file>      # register by name
fastllm run <alias> "Hello"
fastllm serve --cache ~/.cache/huggingface
```

If the model exceeds half of the free+reclaimable memory, `run`/`chat` suggest
`--offload-experts` (streaming the experts from disk): it is the way to run the 80–430 GiB DeepSeek
models on 128 GB of RAM, at the cost of speed.
