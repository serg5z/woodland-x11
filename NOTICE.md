# NOTICE

Woodland X11
Copyright 2026 Sergey Zorin

This product is licensed under the Apache License, Version 2.0 (`LICENSE`), except for the
third-party material listed below, which keeps its own terms.

## Third-party material included in this repository

| What | Where | Origin | Terms |
|---|---|---|---|
| 6×12 bitmap font (served as `fixed` and `6x12`) | `core/x11_font_fixed.c` (generated) | Terminus Font, Copyright (c) 2010 Dimitar Toshkov Zhekov; converted from `Uni2-Terminus12x6.psf` (Debian console-setup) | SIL Open Font License 1.1: `licenses/OFL-Terminus.txt`. Reserved Font Name "Terminus Font": this converted copy is not distributed under that name. |
| X cursor font glyphs | `core/x11_font_cursor.c` (generated) | X.Org font-cursor-misc 1.0.3 (`cursor.pcf`) | `licenses/X.Org-font-cursor-misc-COPYING.txt` |
| X11 colour names and values | `core/x11_colors.c` (generated) | X.Org rgb 1.0.6 (`rgb.txt`) | `licenses/X.Org-rgb-COPYING.txt` (The Open Group and Oracle notices) |
| Rectangle outline rules and arc spans (ported parts: the wide-line fast path for rectangles, `miLineArcI`, `miLineArcD`, the round cap/join clip and polygon edge setup, dash and join rules) | `core/x11_outline.c` | X.Org xserver `mi/miwideline.c`, `mi/mipolyrect.c` | `licenses/X.Org-xserver-mi-COPYING.txt` (The Open Group and Digital Equipment Corporation notices) |
| twm configuration | `wm/twmrc` | X.Org twm 1.0.10 `system.twmrc`, fonts changed to "fixed" | `licenses/X.Org-twm-COPYING.txt` (The Open Group, Evans & Sutherland and other notices) |

`esp32/x11_esp32/crowpanel_lcd.h` and `esp32/x11_esp32/crowpanel_board.h` contain the CrowPanel
Advance 4.3" hardware settings (GPIO assignment, sync timing, pixel clock, I2C addresses and
commands). They were written for this project; `crowpanel_lcd.h` follows the device-configuration
pattern of LovyanGFX's examples.

## Dependencies fetched at build time (not included)

Source distribution of this repository does not include them. A **firmware binary** built from
it contains code from the components marked "linked", and must carry their notices.

| Component | Version | License | In the firmware |
|---|---|---|---|
| ESP-IDF (incl. lwIP, FreeRTOS, mbedTLS, newlib, wpa_supplicant and the other components the build links) | v5.5.5 | Apache-2.0; bundled third-party components under their own licenses (BSD, MIT, …) | linked (IDF64: built from source; Arduino profile: prebuilt in the core) |
| ESP-IDF prebuilt Wi-Fi, PHY and coexistence libraries (`esp_wifi`, `esp_phy`, `esp_coex`) | with v5.5.5 | Apache-2.0 (`components/esp_wifi/lib/LICENSE`) | linked |
| Arduino-ESP32 core | 3.3.12 | LGPL-2.1-or-later (Espressif files Apache-2.0) | linked: a binary release must meet LGPL-2.1 (allow relinking with a modified core, or offer the corresponding source) |
| LovyanGFX | 1.2.36, commit fb494c62e63fdb081c6bdca2761bc552c81bb585 | FreeBSD (LovyanGFX, TFT_eSPI parts), BSD (Adafruit GFX parts), MIT (Adafruit ILI9341 parts): see its `license.txt` | linked |
| espressif/network_provisioning (via the Arduino core's Wi-Fi library) | 1.0.2 | Apache-2.0 | linked (IDF64) |
| espressif/esp_diagnostics (via the Arduino core) | 1.3.4 | Apache-2.0 | linked (IDF64) |
| Other ESP-IDF managed components resolved through the Arduino core (`esp32/idf64/dependencies.lock`, e.g. esp-sr, esp-dl, Zigbee, RainMaker) | as locked | various | downloaded for the IDF64 build, **not linked** into the image |
| X Test Suite (XTS) | commit 6cf94400a09abecd6b86e4eb6441741acecd51f6, cloned in the test container | X.Org licenses | not in the firmware (tests only) |

The X11 vendor string "Woodland Lab experimental X11 (ESP32-S3)" is part of the server's
observable behaviour and is kept unchanged.
