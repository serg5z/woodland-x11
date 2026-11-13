// Woodland Lab X11 experiment for the ESP32-S3 (CrowPanel 4.3"). NOT the production Woodland
// Lab firmware: a separate image that replaces it while flashed.
//
// Milestone 2B: the X11 core's framebuffer is shown on the panel. Boot shows a static test
// pattern before Wi-Fi or X11 start; then the X root window, and whatever clients draw, is
// pushed to the LCD as changed rectangles (x11_lcd.cpp, production panel config, 16 MHz).
// Authentication is mandatory: without a provisioned MIT-MAGIC-COOKIE-1 nothing listens.
// Milestone 2C: window managers run as ordinary clients (twm, Blackbox).
// Milestone 3A: the GT911 touchscreen is the X11 pointer. Every 15 ms loop() reads it with
// production's LovyanGFX call; the touch filter (src/x11_touch.h) turns one finger into
// pointer motion and Button1 (release after 60 ms without a touch, so a lost touch never
// leaves the button down), and the core generates the X events (src/x11_input.c).
// Milestone 3B (diagnostics and client scaling):
//   - connections go through the shared transport (src/x11_net.c, host-tested): lazily grown
//     buffers in PSRAM, one memory budget for connections and core objects, Setup Failed for
//     connections it cannot take (no slot, budget, or internal RAM below the floor);
//   - client slots: X11_SERVER_CLIENTS compiled in (build_esp32.sh, default 6), runtime
//     limit MAXCLIENTS (NVS) up to that;
//   - _WOODLAND_DIAG: read-only root property with heap, PSRAM, stack, Wi-Fi, LCD, touch,
//     core and connection counters (cumulative, plus maxima over the last 5-10 s); read it
//     with any authenticated client (xprop -root _WOODLAND_DIAG, tools/x11_bench.py) - no
//     serial port, so no reset. Never contains the cookie or the Wi-Fi password;
//   - serial output is buffered (4 KiB) so diagnostics do not stall loop().
//
// Serial (115200 baud), one command per line:
//   WIFI_SSID <name>          WIFI_PASS <password>       (stored in NVS "x11exp")
//   COOKIE <32 hex digits>    COOKIE NEW (random cookie, printed once)
//   DISPLAY <n>               (default 0 -> TCP 6000)
//   MAXCLIENTS <n>            runtime client limit 1..X11_SERVER_CLIENTS (NVS; RESTART)
//   OTAKEY NEW | OTAKEY <64 hex> | OTAKEY CLEAR   firmware-update key (NVS; printed once; RESTART)
//   OTA                       OTA status (slots, trial state, counters)
// Milestone 4 (OTA A/B, x11_ota_esp32.cpp): authenticated updates over Wi-Fi into the inactive
// slot, health confirmation and bootloader rollback.
//   STATUS  MEM  CLIENTS  DIAG  FB  FORGET (erase x11exp)  RESTART
//   TOUCH                     touch controller, pointer, poll timing and counters
//   TOUCH ON | TOUCH OFF      touch polling (default on; OFF releases Button1; not stored)
//   TOUCHLOG                  log touches: press/release always, motion at most 10 per second
//   TOUCHCAL x0 x1 y0 y1      raw touch range mapped to 0..799 / 0..479 (stored in NVS)
//   TOUCHCAL RESET            no mapping: production's coordinates (the default)
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <lwip/sockets.h>
#include <netinet/tcp.h>

#include "src/x11_net.h"
#include "src/x11_ota.h"
#include "src/x11_server.h"
#include "src/x11_touch.h"
#include "x11_lcd.h"
#include "x11_ota_esp32.h"
#include "x11_spi_esp32.h"  // empty unless -DX11_SPI_ECHO (SPI echo test image)
#ifdef X11_RUNTIME_PROFILE
#include "x11_runtime_profile.h"
#endif
#ifdef X11_SPI_X11
#include "src/x11spi.h"
#endif

static constexpr size_t OUTQ_MAX = 96 * 1024;            // largest output queue per client (PSRAM)
static constexpr size_t NET_BUDGET = 4u * 1024 * 1024;   // PSRAM for connection buffers + core objects
static constexpr uint16_t MAX_RESOURCES = 1024;          // resource table (all clients)
static constexpr size_t INTERNAL_FREE_MIN = 40 * 1024;   // refuse new clients below this internal free heap
static constexpr size_t INTERNAL_BLOCK_MIN = 12 * 1024;  // ... or below this largest internal block
static constexpr uint32_t REPORT_MS = 60000;
static constexpr uint32_t PATTERN_MS = 3000;
static constexpr uint32_t LCD_BUDGET_US = 8000;          // per loop pass
static constexpr uint32_t REQUEST_BUDGET_US = 6000;      // request processing per loop pass (touch, LCD wait less)
static constexpr int FB_W = 800, FB_H = 480;
static constexpr uint32_t TOUCH_POLL_MS = 15;            // GT911 read interval
static constexpr uint32_t TOUCH_LOG_MS = 100;            // TOUCHLOG: motion lines at most this often

// Maximum over the last 5-10 s (two 5 s buckets): recent peaks without resetting anything.
struct WinMax {
  uint32_t cur = 0, prev = 0, t0 = 0;
  void roll(uint32_t now) {
    if (now - t0 < 5000) return;
    prev = now - t0 < 10000 ? cur : 0;
    cur = 0;
    t0 = now;
  }
  void add(uint32_t v, uint32_t now) {
    roll(now);
    if (v > cur) cur = v;
  }
  uint32_t get(uint32_t now) {
    roll(now);
    return cur > prev ? cur : prev;
  }
};

static Preferences prefs;
static x11_server *server;     // PSRAM
static x11_net *net;           // PSRAM
static uint16_t *framebuffer;  // 800x480 RGB565, PSRAM, shared by the core and the LCD backend
static uint8_t cookie[X11_COOKIE_LEN];
static uint8_t ota_key[X11_OTA_KEY_LEN];
static bool have_ota_key = false;
static bool have_cookie = false;
static String ssid, pass;
static int display_no = 0;
static int max_clients = X11_SERVER_CLIENTS;
static int listen_fd = -1;
#ifdef X11_SPI_X11
static constexpr int SPI_X11_FD = 1000;  // virtual fd: never passed to lwIP/select
static uint8_t spi_x11_in[8192];
static size_t spi_x11_have = 0;
static uint32_t spi_x11_session = 0;
static bool spi_x11_active = false, spi_x11_opened_pending = false;
static uint32_t spi_x11_opens = 0, spi_x11_closes = 0, spi_x11_bytes_in = 0, spi_x11_bytes_out = 0;
#endif
static uint32_t listen_failures = 0, accept_failures = 0, internal_refusals = 0, wifi_losses = 0;
static int last_accept_errno = 0;
static uint32_t last_report = 0;
static wl_status_t last_wifi = WL_IDLE_STATUS;
RTC_NOINIT_ATTR static uint32_t boot_magic, boot_count;  // survive software and watchdog resets

// Touch: filter state and timing (all in loop()).
struct TouchStats {
  uint32_t polls = 0, read_us_max = 0, input_us_max = 0, interval_ms_max = 0, late = 0, events = 0;
  uint64_t read_us_total = 0;
  WinMax interval_win, read_win;
};
static x11_touch touch;
static x11_touch_config touch_cfg;
static TouchStats tstats;
static bool touch_on = true, touch_log = false;
static bool core_ready = false;  // the X11 core is initialised (touch input may go in)
static uint32_t last_touch_poll = 0, last_touch_log = 0;

// Loop timing.
struct LoopStats {
  uint64_t passes = 0;
  uint32_t pass_us_max = 0, net_us_max = 0, lcd_us_max = 0;
  WinMax pass_win, net_win, lcd_win;
};
static LoopStats lstats;

// ---------------------------------------------------------------- platform for the transport

static void *psram_alloc(void *, size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
static void psram_free(void *, void *p) { heap_caps_free(p); }
static uint32_t io_now_ms(void *) { return millis(); }
static uint64_t io_now_us(void *) { return (uint64_t)esp_timer_get_time(); }

static long io_recv(void *, int fd, uint8_t *buf, size_t n) {
#ifdef X11_SPI_X11
  if (fd == SPI_X11_FD) {
    if (!spi_x11_active) return 0;
    if (!spi_x11_have) return X11_IO_WOULD_BLOCK;
    if (n > spi_x11_have) n = spi_x11_have;
    memcpy(buf, spi_x11_in, n);
    spi_x11_have -= n;
    memmove(spi_x11_in, spi_x11_in + n, spi_x11_have);
    return (long)n;
  }
#endif
  int got = lwip_recv(fd, buf, n, MSG_DONTWAIT);
  if (got >= 0) return got;
  return errno == EAGAIN || errno == EWOULDBLOCK ? X11_IO_WOULD_BLOCK : X11_IO_ERROR;
}
static long io_send(void *, int fd, const uint8_t *buf, size_t n) {
#ifdef X11_SPI_X11
  if (fd == SPI_X11_FD) {
    if (!spi_x11_active) return X11_IO_ERROR;
    if (n > XSPI_DATA_MAX) n = XSPI_DATA_MAX;
    if (spi_x11_send(XSPI_DATA, spi_x11_session, buf, n)) return X11_IO_WOULD_BLOCK;
    spi_x11_bytes_out += n;
    return (long)n;
  }
#endif
  int w = lwip_send(fd, buf, n, MSG_DONTWAIT);
  if (w >= 0) return w;
  return errno == EAGAIN || errno == EWOULDBLOCK ? X11_IO_WOULD_BLOCK : X11_IO_ERROR;
}
static void io_close(void *, int fd) {
#ifdef X11_SPI_X11
  if (fd == SPI_X11_FD) {
    if (spi_x11_active) spi_x11_send(XSPI_CLOSE, spi_x11_session, nullptr, 0);
    spi_x11_active = false;
    spi_x11_have = 0;
    spi_x11_opened_pending = false;
    ++spi_x11_closes;
    return;
  }
#endif
  lwip_close(fd);
}

static fd_set rd_set, wr_set;
#ifdef X11_LATENCY_TRACE
static uint64_t select_result_us;
#endif
static int io_ready(void *, int fd, int write) {
#ifdef X11_SPI_X11
  if (fd == SPI_X11_FD) return spi_x11_active && (write || spi_x11_have);
#endif
  return FD_ISSET(fd, write ? &wr_set : &rd_set);
}
#ifdef X11_LATENCY_TRACE
static uint64_t io_read_ready_since_us(void *, int fd) {
#ifdef X11_SPI_X11
  if (fd == SPI_X11_FD) return (uint64_t)esp_timer_get_time();
#else
  (void)fd;
#endif
  return select_result_us;
}
#endif

#ifdef X11_SPI_X11
static void spi_x11_reset() {
  spi_x11_active = false;
  spi_x11_have = 0;
  spi_x11_opened_pending = false;
  x11_net_close_fd(net, SPI_X11_FD, "SPI link lost");
}
static int spi_x11_message(uint8_t op, uint32_t session, const uint8_t *data, size_t n) {
  if (op == XSPI_OPEN) {
    if (!session || !have_cookie) {
      spi_x11_send(XSPI_CLOSE, session, nullptr, 0);
      return 0;
    }
    if (spi_x11_active && session == spi_x11_session) return 0;  // duplicate OPEN
    spi_x11_reset();  // a new host bridge owns the one virtual stream
    spi_x11_session = session;
    spi_x11_active = true;
    spi_x11_have = 0;
    spi_x11_opened_pending = true;
    ++spi_x11_opens;
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < INTERNAL_FREE_MIN ||
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < INTERNAL_BLOCK_MIN) {
      ++internal_refusals;
      x11_net_refuse(net, SPI_X11_FD, 0, 0, "server memory exhausted (internal RAM)");
    } else x11_net_accept(net, SPI_X11_FD, 0, 0);
  } else if (session == spi_x11_session && spi_x11_active) {
    if (op == XSPI_DATA) {
      if (n > sizeof(spi_x11_in) - spi_x11_have) return -1;
      memcpy(spi_x11_in + spi_x11_have, data, n);
      spi_x11_have += n;
      spi_x11_bytes_in += n;
    } else if (op == XSPI_CLOSE) {
      spi_x11_reset();
    }
  }
  return 0;
}
static void spi_x11_controls() {
  if (spi_x11_opened_pending && spi_x11_send(XSPI_OPENED, spi_x11_session, nullptr, 0) == 0)
    spi_x11_opened_pending = false;
}
#endif

static void print_peer(uint32_t ip, uint16_t port) {
  Serial.printf("%u.%u.%u.%u:%u", (unsigned)(ip >> 24 & 255), (unsigned)(ip >> 16 & 255), (unsigned)(ip >> 8 & 255),
                (unsigned)(ip & 255), (unsigned)port);
}

// Connection log: one short line per event, with the caller's address (never credentials).
static void on_event(void *, int slot, const char *what, const char *detail) {
  if (slot >= 0) {
    const x11_conn &k = net->conns[slot];
    Serial.printf("[%d] %s", slot, what);
    if (detail) Serial.printf(" (%s)", detail);
    Serial.print(" from ");
    print_peer(k.ip, k.port);
    if (!strcmp(what, "closed"))
      Serial.printf(" after %u requests, %u errors, queue peak %u", (unsigned)server->clients[slot].requests,
                    (unsigned)server->clients[slot].errors, (unsigned)k.q.peak);
    Serial.println();
  } else {
    unsigned i = (net->refusal_next + X11_NET_HISTORY - 1) % X11_NET_HISTORY;
    Serial.printf("%s (%s) from ", what, detail ? detail : "");
    print_peer(net->refusals[i].ip, net->refusals[i].port);
    Serial.println();
  }
}

// ---------------------------------------------------------------- settings

// ---------------------------------------------------------------- settings

static void load_settings() {
  prefs.begin("x11exp", false);
  ssid = prefs.getString("ssid", "");
  pass = prefs.getString("pass", "");
  display_no = prefs.getUChar("display", 0);
  max_clients = prefs.getUChar("maxcl", X11_SERVER_CLIENTS);
  if (max_clients < 1 || max_clients > X11_SERVER_CLIENTS) max_clients = X11_SERVER_CLIENTS;
  have_cookie = prefs.getBytes("cookie", cookie, sizeof(cookie)) == sizeof(cookie);
  have_ota_key = prefs.getBytes("otakey", ota_key, sizeof(ota_key)) == sizeof(ota_key);
  x11_touch_default_config(&touch_cfg, FB_W, FB_H);
  int16_t cal[4];
  if (prefs.getBytes("tcal", cal, sizeof(cal)) == sizeof(cal)) {
    touch_cfg.raw_x0 = cal[0], touch_cfg.raw_x1 = cal[1], touch_cfg.raw_y0 = cal[2], touch_cfg.raw_y1 = cal[3];
  }
}

static bool parse_hex(const char *s, uint8_t *out, size_t n) {
  if (strlen(s) != 2 * n) return false;
  for (size_t i = 0; i < n; ++i) {
    unsigned v;
    if (sscanf(s + 2 * i, "%2x", &v) != 1) return false;
    out[i] = (uint8_t)v;
  }
  return true;
}


// ---------------------------------------------------------------- diagnostics

static const char *reset_reason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "other_wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    default: return "unknown";
  }
}

// Heap totals and fragmentation (heap_caps_get_info walks each heap, so only when diagnostics
// are read, never per request) and the running image's identity, so every benchmark sample
// names its build.
static size_t mem_diag(char *out, size_t cap) {
  multi_heap_info_t in = {}, ps = {};
  heap_caps_get_info(&in, MALLOC_CAP_INTERNAL);
  heap_caps_get_info(&ps, MALLOC_CAP_SPIRAM);
  const esp_app_desc_t *d = esp_app_get_description();
  char sha[17];
  for (int i = 0; i < 8; ++i) snprintf(sha + 2 * i, 3, "%02x", d->app_elf_sha256[i]);
  int n = snprintf(out, cap,
                   "esp.heap_internal_allocated=%u\nesp.heap_internal_alloc_blocks=%u\nesp.heap_internal_free_blocks=%u\n"
                   "esp.psram_allocated=%u\nesp.psram_alloc_blocks=%u\nesp.psram_free_blocks=%u\n"
                   "esp.app_version=%s\nesp.app_elf_sha256=%s\nesp.idf_version=%s\n",
                   (unsigned)in.total_allocated_bytes, (unsigned)in.allocated_blocks, (unsigned)in.free_blocks,
                   (unsigned)ps.total_allocated_bytes, (unsigned)ps.allocated_blocks, (unsigned)ps.free_blocks,
                   d->version, sha, d->idf_ver);
  if (n < 0) return 0;
  return (size_t)n < cap ? (size_t)n : cap ? cap - 1 : 0;
}

// _WOODLAND_DIAG: generated at each read (key=value lines). Counters are cumulative since
// boot; *_10s are maxima over the last 5-10 s. No credentials, no cookie.
static size_t diag(void *, char *out, size_t cap) {
  uint32_t now = millis();
  const LcdStats &l = lcd_stats();
  int n = snprintf(out, cap,
                   "esp.uptime_ms=%lu\nesp.reset_reason=%s\nesp.boot_count=%lu\n"
                   "esp.heap_internal_free=%u\nesp.heap_internal_min=%u\nesp.heap_internal_largest=%u\n"
                   "esp.psram_free=%u\nesp.psram_min=%u\nesp.psram_largest=%u\nesp.loop_stack_free=%u\n"
                   "esp.wifi_rssi=%d\nesp.wifi_losses=%lu\nesp.listen_failures=%lu\nesp.accept_failures=%lu\n"
                   "esp.last_accept_errno=%d\nesp.internal_refusals=%lu\nesp.clients_compiled=%d\nesp.clients_max=%d\n"
                   "esp.loop_passes=%llu\nesp.loop_pass_us_max=%lu\nesp.loop_pass_us_max_10s=%lu\n"
                   "esp.net_us_max=%lu\nesp.net_us_max_10s=%lu\n"
                   "lcd.ok=%d\nlcd.flushes=%lu\nlcd.pixels=%lu\nlcd.slices=%lu\nlcd.slice_us_avg=%lu\nlcd.slice_us_max=%lu\n"
                   "lcd.flush_us_max_10s=%lu\nlcd.pending_rows=%lu\n"
                   "touch.on=%d\ntouch.polls=%lu\ntouch.read_us_avg=%lu\ntouch.read_us_max=%lu\ntouch.read_us_max_10s=%lu\n"
                   "touch.interval_ms_max=%lu\ntouch.interval_max_10s_ms=%lu\ntouch.late=%lu\ntouch.presses=%lu\n"
                   "touch.releases=%lu\ntouch.button1=%d\ntouch.event_us_max=%lu\n",
                   (unsigned long)now, reset_reason(), (unsigned long)boot_count,
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                   (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                   (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                   (unsigned)uxTaskGetStackHighWaterMark(nullptr), WiFi.status() == WL_CONNECTED ? (int)WiFi.RSSI() : 0,
                   (unsigned long)wifi_losses, (unsigned long)listen_failures, (unsigned long)accept_failures,
                   last_accept_errno, (unsigned long)internal_refusals, X11_SERVER_CLIENTS, max_clients,
                   (unsigned long long)lstats.passes, (unsigned long)lstats.pass_us_max,
                   (unsigned long)lstats.pass_win.get(now), (unsigned long)lstats.net_us_max,
                   (unsigned long)lstats.net_win.get(now), l.ok ? 1 : 0, (unsigned long)l.flushes, (unsigned long)l.pixels,
                   (unsigned long)l.slices, l.slices ? (unsigned long)(l.busy_us / l.slices) : 0UL,
                   (unsigned long)l.max_slice_us, (unsigned long)lstats.lcd_win.get(now), (unsigned long)l.pending_rows,
                   touch_on ? 1 : 0, (unsigned long)tstats.polls,
                   tstats.polls ? (unsigned long)(tstats.read_us_total / tstats.polls) : 0UL,
                   (unsigned long)tstats.read_us_max, (unsigned long)tstats.read_win.get(now),
                   (unsigned long)tstats.interval_ms_max, (unsigned long)tstats.interval_win.get(now),
                   (unsigned long)tstats.late, (unsigned long)touch.presses, (unsigned long)touch.releases,
                   touch.down ? 1 : 0, (unsigned long)tstats.input_us_max);
  size_t used = n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap - 1;
  used += x11_server_stats(server, out + used, cap - used);
  used += x11_net_stats(net, out + used, cap - used);
  used += used < cap ? mem_diag(out + used, cap - used) : 0;
  used += ota_diag(out + used, cap - used);  used += used < cap ? BACKLIGHT_DIAG_TEXT(out + used, cap - used) : 0;
#ifdef X11_SPI_ECHO
  used += spi_echo_diag(out + used, cap - used), used += used < cap ? TOUCH_DIAG_TEXT(out + used, cap - used) : 0, used += used < cap ? PCLK_DIAG_TEXT(out + used, cap - used) : 0;
#ifdef X11_SPI_X11
  if (used < cap) {
    int m = snprintf(out + used, cap - used,
                     "spi.x11.active=%d\nspi.x11.opens=%lu\nspi.x11.closes=%lu\nspi.x11.bytes_in=%lu\nspi.x11.bytes_out=%lu\nspi.x11.pending=%u\n",
                     spi_x11_active ? 1 : 0, (unsigned long)spi_x11_opens, (unsigned long)spi_x11_closes,
                     (unsigned long)spi_x11_bytes_in, (unsigned long)spi_x11_bytes_out, (unsigned)spi_x11_have);
    if (m > 0) used += (size_t)m < cap - used ? (size_t)m : cap - used - 1;
  }
#endif
#endif
#ifdef X11_RUNTIME_PROFILE
  if (used < cap) used += x11_runtime_profile_diag(out + used, cap - used);
#endif
  return used;
}

static void print_mem() {
  Serial.printf("heap internal: free %u, min free %u, largest block %u\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  Serial.printf("psram: free %u of %u, min free %u, largest block %u\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
  Serial.printf("loop task stack high-water mark: %u bytes free\n", (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  if (net)
    Serial.printf("X11 memory (PSRAM): framebuffer %u, LCD panel framebuffer %u, x11_server %u, x11_net %u | budget %u, "
                  "reserved %u (%u per connection), core %u (peak %u), connection buffers %u (peak %u)\n",
                  (unsigned)(FB_W * FB_H * 2), (unsigned)lcd_stats().psram_used, (unsigned)sizeof(x11_server),
                  (unsigned)sizeof(x11_net), (unsigned)net->cfg.budget, (unsigned)net->n.reserved,
                  (unsigned)x11_net_conn_reserve(net), (unsigned)net->n.core_used, (unsigned)net->n.core_peak,
                  (unsigned)net->n.conn_bytes, (unsigned)net->n.conn_bytes_peak);
}

static void print_status() {
  Serial.printf("Woodland X11 experiment | display :%d (TCP %d) | cookie %s | wifi %s", display_no, 6000 + display_no,
                have_cookie ? "set" : "MISSING", ssid.length() ? ssid.c_str() : "(no SSID)");
  if (WiFi.status() == WL_CONNECTED)
    Serial.printf(" connected %s RSSI %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  else
    Serial.printf(" not connected (status %d)\n", (int)WiFi.status());
  if (net) {
    const x11_net_counters &c = net->n;
    Serial.printf("server %s | clients %d/%d (compiled %d) | accepted %u, refused %u slots + %u memory | setup failed %u | "
                  "closed: left %u, protocol %u, overflow %u, errors %u | requests %llu | resources %d | uptime %lus | "
                  "boot %u (%s)\n",
                  listen_fd >= 0 ? "listening" : "not listening", x11_net_open(net), max_clients, X11_SERVER_CLIENTS,
                  (unsigned)c.accepted, (unsigned)c.refused_slots, (unsigned)c.refused_memory, (unsigned)c.setup_failed,
                  (unsigned)c.closed_left, (unsigned)c.closed_protocol, (unsigned)c.closed_overflow,
                  (unsigned)(c.closed_read_error + c.closed_write_error), (unsigned long long)c.requests,
                  x11_resources_used(server), (unsigned long)(millis() / 1000), (unsigned)boot_count, reset_reason());
  }
  const LcdStats &l = lcd_stats();
  Serial.printf("lcd %s | flushes %u rects %u rows %u pixels %u | slices %u avg %u us, last %u us, max %u us | "
                "pending rows %u\n",
                l.ok ? "ok" : "FAILED", l.flushes, l.rects, l.rows, l.pixels, l.slices,
                l.slices ? (unsigned)(l.busy_us / l.slices) : 0, l.last_flush_us, l.max_slice_us, l.pending_rows);
  Serial.printf("touch %s | polls %u, read max %u us, interval max %u ms, late %u | presses %u releases %u | "
                "Button1 %s | pointer grab %d\n",
                touch_on ? "on" : "OFF", tstats.polls, tstats.read_us_max, tstats.interval_ms_max, tstats.late,
                touch.presses, touch.releases, touch.down ? "DOWN" : "up", core_ready ? server->pgrab.client : -1);
}

static void print_clients() {
  for (int i = 0; i < X11_SERVER_CLIENTS; ++i) {
    const x11_conn &k = net->conns[i];
    if (k.fd < 0) continue;
    const x11_client &c = server->clients[k.slot];
    Serial.printf("[%d] ", k.slot);
    print_peer(k.ip, k.port);
    Serial.printf(" %s, %u requests, %u errors, %u resources, input %u/%u, queue %u/%u (peak %u), connected %lus\n",
                  k.set_up ? "set up" : "in setup", c.requests, c.errors, c.resources, (unsigned)k.have, (unsigned)k.in_cap,
                  (unsigned)k.q.len, (unsigned)k.q.cap, (unsigned)k.q.peak,
                  (unsigned long)((millis() - k.since_ms) / 1000));
  }
}

// One line a minute (short, so the serial buffer never stalls loop()).
static void report_line() {
  Serial.printf("up %lus | heap %u/%u | psram %u | stack %u | clients %d | requests %llu | touch late %u | lcd max %u us\n",
                (unsigned long)(millis() / 1000), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)uxTaskGetStackHighWaterMark(nullptr),
                net ? x11_net_open(net) : 0, net ? (unsigned long long)net->n.requests : 0ULL, tstats.late,
                lcd_stats().max_slice_us);
}

// ---------------------------------------------------------------- touch

static void touch_apply(bool changed) {
  if (!changed || !core_ready) return;
  int64_t t0 = esp_timer_get_time();
  x11_pointer_input(server, touch.x, touch.y, touch.down);
  uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
  if (dt > tstats.input_us_max) tstats.input_us_max = dt;
  ++tstats.events;
}

// One GT911 read when due. Bounded: one I2C transaction (about 1 ms at 400 kHz) and the
// events it causes, which go into the clients' bounded queues; nothing waits for the network.
static void touch_poll() {
  if (!touch_on || !core_ready) return;
  uint32_t now = millis();
  uint32_t since = now - last_touch_poll;
  if (since < TOUCH_POLL_MS) return;
  if (tstats.polls) {
    if (since > tstats.interval_ms_max) tstats.interval_ms_max = since;
    tstats.interval_win.add(since, now);
    if (since > 2 * TOUCH_POLL_MS) ++tstats.late;
  }
  last_touch_poll = now;
  int x = 0, y = 0;
  int64_t t0 = esp_timer_get_time();
  int points = lcd_touch_read(x, y);
  uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
  ++tstats.polls;
  tstats.read_us_total += dt;
  if (dt > tstats.read_us_max) tstats.read_us_max = dt;
  tstats.read_win.add(dt, now);
  uint8_t was_down = touch.down;
  bool changed = x11_touch_sample(&touch, now, 1, points, x, y);
  touch_apply(changed);  TOUCH_GESTURE_APPLY(server, touch.down)
  if (touch_log && changed && (touch.down != was_down || now - last_touch_log >= TOUCH_LOG_MS)) {
    last_touch_log = now;
    Serial.printf("touch %s raw %d,%d -> pointer %d,%d\n", touch.down != was_down ? (touch.down ? "press" : "release") : "move",
                  touch.raw_x, touch.raw_y, touch.x, touch.y);
  }
}

static void print_touch() {
  if (!core_ready) {
    Serial.println("touch: not started yet");
    return;
  }
  TouchInfo ti;
  if (lcd_touch_info(ti))
    Serial.printf("touch controller: GT%s fw %04x, config v%u, %ux%u, %u touch points\n", ti.product, ti.fw_version,
                  ti.config_version, ti.x_res, ti.y_res, ti.max_touch);
  else
    Serial.println("touch controller: GT911 registers not readable");
  Serial.printf("touch %s | pointer %d,%d buttons 0x%x | Button1 %s | calibration %s", touch_on ? "on" : "OFF",
                server->ptr_x, server->ptr_y, server->ptr_buttons, touch.down ? "DOWN" : "up",
                touch_cfg.raw_x0 != touch_cfg.raw_x1 ? "" : "none (production coordinates)\n");
  if (touch_cfg.raw_x0 != touch_cfg.raw_x1)
    Serial.printf("x %d..%d y %d..%d\n", touch_cfg.raw_x0, touch_cfg.raw_x1, touch_cfg.raw_y0, touch_cfg.raw_y1);
  Serial.printf("touch polls %u: read avg %u us max %u us | interval max %u ms, late (>%u ms) %u | presses %u releases %u "
                "| pointer updates %u, event generation max %u us | grab client %d%s | input queue %u, dropped %u\n",
                tstats.polls, tstats.polls ? (unsigned)(tstats.read_us_total / tstats.polls) : 0, tstats.read_us_max,
                tstats.interval_ms_max, (unsigned)(2 * TOUCH_POLL_MS), tstats.late, touch.presses, touch.releases,
                tstats.events, tstats.input_us_max, server->pgrab.client, server->frozen ? " (frozen)" : "",
                server->qlen, server->input_dropped);
}


// ---------------------------------------------------------------- serial commands

static void command(char *line) {
  char *arg = strchr(line, ' ');  BACKLIGHT_COMMAND(line)
  if (arg) *arg++ = 0;
  if (!strcmp(line, "WIFI_SSID") && arg) {
    prefs.putString("ssid", arg);
    ssid = arg;
    Serial.println("SSID saved; RESTART to apply");
  } else if (!strcmp(line, "WIFI_PASS") && arg) {
    prefs.putString("pass", arg);
    pass = arg;
    Serial.println("password saved; RESTART to apply");
  } else if (!strcmp(line, "COOKIE") && arg) {
    uint8_t c[X11_COOKIE_LEN];
    if (!strcmp(arg, "NEW")) {
      esp_fill_random(c, sizeof(c));
    } else if (!parse_hex(arg, c, sizeof(c))) {
      Serial.println("COOKIE needs 32 hex digits, or NEW");
      return;
    }
    prefs.putBytes("cookie", c, sizeof(c));
    memcpy(cookie, c, sizeof(c));
    have_cookie = true;
    Serial.print("cookie saved: ");
    for (uint8_t b : c) Serial.printf("%02x", b);
    Serial.println("\n(add it on each client with xauth; RESTART to apply)");
  } else if (!strcmp(line, "DISPLAY") && arg) {
    int d = atoi(arg);
    if (d < 0 || d > 63) { Serial.println("DISPLAY 0..63"); return; }
    prefs.putUChar("display", (uint8_t)d);
    Serial.println("display saved; RESTART to apply");
  } else if (!strcmp(line, "OTAKEY") && arg) {
    uint8_t k[X11_OTA_KEY_LEN];
    if (!strcmp(arg, "CLEAR")) {
      prefs.remove("otakey");
      Serial.println("OTA key removed: OTA off after RESTART");
      return;
    }
    if (!strcmp(arg, "NEW")) {
      esp_fill_random(k, sizeof(k));
    } else if (!parse_hex(arg, k, sizeof(k))) {
      Serial.println("OTAKEY needs 64 hex digits, NEW or CLEAR");
      return;
    }
    prefs.putBytes("otakey", k, sizeof(k));
    Serial.print("OTA key saved: ");  // printed once, for the update host's private key file
    for (uint8_t b : k) Serial.printf("%02x", b);
    Serial.println("\n(keep it in a mode-600 file on the update host; RESTART to apply)");
    memset(k, 0, sizeof(k));
  } else if (!strcmp(line, "OTA")) {
    ota_print_status();
  } else if (!strcmp(line, "MAXCLIENTS") && arg) {
    int m = atoi(arg);
    if (m < 1 || m > X11_SERVER_CLIENTS) { Serial.printf("MAXCLIENTS 1..%d\n", X11_SERVER_CLIENTS); return; }
    prefs.putUChar("maxcl", (uint8_t)m);
    Serial.println("client limit saved; RESTART to apply");
  } else if (!strcmp(line, "DIAG")) {
    // PSRAM, only while the command runs: internal RAM is kept for Wi-Fi and lwIP
    char *text = (char *)heap_caps_malloc(X11_DIAG_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!text) { Serial.println("DIAG: no memory"); return; }
    size_t n = diag(nullptr, text, X11_DIAG_MAX);
    Serial.write((const uint8_t *)text, n);
    heap_caps_free(text);
  } else if (!strcmp(line, "STATUS")) {
    print_status();
    ota_print_status();
  } else if (!strcmp(line, "MEM")) {
    print_mem();
  } else if (!strcmp(line, "CLIENTS")) {
    print_clients();
  } else if (!strcmp(line, "FB")) {
    uint32_t sum = 0;
    for (int i = 0; i < FB_W * FB_H; ++i) sum = sum * 31 + framebuffer[i];
    Serial.printf("framebuffer revision %u, checksum %08lx\n", (unsigned)server->fb_rev, (unsigned long)sum);
  } else if (!strcmp(line, "TOUCH")) {
    if (!arg) {
      print_touch();
    } else if (!strcmp(arg, "ON") || !strcmp(arg, "OFF")) {
      touch_on = !strcmp(arg, "ON");
      if (!touch_on) touch_apply(x11_touch_cancel(&touch));  // never leave Button1 down
      Serial.printf("touch polling %s\n", touch_on ? "on" : "off");
    } else {
      Serial.println("TOUCH, TOUCH ON, TOUCH OFF");
    }
  } else if (!strcmp(line, "TOUCHLOG")) {
    touch_log = !touch_log;
    Serial.printf("touch log %s\n", touch_log ? "on" : "off");
  } else if (!strcmp(line, "TOUCHCAL") && arg) {
    int16_t cal[4] = {0, 0, 0, 0};
    int v[4];
    if (!strcmp(arg, "RESET")) {
      prefs.remove("tcal");
    } else if (sscanf(arg, "%d %d %d %d", &v[0], &v[1], &v[2], &v[3]) == 4 && v[0] != v[1] && v[2] != v[3]) {
      for (int i = 0; i < 4; ++i) cal[i] = (int16_t)v[i];
      prefs.putBytes("tcal", cal, sizeof(cal));
    } else {
      Serial.println("TOUCHCAL <raw x at left> <raw x at right> <raw y at top> <raw y at bottom>, or TOUCHCAL RESET");
      return;
    }
    touch_apply(x11_touch_cancel(&touch));
    touch_cfg.raw_x0 = cal[0], touch_cfg.raw_x1 = cal[1], touch_cfg.raw_y0 = cal[2], touch_cfg.raw_y1 = cal[3];
    x11_touch_config c = touch_cfg;
    x11_touch t0 = touch;
    x11_touch_init(&touch, &c);
    touch.samples = t0.samples, touch.presses = t0.presses, touch.releases = t0.releases;
    Serial.printf("touch calibration %s (saved)\n", cal[0] != cal[1] ? "set" : "removed");
  } else if (!strcmp(line, "FORGET")) {
    prefs.clear();
    Serial.println("x11exp settings erased (Wi-Fi, cookie, display); RESTART");
  } else if (!strcmp(line, "RESTART")) {
    ESP.restart();
  } else {
    Serial.println("commands: WIFI_SSID, WIFI_PASS, COOKIE <hex>|NEW, DISPLAY <n>, MAXCLIENTS <n>, OTAKEY NEW|<hex>|CLEAR, OTA, "
                   "STATUS, MEM, CLIENTS, DIAG, FB, "
                   "FORGET, "
                   "RESTART, TOUCH [ON|OFF], TOUCHLOG, TOUCHCAL x0 x1 y0 y1|RESET");
  }
}

static void serial_poll() {
  static char buf[160];
  static size_t n = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      if (n) {
        buf[n] = 0;
        command(buf);
      }
      n = 0;
    } else if (n < sizeof(buf) - 1) {
      buf[n++] = c;
    }
  }
}

// ---------------------------------------------------------------- X11 transport (src/x11_net.c)

static void open_listener() {
  int fd = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) {
    ++listen_failures;
    return;
  }
  int on = 1;
  lwip_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)(6000 + display_no));
  a.sin_addr.s_addr = htonl(INADDR_ANY);  // the station interface (the only one besides loopback)
  if (lwip_bind(fd, (sockaddr *)&a, sizeof(a)) < 0 || lwip_listen(fd, 4) < 0) {
    ++listen_failures;
    Serial.println("listen failed");
    lwip_close(fd);
    return;
  }
  lwip_fcntl(fd, F_SETFL, lwip_fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
  listen_fd = fd;
  Serial.printf("X11 listening on %s:%d (DISPLAY=%s:%d), MIT-MAGIC-COOKIE-1 required, up to %d clients\n",
                WiFi.localIP().toString().c_str(), 6000 + display_no, WiFi.localIP().toString().c_str(), display_no,
                max_clients);
}

static void close_all(const char *why) {
#ifdef X11_SPI_X11
  // A Wi-Fi loss must not destroy the independent SPI X11 client.
  for (int i = 0; i < X11_SERVER_CLIENTS; ++i)
    if (net->conns[i].fd >= 0 && net->conns[i].fd != SPI_X11_FD) net->conns[i].dead = why;
#else
  x11_net_close_all(net, why);
#endif
  if (listen_fd >= 0) {
    lwip_close(listen_fd);
    listen_fd = -1;
  }
}

static void accept_one() {
  sockaddr_in peer = {};
  socklen_t pl = sizeof(peer);
  int fd = lwip_accept(listen_fd, (sockaddr *)&peer, &pl);
  if (fd < 0) {
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      ++accept_failures;  // e.g. no free lwIP socket (CONFIG_LWIP_MAX_SOCKETS)
      last_accept_errno = errno;
    }
    return;
  }
  int on = 1;
  lwip_setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
  lwip_fcntl(fd, F_SETFL, lwip_fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
  uint32_t ip = ntohl(peer.sin_addr.s_addr);
  uint16_t port = ntohs(peer.sin_port);
  // Wi-Fi and lwIP live in internal RAM: keep a floor, refuse new clients below it.
  if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < INTERNAL_FREE_MIN ||
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < INTERNAL_BLOCK_MIN) {
    ++internal_refusals;
    x11_net_refuse(net, fd, ip, port, "server memory exhausted (internal RAM)");
    return;
  }
  x11_net_accept(net, fd, ip, port);
}

static void network_poll() {
  FD_ZERO(&rd_set);
  FD_ZERO(&wr_set);
  int maxfd = listen_fd;
  FD_SET(listen_fd, &rd_set);
  for (int i = 0; i < X11_SERVER_CLIENTS; ++i) {
    const x11_conn *k = &net->conns[i];
    if (k->fd < 0) continue;
#ifdef X11_SPI_X11
    if (k->fd == SPI_X11_FD) continue;
#endif
    if (k->fd > maxfd) maxfd = k->fd;
    if (x11_net_want_read(net, k)) FD_SET(k->fd, &rd_set);
    if (x11_net_want_write(net, k)) FD_SET(k->fd, &wr_set);
  }
  for (int i = 0; i < X11_NET_REJECT_SLOTS; ++i)
    if (net->rejects[i].fd >= 0
#ifdef X11_SPI_X11
        && net->rejects[i].fd != SPI_X11_FD
#endif
    ) {
      FD_SET(net->rejects[i].fd, &rd_set);
      if (net->rejects[i].fd > maxfd) maxfd = net->rejects[i].fd;
    }
  // short wait: the LCD flush runs after this every loop pass
  timeval tv = {0, x11_net_ready(net) || lcd_stats().pending_rows ? 0 : 5000};
#ifdef X11_RUNTIME_PROFILE
  int64_t select_t0 = esp_timer_get_time();
  int selected = lwip_select(maxfd + 1, &rd_set, &wr_set, nullptr, &tv);
  x11_runtime_profile_select_wait((uint32_t)(esp_timer_get_time() - select_t0));
  if (selected < 0) return;
#else
  if (lwip_select(maxfd + 1, &rd_set, &wr_set, nullptr, &tv) < 0) return;
#endif
#ifdef X11_LATENCY_TRACE
  select_result_us = (uint64_t)esp_timer_get_time();
#endif
  if (FD_ISSET(listen_fd, &rd_set)) accept_one();
#ifndef X11_SPI_X11
  x11_net_pass(net, io_ready);
#endif
}

// ---------------------------------------------------------------- setup / loop

void setup() {
  Serial.setTxBufferSize(4096);  // diagnostics queue up instead of stalling loop()
  Serial.begin(115200);
  delay(200);
  if (boot_magic != 0x57D1A600u) boot_magic = 0x57D1A600u, boot_count = 0;
  ++boot_count;
  Serial.printf("\nWoodland Lab X11 experiment (ESP32-S3) - NOT the production firmware | boot %u, reset %s\n",
                (unsigned)boot_count, reset_reason());
  load_settings();
  framebuffer = (uint16_t *)heap_caps_malloc((size_t)FB_W * FB_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  server = (x11_server *)heap_caps_malloc(sizeof(x11_server), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  net = (x11_net *)heap_caps_malloc(sizeof(x11_net), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!framebuffer || !server || !net) {
    Serial.println("PSRAM allocation failed");
    for (;;) delay(1000);
  }
  // 1. the panel and a static test pattern, before Wi-Fi or X11
  lcd_begin();
  lcd_test_pattern(framebuffer);  PCLK_TRIAL_DIALOG(framebuffer)
  Serial.printf("test pattern shown for %u s\n", (unsigned)(PATTERN_MS / 1000));
  uint32_t t0 = millis();
  while (millis() - t0 < PATTERN_MS) {
    serial_poll();
    delay(20);
  }
  // 2. the transport and the X11 core on the same framebuffer: it paints the root and
  // reports the whole screen
  x11_server_config cfg;
  x11_default_config(&cfg);
  cfg.cookie = have_cookie ? cookie : nullptr;
  cfg.max_clients = (uint16_t)max_clients;
  cfg.max_resources = MAX_RESOURCES;
  x11_net_config ncfg;
  x11_net_default_config(&ncfg, OUTQ_MAX, NET_BUDGET);
  ncfg.request_budget_us = REQUEST_BUDGET_US;
  x11_net_ops ops = {};
  ops.recv = io_recv, ops.send = io_send, ops.close = io_close, ops.now_ms = io_now_ms, ops.now_us = io_now_us;
#ifdef X11_LATENCY_TRACE
  ops.read_ready_since_us = io_read_ready_since_us;
#endif
  ops.alloc = psram_alloc, ops.release = psram_free, ops.on_event = on_event;
  x11_platform plat = {};
  plat.now_ms = io_now_ms;
  plat.framebuffer = framebuffer;
  plat.diag = diag;
  if (x11_net_init(net, server, &ncfg, &ops, &plat) != 0 || x11_server_init_platform(server, &cfg, &plat) != 0) {
    Serial.println("X11 core: out of PSRAM");
    for (;;) delay(1000);
  }
  x11_touch_init(&touch, &touch_cfg);
  last_touch_poll = millis();
  core_ready = true;
  ota_init(have_ota_key ? ota_key : nullptr);
#ifdef X11_SPI_ECHO
  spi_echo_init(), SPI_SHOT_SOURCE(framebuffer, &server->fb_rev, FB_W, FB_H);  // no new lines: normal builds stay identical
#ifdef X11_SPI_X11
  spi_x11_init(spi_x11_message, spi_x11_reset);
#endif
#endif
  if (!have_ota_key) Serial.println("no OTA key: network updates are off (OTAKEY NEW, then RESTART)");
  print_mem();
  if (!have_cookie) Serial.println("no cookie: the X11 server will not listen. Use COOKIE NEW (or COOKIE <hex>), then RESTART");
  if (!ssid.length()) Serial.println("no Wi-Fi: use WIFI_SSID <name> and WIFI_PASS <password>, then RESTART");
  // 3. Wi-Fi
  if (ssid.length()) {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);  // lower latency for request/reply round trips
    WiFi.begin(ssid.c_str(), pass.c_str());
  }
  print_status();
}

void loop() {
#ifdef X11_RUNTIME_PROFILE
  x11_runtime_profile_poll();
#endif
  int64_t pass0 = esp_timer_get_time();
  uint32_t now = millis();
  serial_poll();
  touch_poll();
  wl_status_t w = WiFi.status();
  if (w != last_wifi) {
    if (last_wifi == WL_CONNECTED) ++wifi_losses;
    last_wifi = w;
    if (w == WL_CONNECTED) Serial.printf("Wi-Fi connected: %s\n", WiFi.localIP().toString().c_str());
    else if (listen_fd >= 0) close_all("Wi-Fi lost");
    ota_network(w == WL_CONNECTED);
  }
  // Mandatory authentication: never listen without a cookie.
  if (w == WL_CONNECTED && have_cookie && server->cfg.cookie && listen_fd < 0) open_listener();
  if (listen_fd >= 0) {
    int64_t t = esp_timer_get_time();
    network_poll();
    uint32_t dt = (uint32_t)(esp_timer_get_time() - t);
    if (dt > lstats.net_us_max) lstats.net_us_max = dt;
    lstats.net_win.add(dt, now);
  } else {
    delay(5);
  }
  touch_poll();  // again here: request processing may have taken a while
  int64_t t = esp_timer_get_time();
  lcd_flush(server, LCD_BUDGET_US);  // changed areas only, bounded time per pass
  uint32_t lcd_us = (uint32_t)(esp_timer_get_time() - t);
  if (lcd_us > lstats.lcd_us_max) lstats.lcd_us_max = lcd_us;
  lstats.lcd_win.add(lcd_us, now);
  OtaHealth h = {lcd_stats().ok, core_ready, w == WL_CONNECTED, listen_fd >= 0};
  ota_poll(h);  PCLK_POLL(server) BACKLIGHT_POLL(server)  // at most one flash sector per pass; health confirmation of a new image
#ifdef X11_SPI_ECHO
  spi_echo_poll();
#ifdef X11_SPI_X11
  spi_x11_controls();
#endif
#endif
#ifdef X11_SPI_X11
  x11_net_pass(net, io_ready);
#endif
  if (millis() - last_report >= REPORT_MS) {
    last_report = millis();
    report_line();
  }
  uint32_t pass_us = (uint32_t)(esp_timer_get_time() - pass0);
  ++lstats.passes;
  if (pass_us > lstats.pass_us_max) lstats.pass_us_max = pass_us;
  lstats.pass_win.add(pass_us, now);
}
