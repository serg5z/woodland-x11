# Woodland X11

An X11 server (core protocol, no extensions) that runs on an ESP32-S3 and draws on the
**Elecrow CrowPanel Advance 4.3"** (ESP32-S3-N16R8, 800×480 RGB565 IPS panel, GT911 touch).
X clients connect over Wi-Fi (TCP 6000 + display) or, in the IDF64 profile, over an SPI link to a
host computer. The same portable C core also builds as a host server with a macOS viewer, which
is where most testing happens.

**Status:** release candidate 2 (source of this import: woodland-console `x11-rc2`, commit
12e4efb). The IDF64 RC2 image has run on the panel: OTA install, health confirmation, LCD, Wi-Fi
X11, the SPI client, one-finger touch and two-finger scroll and tap. Experimental software: no
security review beyond MIT-MAGIC-COOKIE-1 authentication and an authenticated OTA protocol.

## What works, and what does not

- All 119 core protocol requests are handled. XTS (X Test Suite) Xproto: 384 PASS, 3 FAIL,
  2 UNSUPPORTED; the FAILs are ChangeHosts 1 and SetAccessControl 1 (host access changes are
  refused by design) and CopyPlane 1.
- One TrueColor visual, depth 16 (RGB565); pixmap depths 16 and 1.
- Fonts: a built-in 6×12 `fixed` font (characters 32–126) and the cursor font. No font server,
  no scalable fonts, no ISO10646 (Unicode) font: xterm in a UTF-8 locale warns that it cannot
  load one and draws a dashed box for each character the font lacks.
- Lines: PolyRectangle supports every line style (solid, on-off and double dashes), cap and
  join style, matching Xorg's pixels (`tools/xts/polyrect_diff.py`), with two deliberate
  differences:
  - each pixel of a rectangle is drawn once, as the protocol requires; Xorg draws some pixels
    twice (corners, folded zero-width or zero-height rectangles), which shows only with
    functions such as GXxor, GXand or GXor, where the results then differ;
  - a GC keeps dash lists of up to 16 lengths; SetDashes accepts longer lists, but dashed
    drawing with them answers BadImplementation (solid drawing with that GC still works).
- Input: touch as the core pointer (Button1); two-finger scroll (Buttons 4/5) and tap
  (Button3); no keyboard input (a default US keymap and modifier map are reported to clients).
- Not supported: extensions (XKB, RENDER, XTEST, SHAPE, …), dashed lines in PolyLine,
  PolySegment and PolyArc (BadImplementation), wide lines and arcs other than rectangles drawn
  pixel-exactly, CopyPlane from depth 16, clip masks over 64 rectangles.
- Known open issues: a few pointer/keyboard grab cases; a two-finger tap held longer than
  300 ms is not a tap; an intermittent RTC-watchdog reset after a software restart (the panel
  boots normally afterwards).

## Layout

| Path | Contents |
|---|---|
| `core/` | portable X11 core: setup, requests, windows, input, raster, fonts, colours |
| `net/`, `input/`, `mt/`, `ota/` | connection transport, single-touch filter, multi-touch chain, OTA protocol |
| `spilink/`, `shot/` | SPI link framing and screenshot messages (IDF64 profile) |
| `pclk/` | LCD pixel-clock trial with automatic rollback (IDF64 profile) |
| `esp32/` | board code (`x11_esp32/`), build scripts, ESP-IDF project (`idf64/`), A/B partition table |
| `host/` | host server, macOS viewer, OTA device simulator |
| `tests/` | C and Python host tests; `idf_socket_capacity_poc/` (lwIP capacity checks) |
| `tools/` | OTA client, diagnostics tools, generators, XTS harness (`xts/`), client probes |
| `licenses/` | third-party license texts (Terminus font; X.Org cursor font, colour names, twm) |

## Host build and tests

macOS (clang, MacPorts X11 clients under `/opt/local` for some tests) is the tested host.
Linux has not been tested since the extraction.

```sh
make                 # host server, sanitizer builds, tests
make test            # C suites + Python tests
./build/x11_host_server -d 1        # a server on :1 (TCP 6001)
tools/run_macos_viewer.sh           # host server with the macOS viewer
```

## Firmware builds (compile only; flashing is up to you)

Both scripts check their toolchain versions and never flash.

| Profile | Script | Needs |
|---|---|---|
| Arduino, 6 clients, Wi-Fi only | `esp32/build_esp32.sh` | `arduino-cli`, esp32 core 3.3.12 |
| IDF64, 64 clients, SPI link, multi-touch chain, pixel clock | `esp32/build_idf64.sh` | ESP-IDF v5.5.5 (`export.sh` sourced), Xtensa toolchain esp-14.2.0_20260121, Arduino-ESP32 3.3.12 (`ARDUINO_ESP32_DIR`), network access (LovyanGFX 1.2.36 and IDF components) |

- **Python:** install ESP-IDF 5.5.5's tools with **Python 3.13**; its environment is
  version-specific, and `export.sh` fails if a different `python3` comes first on `PATH`.
- **Reproducible images:** set `SOURCE_DATE_EPOCH` (the builds embed a compile time). In a git
  checkout it defaults to the last commit that changed the firmware sources. Images built in
  different directories differ only in the app descriptor's ELF hash and the image checksum,
  because the ELF's debug information records absolute paths.
- Output: `X11_BUILD_OUT` / `X11_IDF64_OUT` choose the build directory (outside the source tree).

## Provisioning and use

Over the USB serial port (115200 baud), one command per line: `WIFI_SSID`, `WIFI_PASS`,
`COOKIE NEW` (prints a random MIT-MAGIC-COOKIE-1 once), `OTAKEY NEW`, `DISPLAY`, `MAXCLIENTS`,
`STATUS`, `DIAG`, `RESTART`, and in the IDF64 profile `BACKLIGHT` (full list at the top of
`esp32/x11_esp32/x11_esp32.ino`; `BACKLIGHT` is described below). Without a
cookie nothing listens. Add the cookie to your `~/.Xauthority` with `xauth add`, then:

```sh
DISPLAY=PANEL_IP:0 xclock
xprop -display PANEL_IP:0 -root _WOODLAND_DIAG        # diagnostics (never contains secrets)
tools/screenshot.py --display PANEL_IP:0 --out screen.png   # screenshot over Wi-Fi, PNG on this computer
tools/ota_push.py --host PANEL_IP --key-file KEYFILE status
tools/ota_push.py --host PANEL_IP --key-file KEYFILE --block 16384 --reboot upload IMAGE
```

OTA writes the slot that is **not** running and keeps the other as the fallback; an image that
does not confirm its health within 180 s is rolled back. Keep a known-good image in the other
slot and a USB recovery path before updating.

Screenshots over Wi-Fi use the ordinary X11 GetImage request over the same connection as any
client (cookie from `xauth`): nothing special runs on the panel, and `xwd -root` works too. A
full 800×480 screen is about 750 KB on the network. The IDF64 profile can also send a
screenshot over the SPI link (`shot/`, the `WLSHOT*` messages); the host-side tool for that is
not included in this repository.

### Backlight level (IDF64 profile)

The board's own microcontroller (I2C address 0x30) dims the backlight; the firmware sends it
one command byte, from 0 (brightest) to 245 (darkest). Levels are given in percent, 10 to 100,
mapped linearly (100 % → 0, 40 % → 147, 10 % → 221). The backlight cannot be switched off
this way, and commands above 245 are never sent (246 and up are other board commands).

```sh
xprop -display PANEL_IP:0 -root -f _WOODLAND_BACKLIGHT 8s -set _WOODLAND_BACKLIGHT 40
xprop -display PANEL_IP:0 -root -remove _WOODLAND_BACKLIGHT   # then the same value counts again
```

Over serial: `BACKLIGHT 40` sets the level, `BACKLIGHT` alone reports it.

- **Root property `_WOODLAND_BACKLIGHT`:** type STRING, format 8, a decimal integer from 10 to
  100 (spaces and a trailing NUL are allowed). Any authenticated client can set it. The
  firmware checks the property every 250 ms and acts once per distinct value. Anything else
  (wrong type or format, not a number, out of range) is rejected and changes nothing. Removing
  the property changes nothing either.
- **Writes:** at most one every 30 ms; after an unacknowledged write the firmware retries
  after 1 s. Writes happen in the main loop, which also reads touch on the same I2C bus, so they
  never overlap a touch read.
- **Not stored:** every boot starts at 100 %.
- **Unverified brightness:** the firmware knows only that the microcontroller acknowledged the
  command. Nothing measures the light itself, so `lcd.backlight_physical` always reads
  `unverified`; check the brightness by eye.
- Only this board's microcontroller is supported. The Arduino profile has no runtime
  backlight control.

### Diagnostics

`_WOODLAND_DIAG` (also the serial `DIAG` command) is plain `key=value` lines, generated at each
read. Counters are cumulative since boot; `*_10s` values are maxima over the last 5–10 s. Among
them:

| Keys | Meaning | Builds |
|---|---|---|
| `core.res_windows`, `res_pixmaps`, `res_gcs`, `res_fonts`, `res_cursors`, `res_colormaps` | X11 resources in use, by type | all, host server included |
| `core.pixmap_bytes` | pixel memory of all pixmaps | all |
| `core.properties`, `core.property_bytes` | properties on all windows (the root included) and their data size | all |
| `esp.heap_internal_allocated`, `_alloc_blocks`, `_free_blocks`; `esp.psram_allocated`, `_alloc_blocks`, `_free_blocks` | heap totals and fragmentation, from `heap_caps_get_info` | both firmware profiles |
| `esp.app_version`, `esp.app_elf_sha256`, `esp.idf_version` | the running image: version string, first 8 bytes of its ELF SHA-256, ESP-IDF version | both firmware profiles |
| `lcd.backlight_*` | requested and acknowledged level, its source (`boot`, `serial`, `x11`), I2C state, writes, errors, verdicts on `_WOODLAND_BACKLIGHT` | IDF64 |

- **Cost:** these values are computed only when diagnostics are read: one walk over the
  resource table and the window properties, and one walk over each heap. Nothing is added to
  request handling, drawing, input or the LCD flush. Avoid polling diagnostics at a high rate on
  a busy panel.
- **Telling images apart:** `esp.app_version` does not change between builds of the same
  release, so use `esp.app_elf_sha256` for that.
- **Memory counts:** heap and PSRAM figures cover the whole firmware (Wi-Fi, lwIP, LCD), not
  only the X11 server.

## Conformance testing

`tools/xts/` builds XTS in a Debian container (`Dockerfile`) and runs it against Xvfb or the
sanitizer host server (`run_xts.sh`); `core_diff.py`, `cmap_diff.py`, `font_diff.py`,
`getimage_diff.py` and `polyrect_diff.py` compare replies and drawn pixels with Xvfb.

## License

Copyright 2026 Sergey Zorin. Licensed under the Apache License 2.0 (`LICENSE`). Third-party
material and its terms: `NOTICE.md` and `licenses/`.
