#!/usr/bin/env bash
# fuse_pointwise_ab.sh — A/B della fusione POINTWISE via compiled_decode WHOLE-STEP.
#
# Tesi (tracker speed-gap §5-ter): la fusione elementwise del reference mlx_lm si ottiene con la
# REGIONE WHOLE-STEP compilata (UN apply/token). GMB_FUSE_GEGLU (apply eager per-layer) e' FALSO
# (14x iokit). `is_fusable` fonde solo unary/binary/ternary/broadcast -> matmul/softmax bit-identiche.
#
# Gate REALE (llm_mlx_session_generate.c:640): LLM_COMPILE (master) && LLM_FIXKV (cache statiche)
# && arch_supported (QLAMA: LLM_COMPILE_DENSE; qwen3_5: LLM_COMPILE_QWEN35 + cache statiche GDN = M1).
# NB: sul qwen3_5 il compiled NON aggancia (static caches mixtral/llama-only) -> serve M1.
#
# Payoff/tok/s = SOLO finestra idle. Parita' (ids) = SEMPRE valida.
# Uso: tools/fuse_pointwise_ab.sh [model#] [arch]   arch: llama|qwen35 (default: dedotto dal modello)
set -u
BIN="$(dirname "$0")/../build-release/fastllm"
M="${1:-1}"
ARCH="${2:-}"
[ -z "$ARCH" ] && case "$M" in 1) ARCH=llama;; 4) ARCH=qwen35;; *) ARCH=llama;; esac
case "$ARCH" in
  llama)  ARCHENV="LLM_COMPILE_DENSE=1"  ;;
  qwen35) ARCHENV="LLM_COMPILE_QWEN35=1" ;;
  *) echo "arch sconosciuto: $ARCH (llama|qwen35)"; exit 2 ;;
esac
P="Write three sentences about the sea."
LOAD=$(sysctl -n vm.loadavg 2>/dev/null | awk '{print $2}')
echo "model=$M arch=$ARCH load(1m)=${LOAD:-?}  (tok/s valido solo se < ~2; parita' sempre)"
echo

run() {
  local label="$1"; shift
  echo "== $label =="
  ( export FASTLLM_NO_OFFLOAD_PROMPT=1; for kv in "$@"; do export "$kv"; done; \
    "$BIN" run "$M" "$P" --max-tokens 64 --opt temp=0 2>&1 \
      | grep -aoE "\[[0-9]+ tok · [0-9.]+ tok/s · contesto [0-9]+ tok\]" | tail -1 )
  echo
}
run "eager (baseline)"
run "compiled (NO_FUSE = parita')" LLM_COMPILE=1 LLM_FIXKV=1 $ARCHENV
run "compiled+FUSE (fonde i pointwise = il payoff)" LLM_COMPILE=1 LLM_FIXKV=1 $ARCHENV LLM_COMPILE_FUSE=1

echo "== PARITA' (compiled+FUSE vs eager) =="
norm(){ tr '\n' ' ' | tr -s ' '; }
eag=$(FASTLLM_NO_OFFLOAD_PROMPT=1 GEN_IDS=1 "$BIN" run "$M" "$P" --max-tokens 40 --opt temp=0 2>&1 | grep -a genid | sed 's/\[genid\] //' | norm)
fuse=$(env LLM_COMPILE=1 LLM_FIXKV=1 $ARCHENV LLM_COMPILE_FUSE=1 FASTLLM_NO_OFFLOAD_PROMPT=1 GEN_IDS=1 "$BIN" run "$M" "$P" --max-tokens 40 --opt temp=0 2>&1 | grep -a genid | sed 's/\[genid\] //' | norm)
[ "$(echo $eag)" = "$(echo $fuse)" ] && echo "  PARITA' OK" || echo "  DIVERGE -> muro FUSE, isolare l'astype nella catena"
echo
echo "Se i tok/s non migliorano (in idle): payoff nullo."
