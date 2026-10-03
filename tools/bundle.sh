#!/usr/bin/env bash
# bundle.sh — B5: bundle RILOCABILE + UPDATE ATOMICO + ROLLBACK dell'engine.
#
# Struttura: <root>/rel-<data>-<commit>/ contiene il bundle; <root>/current e' un symlink alla
# versione attiva; <root>/prev alla precedente. Il "deploy" e' lo swap ATOMICO di `current` (via
# rename di un symlink temporaneo); il rollback rimette `prev` in `current`.
#
# Perche' e' necessario: il binario ha rpath ASSOLUTI di build; copiato altrove non parte. Il bundle
# riscrive tutti i @rpath/<lib> in @loader_path/<lib> (install_name_tool) e mette accanto il metallib
# MLX CORRETTO (il metallib del build FastLLM puo' essere incompleto: senza kernel `steel` bq64).
#
# Uso:
#   tools/bundle.sh                       # crea una nuova release + la attiva (atomic)
#   tools/bundle.sh --rollback            # torna alla release precedente (atomic)
#   tools/bundle.sh [build-dir] [root]    # build-dir default build-release, root default dist
set -euo pipefail
cd "$(dirname "$0")/.."

if [ "${1:-}" = "--rollback" ]; then
  ROOT="${2:-dist}"
  [ -L "$ROOT/prev" ] || { echo "rollback: nessuna versione precedente in $ROOT/prev"; exit 2; }
  prev="$(readlink "$ROOT/prev")"
  [ -d "$ROOT/$prev" ] || { echo "rollback: $ROOT/$prev non esiste"; exit 2; }
  cur="$(readlink "$ROOT/current" 2>/dev/null || echo '')"
  rm -f "$ROOT/current"; ln -sfn "$prev" "$ROOT/current"   # current -> prev
  [ -n "$cur" ] && ln -sfn "$cur" "$ROOT/prev"
  echo "rollback: current -> $prev"
  exit 0
fi

B="${1:-build-release}"
ROOT="${2:-dist}"
[ -f "$B/fastllm" ] || { echo "bundle: $B/fastllm non trovato (build prima)"; exit 2; }

VER="rel-$(date +%Y%m%d-%H%M%S)-$(git rev-parse --short HEAD 2>/dev/null || echo nogit)"
D="$ROOT/$VER"
mkdir -p "$D"

# 1) copia eseguibile + dylib (motore/motori/tokenizer/codec) + metallib
cp "$B/fastllm" "$D/"
find "$B" -name '*.dylib' -not -path '*_deps*' -exec cp {} "$D/" \;
find "$B" -maxdepth 2 -name '*.metallib' -not -path '*_deps*' -exec cp {} "$D/" \; 2>/dev/null || true

# metallib MLX COMPLETO (kernel steel bq64). Il build lo genera ORA colocated in $B
# (fix MLX_METAL_PATH in CMakeLists.txt): se quello copiato ha bq64, è già quello giusto.
# Altrimenti override: prima MLX_METALLIB_SRC, poi il dist LLMEnv (fallback storico).
has_bq64() { [ -f "$1" ] && [ "$(strings "$1" 2>/dev/null | grep -c 'steel_attention_.*bq64')" -gt 0 ]; }
if ! has_bq64 "$D/mlx.metallib"; then
  for cand in "${MLX_METALLIB_SRC:-}" "$(dirname "$0")/../../LLMEnv/build/dist/osx-arm64/mlx.metallib"; do
    if [ -n "$cand" ] && has_bq64 "$cand"; then cp "$cand" "$D/mlx.metallib"; break; fi
  done
fi
for m in "$D"/mlx.metallib "$D"/mpp_q4.metallib; do [ -e "$m" ] && cp "$m" "$D/default.metallib"; done

# 2) rpath relativo (@rpath/<lib> -> @loader_path/<lib>)
fix() {
  local x="$1" lib
  install_name_tool -id "@loader_path/$(basename "$x")" "$x" 2>/dev/null || true
  otool -L "$x" | awk '/@rpath\//{print $1}' | sort -u | while read -r lib; do
    install_name_tool -change "$lib" "@loader_path/$(basename "$lib")" "$x" 2>/dev/null || true
  done
  install_name_tool -add_rpath "@loader_path" "$x" 2>/dev/null || true
}
for f in "$D/fastllm" "$D"/*.dylib; do [ -e "$f" ] && fix "$f"; done

# 3) self-check dal bundle (posizione arbitraria)
tmp="$(mktemp -d)"; cp -R "$D" "$tmp/v"
( cd "$tmp" && ./v/fastllm run 1 "The capital of France is" --max-tokens 4 --opt temp=0 >/dev/null 2>&1 ) \
  || { echo "bundle: self-check FALLITO (run)"; ( cd "$tmp" && ./v/fastllm run 1 "x" --max-tokens 2 --opt temp=0 2>&1 | head -3 ); rm -rf "$tmp"; rm -rf "$D"; exit 1; }
rm -rf "$tmp"

# 4) attivazione ATOMICA: prev = current attuale; current -> nuova versione
[ -L "$ROOT/current" ] && ln -sfn "$(readlink "$ROOT/current")" "$ROOT/prev"
[ -L "$ROOT/current" ] && rm -f "$ROOT/current"; ln -sfn "$VER" "$ROOT/current"
echo "bundle: $D attivato (current -> $VER); prev=$(readlink "$ROOT/prev" 2>/dev/null || echo nessuno)"
