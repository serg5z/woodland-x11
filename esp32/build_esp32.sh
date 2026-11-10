#!/usr/bin/env bash
# Build the X11 experiment for the ESP32-S3 (CrowPanel 4.3"). Compiles only; never flashes.
# The portable core is copied into a generated sketch copy under build/, so the sketch in
# the repository stays free of duplicated sources. Same board settings as the production
# Woodland Lab build (core, FQBN, PSRAM, flash, partitions); a separate build directory.
#   esp32/build_esp32.sh
# Output: build/esp32-ab/ (or build/esp32/ for huge_app)/x11_esp32.ino.merged.bin (16 MB image
# for offset 0x0), the app, bootloader and partition table, and sizes.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
EXP="$(dirname "$HERE")"
# Partition layout: "ab" (default, Milestone 4: app0 + app1, OTA) or "huge_app" (the single-slot
# Arduino table the panel runs now; reproduces the Milestone 3B image).
LAYOUT="${X11_LAYOUT:-ab}"
case "$LAYOUT" in
  ab) OUT="$EXP/build/esp32-ab" ;;
  huge_app) OUT="$EXP/build/esp32" ;;
  *) echo "error: X11_LAYOUT must be ab or huge_app" >&2; exit 1 ;;
esac
SKETCH="$EXP/build/esp32-sketch-$LAYOUT/x11_esp32"
# Reproducible images: the Arduino core embeds __DATE__/__TIME__ ("Software Info: Compile
# Date/Time"), which changes the image (and the ELF hash in its app descriptor) on every clean
# build. GCC takes them from SOURCE_DATE_EPOCH: the time of the last commit that changed the
# firmware inputs (manifests and documentation commits do not change the image).
if [[ -z "${SOURCE_DATE_EPOCH:-}" ]]; then
  SOURCE_DATE_EPOCH="$(git -C "$EXP" log -1 --format=%ct -- core net input ota esp32/x11_esp32 esp32/partitions_ab.csv esp32/build_esp32.sh 2>/dev/null || true)"
fi
if [[ -z "$SOURCE_DATE_EPOCH" ]]; then
  echo "error: set SOURCE_DATE_EPOCH (no git history to take it from; see README)" >&2
  exit 1
fi
export SOURCE_DATE_EPOCH
# X11_TEST_FLAGS: extra -D flags for test images only (e.g. -DX11_OTA_TEST_NO_CONFIRM);
# a normal build leaves it empty.
if [[ -n "${X11_TEST_FLAGS:-}" && -z "${X11_BUILD_OUT:-}" ]]; then
  echo "error: X11_TEST_FLAGS needs X11_BUILD_OUT (a test image never goes to build/esp32-ab)" >&2
  exit 1
fi
# X11_BUILD_OUT: build elsewhere (keeps a validated image in build/esp32-ab untouched)
if [[ -n "${X11_BUILD_OUT:-}" ]]; then
  OUT="$X11_BUILD_OUT"
  SKETCH="$X11_BUILD_OUT-sketch/x11_esp32"
fi
FQBN="esp32:esp32:esp32s3:UploadSpeed=115200,FlashSize=16M,PartitionScheme=huge_app,PSRAM=opi"
CORE_VERSION="3.3.12"
# Client slots compiled in (X11_SERVER_CLIENTS); the runtime limit (MAXCLIENTS) can only be lower.
# lwIP has CONFIG_LWIP_MAX_SOCKETS 16 in this prebuilt core: X11 listener 1 + refusal slot 1 +
# OTA listener 1 + OTA session 1 + clients <= 16, so at most 12. Default 6 (Milestone 3B).
CLIENTS="${X11_CLIENTS:-6}"
if (( CLIENTS < 1 || CLIENTS > 12 )); then
  echo "error: X11_CLIENTS must be 1..12 (lwIP sockets: 16 = X11 listener + refusal slot + OTA listener + OTA session + clients)" >&2
  exit 1
fi

installed="$(arduino-cli core list | awk '$1=="esp32:esp32"{print $2}')"
if [[ "$installed" != "$CORE_VERSION" ]]; then
  echo "error: esp32:esp32 core is '$installed', expected $CORE_VERSION" >&2
  exit 1
fi

rm -rf "$SKETCH"
mkdir -p "$SKETCH/src" "$OUT"
cp "$HERE"/x11_esp32/*.ino "$HERE"/x11_esp32/*.h "$HERE"/x11_esp32/*.cpp "$SKETCH/"
cp "$EXP"/core/*.[ch] "$EXP"/net/*.[ch] "$EXP"/input/*.[ch] "$EXP"/ota/*.[ch] "$SKETCH/src/"
# the SPI link only in an SPI test build (a normal image gets no extra sources, so its hash
# does not change)
if [[ "${X11_TEST_FLAGS:-}" == *X11_SPI_ECHO* ]]; then cp "$EXP"/spilink/*.[ch] "$SKETCH/src/"; fi
if [[ "${X11_TEST_FLAGS:-}" == *X11_SPI_SHOT* ]]; then cp "$EXP"/shot/*.[ch] "$SKETCH/src/"; fi
if [[ "${X11_TEST_FLAGS:-}" == *X11_TOUCH_DIAG* ]]; then cp "$EXP"/mt/*.[ch] "$SKETCH/src/"; fi
if [[ "${X11_TEST_FLAGS:-}" == *X11_PCLK* ]]; then  # the pixel-clock setting: X11 policy + the trial logic (pclk/pclk_trial.*)
  cp "$EXP"/pclk/x11_pclk.* "$EXP"/pclk/pclk_trial.h "$EXP"/pclk/pclk_trial.cpp "$SKETCH/src/"
fi
if [[ "$LAYOUT" == ab ]]; then
  cp "$HERE/partitions_ab.csv" "$SKETCH/partitions.csv"  # the sketch's table overrides PartitionScheme
fi
# one flat src/ directory: includes that reach into ../core/ become local
sed -i.bak 's#"\.\./core/#"#; s#"\.\./net/#"#; s#"\.\./input/#"#' "$SKETCH"/src/*.[ch]
rm -f "$SKETCH"/src/*.bak

arduino-cli compile --fqbn "$FQBN" --build-path "$OUT" --warnings default \
  --build-property "compiler.c.extra_flags=-DX11_SERVER_CLIENTS=$CLIENTS -DX11_NET_REJECT_SLOTS=1 -DXR_MAX_POLY=512 ${X11_TEST_FLAGS:-}" \
  --build-property "compiler.cpp.extra_flags=-DX11_SERVER_CLIENTS=$CLIENTS -DX11_NET_REJECT_SLOTS=1 -DXR_MAX_POLY=512 ${X11_TEST_FLAGS:-}" \
  "$SKETCH" "$@"

MERGED="$OUT/x11_esp32.ino.merged.bin"
echo
echo "Flash image (not flashed): $MERGED"
shasum -a 256 "$MERGED" "$OUT/x11_esp32.ino.bin" "$OUT/x11_esp32.ino.partitions.bin" "$OUT/x11_esp32.ino.bootloader.bin" "$OUT/boot_app0.bin"
