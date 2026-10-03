#!/usr/bin/env bash
# bundle.sh — B5: costruisce un bundle RILOCABILE (distribuibile) dell'engine.
#
# Problema: il binario ha rpath ASSOLUTI di build -> copiato altrove non parte.
# Soluzione: copia eseguibile + dylib (motore/motori/tokenizer/codec) + kernel Metal in una
# cartella, e riscrive TUTTI i riferimenti @rpath/<lib> in @loader_path/<lib> (relativi al
# binario) con `install_name_tool`. Risultato: la cartella si puo' spostare/copiare altrove
# e `fastllm` parte standalone (self-check incluso).
#
# Uso: tools/bundle.sh [build-dir] [out-dir]     (default: build-release  dist)
set -euo pipefail
cd "$(dirname "$0")/.."
B="${1:-build-release}"
D="${2:-dist}"
[ -f "$B/fastllm" ] || { echo "bundle: $B/fastllm non trovato (build prima)"; exit 2; }

rm -rf "$D"; mkdir -p "$D"

# 1) Copia eseguibile + tutte le dylib NON-_deps + i kernel metallib
cp "$B/fastllm" "$D/"
find "$B" -name '*.dylib' -not -path '*_deps*' -exec cp {} "$D/" \;
find "$B" -maxdepth 2 -name '*.metallib' -not -path '*_deps*' -exec cp {} "$D/" \; 2>/dev/null || true
# MLX cerca il metallib 'colocato' col nome <lib>.metallib / default.metallib accanto al binario
for m in "$D"/mlx.metallib "$D"/mpp_q4.metallib; do [ -e "$m" ] && cp "$m" "$D/default.metallib"; done

# 2) Riscrittura rpath -> @loader_path (idempotente)
fix() {
  local x="$1" lib
  install_name_tool -id "@loader_path/$(basename "$x")" "$x" 2>/dev/null || true
  otool -L "$x" | awk '/@rpath\//{print $1}' | sort -u | while read -r lib; do
    install_name_tool -change "$lib" "@loader_path/$(basename "$lib")" "$x" 2>/dev/null || true
  done
  install_name_tool -add_rpath "@loader_path" "$x" 2>/dev/null || true
}
for f in "$D/fastllm" "$D"/*.dylib; do [ -e "$f" ] && fix "$f"; done

# 3) Self-check: il bundle parte standalone? (prova da una dir DIVERSA per evitare l'rpath di build)
echo "== bundle: $D ($(ls "$D" | wc -l | tr -d ' ') file) =="
tmp="$(mktemp -d)"; cp -R "$D" "$tmp/b"; \
  ( cd "$tmp" && ./b/fastllm list >/dev/null 2>&1 ) \
  && echo "OK: il bundle parte da una posizione arbitraria (rilocabile)" \
  || { echo "FAIL: il bundle non parte fuori dal build"; ( cd "$tmp" && ./b/fastllm list 2>&1 | head -3 ); rm -rf "$tmp"; exit 1; }
rm -rf "$tmp"
