# ESP-IDF lwIP socket capacity proof

Build-only proof for the SDK feasibility report. It makes no network connections and performs no
flashing. `capacity_assert.c` is compiled with IDF's lwIP VFS and fails if generated Kconfig,
socket/netconn pools, TCP PCB pools, or fd geometry do not meet the checked values.

Tested with ESP-IDF v5.5.5, Arduino-ESP32 3.3.12 as an IDF component, and ESP32-S3. Install the
Arduino Board Manager core 3.3.12 or use the Component Manager. The simplest reproduction of the
tested local-component setup on macOS is:

```sh
cd experiments/x11-esp32/tests/idf_socket_capacity_poc
mkdir -p components
ln -s "$HOME/Library/Arduino15/packages/esp32/hardware/esp32/3.3.12" \
  components/arduino-esp32
```

Then install and export ESP-IDF v5.5.5 (see the repository README, "Firmware builds"),
and run:

```sh
idf.py set-target esp32s3
idf.py reconfigure
grep -E 'CONFIG_(LWIP_MAX_SOCKETS|LWIP_MAX_ACTIVE_TCP|LWIP_MAX_LISTENING_TCP|FREERTOS_HZ)' sdkconfig
idf.py build
idf.py size
```

Expected effective values are 72 sockets, 72 active PCBs, four listening PCBs, and a 1000 Hz
FreeRTOS tick. The C assertions also check generated netconn/PCB mappings and the IDF VFS
descriptor limits. This is a compile-time capacity/configuration check only: it does not measure
dynamic lwIP heap use or demonstrate 72 live TCP clients. The generated `build/` and `sdkconfig`
are local outputs; use the included defaults and pinned dependency lock as the source of truth.
