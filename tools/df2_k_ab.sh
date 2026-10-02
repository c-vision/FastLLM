#!/usr/bin/env bash
# df2_k_ab.sh — A/B del K ADATTIVO DFlash2 + wall-time (SOLO in finestra idle).
#
# Default (golden reference): round 0 -> block 8 (K=7); round 1+ -> block 4 (K=3).
# Baseline A/B: `--opt df2_k=7` = K=7 SEMPRE (comportamento vecchio).
#
# Deterministico (valido sempre): il K totale dei draft (DF2_STATS).
# NON deterministico (SOLO idle): il wall-time per-fase (DF2_TIME) -> ~1.8x meno verify+drafter atteso.
#
# Uso: tools/df2_k_ab.sh
set -u
BIN="$(dirname "$0")/../build-release/fastllm"
P="The capital of France is"
LOAD=$(sysctl -n vm.loadavg 2>/dev/null | awk '{print $2}')
echo "load average (1m) = ${LOAD:-?}  (se > ~2: i TEMPI non sono affidabili, solo DF2_STATS vale)"
echo

run() {
  local label="$1"; shift
  echo "== $label =="
  FASTLLM_NO_OFFLOAD_PROMPT=1 DF2_STATS=1 DF2_TIME=1 "$BIN" run 4 "$P" --max-tokens 48 --opt temp=0 "$@" 2>&1 \
    | grep -aE "df2stats total|df2time\] rounds" | tail -2
  echo
}
run "ADATTIVO (default golden: K=7 poi K=3)"
run "K=7 SEMPRE (baseline vecchio)" --opt df2_k=7

echo "Atteso (idle): il K totale adattivo ~ meta' del K=7; il round ~200 ms vs ~285 (verify+drafter ~1.8x meno)."
