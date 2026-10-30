#!/usr/bin/env bash
# longctx_bench.sh <model#> <tokens> [max_new]
# Long-context decode bench WITH accelerations (auto-policy: KV int8 Splash, OQ_A8,
# FUSE_ROPE, GATEUP_FUSE, spec n-gram). Deterministic metrics via LLM_DISPATCH=1.
set -u
MODEL="${1:?model#}"; CTX="${2:?context tokens}"; NEW="${3:-16}"
BIN="$(dirname "$0")/../build-release/fastllm"
PROMPT_FILE="$(mktemp -t longctx.XXXXXX)"
# ~2 caratteri per token con parole esadecimali varie; seme fisso = prompt riproducibile
awk -v target="$CTX" 'BEGIN { srand(1234); n = 0; chars = "abcdefghijklmnopqrstuvwxyz0123456789";
    while (n < target * 8) { len = 6 + int(rand() * 6); w = "";
        for (i = 0; i < len; i++) w = w substr(chars, 1 + int(rand() * 36), 1);
        printf "%s%s", (n ? " " : ""), w; n += len + 1 }
    printf "\n" }' >"$PROMPT_FILE"
echo "== model=$MODEL ctx=${CTX}tok (prompt $(( $(wc -c <"$PROMPT_FILE") / 1024 ))KB) new=$NEW =="
LLM_DISPATCH=1 FASTLLM_NO_OFFLOAD_PROMPT=1 "$BIN" run "$MODEL" --max-tokens "$NEW" --opt temp=0 \
  <"$PROMPT_FILE" 2>&1 | grep -aiE "kvquant auto|auto-policy|p4-plan|nodes/token|MB_in/token|floor_us|spec_tok|kv/token|PESI|ALTRO|prompt|context|error" | tail -40
rm -f "$PROMPT_FILE"
