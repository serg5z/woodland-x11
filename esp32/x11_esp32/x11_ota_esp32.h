// OTA receiver for the X11 experiment on the ESP32-S3 (WLOTA2, src/x11_ota.c).
//
// - Listens on TCP 5232 only when an OTA key is provisioned (serial OTAKEY), like X11 with its
//   cookie: there is never an unauthenticated firmware endpoint.
// - WLOTA2: the client sends one block (4 or 16 KiB) and waits; the block is received into
//   PSRAM, then committed one 4 KiB sector per loop pass (erase, program: each timed) into the
//   inactive slot only, then acknowledged. Nothing is read from the socket during a commit.
// - Health confirmation: verifyRollbackLater() returns true, so the Arduino core does not
//   mark a new image valid at startup. ota_poll() marks it valid only after the LCD, the X11
//   core, Wi-Fi, the X11 listener and the OTA listener all work and the image has run for
//   OTA_HEALTHY_MS; if that does not happen within OTA_HEALTH_DEADLINE_MS it marks the image
//   invalid and reboots into the previous one. A reset before confirmation (crash, watchdog,
//   power loss) makes the bootloader abort the trial and boot the previous image.
#pragma once
#include <stddef.h>
#include <stdint.h>

static constexpr uint32_t OTA_HEALTHY_MS = 60000;           // run this long, healthy, before confirming
static constexpr uint32_t OTA_HEALTH_DEADLINE_MS = 180000;  // not healthy by then: roll back
static constexpr size_t OTA_READ_BYTES = 4096;              // per socket read (PSRAM buffer)
static constexpr int OTA_READS_PER_PASS = 4;                  // up to 16 KiB received per loop pass

struct OtaHealth {
  bool lcd_ok, core_ok, wifi_ok, x11_listening;
};

// Key from NVS (32 bytes), or nullptr: OTA stays off.
void ota_init(const uint8_t *key);
// Opens/closes the listener with the network (call when Wi-Fi comes or goes).
void ota_network(bool up);
// Once per loop pass: accept, x11_ota_pump (read up to OTA_READS_PER_PASS x OTA_READ_BYTES, never while a
// block is committing; one sector committed per pass; ACKs), health.
void ota_poll(const OtaHealth &h);
// Diagnostics lines ("ota.*") for _WOODLAND_DIAG; never the key.
size_t ota_diag(char *out, size_t cap);
// One-line status for the serial STATUS command.
void ota_print_status();
// Sockets used by OTA (listener + session), for the socket budget.
int ota_sockets();
