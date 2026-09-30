#!/usr/bin/env bash
# ============================================================================
# fastllm_smoke_models.sh - smoke del motore FastLLM su TUTTI i modelli "chat"
# del registro. NESSUNA comparazione/performance: solo load+generate OK.
# NON-interattivo: FASTLLM_NO_OFFLOAD_PROMPT=1 (niente [Y/n] sui modelli > RAM).
# Uso: tools/fastllm_smoke_models.sh [filtro]
# ============================================================================
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
BIN="$ROOT/build-release/fastllm"
export FASTLLM_NO_OFFLOAD_PROMPT=1
FILTER="${1:-}"
ok=0; fail=0
echo "=== fastllm smoke: modelli chat del registro ==="
"$BIN" list 2>/dev/null | while read -r idx name typ rest; do
  [ "$typ" = "chat" ] || continue
  if [ -n "$FILTER" ]; then case "$name" in *"$FILTER"*) ;; *) continue;; esac; fi
  t0=$(date +%s)
  out="$("$BIN" run "$idx" "Ciao, presentati in una frase." --max-tokens 16 2>&1)"
  rc=$?
  dt=$(( $(date +%s) - t0 ))
  gen="$(printf '%s\n' "$out" | grep -av -e '^\[' -e '^[[:space:]]*$' | tail -1 | cut -b 1-90)"
  if [ $rc -eq 0 ] && [ -n "$gen" ]; then
    echo "OK   #$idx $name  (${dt}s)  [$gen]"
  else
    why="$(printf '%s\n' "$out" | grep -ai -e 'error' -e 'fallito' -e 'non ' -e 'impossibile' -e 'unsupported' -e 'unknown' -e 'abort' -e 'exception' -e 'fatal' | tail -1 | cut -b 1-150)"
    echo "FAIL #$idx $name  (${dt}s)  [${why:-rc=$rc}]"
  fi
done
