#!/usr/bin/env bash
# fuse_pointwise_ab.sh — A/B della fusione POINTWISE via compiled_decode WHOLE-STEP.
#
# Tesi (tracker speed-gap §5-ter): la fusione elementwise del reference mlx_lm si ottiene con la
# REGIONE WHOLE-STEP compilata (UN apply/token), NON con GMB_FUSE_GEGLU (apply eager per-layer =
# 60 barriere/token, 14x iokit, falsificato). `is_fusable` fonde solo unary/binary/ternary/broadcast
# -> matmul/softmax restano bit-identiche -> IN PRINCIPIO parity-safe.
#
# NON deterministico: il payoff e' traffico GPU / tok/s -> SOLO finestra idle.
# Deterministico: la PARITA' (ids GEN_IDS) compiled+FUSE vs eager.
#
# Uso: tools/fuse_pointwise_ab.sh [model#]   (default 4 = qwen38/qwen3_5)
set -u
BIN="$(dirname "$0")/../build-release/fastllm"
M="${1:-4}"
P="Write three sentences about the sea."
LOAD=$(sysctl -n vm.loadavg 2>/dev/null | awk '{print $2}')
echo "load average (1m) = ${LOAD:-?}  (tok/s valido solo se < ~2; la parita' vale sempre)"
echo

run() {
  local label="$1"; shift
  echo "== $label =="
  ( export FASTLLM_NO_OFFLOAD_PROMPT=1; for kv in "$@"; do export "$kv"; done; \
    "$BIN" run "$M" "$P" --max-tokens 64 --opt temp=0 2>&1 \
      | grep -aoE "\[[0-9]+ tok · [0-9.]+ tok/s · contesto [0-9]+ tok\]" | tail -1 )
  echo
}
# Gate REALE (llm_mlx_session_generate.c:640): LLM_COMPILE (master) && LLM_FIXKV (cache statiche)
# && arch_supported (LLM_COMPILE_QWEN35). Senza questi il path compiled NON si attiva (resta eager).
run "eager (baseline)"
run "compiled qwen3_5 (NO_FUSE = parita')" LLM_COMPILE=1 LLM_FIXKV=1 LLM_COMPILE_QWEN35=1
run "compiled+FUSE (fonde i pointwise = il payoff)" LLM_COMPILE=1 LLM_FIXKV=1 LLM_COMPILE_QWEN35=1 LLM_COMPILE_FUSE=1

echo "== PARITA' (compiled+FUSE vs eager) =="
eag=$(FASTLLM_NO_OFFLOAD_PROMPT=1 GEN_IDS=1 "$BIN" run "$M" "$P" --max-tokens 40 --opt temp=0 2>&1 | grep -a genid | sed 's/\[genid\] //' | tr '\n' ' ')
fuse=$(LLM_COMPILE=1 LLM_FIXKV=1 LLM_COMPILE_QWEN35=1 LLM_COMPILE_FUSE=1 FASTLLM_NO_OFFLOAD_PROMPT=1 GEN_IDS=1 "$BIN" run "$M" "$P" --max-tokens 40 --opt temp=0 2>&1 | grep -a genid | sed 's/\[genid\] //' | tr '\n' ' ')
[ "$(echo $eag)" = "$(echo $fuse)" ] && echo "  PARITA' OK" || echo "  DIVERGE (~1e-3 atteso: muro FUSE) -> isolare l'astype nella catena"
echo
echo "Se i tok/s non migliorano: payoff nullo (atenersi al tracker §V: build 2-3%/eval 97%)."
