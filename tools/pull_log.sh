#!/usr/bin/env bash
# Read the adapter's flash log over USB in BOOTSEL mode and decode it.
#
# 1. Unplug the adapter from the PS5.
# 2. Hold the Pico's BOOTSEL button, plug the Micro-USB cable into this computer,
#    release BOOTSEL (the RPI-RP2 drive appears). The firmware does NOT run in this
#    mode, so the PS5 session's log is read exactly as it was written.
# 3. Run this script.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PICOTOOL="$ROOT/.tools/picotool/picotool/picotool"
PY="$ROOT/.venv/bin/python"
mkdir -p "$ROOT/logs"
OUT="$ROOT/logs/flashlog-$(date +%Y%m%d-%H%M%S).bin"

# Log region: last 1 MiB of flash (DIAG_FLASH_REGION_OFFSET), XIP address 0x10100000.
"$PICOTOOL" save -r 0x10100000 0x10200000 "$OUT"
echo "saved $OUT"
"$PY" -I "$ROOT/tools/adapter_log.py" "$OUT" --descriptors "$ROOT/logs/descriptors" "$@"
echo
echo "The Pico is still in BOOTSEL mode. Reboot it into the firmware with:"
echo "  $PICOTOOL reboot"
