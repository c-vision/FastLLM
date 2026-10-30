#!/usr/bin/env bash
# update_tap.sh — copia le formule Homebrew generate (fastllm.rb di FastLLM, llmenv.rb di LLMEnv) nel tap
# c-vision/homebrew-fastllm e fa il commit. NON fa il push: lo si fa dopo aver controllato `git show` nel tap.
#
# USO: tools/update_tap.sh [--tap-dir DIR]     (default: ../homebrew-fastllm, creata se manca da un clone del repo)
set -euo pipefail
cd "$(dirname "$0")/.."
TAP="../homebrew-fastllm"
while [ $# -gt 0 ]; do case "$1" in --tap-dir) TAP="$2"; shift 2 ;; *) echo "opzione sconosciuta: $1" >&2; exit 1 ;; esac; done
[ -f Formula/fastllm.rb ] && [ -f ../LLMEnv/Formula/llmenv.rb ] || { echo "mancano Formula/fastllm.rb o ../LLMEnv/Formula/llmenv.rb (esegui i make_macos_package.sh)" >&2; exit 1; }
if [ ! -d "$TAP/.git" ]; then git clone https://github.com/c-vision/homebrew-fastllm.git "$TAP"; fi
mkdir -p "$TAP/Formula"
cp Formula/fastllm.rb "$TAP/Formula/fastllm.rb"
cp ../LLMEnv/Formula/llmenv.rb "$TAP/Formula/llmenv.rb"
V="$(tr -d '[:space:]' < VERSION)"; E="$(tr -d '[:space:]' < ../LLMEnv/VERSION)"
git -C "$TAP" add Formula
if git -C "$TAP" diff --cached --quiet; then echo "tap gia' aggiornato"; exit 0; fi
git -C "$TAP" commit -q -m "fastllm $V, llmenv $E"
echo "== tap aggiornato in $TAP: fastllm $V, llmenv $E (push da fare a mano)"
