# fastllm — CLI guide

`fastllm` is the C binary of FastLLM: a Hugging Face **hub** (download and cache), an
OpenAI/Anthropic **server**, and an Ollama-style **CLI** to choose, inspect and run models.
The engine is not in this repository: it is [LLMEnv](https://github.com/c-vision/LLMEnv) (`LLMEngine`,
`LLMCodec`, `LLMToken`, `LLMJinja`), consumed as source.

> **Golden rule.** Engine parameters are **not environment variables**: they go through the
> **options registry** with `--opt <name>=<value>` (repeatable, before the model is loaded). The
> always up-to-date list is `fastllm options`. The only variables read are **service** variables (§9).

---

## 1. Build and install

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target fastllm -j8
export PATH="$PWD/build-release:$PATH"
fastllm help
```

Requirements: macOS on Apple Silicon (Metal), a C11 compiler and, for the MLX lane, the toolchain
that LLMEnv requires. The binary is built as `build-release/fastllm` and loads the dylibs
`libllm_ai_mlx`, `libllm_ai_llamacpp`, `libllmcodec_ggml` and `libllmtoken` from the same folder
(`@rpath`).

---

## 2. How `fastllm` finds a model

For `<name|#n>` (used by `run`, `chat`, `test`, `show`, `rm`) the lookup order is:

1. **explicit path** (`./model.gguf`, `~/ai/Models/…`);
2. **local registry** — aliases and the `#n` index of `fastllm list` (`$FASTLLM_HOME/models.tsv`);
3. **Hugging Face cache** — already downloaded `models--org--name` repositories;
4. **search directories** — `$FASTLLM_HOME/paths`, one per line (see `fastllm paths`).

There is no implicit, machine-specific default: you decide the search directories.

| file | contents | managed by |
|---|---|---|
| `$FASTLLM_HOME/paths` | model search directories | `fastllm paths` |
| `$FASTLLM_HOME/models.tsv` | local registry, alias → path | `fastllm add/rm/show` |
| HF cache (`HF_HUB_CACHE`/`HF_HOME`) | downloaded repositories | `fastllm pull/models/doctor/serve` |

`$FASTLLM_HOME` is `~/.fastllm` when not set.

---

## 3. Managing models

### `pull <repo>` — download a repository (or some of its files)

```sh
fastllm pull mlx-community/Qwen3-8B-4bit
fastllm pull antirez/deepseek-v4-gguf --file '*IQ2XXS*imatrix-0731.gguf'
fastllm pull antirez/glm-5.3-flash-gguf --file 'GLM-5.3-Flash-Q2.gguf'
fastllm pull org/model --revision main --cache ~/.cache/huggingface \
    --dest ~/ai/Models --max-bytes 5000000000 --stall-timeout 60 --stall-retries 3 --repair
```

| flag | effect |
|---|---|
| `--revision <ref>` | branch or commit (default `main`) |
| `--cache <dir>` | cache root (default: the Hugging Face one) |
| `--endpoint <url>` | a server other than `huggingface.co` |
| `--dest <dir>` | mirror into `<dir>/<repo-name>` with **hardlinks** (never symlinks) |
| `--file <glob>` | download **only** the matching files: `*` = any sequence (including `/`), `?` = one character. **Repeatable**, at most 16. Without `--file` the **whole** repository is downloaded |
| `--max-bytes <n>` | byte limit for **one** file |
| `--stall-timeout <s>` | seconds without bytes before retrying (default 60) |
| `--stall-retries <n>` | `Range` resumes after a stall (default 3) |
| `--offline` | never use the network: a missing file is an error |
| `--repair` | allow replacing a blob that does not verify |
| `--quiet` | no progress lines |

Notes that matter:

- there is **one** commit (the manifest's): a pull never produces a mixed snapshot;
- the download is **resumable**: partial bytes stay in `<etag>.incomplete` and restart from there;
- a `--file` that selects **nothing** is an **error** (`no file matches…`), never a success with
  zero files: the message also says how many files the repository has;
- `--dest` is a hardlink when cache and destination are on the same volume, an atomic copy
  otherwise. The model is mirrored **only** after the cache is complete.

The per-model commands (ds4, GLM, Qwen, MLX, OCR, ASR) are in [MODELS.md](MODELS.md).

### `models [<filter>]` — what is already in the cache (no network)

```sh
fastllm models
fastllm models qwen
fastllm models mlx-community/* --json
fastllm models --quiet            # ids only, one per line
```

The filter has **three forms**: `org/name`, `name` (last part of the id), `org/*`. Zero matches =
exit 1 with empty stdout (the `grep` convention).

### `list` (`ls`) — available models, numbered

Merges the **local registry** and the **HF cache**; the number `#n` is the one used by
`run`/`chat`/`rm`/`show`.

```sh
fastllm list
fastllm list --cache ~/.cache/huggingface --json
```

### `add <name> <path>` / `rm` / `show` — local registry

```sh
fastllm add llama1b ~/ai/Models/Llama-3.2-1B-Instruct-4bit
fastllm add ds4 ~/ds4/ds4flash.gguf --modality chat
fastllm add doodle-ocr ~/ai/Models/gguf/PaddleOCR-VL-1.6-GGUF.gguf \
    --mmproj ~/ai/Models/gguf/PaddleOCR-VL-1.6-GGUF-mmproj.gguf --modality ocr

fastllm show ds4          # details of the entry
fastllm rm ds4            # removes the entry, NOT the files
fastllm rm 2
fastllm rm doodle-ocr --purge   # also deletes the files
```

`--modality chat|vlm|ocr|audio` decides how `run` interprets the second argument (text, image,
audio file). `vlm`/`ocr` need `--mmproj`.

---

## 4. Finding models on disk

### `paths` — where to look

```sh
fastllm paths                      # NUMBERED list (the number is what rm takes)
fastllm paths add ~/ai/Models
fastllm paths add /Volumes/disk/ai/Models
fastllm paths rm 2
fastllm paths scan --json          # inventory: where models EXIST
```

### `search` — find models

```sh
fastllm search                          # every accessible disk
fastllm search ~/ai --max-depth 4
fastllm search ~/ai --roots --max-depth 4   # tree of the ROOT FOLDERS
fastllm search /Volumes/disk --type gguf --json
fastllm search ~/ai --walk              # force the walk (no Spotlight)
```

Default: a **numbered, alphabetical** list of models, name on one line and path below, with no
shortening; duplicates are **all** shown. A "model" is: a directory with `config.json` + weights
(`*.safetensors`/`.bin`/`.npz`), a `*.gguf` file (≥ 1 MB, not `ggml-vocab-*`; N ggufs in one
folder = N models), or an HF cache repository.

### `test` — is the model complete and not corrupted?

```sh
fastllm test ds4
fastllm test 2
fastllm test /Volumes/disk/ai/models/Qwen3.8-27B-DFlash2
```

Checks **without loading the model**: GGUF header + the tensors that fit in the file; for an MLX/HF
directory `config.json`, the weights present and the safetensors header; for a cache repository the
blobs and snapshot (no broken symlinks); in every case the incomplete-download markers
(`*.part`/`*.incomplete`/`*.download`/`*.tmp`). **Exit 0 = complete, 1 = problem, with the reason.**

---

## 5. Generation

### `run <name|#n> [prompt]`

```sh
fastllm run llama1b "Hello, introduce yourself in one sentence."
fastllm run 0 --cache ~/.cache/huggingface --system "Be concise." \
    --max-tokens 256 --think --offload-experts
fastllm run ds4 "Explain MoE in two lines." --context 32k
```

The text is streamed **token by token** to stdout. Without `[prompt]` it reads one line at a time
from stdin (one generation per line).

### `chat <name|#n>`

```sh
fastllm chat llama1b
fastllm chat 0 --system "You are an assistant." --max-tokens 512 --think
```

REPL commands: `/exit` `/quit` `/clear` `/new` `/system` `/thinking` `/prompt` `/compact` `/help`.

| flag (run/chat) | effect |
|---|---|
| `--system <text>` | system message (for `ocr`/`vlm`/`audio`: the instruction to the model) |
| `--max-tokens <n>` | limit on generated tokens |
| `--context <n>` | context in tokens (`32k`, `1m`, a number); default: the model's own |
| `--think` | enable and show thinking (default: **off**) |
| `--offload-experts` | stream the MoE experts from disk (models larger than RAM) |
| `--cache <dir>` | root from which the model is resolved |

With `--modality vlm|ocr|audio` the second argument is a **file** (image/audio) and `--system` is
the instruction (`"OCR:"`, `"Table Recognition:"`).

> If the model exceeds half of the free+reclaimable memory and `--offload-experts` is missing,
> `run`/`chat` ask `[Y/n]` on the terminal. The question is disabled with
> `FASTLLM_NO_OFFLOAD_PROMPT=1` (the option stays `--offload-experts`).

---

## 6. Server

### `serve` — OpenAI + Anthropic

```sh
fastllm serve
fastllm serve --host 0.0.0.0 --port 8000 --cache ~/.cache/huggingface \
    --max-body 64000000 --offload-experts --quiet
fastllm serve --once          # serve ONE request and exit (smoke test)
```

Routes: `/v1/models`, `/v1/chat/completions`, `/v1/completions`, `/v1/responses`,
`/v1/messages` (Anthropic), `/v1/embeddings`, `/health`, plus the management routes
`/v1/models/load`, `/v1/models/status`, `/v1/models/unload`. A route that cannot be served answers
with **its own** status code and **its own** reason (never a 200 with an empty body).

| flag | effect |
|---|---|
| `--host <if>` | listening interface (default `127.0.0.1`) |
| `--port <n>` | port (default 8000) |
| `--max-body <n>` | byte limit of a request body |
| `--parallel <n>` | I/O pool + workers (default 1) |
| `--batch` | continuous batching: N requests decode together (implies `--parallel`) |
| `--vision-model` / `--vision-mmproj` | VLM for image input (both are required) |
| `--once` | one request, then exit |

### `ps` — is a server listening?

```sh
fastllm ps
fastllm ps --host 127.0.0.1 --port 8000
```

---

## 7. Inspection and media

### `compare <a.gguf> <b.gguf>`

**Per-tensor numeric** comparison (dequantizes the common tensors):

```sh
fastllm compare base.gguf quant.gguf
fastllm compare a.gguf b.gguf --glob 'blk.*.ffn_*'
```

`rel_l2` and `avg_diff%` (0 = identical, ~100 = random): useful for **lineage** (frozen layers have
`avg_diff ≈ 0`) and for the per-role breakdown after quantization or surgery.

### `tensors <model.gguf>`

```sh
fastllm tensors model.gguf
fastllm tensors model.gguf --diffable   # deterministic output: two identical files give the same text
```

Metadata (key-value) and the tensor table (type, name, dims; offset and bytes without `--diffable`).

### `gmbarch <url|org/name|folder>`

```sh
fastllm gmbarch mlx-community/Mixtral-8x22B-4bit
fastllm gmbarch ~/ai/Models/Qwen3-8B-4bit
```

Creates `<model_type>.gmbarch` in the **LLMEnv ABI** folder (tensor roles, the `config.json` field
map, attention/MoE/vision structures) **only if the architecture has no descriptor yet**: an
existing one is never overwritten. The same check runs by itself after `pull` and `add`.

### `asr <model_dir|model.gguf> <file>`

```sh
fastllm asr ~/ai/Models/Qwen3-ASR-0.6B speech.wav
fastllm asr parakeet-tdt.gguf audio.mp3
```

Two families: **Qwen3-ASR** (HF safetensors, 0.6B/1.7B) and **Parakeet-TDT** in GGUF (25 European
languages; on macOS it also accepts mp3/m4a/aiff). The text is streamed **token by token** (2 s
chunks + encoder window cache + prefix rollback).

---

## 8. Diagnostics

### `doctor [<filter>]`

```sh
fastllm doctor
fastllm doctor qwen
fastllm doctor mlx-community/* --quiet     # only the PROBLEMs
```

**Actually hashes** the cache bytes and compares them with the blob name (the same check `pull`
uses). Two orders: PROBLEMs change the exit code, notes do not. Damage is not touched: the cure is
`pull --repair`. Zero matches = failure.

### `options`

```sh
fastllm options
fastllm options --json
```

Lists **all** engine options (registry, no env) with the current value and an example. It loads no
model: it is the source of truth for `--opt`.

---

## 9. Global options, environment, exit codes

| option | effect |
|---|---|
| `--opt <name>=<value>` | engine parameter (repeatable, before the load) |
| `--context <n>` | context in tokens (`32k`, `1m`), alias of the registry option `context` |
| `--offload-experts` | stream the MoE experts from disk |
| `--cache <dir>` | cache root (`pull`, `models`, `list`, `serve`, `doctor`) |
| `--quiet` | no progress/summary |
| `-h`, `--help` | command help (exit 0) |

A flag that the verb **does not read** is an **error**, not an ignored option.

Environment variables (**service** only, never engine parameters):

| variable | use |
|---|---|
| `HF_TOKEN` | Hugging Face token for private/gated repositories (never in files, logs or commits) |
| `FASTLLM_HOME` | root of the state files (default `~/.fastllm`) |
| `FASTLLM_PORT` | default port for `ps` (then 8000) |
| `FASTLLM_NO_OFFLOAD_PROMPT=1` | disables the `[Y/n]` offload question |
| `HF_HUB_CACHE` / `HF_HOME` | location of the Hugging Face cache |

Exit codes: `0` success · `1` wrong usage/lane error (the exact code is in the message) ·
`2` verb not wired yet (it says which piece is missing).

**DS4 engine choice.** Since 2026-10-30 there is **one** DS4 engine: the **vendor**
(`LLMEngine/ds4`), which covers all the families. The native engine (formerly ds4-slow) is
**retired** — it was slower and used twice the RAM on the same GGUF. The option remains for
compatibility:

```sh
fastllm --opt ds4_engine=auto   run model.gguf   # default: vendor
fastllm --opt ds4_engine=vendor run model.gguf   # same
fastllm --opt ds4_engine=native run model.gguf   # accepted, warns that the native engine is retired
```

`auto` routes `deepseek4`, `deepseek41`, `glm5-next` and `qwen4exp` to the vendor, and `glm-dsa` to
llama.cpp (the vendor can be forced with `vendor`). The value lives in the registry
(`fastllm options`), not in an environment variable.

**Vendor MTP/DSpark.** Registry options, read when the engine opens:

```sh
fastllm --opt ds4_mtp=/path/DSpark-support.gguf --opt ds4_dspark=1 run model.gguf
```

`ds4_dspark=1` requires `ds4_mtp` (explicit error if missing); an unreadable path is named before
opening.

**Vision stays on llama.cpp.** The MLX and ds4 engines do **not** expose `vision_*` in their ABI:
VLM and OCR go through `--vision-model`/`--vision-mmproj` with the llama.cpp/mtmd engine. There is
no `--opt ds4_vision` — it would be a promise the engine does not keep.

---

## 10. Recipes

```sh
# From repository to first token, in five lines
fastllm pull mlx-community/Qwen3-8B-4bit
fastllm list
fastllm test mlx-community/Qwen3-8B-4bit
fastllm run mlx-community/Qwen3-8B-4bit "Hello"

# A huge GGUF, with the experts on disk
fastllm pull antirez/glm-5.3-flash-gguf --file 'GLM-5.3-Flash-Q2.gguf'
fastllm add glm53 ~/.cache/huggingface/hub/models--antirez--glm-5.3-flash-gguf/snapshots/*/GLM-5.3-Flash-Q2.gguf
fastllm run glm53 --offload-experts --context 32k "Summarize in three points."

# Server on the local network
fastllm serve --host 0.0.0.0 --port 8000 --cache ~/.cache/huggingface --batch --parallel 4

# A local model on an external disk
fastllm paths add /Volumes/disk/ai/Models
fastllm search /Volumes/disk/ai/Models --type mlx
fastllm add local 1
fastllm chat local
```

---

## 11. Common problems

| symptom | cause | remedy |
|---|---|---|
| `pull` downloads too much | the repository holds several variants | `--file '<glob>'` (see [MODELS.md](MODELS.md)) |
| `test` exits 1 | incomplete download or corrupted blob | retry `pull`; if the blob is corrupted, `pull --repair` |
| `doctor` reports PROBLEM | bytes differ from the blob name | `pull --repair` on the repository named |
| "engine not available" | LLMEnv dylib not found next to the binary | rebuild `fastllm` (the dylibs are produced in the build) |
| the model does not fit in RAM | resident size too large | `--offload-experts` and a shorter `--context` |
| `run` asks `[Y/n]` | model above half of the free memory | answer, or `FASTLLM_NO_OFFLOAD_PROMPT=1` |

---

## 12. References

- [MODELS.md](MODELS.md) — download commands for every supported model.
- [README.md](README.md) — overview, tested models and what to expect.
- `fastllm help <command>` — the live help of each verb (this guide follows it).
- `docs/quant-coverage.md`, `docs/json-contract.md`, `docs/mlx-quant-bridge.md` — internal contracts.
