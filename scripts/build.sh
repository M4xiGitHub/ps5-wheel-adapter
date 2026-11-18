#!/usr/bin/env bash
# Builds both firmware variants, runs the host tests and writes dist/ with checksums.
#   dist/ps5-adapter-console-<ver>.uf2   for the PS5
#   dist/ps5-adapter-bench-<ver>.uf2     PC bench only (adds a CDC diagnostics port)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PATH="$ROOT/.venv/bin:$ROOT/.tools/arm-gnu-toolchain/bin:$PATH"
[ -x "$ROOT/.tools/arm-gnu-toolchain/bin/arm-none-eabi-gcc" ] || { echo "run scripts/setup.sh first"; exit 1; }

VER="$(awk '/^#define FW_VERSION_(MAJOR|MINOR|PATCH) /{printf "%s%s", sep, $3; sep="."}' "$ROOT/firmware/src/main.c")"
[ -n "$VER" ] || { echo "cannot read FW_VERSION from firmware/src/main.c"; exit 1; }

# CMake build trees remember absolute paths: start fresh if the project was moved.
fresh_if_moved() {
  if [ -f "$1/CMakeCache.txt" ] && ! grep -qxF "CMAKE_HOME_DIRECTORY:INTERNAL=$2" "$1/CMakeCache.txt"; then
    echo "build tree $1 is from another location: removing it"
    rm -rf "$1"
  fi
}
fresh_if_moved "$ROOT/build/tests" "$ROOT/tests"
fresh_if_moved "$ROOT/build/firmware" "$ROOT/firmware"

echo "== host tests"
cmake -S "$ROOT/tests" -B "$ROOT/build/tests" -G Ninja -DPython3_EXECUTABLE="$ROOT/.venv/bin/python" >/dev/null
cmake --build "$ROOT/build/tests"
ctest --test-dir "$ROOT/build/tests" --output-on-failure

echo "== firmware $VER"
cmake -S "$ROOT/firmware" -B "$ROOT/build/firmware" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPICO_TOOLCHAIN_PATH="$ROOT/.tools/arm-gnu-toolchain" >/dev/null
cmake --build "$ROOT/build/firmware"

mkdir -p "$ROOT/dist" "$ROOT/build/elf"
rm -f "$ROOT"/dist/*.uf2 "$ROOT"/dist/*.elf "$ROOT/dist/SHA256SUMS" "$ROOT/dist/BUILDINFO.txt"
cp "$ROOT/build/firmware/ps5_adapter_console.uf2" "$ROOT/dist/ps5-adapter-console-$VER.uf2"
cp "$ROOT/build/firmware/ps5_adapter_console_noffb.uf2" "$ROOT/dist/ps5-adapter-console-noffb-$VER.uf2"
cp "$ROOT/build/firmware/ps5_adapter_bench.uf2" "$ROOT/dist/ps5-adapter-bench-$VER.uf2"
# ELFs (debug symbols) stay in build/: their debug info contains local absolute paths.
cp "$ROOT/build/firmware/ps5_adapter_console.elf" "$ROOT/build/elf/ps5-adapter-console-$VER.elf"
cp "$ROOT/build/firmware/ps5_adapter_bench.elf" "$ROOT/build/elf/ps5-adapter-bench-$VER.elf"
(cd "$ROOT/dist" && shasum -a 256 ./*.uf2 | sed 's# \./# #' > SHA256SUMS)

{
  echo "version        $VER"
  echo "built          $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "host           $(uname -sm)"
  echo "compiler       $(arm-none-eabi-gcc --version | head -1)"
  echo "cmake          $(cmake --version | head -1)"
  while read -r name url commit rest; do
    case "$name" in ''|'#'*) continue ;; esac
    echo "$(printf '%-14s' "$name") $commit $rest"
  done < "$ROOT/deps.lock"
  echo
  (cd "$ROOT/build/firmware" && arm-none-eabi-size ps5_adapter_console.elf ps5_adapter_console_noffb.elf ps5_adapter_bench.elf)
} > "$ROOT/dist/BUILDINFO.txt"

echo "== dist"
cat "$ROOT/dist/SHA256SUMS"
