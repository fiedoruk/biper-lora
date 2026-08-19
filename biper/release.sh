#!/usr/bin/env bash
# Builds the merged Biper image that a web installer flashes over Web Serial.
#
# PlatformIO produces the application image only. A browser flashes from offset
# 0, so it needs an image with the bootloader, the partition table and the OTA
# data entry — without them the cube does not boot after flashing and looks
# bricked. The offsets are NOT guessed: they come from the same build's
# `idedata.json` and from `huge_app.csv`.
#
#   bash biper/release.sh [environment]      # default: Biper_AP_C6L_spike
set -euo pipefail

ENV="${1:-Biper_AP_C6L_spike}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
B="$ROOT/.pio/build/$ENV"
[ -f "$B/firmware.bin" ] || { echo "error: $B/firmware.bin is missing — run: pio run -e $ENV"; exit 1; }

# Paths are not guessed or hardcoded — PlatformIO itself is asked, so the
# script works wherever it is installed.
CORE_DIR="$(pio system info --json-output | "$(command -v python3)" -c 'import json,sys;print(json.load(sys.stdin)["core_dir"]["value"])')"
PY="$(pio system info --json-output | "$(command -v python3)" -c 'import json,sys;print(json.load(sys.stdin)["python_exe"]["value"])')"
ESPTOOL="$(find "$CORE_DIR/packages/tool-esptoolpy" -maxdepth 1 -name esptool.py | head -1)"
BOOTAPP="$(find "$CORE_DIR/packages/framework-arduinoespressif32/tools/partitions" -name boot_app0.bin | head -1)"

for f in "$ESPTOOL" "$BOOTAPP" "$PY"; do
  [ -e "$f" ] || { echo "error: not found: $f"; exit 1; }
done

# The product version lives in biper/VERSION — the panel keeps its own
# component version and the two no longer have to move together.
VERSION="$(tr -d '[:space:]' < "$ROOT/biper/VERSION")"
COMMIT_SHA="$(git -C "$ROOT" rev-parse --short=7 HEAD)"
OUT="$ROOT/biper/releases/Biper_Unit_C6L-v${VERSION}-${COMMIT_SHA}-merged.bin"
mkdir -p "$(dirname "$OUT")"

"$PY" "$ESPTOOL" --chip esp32c6 merge_bin -o "$OUT" \
  --flash_mode dio --flash_size 4MB \
  0x0     "$B/bootloader.bin" \
  0x8000  "$B/partitions.bin" \
  0xe000  "$BOOTAPP" \
  0x10000 "$B/firmware.bin" >/dev/null

SUM="$(shasum -a 256 "$OUT" | cut -d' ' -f1)"
BYTES="$(wc -c < "$OUT" | tr -d ' ')"
echo "OK  $(basename "$OUT")"
echo "    bytes  : $BYTES"
echo "    sha256 : $SUM"
echo "    env    : $ENV, commit $COMMIT_SHA"
