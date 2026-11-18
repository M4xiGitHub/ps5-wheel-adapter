#!/usr/bin/env bash
# Packs what the PC next to the hardware needs (firmware, diagnostic tools, test docs)
# into dist/ps5-adapter-kit-<ver>.zip. Run scripts/build.sh first.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VER="$(awk '/^#define FW_VERSION_(MAJOR|MINOR|PATCH) /{printf "%s%s", sep, $3; sep="."}' "$ROOT/firmware/src/main.c")"
for f in "ps5-adapter-console-$VER.uf2" "ps5-adapter-bench-$VER.uf2"; do
  [ -f "$ROOT/dist/$f" ] || { echo "dist/$f missing: run scripts/build.sh"; exit 1; }
done

NAME="ps5-adapter-kit-$VER"
STAGE="$ROOT/build/kit/$NAME"
rm -rf "$ROOT/build/kit"
mkdir -p "$STAGE/firmware" "$STAGE/tools" "$STAGE/docs"

cp "$ROOT/dist/ps5-adapter-console-$VER.uf2" "$ROOT/dist/ps5-adapter-bench-$VER.uf2" "$STAGE/firmware/"
(cd "$STAGE/firmware" && shasum -a 256 ./*.uf2 | sed 's# \./# #' > SHA256SUMS)
cp "$ROOT"/tools/adapter_log.py "$ROOT"/tools/adapter_monitor.py "$ROOT"/tools/auth_probe.py \
   "$ROOT"/tools/hid_probe.py "$STAGE/tools/"
cp "$ROOT"/docs/TEST_PLAN.md "$ROOT"/docs/HARDWARE.md "$ROOT"/docs/DIAGNOSTICS.md "$STAGE/docs/"
grep -E '^(hidapi|pyserial|cryptography)==' "$ROOT/requirements.txt" > "$STAGE/requirements-tools.txt"
sed "s/VERSION/$VER/g" "$ROOT/scripts/kit_START_HERE.md" > "$STAGE/START_HERE.md"
cp "$ROOT/THIRD_PARTY_NOTICES.md" "$STAGE/"

rm -f "$ROOT/dist/$NAME.zip"
(cd "$ROOT/build/kit" && zip -qr -X "$ROOT/dist/$NAME.zip" "$NAME")
(cd "$ROOT/dist" && grep -v ' ps5-adapter-kit-' SHA256SUMS > SHA256SUMS.tmp; shasum -a 256 "$NAME.zip" >> SHA256SUMS.tmp; mv SHA256SUMS.tmp SHA256SUMS)
echo "dist/$NAME.zip"
unzip -l "$ROOT/dist/$NAME.zip"
