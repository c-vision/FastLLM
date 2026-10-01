#!/usr/bin/env bash
# longctx_bench.sh <model#> <tokens> [max_new]
# Long-context decode bench WITH accelerations (auto-policy: KV int8 Splash, OQ_A8,
# FUSE_ROPE, GATEUP_FUSE, spec n-gram). Deterministic metrics via LLM_DISPATCH=1.
set -u
MODEL="${1:?model#}"; CTX="${2:?context tokens}"; NEW="${3:-16}"
BIN="$(dirname "$0")/../build-release/fastllm"
PY="$(command -v python3)"
PROMPT_FILE="$(mktemp -t longctx.XXXXXX)"
"$PY" - "$CTX" >"$PROMPT_FILE" <<'PY'
import sys, random
random.seed(1234)
target = int(sys.argv[1])              # desired ~tokens; ~2 chars/token for varied hex words
words = []
n = 0
while n < target * 8:
    w = "".join(random.choice("abcdefghijklmnopqrstuvwxyz0123456789") for _ in range(random.randint(6, 11)))
    words.append(w); n += len(w) + 1
sys.stdout.write(" ".join(words) + "\n")
PY
echo "== model=$MODEL ctx=${CTX}tok (prompt $(( $(wc -c <"$PROMPT_FILE") / 1024 ))KB) new=$NEW =="
LLM_DISPATCH=1 FASTLLM_NO_OFFLOAD_PROMPT=1 "$BIN" run "$MODEL" --max-tokens "$NEW" --opt temp=0 \
  <"$PROMPT_FILE" 2>&1 | grep -aiE "kvquant auto|auto-policy|p4-plan|nodes/token|MB_in/token|floor_us|spec_tok|kv/token|PESI|ALTRO|prompt|context|error" | tail -40
rm -f "$PROMPT_FILE"
