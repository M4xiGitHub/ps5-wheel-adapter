#!/usr/bin/env bash
# Reproducible development setup for the PS5 wheel adapter.
#
# Installs everything under the project directory:
#   .tools/arm-gnu-toolchain   Arm GNU Toolchain 14.2.Rel1 (SHA-256 verified)
#   .venv/                     Python venv with pinned cmake, ninja, hidapi, pyserial, cryptography
#   deps/<name>                Source dependencies at the exact commits in deps.lock
#   .tools/picotool            picotool built from the pinned source (UF2 conversion, BOOTSEL readout)
#
# System prerequisites (not installed by this script):
#   macOS:  Xcode Command Line Tools (clang, git), Homebrew packages: libusb pkgconf
#   Linux:  build-essential git python3-venv libusb-1.0-0-dev pkg-config
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLS="$ROOT/.tools"
# Sub-builds (picotool, pico-sdk host tools) must find the pinned cmake/ninja.
export PATH="$ROOT/.venv/bin:$PATH"
mkdir -p "$TOOLS" "$ROOT/deps"

log() { printf '\n== %s\n' "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

OS="$(uname -s)"
ARCH="$(uname -m)"

# ---------------------------------------------------------------- prerequisites
log "Checking system prerequisites ($OS $ARCH)"
for t in git curl tar python3 cc c++; do
  command -v "$t" >/dev/null || die "missing '$t'"
done
if [ "$OS" = Darwin ]; then
  # Prefer Homebrew's libusb even if another pkg-config (e.g. MacPorts) is first on PATH.
  if command -v brew >/dev/null; then
    export PKG_CONFIG_PATH="$(brew --prefix)/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
  fi
  if ! pkg-config --exists libusb-1.0 2>/dev/null; then
    die "libusb-1.0 not found. Install with: brew install libusb pkgconf"
  fi
else
  pkg-config --exists libusb-1.0 2>/dev/null || die "libusb-1.0 dev package not found (apt install libusb-1.0-0-dev pkg-config)"
fi

# ---------------------------------------------------------------- Arm toolchain
ARM_VER=14.2.rel1
case "$OS-$ARCH" in
  Darwin-arm64)   ARM_HOST=darwin-arm64;  ARM_SHA=c7c78ffab9bebfce91d99d3c24da6bf4b81c01e16cf551eb2ff9f25b9e0a3818 ;;
  Darwin-x86_64)  ARM_HOST=darwin-x86_64; ARM_SHA=2d9e717dd4f7751d18936ae1365d25916534105ebcb7583039eff1092b824505 ;;
  Linux-x86_64)   ARM_HOST=x86_64;        ARM_SHA=62a63b981fe391a9cbad7ef51b17e49aeaa3e7b0d029b36ca1e9c3b2a9b78823 ;;
  Linux-aarch64)  ARM_HOST=aarch64;       ARM_SHA=87330bab085dd8749d4ed0ad633674b9dc48b237b61069e3b481abd364d0a684 ;;
  *) die "unsupported host $OS-$ARCH" ;;
esac
ARM_NAME="arm-gnu-toolchain-$ARM_VER-$ARM_HOST-arm-none-eabi"
ARM_DIR="$TOOLS/arm-gnu-toolchain"
if [ ! -x "$ARM_DIR/bin/arm-none-eabi-gcc" ]; then
  log "Downloading $ARM_NAME"
  TARBALL="$TOOLS/$ARM_NAME.tar.xz"
  curl -fL --retry 3 -o "$TARBALL" \
    "https://developer.arm.com/-/media/Files/downloads/gnu/$ARM_VER/binrel/$ARM_NAME.tar.xz"
  echo "$ARM_SHA  $TARBALL" | shasum -a 256 -c - || die "toolchain checksum mismatch"
  rm -rf "$ARM_DIR" "$TOOLS/$ARM_NAME"
  tar -xJf "$TARBALL" -C "$TOOLS"
  mv "$TOOLS/$ARM_NAME" "$ARM_DIR"
  rm -f "$TARBALL"
fi
"$ARM_DIR/bin/arm-none-eabi-gcc" --version | head -1

# ---------------------------------------------------------------- Python venv
# A venv is not relocatable (its scripts hard-code its path): recreate it after a move.
if [ -x "$ROOT/.venv/bin/python" ] && ! "$ROOT/.venv/bin/cmake" --version >/dev/null 2>&1; then
  log "Python venv belongs to another location: recreating"
  rm -rf "$ROOT/.venv"
fi
if [ ! -x "$ROOT/.venv/bin/python" ]; then
  log "Creating Python venv"
  python3 -m venv "$ROOT/.venv"
fi
log "Installing pinned Python packages"
"$ROOT/.venv/bin/python" -m pip install --quiet --upgrade pip
"$ROOT/.venv/bin/python" -m pip install --quiet -r "$ROOT/requirements.txt"
"$ROOT/.venv/bin/cmake" --version | head -1

# ---------------------------------------------------------------- source deps
log "Fetching source dependencies from deps.lock"
while read -r name url commit _rest; do
  case "$name" in ''|'#'*) continue ;; esac
  dir="$ROOT/deps/$name"
  if [ -d "$dir/.git" ] && [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null)" = "$commit" ]; then
    echo "  $name @ ${commit:0:12} (present)"
    continue
  fi
  rm -rf "$dir"
  git init -q "$dir"
  git -C "$dir" remote add origin "$url"
  git -C "$dir" fetch -q --depth 1 origin "$commit"
  git -C "$dir" -c advice.detachedHead=false checkout -q FETCH_HEAD
  [ "$(git -C "$dir" rev-parse HEAD)" = "$commit" ] || die "$name: checkout mismatch"
  echo "  $name @ ${commit:0:12} (fetched)"
done < "$ROOT/deps.lock"

# ---------------------------------------------------------------- picotool
PICOTOOL_PREFIX="$TOOLS/picotool"
if [ ! -x "$PICOTOOL_PREFIX/bin/picotool" ]; then
  log "Building picotool from pinned source"
  rm -rf "$ROOT/build/picotool"
  PT_LOG="$ROOT/build/picotool-build.log"
  mkdir -p "$ROOT/build"
  { cmake -S "$ROOT/deps/picotool" -B "$ROOT/build/picotool" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DPICO_SDK_PATH="$ROOT/deps/pico-sdk" \
      -DPICOTOOL_FLAT_INSTALL=1 \
      -DCMAKE_INSTALL_PREFIX="$PICOTOOL_PREFIX" &&
    cmake --build "$ROOT/build/picotool" --parallel &&
    cmake --install "$ROOT/build/picotool"; } >"$PT_LOG" 2>&1 || { tail -30 "$PT_LOG"; die "picotool build failed (see $PT_LOG)"; }
fi
PICOTOOL_BIN="$PICOTOOL_PREFIX/picotool/picotool"
[ -x "$PICOTOOL_BIN" ] || PICOTOOL_BIN=""
[ -n "$PICOTOOL_BIN" ] || die "picotool build failed"
"$PICOTOOL_BIN" version || true

log "Setup complete. Next: scripts/build.sh"
