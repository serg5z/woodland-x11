# X11 on ESP32-S3, Milestone 0: host build and tests (macOS or Linux, any C99 compiler).
#   make          build the host server and the unit tests
#   make test     run the C unit tests (ASan/UBSan) and the host end-to-end tests
CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -Werror -pedantic -O1 -g
SAN = -fsanitize=address,undefined

all: build/x11_host_server build/x11_host_server_san build/x11_host_server_trace build/test_latency_trace build/ota_device_sim build/test_ota build/test_setup build/test_requests build/test_core_complete build/test_polyrect build/test_viewer_scroll build/test_stream build/test_event_drain build/test_render build/test_outq build/test_wm build/test_input build/test_copy build/test_colormap build/test_text build/test_spilink build/test_x11spi build/spilink_vectors build/test_shot build/test_mt build/test_gesture build/gesture_replay build/test_gate build/test_touchchain build/test_pclk build/test_board_start build/test_backlight_x11
ifeq ($(shell uname -s),Darwin)
all: build/macos_viewer build/test_viewer_view
VIEWER_TEST = ./build/test_viewer_view
endif

CORE = core/x11_setup.c core/x11_server.c core/x11_window.c core/x11_input.c core/x11_render.c core/x11_outline.c core/x11_font.c core/x11_font_fixed.c core/x11_font_cursor.c core/x11_colors.c
CORE_H = core/x11_setup.h core/x11_server.h core/x11_internal.h core/x11_render.h core/x11_font.h core/x11_colors.h
VIEWER = host/viewer_link.c
VIEWER_H = host/viewer_link.h

MT = mt/x11_mt.c mt/x11_gesture.c mt/x11_gate.c mt/x11_touchchain.c
MT_H = mt/x11_mt.h mt/x11_gesture.h mt/x11_gate.h mt/x11_touchchain.h

INPUT = input/x11_touch.c
INPUT_H = input/x11_touch.h

NET = net/x11_outq.c net/x11_net.c
NET_H = net/x11_outq.h net/x11_net.h

build/x11_host_server: host/x11_host_server.c host/x11_names.h host/png_write.c host/png_write.h $(VIEWER) $(VIEWER_H) $(CORE) $(CORE_H) $(NET) $(NET_H) $(INPUT) $(INPUT_H) $(MT) $(MT_H)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ host/x11_host_server.c host/png_write.c $(VIEWER) $(CORE) $(NET) $(INPUT) $(MT) -lm

# Native AppKit viewer (macOS only); the server and renderer remain the normal host build.
build/macos_viewer: host/macos_viewer.m host/viewer_scroll.c host/viewer_scroll.h
	@mkdir -p build
	clang -fobjc-arc -Wall -Wextra -Werror -framework AppKit -o $@ host/macos_viewer.m host/viewer_scroll.c

# The viewer's view driven with synthetic AppKit events (macOS only; no screen input).
build/test_viewer_view: tests/test_viewer_view.m host/macos_viewer.m host/viewer_scroll.c host/viewer_scroll.h
	@mkdir -p build
	clang -fobjc-arc -Wall -Wextra -Werror -fsanitize=address,undefined -framework AppKit -o $@ tests/test_viewer_view.m host/viewer_scroll.c

build/test_viewer_scroll: tests/test_viewer_scroll.c host/viewer_scroll.c host/viewer_scroll.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_viewer_scroll.c host/viewer_scroll.c

# The same server with AddressSanitizer and UndefinedBehaviorSanitizer, for the stress tests.
build/x11_host_server_san: host/x11_host_server.c host/x11_names.h host/png_write.c host/png_write.h $(VIEWER) $(VIEWER_H) $(CORE) $(CORE_H) $(NET) $(NET_H) $(INPUT) $(INPUT_H) $(MT) $(MT_H)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SAN) -fno-omit-frame-pointer -o $@ host/x11_host_server.c host/png_write.c $(VIEWER) $(CORE) $(NET) $(INPUT) $(MT) -lm

# Host server (ASan/UBSan) with test fonts compiled in from PCF: the XTS fixtures
# (xts5/fonts, tools/xts). Not part of `all`; never in a firmware image.
#   make build/x11_host_server_testfonts TEST_FONT_DIR=/path/to/xts5/fonts
build/test_fonts.c: tools/gen_pcf_fonts.py
	@test -n "$(TEST_FONT_DIR)" || { echo "set TEST_FONT_DIR (a directory with fonts.dir and PCF files)"; exit 1; }
	@mkdir -p build
	python3 tools/gen_pcf_fonts.py $@ $(TEST_FONT_DIR)

build/x11_host_server_testfonts: build/test_fonts.c host/x11_host_server.c host/x11_names.h host/png_write.c host/png_write.h $(VIEWER) $(VIEWER_H) $(CORE) $(CORE_H) $(NET) $(NET_H) $(INPUT) $(INPUT_H) $(MT) $(MT_H)
	$(CC) $(CFLAGS) $(SAN) -fno-omit-frame-pointer -DX11_TEST_FONTS -Icore -o $@ host/x11_host_server.c host/png_write.c $(VIEWER) $(CORE) build/test_fonts.c $(NET) $(INPUT) $(MT) -lm

# Host server with the same fixed-size trace as the test-only firmware profile.
build/x11_host_server_trace: host/x11_host_server.c host/x11_names.h host/png_write.c host/png_write.h $(VIEWER) $(VIEWER_H) $(CORE) $(CORE_H) $(NET) $(NET_H) net/x11_latency_trace.c net/x11_latency_trace.h $(INPUT) $(INPUT_H) $(MT) $(MT_H)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SAN) -fno-omit-frame-pointer -DX11_LATENCY_TRACE -o $@ host/x11_host_server.c host/png_write.c $(VIEWER) $(CORE) $(NET) net/x11_latency_trace.c $(INPUT) $(MT) -lm

build/test_latency_trace: tests/test_latency_trace.c net/x11_latency_trace.c net/x11_latency_trace.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SAN) -DX11_LATENCY_TRACE -o $@ tests/test_latency_trace.c net/x11_latency_trace.c

OTA = ota/x11_ota.c ota/x11_sha256.c
OTA_H = ota/x11_ota.h ota/x11_sha256.h

# Simulated CrowPanel for the OTA tests (flash image file, bootloader model, WLOTA1 engine)
build/ota_device_sim: host/ota_device_sim.c $(OTA) $(OTA_H)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SAN) -o $@ host/ota_device_sim.c $(OTA)

build/test_ota: tests/test_ota.c $(OTA) $(OTA_H)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_ota.c $(OTA)

build/test_setup: tests/test_setup.c core/x11_setup.c core/x11_setup.h host/x11_names.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_setup.c core/x11_setup.c

build/test_requests: tests/test_requests.c $(CORE) $(CORE_H)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_requests.c $(CORE) -lm

build/test_core_complete: tests/test_core_complete.c $(CORE) $(CORE_H)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_core_complete.c $(CORE) -lm

build/test_polyrect: tests/test_polyrect.c $(CORE) $(CORE_H)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_polyrect.c $(CORE) -lm

build/test_event_drain: tests/test_event_drain.c spilink/event_drain.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_event_drain.c

build/test_stream: tests/test_stream.c $(CORE) $(CORE_H)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_stream.c $(CORE) -lm

build/test_spilink: tests/test_spilink.c spilink/spilink.c spilink/spilink.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_spilink.c spilink/spilink.c

build/test_x11spi: tests/test_x11spi.c spilink/x11spi.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_x11spi.c

build/test_pclk: tests/test_pclk.cpp pclk/x11_pclk.cpp pclk/x11_pclk.h pclk/pclk_trial.cpp pclk/pclk_trial.h
	@mkdir -p build
	$(CXX) -std=c++17 -Wall -Wextra -Werror -O1 -g $(SAN) -Ipclk -o $@ tests/test_pclk.cpp pclk/x11_pclk.cpp pclk/pclk_trial.cpp

build/test_board_start: tests/test_board_start.cpp esp32/x11_esp32/crowpanel_board.h
	@mkdir -p build
	$(CXX) -std=c++17 -Wall -Wextra -Werror -O1 -g $(SAN) -o $@ tests/test_board_start.cpp

# The firmware's _WOODLAND_BACKLIGHT code (C++) against the real core (C, compiled separately).
build/test_backlight_x11: tests/test_backlight_x11.cpp esp32/x11_esp32/crowpanel_board.h $(CORE) $(CORE_H)
	@mkdir -p build/backlight_core
	for f in $(CORE); do $(CC) $(CFLAGS) $(SAN) -c $$f -o build/backlight_core/$$(basename $$f .c).o || exit 1; done
	$(CXX) -std=c++17 -Wall -Wextra -Werror -O1 -g $(SAN) -o $@ tests/test_backlight_x11.cpp build/backlight_core/*.o -lm

build/test_touchchain: tests/test_touchchain.c $(MT) $(MT_H) $(CORE) $(CORE_H) $(INPUT) $(INPUT_H)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_touchchain.c $(MT) $(CORE) $(INPUT) -lm

build/test_gate: tests/test_gate.c mt/x11_gate.c mt/x11_gate.h mt/x11_gesture.c mt/x11_mt.c input/x11_touch.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_gate.c mt/x11_gate.c mt/x11_gesture.c mt/x11_mt.c input/x11_touch.c -lm

build/test_gesture: tests/test_gesture.c mt/x11_gesture.c mt/x11_gesture.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_gesture.c mt/x11_gesture.c -lm

build/gesture_replay: tools/gesture_replay.c mt/x11_gesture.c mt/x11_gesture.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tools/gesture_replay.c mt/x11_gesture.c -lm

build/test_mt: tests/test_mt.c mt/x11_mt.c mt/x11_mt.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_mt.c mt/x11_mt.c

build/test_shot: tests/test_shot.c shot/shot_msg.c shot/shot_msg.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_shot.c shot/shot_msg.c

build/spilink_vectors: tools/spilink/spilink_vectors.c spilink/spilink.c spilink/spilink.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SAN) -o $@ tools/spilink/spilink_vectors.c spilink/spilink.c

build/test_colormap: tests/test_colormap.c $(CORE) $(CORE_H)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_colormap.c $(CORE) -lm

build/test_copy: tests/test_copy.c $(CORE) $(CORE_H)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_copy.c $(CORE) -lm

build/test_text: tests/test_text.c $(CORE) $(CORE_H)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_text.c $(CORE) -lm

build/test_render: tests/test_render.c $(CORE) $(CORE_H)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_render.c $(CORE) -lm

build/test_wm: tests/test_wm.c $(CORE) $(CORE_H)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_wm.c $(CORE) -lm

build/test_input: tests/test_input.c $(CORE) $(CORE_H) input/x11_touch.c input/x11_touch.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_input.c input/x11_touch.c $(CORE) -lm

build/test_outq: tests/test_outq.c net/x11_outq.c net/x11_outq.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_outq.c net/x11_outq.c

test: all
	./build/test_outq
	./build/test_setup
	./build/test_requests
	./build/test_core_complete
	./build/test_polyrect
	./build/test_viewer_scroll
	$(VIEWER_TEST)
	./build/test_stream
	./build/test_event_drain
	./build/test_render
	./build/test_copy
	./build/test_colormap
	./build/test_text
	./build/test_spilink
	./build/test_x11spi
	./build/test_shot
	./build/test_mt
	./build/test_gesture
	./build/test_gate
	./build/test_touchchain
	./build/test_pclk
	./build/test_board_start
	./build/test_backlight_x11
	./build/test_wm
	./build/test_input
	./build/test_ota
	./build/test_latency_trace
	python3 -m unittest -v tests/test_host.py tests/test_core_complete_host.py tests/test_viewer_link.py tests/test_fuzz.py tests/test_xclock.py tests/test_wm.py tests/test_touch.py tests/test_mt_host.py tests/test_scale.py tests/test_bench.py tests/test_runtime_profile_bench.py tests/test_latency_trace.py tests/test_ota.py tests/test_spilink_py.py tests/test_touch_diag.py tests/test_screenshot.py tests/test_display_settings.py

clean:
	rm -rf build

.PHONY: all test clean
