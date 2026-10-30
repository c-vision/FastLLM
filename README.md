# FastLLM

FastLLM is a single command-line program, `fastllm`, for running large language models locally on Apple Silicon Macs. It works
like Ollama: you download a model, then chat with it in the terminal or serve it over an OpenAI- and Anthropic-compatible HTTP API.

It runs **MLX** models (Hugging Face safetensors folders, quantized to 2, 3, 4, 6 or 8 bits, plus MXFP4 and MXFP8) and **GGUF**
models. It also downloads models from Hugging Face with resume, checks the files on disk, and creates the architecture descriptors
the engine needs.

The inference engine is not in this repository. It is [LLMEnv](https://github.com/c-vision/LLMEnv) (`LLMEngine`, `LLMCodec`, `LLMToken`, `LLMJinja`), a separate
package that FastLLM finds next to itself. Every install method below installs it for you if it is not on the machine yet, and
leaves it alone if it is.

## Install

Requires a Mac with Apple Silicon and macOS 14 or later. On macOS 26.2 and later you get the engine build with the NAX kernels
(the tensor units of the M5), which speeds up the prefill; on older systems you get the compatible build. The installers pick the right one.

**Homebrew**

```sh
brew tap c-vision/fastllm
brew trust c-vision/fastllm
brew install fastllm
```

This also installs the engine (the `llmenv` formula of the same tap). Upgrade with `brew upgrade fastllm`.

*About `brew trust`.* Homebrew 7 and later refuse to load formulae from a tap that is not its own until you say you trust it, because a
formula is code that runs on your Mac. Without it, `brew install fastllm` stops with "Refusing to load formula … from untrusted tap" and tells
you the command to run. `brew trust c-vision/fastllm` trusts the whole tap (the two formulae `fastllm` and `llmenv`, both built from the
[GitHub releases](https://github.com/c-vision/FastLLM/releases) of this project). To trust only what you install, use
`brew trust --formula c-vision/fastllm/fastllm c-vision/fastllm/llmenv` instead. The choice is saved in `~/.homebrew/trust.json`, and you can
read the tap's formulae in its [repository](https://github.com/c-vision/homebrew-fastllm) before trusting it. If you would rather not trust
a tap, use the installer script below.

**Installer script** (no sudo, installs into `~/.local`)

```sh
curl -fsSL https://github.com/c-vision/FastLLM/releases/latest/download/install.sh | bash
```

Run it again whenever you like: it downloads nothing that is already in place. Useful options: `--prefix DIR`, `--version X.Y.Z`,
`--pull-starter` (also downloads a first model that fits your RAM), `--uninstall`. Add `~/.local/bin` to your `PATH` if the script tells you to.
To check the script before running it, download it from the [releases page](https://github.com/c-vision/FastLLM/releases) and read it; its
SHA-256 is in `SHA256SUMS-<version>`.

**Installer package** — download `fastllm-<version>-macos-arm64.pkg` from the [releases page](https://github.com/c-vision/FastLLM/releases) and the
`llmenv-<version>-macos14-arm64.pkg` (or `macos26` on macOS 26.2+) from the [LLMEnv releases](https://github.com/c-vision/LLMEnv/releases). Both install
into `/usr/local`. The packages are not signed with an Apple Developer ID, so macOS asks for confirmation when you open them (right-click, Open).

**From source** — see [Building and testing](#building-and-testing).

Check the installation at any time, and update:

```sh
fastllm doctor --install     # binary, engine version and variant, Metal kernels, model descriptors
fastllm upgrade              # update fastllm and the engine (fastllm upgrade --check only looks)
fastllm version
```

## Quick start

```sh
fastllm pull mlx-community/Qwen3-8B-4bit     # download a model
fastllm run Qwen3-8B-4bit "Hello"            # one answer
fastllm chat Qwen3-8B-4bit                   # conversation

# Use a model you already have
fastllm add my-model ~/models/some-model.gguf
fastllm run my-model "Hello"

# Serve an OpenAI/Anthropic compatible API
fastllm serve --port 8080
```

A model can be named by its registry name, by its number from `fastllm list`, by a path, or by a Hugging Face id.

## Commands

| Command | What it does |
|---|---|
| `pull <org/repo>` | Download a repository into the Hugging Face cache, with resume. `--file <glob>` downloads only some files. |
| `models`, `list` | Show the models available locally, numbered. |
| `add`, `rm`, `show` | Register, remove or inspect a local model. |
| `run <model> [prompt]` | One generation, streamed token by token. For speech and OCR models the second argument is the audio or image file. |
| `chat <model>` | Multi-turn conversation. |
| `serve` | OpenAI/Anthropic compatible server (`/v1/chat/completions`, `/v1/messages`, `/v1/models`, embeddings). |
| `doctor` | Hash the cached files and report any that are not what Hugging Face promised. |
| `test <model>` | Check that a model is complete and not corrupted on disk. |
| `gmbarch <url>` | Create the architecture descriptor for a model from its Hugging Face URL, without downloading the weights. |
| `options` | List every engine option with an example (`--opt name=value`). |

Run `fastllm help` for the full list and `fastllm help <command>` for examples.

## Tested models

These models were run end to end on a Mac with Apple Silicon and 128 GB of memory. Every row is one run of `tools/bench_models.sh`:
a unique prompt of about 2000-3000 tokens for the prefill speed, then 128 generated tokens for the decode speed, default sampling.
Very large models read from disk are limited by SSD speed, and their numbers include the first access to the weights.

| Model | Format | Prefill tok/s | Decode tok/s |
|---|---|---:|---:|
| DeepSeek-R1-Distill-Qwen-32B | MLX 8 bit | 501 | 13.3 |
| DeepSeek-V2-Lite-Chat | MLX 4 bit | 1953 | 116 |
| gemma-2-2b-it | GGUF Q4_K_M | 4403 | 44 |
| gemma-3-1b-it | MLX 4 bit | 4126 | 317.6 |
| gemma-3-270m-it | GGUF Q4_K_M | 6907 | 297 |
| gemma-4-26b-a4b-it (MoE, 128 experts) | MLX 4 bit | 716 | 69.5 |
| gemma-4-31b-it | MLX 4 bit | 523 | 19.3 |
| GLM-4.5-Air | MLX 4 bit | 143 | 27.5 |
| gpt-oss-20b | MXFP4 + Q8 | 4039 | 137 |
| Granite-3.3-2B-Instruct | MLX 4 bit | 2732 | 119 |
| Kimi-Linear-48B-A3B-Instruct | MLX 4 bit | 2198 | 101 |
| LFM2-8B-A1B | MLX 4 bit | 1472 | 208 |
| LFM2.5-24B-A2B | MLX 8 bit | 547 | 63 |
| Llama-2-7B-Chat | GGUF Q4_K_M | 1499 | 9.0 |
| Llama-3.2-1B-Instruct | MLX 4 bit | 9501 | 561 |
| Mixtral-8x22B (base model) | MLX 4 bit, weights read from disk (74 GB) | 16 | 11.2 |
| OLMoE-1B-7B-Instruct | GGUF Q4_K_M | 6572 | 42 |
| Phi-3-mini-4k-instruct | MLX 4 bit | 544 | 25.6 |
| Qwen3-0.6B | MLX 4 bit | 5539 | 267 |
| Qwen3-0.6B | MLX 8 bit | 5744 | 342 |
| Qwen3-8B | MLX 4 bit | 2041 | 95 |
| Qwen3-Coder-30B-A3B-Instruct | MLX 8 bit | 435 | 67 |
| Qwen3-Coder-Next | MXFP4 | 929 | 77 |
| Qwen3.5-27B (Claude-4.6-Opus distilled) | MLX 6 bit | 525 | 18.2 |
| Qwen3.5-4B | MLX 4 bit | 3638 | 119 |
| Qwen3.6-27B | MLX 4 bit | 748 | 27.5 |
| Qwen3.6-35B-A3B (OptiQ) | MLX 4 bit | 1871 | 62 |
| Qwen3.8-27B | MLX 8 bit | 490 | 17.6 (37.8 with the DFlash2 draft model) |
| Qwen3.8-Flash-Next | GGUF Q2, 137 GB (ds4 engine) | 728 | 52 |
| Qwen3.8-Flash-Next | MLX 2 bit, weights read from disk | 62 | 18.5 |
| Qwen3.8-Flash-Next | MLX 4 bit, weights read from disk (104 GB) | 29 | 4.5 |
| SmolLM2-135M-Instruct | MLX 8 bit | 14105 | 492 |
| StableLM-2-zephyr-1.6B | MLX 4 bit | 1451 | 41.6 |
| StarCoder2-3B (base model) | MLX 4 bit | 1625 | 86.2 |
| Ternary-Bonsai-4B | MLX 2 bit | 2895 | 182 |
| TinyLlama-1.1B-Chat | GGUF Q4_K_M | 12926 | 252 |

Notes:

- **Speculative decoding.** Qwen3.8-27B uses the `Qwen3.8-27B-DFlash2` draft model automatically when it sits next to the model.
  The output text is identical; decode went from 16.5 to 37.8 tok/s on a short coding prompt.
- **Recurrent models and conversations.** Models with recurrent state (Qwen3.5, 3.6 and 3.8, Qwen3-Next, LFM2, Kimi-Linear, Mamba, Jamba)
  keep a snapshot of that state at the end of the conversation so far (just before the assistant header), so the next turn of a growing
  conversation prefills only the new tokens. Measured with a 4000-5000 token conversation: Qwen3.6-27B 7.7 s to 0.6 s for the second turn,
  Qwen3-Coder-Next 6.4 s to 0.4 s, Kimi-Linear 2.8 s to 0.25 s, with identical text. Tested on those three plus Qwen3.6-35B-A3B; LFM2, Mamba, Jamba, Falcon-H1 and the other recurrent families use the same code but have not been run. The snapshots live in memory only (at most 4, option
  `prefix_recurrent_max`) and are lost when the server stops. `tools/prefix_recurrent_check.sh` runs this check on any model. Qwen3.8-27B
  with its DFlash2 draft model does not reuse prefixes yet.
- **Short prompts.** Phi-3 decodes at 111 tok/s and StableLM-2 at 235 tok/s on a short prompt; the table shows the long-prompt speed.
  The Phi-3 prefill prompt had 4187 tokens, Mixtral's had 1752. gemma-3-270m was measured on a short prompt.

### Speech and OCR

FastLLM is a chat engine, but `run` also routes two non-chat models by the architecture stored in the GGUF file:

| Model | What it does | Result |
|---|---|---|
| PaddleOCR-VL 1.6 (GGUF + mmproj) | Reads text from images | Reads a test invoice correctly |
| Parakeet-TDT 0.6B v3 (NVIDIA, GGUF Q8) | Transcribes speech in 25 European languages | Word-for-word on the JFK sample, English and Italian correct; 216 s of audio in 7.6 s on CPU |

```sh
fastllm run parakeet.gguf speech.wav
fastllm run PaddleOCR-VL.gguf invoice.jpg    # the *-mmproj.gguf file sits next to the model
```

## Models served by the ds4 engine

The GGUF files of the [DwarfStar (ds4)](https://github.com/antirez/ds4) project run on a dedicated engine that FastLLM starts
automatically, chosen by the architecture inside the GGUF (`deepseek4`, `deepseek41`, `glm-dsa`, `glm5-next`, `qwen4exp`). The models below were tested by the ds4 project (see `QA_BEFORE_RELEASES.md` in its repository). The list comes from its model guide
and download script. These are not general GGUF files: use the vendor's downloads. Sizes are file sizes from the vendor's guide.

| Model | Variants | Notes |
|---|---|---|
| DeepSeek V4 Flash (0731) | Q2 (about 81 GiB), Q2 with Q4 on the last six expert layers, Q4, MXFP4 | Q2 is the starting point for 96-128 GB Macs. A separate DSpark support file enables speculative decoding. |
| DeepSeek V4 Flash Vision (experimental) | Q2, Q2/Q4, MXFP4, plus a vision encoder | A different checkpoint from Flash 0731; the encoder is passed with `--vision`. |
| DeepSeek V4 PRO (0813) | Q2 with imatrix, Q4 in two pieces | Needs 512 GB of memory, or SSD streaming. |
| DeepSeek V4.1 Flash | Q2 (341 GiB file), Q4 (483 GiB), MXFP4 (467 GiB), vision encoder | Includes 189 GiB of Engram tables read straight from disk, so keep the file on a fast SSD. Not compatible with V4 Flash files. |
| GLM 5.2 | IQ2_XXS, Q2_K, Q4_K, and an 11-shard Q4_K_XL | Much larger than GLM 5.3 Flash. |
| GLM 5.3 | Q2 (about 197 GiB) | Needs a large machine or SSD streaming. |
| GLM 5.3 Flash | Q2 (about 90 GiB), Q4 (about 178 GiB), vision encoder | Q2 fits a 128 GB Mac. A FP8 package (305 GiB) exists, but the vendor has not implemented inference for it. |
| Qwen3.8 Flash Next | Q2 (137.10 GiB, tested), Q4_K (165.11 GiB), Q8_0 vision encoder | Runs on 64 GB Macs with a short context, because 95 GiB of its tables stay on disk. |

Memory is the real limit for all of them. Leave room for the context and the runtime, and read the vendor's guide in
`ds4/docs/MODELS.md` before downloading anything over 100 GB.

## Models that should work but were not tested

No model in this list has been run through FastLLM. The code exists and passes synthetic tests, but several architectures that looked
fine turned out to have at least one bug when first run on a real checkpoint (Phi-3, StableLM, StarCoder2, Gemma-4 MoE and others). Treat
them as unverified. A good first check is a short correct answer, the same answer with a long prompt, and a comparison with the GGUF
version of the same model.

| Format | Families |
|---|---|
| GGUF | Any architecture supported by llama.cpp: Llama 2 and 3, Mistral, Mixtral, Gemma 1 to 3, Qwen 2 to 3, Phi-2 to 4, OLMo, DeepSeek, GLM, Granite, Command-R, Falcon, StarCoder, Yi, InternLM, MiniCPM. Five models of different families were run. |
| MLX, same family as a tested model | Qwen2, Qwen2.5, Qwen3, Qwen3-MoE, Qwen3-Next, Qwen3.5-MoE, Llama 3.x, Mistral, OLMo 2 and 3, OLMoE, GLM-4.x, DeepSeek-V2 and V3, Kimi, ERNIE 4.5, GraniteMoE, LFM2, gpt-oss, dense Gemma-4 |
| MLX, in the engine but never run | Qwen2-MoE, DeepSeek-V3.x, Kimi-K2, dense GLM-4, GLM-4-MoE-Lite, Mamba, Mamba2, Falcon-Mamba, RWKV7, Falcon-H1, Jamba, GraniteMoE-hybrid, GPT-NeoX and Pythia, InternLM2, MiniCPM, Hunyuan, GPT-2, GPT-BigCode, OpenELM, Mistral3, dots1, Telechat3, Youtu, JoyAI, Lille |
| Quantization | Affine 2, 3, 4, 6 and 8 bit, MXFP4, MXFP8, AWQ, GPTQ, bitsandbytes (see `docs/quant-coverage.md`) |

## Architecture descriptors

Every model architecture has a descriptor (`.gmbarch`) in the `ABI` folder of LLMEnv. `fastllm pull` and `fastllm add` create a missing
descriptor by themselves from `config.json` and the tensor names, and `fastllm gmbarch <url>` does the same by hand, downloading only
`config.json` and the tensor index. An existing descriptor is never overwritten.

## Weights: resident or packed

By default the weights of an MLX model are loaded into memory. For a model larger than 55% of the physical memory, FastLLM switches by
itself to the `packed` backend, which reads the weights from the model files on demand. This is what lets the 74 GB Mixtral and the
104 GB Qwen3.8-Flash-Next 4 bit run on a 128 GB Mac. `--opt weights_backend=packed` forces it.

Packed is slower, mostly in prefill. Measured with `tools/packed_vs_resident.sh` on models that use the packed kernels:

| Model | Prefill tok/s, resident / packed | Decode tok/s, resident / packed |
|---|---:|---:|
| Qwen3.6-27B (4 bit) | 824 / 200 | 29.9 / 27.0 |
| Qwen3.6-35B-A3B (4 bit) | 1615 / 1441 | 67.6 / 67.2 |

The packed kernels cover only some architectures today (those with GatedDeltaNet layers); other models fall back to the normal path
without a warning. The script also checks that both backends give the same text: small differences after a few dozen tokens are expected,
a difference at the very start is a bug.

## Building and testing

From source you need [LLMEnv](https://github.com/c-vision/LLMEnv) checked out next to this directory (`../LLMEnv`): the build compiles the engine together with FastLLM.
A development build runs from the build folder, where the engine libraries sit next to the binary.

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
ctest --test-dir build-release --output-on-failure
```

The default deployment target is macOS 26.2, which builds the NAX kernels. For a binary that also runs on older systems, configure with
`-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0`. `tools/make_macos_package.sh` makes the release packages from such a build, and `tools/install.sh` is the installer script.

The test suites do not use the network. Useful tools in `tools/`:

| Tool | Purpose |
|---|---|
| `bench_models.sh` | Prefill and decode speed of one or more models |
| `packed_vs_resident.sh` | Compare the packed and resident backends: same text, and how much slower |
| `fastllm_eval.c` | Capability evaluation of a model |
| `mut_prove.c` | Mutation check: shows which tests turn red when the code is broken |
| `hf_probe.c`, `hf_fetch_probe.c`, `hf_manifest_probe.c` | Inspect the Hugging Face cache and downloads |

More files: `CLI.md` (command reference), `MODELS.md` (model notes).

## License

See `LICENSE`.
