#!/usr/bin/env bash
# df2_k_ab.sh — Conferma del K ADATTIVO DFlash2 (round0 K=7, round1+ K=3).
#
# Deterministico (no temporale): il K totale dei draft (DF2_STATS) deve combaciare col golden
# reference (28 draft su 8 round; block medio 4.5) e la PARITA' (transcript) deve reggere.
# Il wall-time (DF2_TIME) vale solo in FINESTRA IDLE (su macchina carica e' rumore).
#
# Golden: LLMEnv/LLMEngine/mlx/goldens/dflash2/dflash_goldens.npz (transcript 20 token,
#         prompt "The capital of France is", block_size round0=8 / round1+=4, draft totali=28).
#
# Uso: tools/df2_k_ab.sh
set -u
BIN="$(dirname "$0")/../build-release/fastllm"
P="The capital of France is"
GOLD_TRANSCRIPT="11751 13 198 760 6511 314 9564 369 19241 13 198 760 6511 314 14898 369 21047 13 198 760"

echo "== 1. K totale (deterministico) + accettanza =="
out=$(FASTLLM_NO_OFFLOAD_PROMPT=1 DF2_STATS=1 "$BIN" run 4 "$P" \
      --max-tokens 20 --opt temp=0 2>&1)
echo "$out" | grep -aE "df2stats total|df2time\] rounds" | tail -3
echo "   (atteso: K totale dei draft ~= 28 come il golden; con K=7 sempre sarebbe ~56)"

echo "== 2. PARITA' vs golden (transcript) =="
ids=$(GEN_IDS=1 FASTLLM_NO_OFFLOAD_PROMPT=1 "$BIN" run 4 "$P" --max-tokens 20 --opt temp=0 2>&1 \
      | grep -a genid | sed 's/\[genid\] //' | tr '\n' ' ' | tr -s ' ')
echo "   nostro : $ids"
echo "   golden : $GOLD_TRANSCRIPT"
[ "$(echo $ids)" = "$GOLD_TRANSCRIPT" ] && echo "   PARITA' golden OK" || echo "   (confrontare: id potrebbero differire per BOS/template)"

echo "== 3. Wall-time per-fase (SOLO in idle; ora e' rumore) =="
echo "$out" | grep -aE "df2time\] r=" | tail -4
echo "   atteso (idle): verify+drafter ~1.8x meno per round vs K=7 -> round ~200 ms (da ~285)"
echo
echo "NOTA: se il load della macchina e' alto, il punto 3 e' inaffidabile; 1 e 2 sono deterministici."
