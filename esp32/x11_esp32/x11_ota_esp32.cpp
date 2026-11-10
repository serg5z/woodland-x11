// OTA receiver for the X11 experiment on the ESP32-S3; see x11_ota_esp32.h.
#include "x11_ota_esp32.h"

#include <Arduino.h>
#include <esp_app_desc.h>
#include <esp_flash_partitions.h>
#include <esp_heap_caps.h>
#include <esp_image_format.h>
#include <esp_timer.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_random.h>
#include <lwip/sockets.h>
#include <netinet/tcp.h>

#include "src/x11_ota.h"

// The Arduino core marks a new image valid in initArduino() unless this returns true; the
// health check below decides instead.
extern "C" bool verifyRollbackLater() { return true; }

// Kept in the image (and found by tools/ota_push.py and the receiver): firmware with this
// marker can be updated over the network again.
extern "C" __attribute__((used)) const char wl_ota_marker[] = X11_OTA_MARKER;

static bool enabled = false;
static int listen_fd = -1, sess_fd = -1;
static x11_ota_endpoint endpoint;
static x11_ota_session *sess_mem;  // PSRAM (internal RAM is kept for Wi-Fi and lwIP)
#define session (*sess_mem)
static const esp_partition_t *target = nullptr;
static uint32_t target_size = 0, erased_upto = 0;
static bool reboot_pending = false;
static uint32_t reboot_at = 0;
static uint8_t outbuf[2 * X11_OTA_REPLY_LEN + X11_OTA_HELLO_LEN];
static uint8_t *inbuf;      // OTA_READ_BYTES, PSRAM
static uint8_t *block_buf;  // X11_OTA_MAX_BLOCK, PSRAM
static const char *health_note = "";
static uint32_t healthy_since = 0;
static uint32_t verdict_rollbacks = 0;
static_assert(OTA_HEALTHY_MS == X11_OTA_HEALTHY_MS && OTA_HEALTH_DEADLINE_MS == X11_OTA_HEALTH_DEADLINE_MS,
              "trial timing: one definition (ota/x11_ota.h)");
#ifdef X11_OTA_TEST_NO_CONFIRM
static constexpr int OTA_NO_CONFIRM = 1;  // M4 rollback test build: never confirms; NOT for normal use
#else
static constexpr int OTA_NO_CONFIRM = 0;
#endif

// ---------------------------------------------------------------- platform ops (esp_ota_*)

static const char *state_name(const esp_partition_t *p) {
  esp_ota_img_states_t st;
  if (!p || esp_ota_get_state_partition(p, &st) != ESP_OK) return "none";
  switch (st) {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    default: return "undefined";
  }
}

static const esp_partition_t *inactive() {
  const esp_partition_t *run = esp_ota_get_running_partition();
  const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
  return next && next != run && next->address != run->address ? next : nullptr;  // single-slot table: none
}

static void op_slots(void *, x11_ota_slots *o) {
  const esp_partition_t *run = esp_ota_get_running_partition(), *other = inactive();
  o->has_inactive = other != nullptr;
  o->inactive_size = other ? other->size : 0;
  o->running = run && run->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1 ? 1 : 0;
  snprintf(o->text, sizeof(o->text), "running=%s %s other=%s %s", run ? run->label : "?", state_name(run),
           other ? other->label : "none", other ? state_name(other) : "");
}

// esp_ota_begin(OTA_WITH_SEQUENTIAL_WRITES) erases nothing, refuses to start while the running
// image is unconfirmed, and invalidates the otadata entry of the target slot (a half-written
// slot is never selected). Its handle is released at once: the sectors are erased and
// programmed below with the partition API, each exactly once and separately timed
// (esp_ota_write would erase+program in one call; esp_ota_write_with_offset asserts on a
// sequential-mode handle; esp_ota_begin with a size erases everything in one long stall).
static int op_begin(void *, uint32_t size) {
  target = inactive();
  if (!target || size > target->size) return -1;
  esp_ota_handle_t h = 0;
  if (esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK) return -1;
  esp_ota_abort(h);
  target_size = size, erased_upto = 0;
  return 0;
}

static int op_write_sector(void *, uint32_t off, const uint8_t *d, size_t n, uint32_t *erase_us, uint32_t *program_us) {
  if (!target || off % X11_OTA_SECTOR || n > X11_OTA_SECTOR || off != erased_upto || off + n > target_size) return -1;
  int64_t t0 = esp_timer_get_time();
  if (esp_partition_erase_range(target, off, X11_OTA_SECTOR) != ESP_OK) return -1;
  int64_t t1 = esp_timer_get_time();
  erased_upto = off + X11_OTA_SECTOR;
  if (esp_partition_write(target, off, d, n) != ESP_OK) return -1;
  int64_t t2 = esp_timer_get_time();
  *erase_us = (uint32_t)(t1 - t0), *program_us = (uint32_t)(t2 - t1);
  return 0;
}

static int op_finish(void *, uint32_t size) {  // segments, checksum, appended SHA-256 (as esp_ota_end does)
  if (!target) return -1;
  esp_partition_pos_t pos = {target->address, target->size};
  esp_image_metadata_t meta = {};
  return esp_image_verify(ESP_IMAGE_VERIFY, &pos, &meta) == ESP_OK && meta.image_len == size ? 0 : -1;
}

static void op_abort(void *) {  // the partly written slot stays unselected (its otadata entry is invalid)
  target_size = erased_upto = 0;
}

static int op_activate(void *) { return target && esp_ota_set_boot_partition(target) == ESP_OK ? 0 : -1; }

static int op_boot_other(void *) {
  const esp_partition_t *other = inactive();
  esp_app_desc_t d;
  if (!other || esp_ota_get_partition_description(other, &d) != ESP_OK) return -1;
  return esp_ota_set_boot_partition(other) == ESP_OK ? 0 : -1;  // verifies the image first
}

static void op_reboot(void *) {
  reboot_pending = true;
  reboot_at = millis() + 300;  // let the last reply leave
}

static void op_random(void *, uint8_t *o, size_t n) { esp_fill_random(o, n); }
static uint32_t op_now(void *) { return millis(); }
static uint64_t op_now_us(void *) { return (uint64_t)esp_timer_get_time(); }
static const char *op_idf(void *) { return esp_app_get_description()->idf_ver; }

static x11_ota_ops OPS = {op_slots, op_begin, op_write_sector, op_finish, op_abort, op_activate, op_boot_other,
                          op_reboot, op_random, op_now, op_now_us, op_idf, nullptr, nullptr};

// ---------------------------------------------------------------- transport

void ota_init(const uint8_t *key) {
  sess_mem = (x11_ota_session *)heap_caps_calloc(1, sizeof(x11_ota_session), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  inbuf = (uint8_t *)heap_caps_malloc(OTA_READ_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  block_buf = (uint8_t *)heap_caps_malloc(X11_OTA_MAX_BLOCK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!sess_mem || !inbuf || !block_buf) {
    Serial.println("OTA: no PSRAM for the receiver: OTA off");
    enabled = false;
    return;
  }
  OPS.block_buf = block_buf;
  enabled = key != nullptr;
  if (enabled) x11_ota_endpoint_init(&endpoint, key);
  memset(&session, 0, sizeof(session));
  session.state = X11_OTA_S_DONE;
}

static void close_session() {
  if (sess_fd < 0) return;
  x11_ota_closed(&session);
  lwip_close(sess_fd);
  sess_fd = -1;
}

void ota_network(bool up) {
  if (!enabled) return;
  if (!up) {
    close_session();
    if (listen_fd >= 0) lwip_close(listen_fd), listen_fd = -1;
    return;
  }
  if (listen_fd >= 0) return;
  int fd = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) return;
  int on = 1;
  lwip_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(X11_OTA_PORT);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (lwip_bind(fd, (sockaddr *)&a, sizeof(a)) < 0 || lwip_listen(fd, 1) < 0) {
    lwip_close(fd);
    return;
  }
  lwip_fcntl(fd, F_SETFL, lwip_fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
  listen_fd = fd;
  Serial.printf("OTA listening on TCP %d (WLOTA2, key required)\n", X11_OTA_PORT);
  if (OTA_NO_CONFIRM) Serial.println("OTA: TEST BUILD (X11_OTA_TEST_NO_CONFIRM): this image never confirms itself");
}

int ota_sockets() { return (listen_fd >= 0) + (sess_fd >= 0); }

static void send_all(const uint8_t *p, size_t n) {  // the HELLO (one 64-byte message)
  while (n && sess_fd >= 0) {
    int w = lwip_send(sess_fd, p, n, 0);
    if (w <= 0) return;
    p += w, n -= (size_t)w;
  }
}

// Socket callbacks for x11_ota_pump (the shared transport loop, host-tested).
static long io_recv(void *, uint8_t *buf, size_t n) {
  int got = lwip_recv(sess_fd, buf, n, MSG_DONTWAIT);
  if (got > 0) return got;
  if (got == 0) return 0;
  return errno == EAGAIN || errno == EWOULDBLOCK ? -1 : -2;
}

static long io_send(void *, const uint8_t *buf, size_t n) {  // 96-byte messages; wait up to 2 s for room
  uint32_t t0 = millis();
  for (;;) {
    int w = lwip_send(sess_fd, buf, n, MSG_DONTWAIT);
    if (w > 0) return w;
    if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return -2;
    if (millis() - t0 > 2000) return -2;
    delay(1);
  }
}

static void health(const OtaHealth &h) {
  const esp_partition_t *run = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  if (!run || esp_ota_get_state_partition(run, &st) != ESP_OK || st != ESP_OTA_IMG_PENDING_VERIFY) {
    health_note = "confirmed";
    return;
  }
  uint32_t now = millis();
  bool ok = h.lcd_ok && h.core_ok && h.wifi_ok && h.x11_listening && listen_fd >= 0;
  health_note = ok ? (OTA_NO_CONFIRM ? "trial: healthy (test build: never confirms)" : "trial: healthy")
                   : "trial: waiting for LCD, X11, Wi-Fi and listeners";
  int verdict = x11_ota_trial_step(ok, now, &healthy_since, esp_ota_check_rollback_is_possible(), OTA_NO_CONFIRM);
  if (verdict == X11_OTA_TRIAL_CONFIRM) {
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) Serial.println("OTA: new firmware confirmed (healthy)");
  } else if (verdict == X11_OTA_TRIAL_ROLLBACK) {
    ++verdict_rollbacks;
    Serial.println("OTA: new firmware not healthy in time: rolling back");
    Serial.flush();
    esp_ota_mark_app_invalid_rollback_and_reboot();  // does not return when it works
  }
}

void ota_poll(const OtaHealth &h) {
  health(h);
  if (reboot_pending && (int32_t)(millis() - reboot_at) >= 0) {
    Serial.println("OTA: rebooting");
    Serial.flush();
    esp_restart();
  }
  if (!enabled || listen_fd < 0) return;
  int fd = lwip_accept(listen_fd, nullptr, nullptr);
  if (fd >= 0) {
    if (sess_fd >= 0 || reboot_pending) {  // one session at a time
      uint8_t busy[X11_OTA_REPLY_LEN] = {'W', 'L', 'O', 'T', 'A', '2', 'R', 'S', X11_OTA_BUSY};
      lwip_send(fd, busy, sizeof(busy), MSG_DONTWAIT);
      lwip_close(fd);
    } else {
      int on = 1;
      lwip_setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
      lwip_fcntl(fd, F_SETFL, lwip_fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
      sess_fd = fd;
      size_t n = x11_ota_open(&session, &endpoint, &OPS, outbuf, sizeof(outbuf));
      send_all(outbuf, n);
    }
  }
  if (sess_fd < 0) return;
  // the buffer length is passed explicitly (M3: a sizeof(pointer) here read 4 bytes per pass)
  x11_ota_io io = {io_recv, io_send, nullptr, inbuf, OTA_READ_BYTES, OTA_READS_PER_PASS};
  if (!x11_ota_pump(&session, &io)) {  // the session is over: client gone, error, or finished
    lwip_shutdown(sess_fd, SHUT_WR);
    close_session();  // aborts an unfinished upload; reboots if one was requested
  }
}

size_t ota_diag(char *out, size_t cap) {
  if (!sess_mem) return 0;
  const esp_partition_t *run = esp_ota_get_running_partition(), *other = inactive();
  int n = snprintf(out, cap,
                   "ota.enabled=%d\nota.listening=%d\nota.running=%s\nota.running_state=%s\nota.other=%s\n"
                   "ota.other_state=%s\nota.health=%s\nota.sessions=%lu\nota.auth_failures=%lu\nota.lockouts=%lu\n"
                   "ota.uploads_ok=%lu\nota.uploads_failed=%lu\nota.aborted=%lu\nota.last_status=%lu\n"
                   "ota.health_rollbacks=%lu\nota.receiver=%s\nota.protocol=WLOTA2\nota.block_max=%u\nota.test_build=%s\n",
                   enabled ? 1 : 0, listen_fd >= 0 ? 1 : 0, run ? run->label : "?", state_name(run),
                   other ? other->label : "none", other ? state_name(other) : "none", health_note,
                   (unsigned long)endpoint.sessions, (unsigned long)endpoint.auth_failures,
                   (unsigned long)endpoint.lockouts, (unsigned long)endpoint.uploads_ok,
                   (unsigned long)endpoint.uploads_failed, (unsigned long)endpoint.aborted,
                   (unsigned long)endpoint.last_status, (unsigned long)verdict_rollbacks, wl_ota_marker,
                   (unsigned)X11_OTA_MAX_BLOCK, OTA_NO_CONFIRM ? "no-confirm" : "none");
  if (n < 0) return 0;
  size_t used = (size_t)n < cap ? (size_t)n : cap ? cap - 1 : 0;
  return used + x11_ota_stats_text(&endpoint, out + used, cap - used);
}

void ota_print_status() {
  if (!sess_mem) return;
  const esp_partition_t *run = esp_ota_get_running_partition(), *other = inactive();
  Serial.printf("ota %s | running %s (%s) | other %s (%s) | %s | uploads %lu ok, %lu failed | auth failures %lu\n",
                enabled ? (listen_fd >= 0 ? "listening" : "on") : "OFF (no OTAKEY)", run ? run->label : "?",
                state_name(run), other ? other->label : "none", other ? state_name(other) : "-", health_note,
                (unsigned long)endpoint.uploads_ok, (unsigned long)endpoint.uploads_failed,
                (unsigned long)endpoint.auth_failures);
}
