#!/usr/bin/env bash
# Build a full 64-slot Woodland X11 image with source-built ESP-IDF lwIP. Never flashes hardware.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
EXP="$(dirname "$HERE")"
PROJECT_TEMPLATE="$HERE/idf64"
OUT="${X11_IDF64_OUT:-$EXP/build/esp32-idf64}"
PROJECT="$OUT/project"
BUILD="$OUT/build"
CORE="${ARDUINO_ESP32_DIR:-$HOME/Library/Arduino15/packages/esp32/hardware/esp32/3.3.12}"
LOVYAN_COMMIT="fb494c62e63fdb081c6bdca2761bc552c81bb585"
LOVYAN="$OUT/deps/LovyanGFX"
IDF_EXPECTED="ESP-IDF v5.5.5"
RUNTIME_PROFILE="${X11_RUNTIME_PROFILE:-0}"
case "$OUT" in ""|/|"$EXP") echo "error: unsafe X11_IDF64_OUT: $OUT" >&2; exit 1;; esac
if [[ "$RUNTIME_PROFILE" == 1 && -z "${X11_IDF64_OUT:-}" ]]; then
  echo "error: runtime profile builds require an isolated X11_IDF64_OUT" >&2; exit 1
fi

if [[ -z "${IDF_PATH:-}" || ! -f "$IDF_PATH/tools/cmake/project.cmake" ]]; then
  echo "error: source ESP-IDF v5.5.5 export.sh before building" >&2; exit 1
fi
if [[ "$(idf.py --version)" != "$IDF_EXPECTED" ]]; then
  echo "error: expected $IDF_EXPECTED, got $(idf.py --version)" >&2; exit 1
fi
if ! xtensa-esp32s3-elf-gcc --version | grep -q 'esp-14.2.0_20260121'; then
  echo "error: expected Xtensa toolchain esp-14.2.0_20260121" >&2; exit 1
fi
if [[ ! -f "$CORE/package.json" ]] || ! grep -q '"version": "3.3.12"' "$CORE/package.json"; then
  echo "error: set ARDUINO_ESP32_DIR to Arduino-ESP32 3.3.12" >&2; exit 1
fi
if [[ -z "${SOURCE_DATE_EPOCH:-}" ]]; then
  SOURCE_DATE_EPOCH="$(git -C "$EXP" log -1 --format=%ct -- core net input ota spilink shot mt pclk esp32/x11_esp32 esp32/partitions_ab.csv 2>/dev/null || true)"
fi
if [[ -z "$SOURCE_DATE_EPOCH" ]]; then
  echo "error: set SOURCE_DATE_EPOCH (no git history to take it from; see README)" >&2; exit 1
fi
export SOURCE_DATE_EPOCH

mkdir -p "$OUT/deps"
if [[ ! -d "$LOVYAN/.git" ]]; then
  git clone --no-checkout https://github.com/lovyan03/LovyanGFX.git "$LOVYAN"
fi
if [[ "$(git -C "$LOVYAN" rev-parse HEAD 2>/dev/null || true)" != "$LOVYAN_COMMIT" ]]; then
  git -C "$LOVYAN" fetch --depth 1 origin "$LOVYAN_COMMIT"
  git -C "$LOVYAN" checkout --detach "$LOVYAN_COMMIT"
fi
if ! grep -q '^version=1\.2\.36$' "$LOVYAN/library.properties"; then
  echo "error: LovyanGFX source is not version 1.2.36" >&2; exit 1
fi

# Stage sources outside the Arduino path; no existing Arduino build output is touched.
mkdir -p "$OUT"
if [[ -f "$PROJECT/sdkconfig" ]]; then cp "$PROJECT/sdkconfig" "$OUT/sdkconfig"; fi
mkdir -p "$PROJECT/main/src" "$PROJECT/components"
if [[ -d "$PROJECT/main" ]]; then rm -rf "$PROJECT/main"; fi
mkdir -p "$PROJECT/main/src"
cp "$PROJECT_TEMPLATE/CMakeLists.txt" "$PROJECT_TEMPLATE/sdkconfig.defaults" \
  "$PROJECT_TEMPLATE/version.txt" "$PROJECT/"
if [[ "$RUNTIME_PROFILE" == 1 ]]; then
  cat >> "$PROJECT/sdkconfig.defaults" <<'EOF'

# Test-only dual-core profiling; ESP Timer supplies a 1 MHz runtime-stat clock.
CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y
CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER=y
EOF
  profile_version="$(tr -d '\n' < "$PROJECT/version.txt")-rtprof1"
  printf '%s\n' "$profile_version" > "$PROJECT/version.txt"
fi
cp "$PROJECT_TEMPLATE/main/CMakeLists.txt" "$PROJECT/main/"
cp "$HERE/partitions_ab.csv" "$PROJECT/partitions.csv"
cp "$PROJECT_TEMPLATE/dependencies.lock" "$PROJECT/dependencies.lock"
if [[ ! -e "$PROJECT/components/arduino-esp32" ]]; then
  ln -s "$CORE" "$PROJECT/components/arduino-esp32"
fi
if [[ ! -e "$PROJECT/components/LovyanGFX" ]]; then
  ln -s "$LOVYAN" "$PROJECT/components/LovyanGFX"
fi

cp "$HERE/x11_esp32/x11_esp32.ino" "$PROJECT/main/x11_esp32.cpp"
cp "$HERE/x11_esp32"/*.h "$HERE/x11_esp32"/*.cpp "$PROJECT/main/"
cp "$EXP/core"/*.[ch] "$EXP/net"/*.[ch] "$EXP/input"/*.[ch] "$EXP/ota"/*.[ch] \
  "$EXP/spilink"/*.[ch] "$EXP/shot"/*.[ch] "$EXP/mt"/*.[ch] "$PROJECT/main/src/"
cp "$EXP/pclk/x11_pclk.cpp" "$EXP/pclk/x11_pclk.h" \
  "$EXP/pclk/pclk_trial.cpp" "$EXP/pclk/pclk_trial.h" \
  "$PROJECT/main/src/"
cp "$EXP/tests/idf_socket_capacity_poc/main/capacity_assert.c" "$PROJECT/main/src/"
python3 - "$PROJECT/main/src" <<'PY'
from pathlib import Path
import sys
root = Path(sys.argv[1])
for path in root.iterdir():
    if path.is_file() and path.suffix in (".c", ".h", ".cpp"):
        data = path.read_text()
        data = data.replace('"../core/', '"').replace('"../net/', '"').replace('"../input/', '"')
        path.write_text(data)
PY

mkdir -p "$BUILD"
if [[ -f "$OUT/sdkconfig" ]]; then cp "$OUT/sdkconfig" "$PROJECT/sdkconfig"; fi
(
  cd "$PROJECT"
  if [[ ! -f "$BUILD/config/sdkconfig.h" ]]; then idf.py -B "$BUILD" set-target esp32s3; fi
  idf.py -B "$BUILD" build
  idf.py -B "$BUILD" size
) 2>&1 | tee "$OUT/build.log"
cp "$PROJECT/sdkconfig" "$OUT/sdkconfig"

SDKCONFIG_H="$BUILD/config/sdkconfig.h"
COMPILE_COMMANDS="$BUILD/compile_commands.json"
APP="$BUILD/woodland_x11_64.bin"
for expected in \
  '#define CONFIG_LWIP_MAX_SOCKETS 72' \
  '#define CONFIG_LWIP_MAX_ACTIVE_TCP 72' \
  '#define CONFIG_LWIP_MAX_LISTENING_TCP 4' \
  '#define CONFIG_SPIRAM_MODE_OCT 1' \
  '#define CONFIG_ESPTOOLPY_FLASHSIZE_16MB 1' \
  '#define CONFIG_FREERTOS_HZ 1000' \
  '#define CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 1' \
  '#define CONFIG_AUTOSTART_ARDUINO 1'; do
  grep -Fq "$expected" "$SDKCONFIG_H" || { echo "error: missing generated config: $expected" >&2; exit 1; }
done
if [[ "$RUNTIME_PROFILE" == 1 ]]; then
  grep -Fq '#define CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS 1' "$SDKCONFIG_H"
  grep -Fq '#define CONFIG_FREERTOS_USE_TRACE_FACILITY 1' "$SDKCONFIG_H"
  grep -Fq '#define CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER 1' "$SDKCONFIG_H"
  grep -Fq -- '-DX11_RUNTIME_PROFILE' "$COMPILE_COMMANDS"
else
  if grep -Fq -- '-DX11_RUNTIME_PROFILE' "$COMPILE_COMMANDS"; then
    echo "error: profiling instrumentation unexpectedly enabled in normal build" >&2; exit 1
  fi
fi
grep -Fq -- '-DFD_SETSIZE=80' "$COMPILE_COMMANDS"
grep -Fq -- '-DX11_SERVER_CLIENTS=64' "$COMPILE_COMMANDS"
for define in X11_SPI_ECHO X11_SPI_X11 X11_SPI_SHOT X11_TOUCH_DIAG X11_PCLK X11_BACKLIGHT; do
  grep -Fq -- "-D$define" "$COMPILE_COMMANDS"
done
sed -n '/Memory Type Usage Summary/,/Total image size/p' "$OUT/build.log" > "$OUT/memory-report.txt"
test -s "$APP"
python "$IDF_PATH/components/esptool_py/esptool/esptool.py" --chip esp32s3 image_info "$APP" \
  > "$OUT/image-info.txt"
if [[ "$RUNTIME_PROFILE" == 1 ]]; then
  python3 - "$APP" <<'PY'
import sys
from pathlib import Path
if b"1.0.64-idf.1-rtprof1" not in Path(sys.argv[1]).read_bytes():
    raise SystemExit("error: profile marker missing from application descriptor")
PY
fi
python "$IDF_PATH/components/partition_table/gen_esp32part.py" --quiet \
  "$BUILD/partition_table/partition-table.bin" "$OUT/partition-table.csv"
grep -Fq 'app0,app,ota_0,0x10000,3M' "$OUT/partition-table.csv"
grep -Fq 'app1,app,ota_1,0x310000,3M' "$OUT/partition-table.csv"
python3 "$EXP/tools/ota_push.py" check "$APP" > "$OUT/ota-image-check.txt"
shasum -a 256 "$APP" > "$OUT/sha256.txt"
wc -c < "$APP" | tr -d ' ' > "$OUT/image-size-bytes.txt"
{
  echo "source_commit=$(git -C "$EXP" rev-parse HEAD 2>/dev/null || echo "${X11_SOURCE_COMMIT:-none}")"
  echo "firmware_version=$(tr -d '\n' < "$PROJECT/version.txt")"
  echo "idf_version=$(idf.py --version)"
  echo "xtensa_compiler=$(xtensa-esp32s3-elf-gcc --version | head -1)"
  echo "arduino_core=3.3.12"
  echo "lovyangfx_commit=$LOVYAN_COMMIT"
  echo "source_date_epoch=$SOURCE_DATE_EPOCH"
  echo "app_partition_bytes=3145728"
  echo "psram_mode=opi-oct, 80MHz"
  echo "lwip_sockets=72 active_tcp=72 listening_tcp=4 fd_setsize=80 x11_slots=64"
  echo "runtime_profile=$RUNTIME_PROFILE"
  if [[ "$RUNTIME_PROFILE" == 1 ]]; then
    echo "profile_source_hashes:"
    shasum -a 256 "$HERE/x11_esp32/x11_esp32.ino" \
      "$HERE/x11_esp32/x11_runtime_profile.cpp" "$HERE/x11_esp32/x11_runtime_profile.h" \
      "$HERE/../net/x11_net.c" "$HERE/../net/x11_net.h" \
      "$HERE/../net/x11_latency_trace.c" "$HERE/../net/x11_latency_trace.h" \
      "$HERE/../core/x11_server.c" "$HERE/../core/x11_render.c" \
      "$PROJECT_TEMPLATE/main/CMakeLists.txt" "$PROJECT_TEMPLATE/sdkconfig.defaults" \
      "$HERE/build_idf64.sh"
  fi
  cat "$OUT/image-size-bytes.txt"
  cat "$OUT/sha256.txt"
} > "$OUT/build-manifest.txt"
echo "Build verified; not flashed. Artifact: $APP"
cat "$OUT/build-manifest.txt"
