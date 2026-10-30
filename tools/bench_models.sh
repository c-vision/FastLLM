#!/usr/bin/env bash
# bench_models.sh — misura prefill e decode di FastLLM sui modelli indicati (una riga TSV per modello).
#
# Uso: tools/bench_models.sh [--cap-mb N] [--timeout S] [--prompt-tokens N] <modello>...
#   <modello>  nome del registro, #n, percorso di una cartella MLX o di un .gguf
#
# Per ogni modello: un prompt UNICO di ~N token (default 1500; il prefix-cache persistente non puo' falsare il prefill) e
# 128 token generati. Un processo alla volta, con tetto di RSS (--cap-mb, default 100000) e timeout (default 900 s).
# Colonne: modello  prefill_tok  prefill_tok/s  decode_tok/s  esito
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build-release/fastllm"
CAP=100000; TMO=900; NTOK=1500
while [ $# -gt 0 ]; do
  case "$1" in
    --cap-mb) CAP="$2"; shift 2 ;;
    --timeout) TMO="$2"; shift 2 ;;
    --prompt-tokens) NTOK="$2"; shift 2 ;;
    *) break ;;
  esac
done
MEMCAP="${MEMCAP:-$HOME/Documents/Projects/GMBasic/tools/memcap.sh}"
export FASTLLM_NO_OFFLOAD_PROMPT=1
for m in "$@"; do
  # ~25 token per riga italiana: righe = NTOK/25; il numero iniziale rende il prompt unico a ogni esecuzione
  stamp="$(date +%s)$RANDOM"
  prompt="Riferimento $stamp. Leggi il testo e riassumilo in due frasi."$'\n'
  n=$(( NTOK / 25 ))
  for i in $(seq 1 "$n"); do
    prompt+="Riga $i: il progetto $stamp-$i descrive un motore di inferenza locale che carica modelli quantizzati e genera testo."$'\n'
  done
  out="$("$MEMCAP" "$CAP" perl -e "alarm $TMO; exec @ARGV" "$BIN" run "$m" "$prompt" --max-tokens 128 2>&1 >/dev/null | grep -a "tok/s decode")"
  if [ -n "$out" ]; then
    # [N tok · X tok/s decode · prefill P tok in T s (Y tok/s) · contesto C tok]
    dec="$(printf '%s' "$out" | sed -E 's/.* ([0-9.]+) tok\/s decode.*/\1/')"
    ptok="$(printf '%s' "$out" | sed -E 's/.*prefill ([0-9]+) tok.*/\1/')"
    pps="$(printf '%s' "$out" | sed -E 's/.*\(([0-9]+) tok\/s\).*/\1/')"
    printf '%s\t%s\t%s\t%s\tOK\n' "$(basename "$m")" "$ptok" "$pps" "$dec"
  else
    printf '%s\t-\t-\t-\tFAIL\n' "$(basename "$m")"
  fi
done
