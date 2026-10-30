#!/usr/bin/env bash
# install.sh — installs FastLLM (and, if missing, the LLMEnv engine) on macOS with Apple Silicon. IDEMPOTENT.
#
#   curl -fsSL https://github.com/c-vision/FastLLM/releases/latest/download/install.sh | bash
#
# Installs without sudo into ~/.local: bin/fastllm, lib/llmenv (the engine), share/fastllm. Running it again does not redo
# what is already in place: if fastllm is at the requested version and a suitable engine is present, nothing is downloaded.
# Homebrew does the same job with `brew install c-vision/fastllm/fastllm`.
#
# Usage: install.sh [--version X.Y.Z|latest] [--prefix DIR] [--from DIR|URL] [--engine-from DIR|URL]
#                 [--force] [--no-engine] [--pull-starter] [--uninstall] [--quiet]
#   --prefix        where to install (default ~/.local)
#   --from          local folder or base URL with the FastLLM release files (offline tests)
#   --engine-from   like --from, for the LLMEnv release files
#   --force         reinstall even if this version is already there
#   --no-engine     leave the LLMEnv engine alone
#   --pull-starter  download a first model that fits the RAM of this Mac
#   --uninstall     remove fastllm from the prefix (the engine stays: other programs may use it)
set -euo pipefail

REPO="c-vision/FastLLM"; ENGINE_REPO="c-vision/LLMEnv"
VERSION="latest"; PREFIX="${HOME}/.local"; FROM=""; ENGINE_FROM=""; FORCE=0; NOENGINE=0; STARTER=0; UNINSTALL=0; QUIET=0
while [ $# -gt 0 ]; do
    case "$1" in
        --version) VERSION="$2"; shift 2 ;;
        --prefix) PREFIX="$2"; shift 2 ;;
        --from) FROM="$2"; shift 2 ;;
        --engine-from) ENGINE_FROM="$2"; shift 2 ;;
        --force) FORCE=1; shift ;;
        --no-engine) NOENGINE=1; shift ;;
        --pull-starter) STARTER=1; shift ;;
        --uninstall) UNINSTALL=1; shift ;;
        --quiet) QUIET=1; shift ;;
        -h|--help) sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "install: unknown option: $1" >&2; exit 2 ;;
    esac
done
say() { [ "$QUIET" = 1 ] || echo "$@"; }
die() { echo "install: $*" >&2; exit 1; }

[ "$(uname -s)" = "Darwin" ] || die "macOS only"
[ "$(uname -m)" = "arm64" ] || die "an Apple Silicon Mac (arm64) is required"
OSVER="$(sw_vers -productVersion)"
case "$OSVER" in 1[0-3].*|[0-9].*) die "macOS 14 or later is required (this is $OSVER)" ;; esac

if [ "$UNINSTALL" = 1 ]; then
    rm -f "$PREFIX/bin/fastllm"; rm -rf "$PREFIX/share/fastllm"
    say "fastllm removed from $PREFIX (the LLMEnv engine was kept; to remove it: install-llmenv.sh --uninstall)"; exit 0
fi

fetch() { case "$1" in http*) curl -fsSL --retry 3 "$1" ;; *) cat "$1" ;; esac; }

if [ "$VERSION" = latest ]; then
    if [ -n "$FROM" ]; then VERSION="$(fetch "$FROM/LATEST" | tr -d '[:space:]')"
    else VERSION="$(curl -fsSL --retry 3 "https://api.github.com/repos/$REPO/releases/latest" | sed -n 's/.*"tag_name": *"v\{0,1\}\([^"]*\)".*/\1/p' | head -1)"
    fi
    [ -n "$VERSION" ] || die "cannot read the latest release of $REPO"
fi
BASE="${FROM:-https://github.com/$REPO/releases/download/v$VERSION}"
NAME="fastllm-$VERSION-macos-arm64"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

# --- fastllm: already at the requested version? ---
HAVE=""
if [ -x "$PREFIX/bin/fastllm" ]; then HAVE="$("$PREFIX/bin/fastllm" version 2>/dev/null | sed -n '1s/^fastllm //p')"; fi
if [ "$HAVE" = "$VERSION" ] && [ "$FORCE" = 0 ]; then
    say "fastllm $VERSION is already installed in $PREFIX/bin. Nothing to download."
    REQ=""
else
    say "== fastllm $VERSION from $BASE"
    fetch "$BASE/$NAME.tar.gz" > "$TMP/$NAME.tar.gz" || die "download failed: $BASE/$NAME.tar.gz"
    fetch "$BASE/SHA256SUMS-$VERSION" > "$TMP/SHA256SUMS" || die "download failed: SHA256SUMS-$VERSION"
    want="$(awk -v n="$NAME.tar.gz" '$2==n || $2=="*"n {print $1}' "$TMP/SHA256SUMS" | head -1)"
    [ -n "$want" ] || die "$NAME.tar.gz is not in SHA256SUMS-$VERSION"
    [ "$want" = "$(shasum -a 256 "$TMP/$NAME.tar.gz" | awk '{print $1}')" ] || die "SHA-256 mismatch: the download is corrupt or was altered"
    say "   SHA-256 verified"
    tar -C "$TMP" -xzf "$TMP/$NAME.tar.gz"
    [ -x "$TMP/$NAME/bin/fastllm" ] || die "the package does not contain bin/fastllm"
    xattr -cr "$TMP/$NAME" 2>/dev/null || true
    mkdir -p "$PREFIX/bin" "$PREFIX/share"
    # the REAL binary goes in bin/ (not a link): the rpath @executable_path/../lib/llmenv starts from its folder.
    # rename(2) is atomic: a running fastllm never sees a half-written file.
    cp "$TMP/$NAME/bin/fastllm" "$PREFIX/bin/.fastllm.new.$$"
    perl -e 'rename($ARGV[0],$ARGV[1]) or die "rename: $!"' "$PREFIX/bin/.fastllm.new.$$" "$PREFIX/bin/fastllm"
    rm -rf "$PREFIX/share/fastllm.new" "$PREFIX/share/fastllm"
    cp -R "$TMP/$NAME/share/fastllm" "$PREFIX/share/fastllm"
    REQ="$(sed -n 's/^llmenv=//p' "$PREFIX/share/fastllm/REQUIRES" | head -1)"
    say "== fastllm $VERSION installed in $PREFIX/bin/fastllm"
fi
[ -n "${REQ:-}" ] || REQ="$(sed -n 's/^llmenv=//p' "$PREFIX/share/fastllm/REQUIRES" 2>/dev/null | head -1)"

# --- LLMEnv engine: installed by the LLMEnv script, which is idempotent ---
if [ "$NOENGINE" = 0 ]; then
    [ -n "$REQ" ] || die "cannot tell which LLMEnv version is needed (share/fastllm/REQUIRES is missing)"
    EBASE="${ENGINE_FROM:-https://github.com/$ENGINE_REPO/releases/download/v$REQ}"
    fetch "$EBASE/install-llmenv.sh" > "$TMP/install-llmenv.sh" || die "download failed: $EBASE/install-llmenv.sh"
    # the engine script is checked against the sums published with the LLMEnv release
    fetch "$EBASE/SHA256SUMS-$REQ" > "$TMP/ESUMS" || die "download failed: SHA256SUMS-$REQ"
    ewant="$(awk '$2=="install-llmenv.sh" || $2=="*install-llmenv.sh" {print $1}' "$TMP/ESUMS" | head -1)"
    [ -n "$ewant" ] && [ "$ewant" = "$(shasum -a 256 "$TMP/install-llmenv.sh" | awk '{print $1}')" ] \
        || die "install-llmenv.sh does not match the published SHA-256"
    EARGS=(--version "$REQ" --prefix "$PREFIX")
    [ -n "$ENGINE_FROM" ] && EARGS+=(--from "$ENGINE_FROM")
    [ "$QUIET" = 1 ] && EARGS+=(--quiet)
    bash "$TMP/install-llmenv.sh" "${EARGS[@]}"
fi

# --- check ---
say ""
"$PREFIX/bin/fastllm" doctor --install || say "(see the problems above)"
case ":$PATH:" in
    *":$PREFIX/bin:"*) ;;
    *) say ""; say "Add fastllm to your PATH (and restart the terminal):"; say "  echo 'export PATH=\"$PREFIX/bin:\$PATH\"' >> ~/.zshrc" ;;
esac

# --- first model, chosen by RAM ---
RAM_GB=$(( $(sysctl -n hw.memsize) / 1073741824 ))
if [ "$RAM_GB" -lt 16 ]; then STARTER_REPO="mlx-community/Llama-3.2-1B-Instruct-4bit"; else STARTER_REPO="mlx-community/Qwen3-8B-4bit"; fi
if [ "$STARTER" = 1 ]; then
    say ""; say "== first model ($RAM_GB GB of RAM): $STARTER_REPO"
    "$PREFIX/bin/fastllm" pull "$STARTER_REPO"
    say "   try it:  fastllm run ${STARTER_REPO#*/} \"Hello\""
else
    say ""
    say "To get started ($RAM_GB GB of RAM):"
    say "  fastllm pull $STARTER_REPO"
    say "  fastllm run ${STARTER_REPO#*/} \"Hello\""
fi
exit 0
