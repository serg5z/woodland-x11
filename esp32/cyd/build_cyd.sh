#!/usr/bin/env bash
# Reproducible compile-only profile for the ESP32-2432S028R CYD. Never flashes.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${X11_CYD_BUILD_OUT:-$ROOT/build/cyd}"
SKETCH="$OUT/sketch/CYDNetX11"
FQBN="esp32:esp32:jczn_2432s028r:UploadSpeed=115200,CPUFreq=240,FlashFreq=80,FlashMode=qio,FlashSize=4M,PartitionScheme=default,DebugLevel=none,LoopCore=1,EventsCore=1,EraseFlash=none,JTAGAdapter=default,ZigbeeMode=default"
CORE_VERSION=3.3.12
LGFX_VERSION=1.2.36
CLIENT_FLAGS="-DX11_SERVER_CLIENTS=16 -DX11_NET_REJECT_SLOTS=1 -DXR_MAX_POLY=512 -DX11_DIAG_MAX=2000 -DX11_ATOM_POOL=4096"

installed_core="$(arduino-cli core list | awk '$1 == "esp32:esp32" { print $2 }')"
if [[ "$installed_core" != "$CORE_VERSION" ]]; then
  echo "error: esp32:esp32 core is '$installed_core'; expected $CORE_VERSION" >&2
  exit 1
fi
installed_lgfx="$(arduino-cli lib list | awk '$1 == "LovyanGFX" { print $2 }')"
if [[ "$installed_lgfx" != "$LGFX_VERSION" ]]; then
  echo "error: LovyanGFX is '$installed_lgfx'; expected $LGFX_VERSION" >&2
  exit 1
fi

if [[ -z "${SOURCE_DATE_EPOCH:-}" ]]; then
  SOURCE_DATE_EPOCH="$(git -C "$ROOT" log -1 --format=%ct -- core net input esp32/cyd)"
fi
export SOURCE_DATE_EPOCH

rm -rf "$SKETCH" "$OUT/build"
mkdir -p "$SKETCH/src/core" "$SKETCH/src/net" "$SKETCH/src/input" "$OUT"
cp "$ROOT/esp32/cyd/CYDNetX11.ino" "$ROOT/esp32/cyd/LGFX_CYD.h" "$SKETCH/"
cp "$ROOT"/core/*.[ch] "$SKETCH/src/core/"
cp "$ROOT"/net/*.[ch] "$SKETCH/src/net/"
cp "$ROOT"/input/*.[ch] "$SKETCH/src/input/"

arduino-cli compile \
  --fqbn "$FQBN" \
  --build-path "$OUT/build" \
  --output-dir "$OUT" \
  --export-binaries \
  --warnings all \
  --build-property "compiler.c.extra_flags=$CLIENT_FLAGS" \
  --build-property "compiler.cpp.extra_flags=$CLIENT_FLAGS" \
  "$SKETCH"

APP="$OUT/CYDNetX11.ino.bin"
if [[ ! -f "$APP" ]]; then
  echo "error: expected application image was not produced: $APP" >&2
  exit 1
fi
printf '\nApplication image (not flashed): %s\n' "$APP"
wc -c < "$APP"
shasum -a 256 "$APP"
