#!/usr/bin/env bash
# packed_vs_resident.sh — regressione e costo del backend `packed` (pesi letti dai file a richiesta) contro quello residente.
#
# Uso: tools/packed_vs_resident.sh [--cap-mb N] [--prompt-tokens N] [--max-tokens N] <modello>...
#   <modello>  nome del registro, #n, percorso di una cartella MLX
#
# Per ogni modello esegue lo STESSO prompt due volte (greedy: temp=0, niente prefix-cache, niente speculazione) e confronta
# il testo generato byte per byte. Stampa una riga TSV:
#   modello  esito  prefill_res  prefill_pack  decode_res  decode_pack  rallentamento_prefill  rallentamento_decode
# esito = IDENT se i testi coincidono byte per byte, DRIFT@N se divergono dopo N byte (N >= --min-prefix, default 40: normale con kernel
# diversi), DIFF se divergono prima (difetto), FAIL se una delle due esecuzioni non produce statistiche.
# ATTENZIONE: `packed` oggi e' realizzato solo per i layer GatedDeltaNet e alcune proiezioni 2/4 bit; per le altre architetture il motore
# ricade su MLX in silenzio, quindi il rallentamento misurato e' ~1.0x e non dice nulla sul costo del packed. Usare modelli GDN
# (Qwen3.5/3.6/3.8, Qwen3-Next).
# Esce con 1 se almeno un modello e' DIFF o FAIL. Un processo alla volta, con tetto di RSS (memcap) e timeout.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build-release/fastllm"
CAP=30000; NTOK=1500; GEN=64; TMO=600; MINPFX=40
while [ $# -gt 0 ]; do
  case "$1" in
    --cap-mb) CAP="$2"; shift 2 ;;
    --prompt-tokens) NTOK="$2"; shift 2 ;;
    --max-tokens) GEN="$2"; shift 2 ;;
    --timeout) TMO="$2"; shift 2 ;;
    --min-prefix) MINPFX="$2"; shift 2 ;;
    *) break ;;
  esac
done
MEMCAP="${MEMCAP:-$HOME/Documents/Projects/GMBasic/tools/memcap.sh}"
export FASTLLM_NO_OFFLOAD_PROMPT=1
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
# prompt fisso (non unico): le due esecuzioni devono vedere gli stessi token
prompt="Leggi il testo e riassumilo in due frasi."$'\n'
for i in $(seq 1 $(( NTOK / 25 ))); do
  prompt+="Riga $i: il progetto $i descrive un motore di inferenza locale che carica modelli quantizzati e genera testo."$'\n'
done
fail=0
run_one() {   # $1=modello $2=backend (resident|packed) -> $WORK/$2.out, $WORK/$2.err
  local opt=()
  [ "$2" = packed ] && opt=(--opt weights_backend=packed)
  "$MEMCAP" "$CAP" perl -e "alarm $TMO; exec @ARGV" "$BIN" --opt temp=0 --opt prefix_cache=off --opt spec_ngram=0 --opt seed=1 \
      ${opt[@]+"${opt[@]}"} run "$1" "$prompt" --max-tokens "$GEN" >"$WORK/$2.out" 2>"$WORK/$2.err"
  grep -a "tok/s decode" "$WORK/$2.err" | tail -1
}
for m in "$@"; do
  sr="$(run_one "$m" resident)"; sp="$(run_one "$m" packed)"
  name="$(basename "$m")"
  if [ -z "$sr" ] || [ -z "$sp" ]; then printf '%s\tFAIL\t-\t-\t-\t-\t-\t-\n' "$name"; fail=1; continue; fi
  dec_r="$(printf '%s' "$sr" | sed -E 's/.* ([0-9.]+) tok\/s decode.*/\1/')"
  dec_p="$(printf '%s' "$sp" | sed -E 's/.* ([0-9.]+) tok\/s decode.*/\1/')"
  pre_r="$(printf '%s' "$sr" | sed -E 's/.*\(([0-9]+) tok\/s\).*/\1/')"
  pre_p="$(printf '%s' "$sp" | sed -E 's/.*\(([0-9]+) tok\/s\).*/\1/')"
  # kernel diversi => arrotondamenti diversi: un greedy puo' divergere dopo qualche token senza che sia un difetto.
  # IDENT = byte identici; DRIFT = prefisso comune >= MINPFX byte; DIFF = diverge subito (difetto reale).
  if cmp -s "$WORK/resident.out" "$WORK/packed.out"; then esito=IDENT
  else
    pfx="$(cmp "$WORK/resident.out" "$WORK/packed.out" 2>/dev/null | sed -E 's/.*char ([0-9]+),.*/\1/')"
    if [ -n "$pfx" ] && [ "$pfx" -ge "$MINPFX" ] 2>/dev/null; then esito="DRIFT@$pfx"; else esito=DIFF; fail=1; fi
  fi
  sd="$(awk -v a="$dec_r" -v b="$dec_p" 'BEGIN{ if (b > 0) printf "%.2fx", a / b; else print "-" }')"
  spf="$(awk -v a="$pre_r" -v b="$pre_p" 'BEGIN{ if (b > 0) printf "%.2fx", a / b; else print "-" }')"
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$name" "$esito" "$pre_r" "$pre_p" "$dec_r" "$dec_p" "$spf" "$sd"
  [ "$esito" = DIFF ] && diff <(head -c 400 "$WORK/resident.out") <(head -c 400 "$WORK/packed.out") | head -6
done
exit $fail
