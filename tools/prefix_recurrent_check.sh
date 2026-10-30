#!/usr/bin/env bash
# prefix_recurrent_check.sh — il riuso del prefisso funziona sui modelli a stato ricorrente (GDN/KDA/SSM/conv) e non cambia il testo?
#
# Uso: tools/prefix_recurrent_check.sh [--cap-mb N] [--lines N] [--max-tokens N] [--port P] <percorso-modello>...
#   --lines N   righe di contesto nel system prompt (default 120, circa 3000 token): piu' righe = prefill piu' lungo = guadagno piu' evidente
#
# Per ogni modello: un server FRESCO, turno 1 (prompt lungo) e turno 2 (la conversazione cresce: il turno 1 e' un prefisso del 2) con
# FMLX_PREFIX_DEBUG; poi un SECONDO server fresco che esegue solo il turno 2 (nessun prefisso salvato = prefill completo). Si confronta
# il testo (greedy, temp 0) e si misura il tempo del turno 2 con e senza riuso.
# SERVE_OPTS="--opt prefix_cache=off" spegne la cache persistente su disco (utile per i modelli a sola attenzione, che la usano: senza, il server
# "fresco" riprende i prefissi del primo e il confronto non e' pulito).
# Riga TSV: modello  hit  token_riusati  t2_con_riuso  t2_senza  rapporto  testo
#   hit = HIT se il turno 2 ha riusato lo stato; testo = IDENT | DRIFT@N (diverge dopo N byte: normale, la tail si prefilla in chunk
#   diversi) | DIFF (diverge subito: difetto). Esce con 1 se qualche modello non e' HIT o e' DIFF.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build-release/fastllm"
CAP=100000; LINES=120; MAXTOK=64; PORT=8197; MINPFX=40
while [ $# -gt 0 ]; do
  case "$1" in
    --cap-mb) CAP="$2"; shift 2 ;;
    --lines) LINES="$2"; shift 2 ;;
    --max-tokens) MAXTOK="$2"; shift 2 ;;
    --port) PORT="$2"; shift 2 ;;
    *) break ;;
  esac
done
command -v jq >/dev/null || { echo "serve jq" >&2; exit 2; }
MEMCAP="${MEMCAP:-$HOME/Documents/Projects/GMBasic/tools/memcap.sh}"
WORK="$(mktemp -d)"; SRV=""
cleanup() { pkill -f "serve --port $PORT" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT
[ -n "${KEEP_OUT:-}" ] && trap - EXIT

SYS=""
for i in $(seq 1 "$LINES"); do SYS+="Line $i: the project $i describes a local inference engine that loads quantized models and generates text. "; done

start_server() {   # $1 = log
  curl -s -o /dev/null -m 1 "http://127.0.0.1:$PORT/health" && { echo "porta $PORT gia' occupata" >&2; return 1; }
  FMLX_PREFIX_DEBUG=1 "$MEMCAP" "$CAP" "$BIN" ${SERVE_OPTS:-} serve --port "$PORT" > "$1" 2>&1 &
  SRV=$!
  for _ in $(seq 1 60); do curl -s -o /dev/null "http://127.0.0.1:$PORT/health" && return 0; sleep 1; done
  return 1
}
stop_server() {
  # il memcap e' uno script che lancia il server come figlio: si ferma il server per porta e si attende che la porta si liberi,
  # altrimenti il "server fresco" successivo non parte e la richiesta finisce sul vecchio (che ha gli snapshot): misura falsata
  pkill -f "serve --port $PORT" 2>/dev/null; [ -n "$SRV" ] && kill "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null; SRV=""
  for _ in $(seq 1 30); do curl -s -o /dev/null -m 1 "http://127.0.0.1:$PORT/health" || return 0; sleep 1; done
  echo "il server sulla porta $PORT non si ferma" >&2; return 1
}
post() {   # $1 = json, $2 = out ; stampa i secondi
  local t0 t1
  t0=$(perl -MTime::HiRes=time -e 'printf "%.3f", time')
  curl -s -m 1500 "http://127.0.0.1:$PORT/v1/chat/completions" -H 'Content-Type: application/json' -d @"$1" > "$2"
  t1=$(perl -MTime::HiRes=time -e 'printf "%.3f", time')
  awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.2f", b-a}'
}

fail=0
for M in "$@"; do
  name="$(basename "$M")"
  mk() { jq -n --arg m "$M" --arg s "$SYS" --argjson n "$MAXTOK" --arg a "${1:-}" --arg q2 "${2:-}" '
      {model:$m, max_tokens:$n, temperature:0,
       messages: ([{role:"system",content:$s},{role:"user",content:"Summarize the lines above in one short sentence."}]
                 + (if $a != "" then [{role:"assistant",content:$a},{role:"user",content:$q2}] else [] end))}'; }
  mk > "$WORK/t1.json"
  if ! start_server "$WORK/s1.log"; then printf '%s\tFAIL\t-\t-\t-\t-\tserver\n' "$name"; fail=1; stop_server; continue; fi
  post "$WORK/t1.json" "$WORK/o1.json" >/dev/null
  A="$(jq -r '.choices[0].message.content // empty' "$WORK/o1.json")"
  if [ -z "$A" ]; then printf '%s\tFAIL\t-\t-\t-\t-\tturno1: %s\n' "$name" "$(jq -r '.error.message // "vuoto"' "$WORK/o1.json" 2>/dev/null | head -c 80)"; tail -3 "$WORK/s1.log" | sed 's/^/    server: /' >&2; fail=1; stop_server; continue; fi
  mk "$A" "Now say how many lines there were." > "$WORK/t2.json"
  t_reuse="$(post "$WORK/t2.json" "$WORK/o2_reuse.json")"
  full="$(grep -a 'prefix-auto\] recurrent' "$WORK/s1.log" | tail -1 | sed -E 's/.*full_match=([0-9]+).*/\1/')"
  hit="$(grep -a '^\[prefix\] ' "$WORK/s1.log" | tail -1 | grep -q 'HIT' && echo HIT || echo miss)"
  stop_server
  start_server "$WORK/s2.log" >/dev/null
  t_fresh="$(post "$WORK/t2.json" "$WORK/o2_fresh.json")"
  stop_server
  jq -r '.choices[0].message.content // empty' "$WORK/o2_reuse.json" > "$WORK/a.txt"
  jq -r '.choices[0].message.content // empty' "$WORK/o2_fresh.json" > "$WORK/b.txt"
  [ -n "${SHOW_TEXT:-}" ] && { echo "--- con riuso:"; cat "$WORK/a.txt"; echo "--- senza:"; cat "$WORK/b.txt"; } >&2
  if cmp -s "$WORK/a.txt" "$WORK/b.txt"; then txt=IDENT
  else
    pfx="$(cmp "$WORK/a.txt" "$WORK/b.txt" 2>/dev/null | sed -E 's/.*char ([0-9]+),.*/\1/')"
    if [ -n "$pfx" ] && [ "$pfx" -ge "$MINPFX" ] 2>/dev/null; then txt="DRIFT@$pfx"; else txt=DIFF; fi
  fi
  [ ! -s "$WORK/a.txt" ] && txt=VUOTO
  ratio="$(awk -v a="$t_fresh" -v b="$t_reuse" 'BEGIN{ if (b > 0) printf "%.1fx", a / b; else print "-" }')"
  printf '%s\t%s\t%s\t%ss\t%ss\t%s\t%s\n' "$name" "$hit" "${full:-0}" "$t_reuse" "$t_fresh" "$ratio" "$txt"
  { [ "$hit" != HIT ] || [ "$txt" = DIFF ] || [ "$txt" = VUOTO ]; } && fail=1
done
exit $fail
