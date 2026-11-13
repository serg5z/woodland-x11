file(GLOB X11_CORE "src/*.c")
# Keep the bounded trace module out of ordinary firmware builds. It is linked only
# with the explicitly enabled test runtime profile.
list(FILTER X11_CORE EXCLUDE REGEX "/x11_latency_trace\\.c$")
set(X11_BOARD
  "x11_esp32.cpp" "x11_lcd.cpp" "x11_ota_esp32.cpp" "x11_spi_esp32.cpp"
  "src/x11_pclk.cpp" "src/pclk_trial.cpp" "src/capacity_assert.c")
if(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS)
  list(APPEND X11_BOARD "x11_runtime_profile.cpp" "src/x11_latency_trace.c")
endif()

idf_component_register(SRCS ${X11_CORE} ${X11_BOARD}
  INCLUDE_DIRS "." "src"
  REQUIRES arduino-esp32 LovyanGFX lwip esp_wifi nvs_flash app_update esp_partition esp_timer
    esp_driver_gpio esp_driver_spi)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
  X11_SERVER_CLIENTS=64 X11_NET_REJECT_SLOTS=1 XR_MAX_POLY=512
  X11_SPI_ECHO X11_SPI_X11 X11_SPI_SHOT X11_TOUCH_DIAG X11_PCLK X11_BACKLIGHT)
if(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE X11_RUNTIME_PROFILE X11_LATENCY_TRACE)
endif()
# Existing board code has these legacy warnings; keep them visible without failing this migration build.
target_compile_options(${COMPONENT_LIB} PRIVATE
  -Wno-error=format -Wno-error=format-truncation -Wno-error=misleading-indentation)
