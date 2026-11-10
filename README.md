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
- Fonts: a built-in 6×12 `fixed` font and the cursor font. No font server, no scalable fonts.
- Input: touch as the core pointer (Button1); two-finger scroll (Buttons 4/5) and tap
  (Button3); no keyboard input (a default US keymap and modifier map are reported to clients).
- Not supported: extensions (XKB, RENDER, XTEST, SHAPE, …), wide/dashed lines and arcs drawn
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
`STATUS`, `RESTART` (full list at the top of `esp32/x11_esp32/x11_esp32.ino`). Without a
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

## Conformance testing

`tools/xts/` builds XTS in a Debian container (`Dockerfile`) and runs it against Xvfb or the
sanitizer host server (`run_xts.sh`); `core_diff.py`, `cmap_diff.py`, `font_diff.py` and
`getimage_diff.py` compare replies with Xvfb.

## License

Copyright 2026 Sergey Zorin. Licensed under the Apache License 2.0 (`LICENSE`). Third-party
material and its terms: `NOTICE.md` and `licenses/`.
