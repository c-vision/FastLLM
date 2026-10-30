#!/usr/bin/env bash
# make_macos_package.sh — impacchetta FastLLM per macOS (Apple Silicon) da una build di rilascio.
#
# Il pacchetto contiene SOLO fastllm (binario + sorgenti Metal di ds4 + documenti): il motore LLMEnv non e' dentro,
# lo installa install.sh (o Homebrew, o il .pkg di LLMEnv) e fastllm lo trova con un rpath relativo.
#   - fastllm-<ver>-macos-arm64.tar.gz   (bin/fastllm, share/fastllm/{REQUIRES,ds4/metal}, share/doc/fastllm)
#   - fastllm-<ver>-macos-arm64.pkg      (installa in /usr/local; richiede LLMEnv in /usr/local/lib/llmenv)
#   - SHA256SUMS-<ver>, install.sh, LATEST
#   - Formula/fastllm.rb                 (Homebrew; la copia nel tap la fa tools/update_tap.sh)
#
# USO: tools/make_macos_package.sh --build-dir DIR [--version X.Y.Z] [--out DIR] [--repo owner/name]
#   --build-dir  build di rilascio configurata con -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 (contiene fastllm)
#
# rpath del binario, in quest'ordine: @executable_path/../lib/llmenv (install.sh e .pkg), poi i percorsi assoluti
# /opt/homebrew/opt/llmenv/lib/llmenv e /usr/local/lib/llmenv. Quelli assoluti servono a Homebrew, che collega il binario
# da una cartella diversa dalla Cellar.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"

VERSION="$(tr -d '[:space:]' < VERSION)"; REPO="c-vision/FastLLM"; OUT="$ROOT/build/package"; BUILD=""
while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) BUILD="$2"; shift 2 ;;
        --version) VERSION="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --repo) REPO="$2"; shift 2 ;;
        *) echo "opzione sconosciuta: $1" >&2; exit 1 ;;
    esac
done
[ "$(uname -s)" = "Darwin" ] && [ "$(uname -m)" = "arm64" ] || { echo "solo macOS su Apple Silicon" >&2; exit 1; }
[ -n "$BUILD" ] && [ -x "$BUILD/fastllm" ] || { echo "serve --build-dir con dentro fastllm (build di rilascio)" >&2; exit 1; }
LLMENV_VER="$(tr -d '[:space:]' < ../LLMEnv/VERSION)"

MINOS="$(otool -l "$BUILD/fastllm" | awk '/LC_BUILD_VERSION/{r=1} r&&/minos/{print $2; exit}')"
case "$MINOS" in 14.*|15.*) ;; *) echo "ERRORE: fastllm ha minos $MINOS: ricostruire con -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0" >&2; exit 1 ;; esac

export COPYFILE_DISABLE=1
NAME="fastllm-$VERSION-macos-arm64"
STAGE="$OUT/stage/$NAME"
mkdir -p "$OUT"
rm -rf "$OUT/stage" "$OUT/$NAME.tar.gz" "$OUT/$NAME.pkg"
mkdir -p "$STAGE/bin" "$STAGE/share/fastllm/ds4" "$STAGE/share/doc/fastllm"

cp "$BUILD/fastllm" "$STAGE/bin/fastllm"
# rpath: via quelli assoluti di build, dentro quelli di rilascio
while IFS= read -r rp; do install_name_tool -delete_rpath "$rp" "$STAGE/bin/fastllm" 2>/dev/null || true
done < <(otool -l "$STAGE/bin/fastllm" | awk '/LC_RPATH/{r=1} r&&/ path /{print $2; r=0}')
for rp in "@executable_path/../lib/llmenv" "/opt/homebrew/opt/llmenv/lib/llmenv" "/usr/local/lib/llmenv"; do
    install_name_tool -add_rpath "$rp" "$STAGE/bin/fastllm"
done
codesign --force -s - "$STAGE/bin/fastllm" >/dev/null 2>&1
if otool -l "$STAGE/bin/fastllm" | awk '/LC_RPATH/{r=1} r&&/ path /{print $2; r=0}' | grep -q '/Users/'; then
    echo "ERRORE: rpath di build residui" >&2; exit 1
fi
# dipendenze: solo librerie di sistema e dylib di LLMEnv (@rpath), nient'altro
if otool -L "$STAGE/bin/fastllm" | tail -n +2 | awk '{print $1}' | grep -v '^@rpath/libllm' | grep -v '^/usr/lib/\|^/System/'; then
    echo "ERRORE: dipendenze inattese (vedi sopra)" >&2; exit 1
fi

# kernel Metal di ds4: il motore li legge come file (ds4_resource_dir in LLMEnv/LLMEngine/ds4/bridge)
cp -R ../LLMEnv/LLMEngine/ds4/vendor/metal "$STAGE/share/fastllm/ds4/metal"
echo "llmenv=$LLMENV_VER" > "$STAGE/share/fastllm/REQUIRES"
cp -f LICENSE README.md CLI.md MODELS.md "$STAGE/share/doc/fastllm/"
echo "$VERSION" > "$STAGE/share/doc/fastllm/VERSION"
xattr -cr "$STAGE" 2>/dev/null || true

tar -C "$OUT/stage" -czf "$OUT/$NAME.tar.gz" "$NAME"
pkgbuild --root "$STAGE" --install-location /usr/local --identifier "io.github.c-vision.fastllm" --version "$VERSION" \
    "$OUT/$NAME.pkg" >/dev/null
cp -f tools/install.sh "$OUT/install.sh"; chmod +x "$OUT/install.sh"
echo "$VERSION" > "$OUT/LATEST"
( cd "$OUT" && shasum -a 256 "$NAME.tar.gz" "$NAME.pkg" install.sh > "SHA256SUMS-$VERSION" )
SHA="$(shasum -a 256 "$OUT/$NAME.tar.gz" | awk '{print $1}')"

mkdir -p "$ROOT/Formula"
cat > "$ROOT/Formula/fastllm.rb" <<RB
class Fastllm < Formula
  desc "Run MLX and GGUF language models locally on Apple Silicon, with an OpenAI-compatible server"
  homepage "https://github.com/$REPO"
  url "https://github.com/$REPO/releases/download/v$VERSION/$NAME.tar.gz"
  sha256 "$SHA"
  version "$VERSION"
  license "Apache-2.0"

  depends_on arch: :arm64
  depends_on :macos
  # The inference engine is a separate formula, so several programs can share one copy.
  depends_on "llmenv"

  def install
    bin.install "bin/fastllm"
    pkgshare.install "share/fastllm/ds4", "share/fastllm/REQUIRES"
    doc.install Dir["share/doc/fastllm/*"]
    # no link to the engine is needed: the binary has /opt/homebrew/opt/llmenv/lib/llmenv in its rpath
  end

  test do
    assert_match version.to_s, shell_output("#{bin}/fastllm version")
    assert_match "installation OK", shell_output("#{bin}/fastllm doctor --install")
  end
end
RB
echo "== pacchetto: $OUT"; ls -la "$OUT"/*.tar.gz "$OUT"/*.pkg; cat "$OUT/SHA256SUMS-$VERSION"
