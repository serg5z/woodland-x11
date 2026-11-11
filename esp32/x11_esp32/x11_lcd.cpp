// LCD backend, see x11_lcd.h. The bring-up follows the board's verified initialisation order
// (LovyanGFX init, DMA, a black screen, then frame pushes). Nothing here changes pins,
// timings, the pixel clock, PSRAM or flash settings.
#include "x11_lcd.h"

#include <Arduino.h>
#include <Wire.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include "crowpanel_board.h"
#include "crowpanel_lcd.h"
#include "src/x11_font.h"
#include "src/x11_render.h"

static LGFX gfx;
static LcdStats stats;

static constexpr int W = 800, H = 480;

// The board's I2C devices through Arduino's Wire and GPIO calls (crowpanel_board.h).
struct BoardIo {
  bool answers(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
  }
  void send(uint8_t addr, uint8_t byte) {
    Wire.beginTransmission(addr);
    Wire.write(byte);
    bool acked = Wire.endTransmission() == 0;
    Serial.printf("board: 0x%02x <- %u %s\n", addr, byte, acked ? "ok" : "not acknowledged");
  }
  void pin_low(int gpio) {
    pinMode(gpio, OUTPUT);
    digitalWrite(gpio, LOW);
  }
  void pin_release(int gpio) { pinMode(gpio, INPUT); }
  void wait_ms(int ms) { delay(ms); }
  void log(const char *text) { Serial.println(text); }
};

bool lcd_begin() {  LCD_BEGIN_HOOK()  // the pixel clock (test builds)
  Wire.begin(crowpanel43_board::SDA_GPIO, crowpanel43_board::SCL_GPIO);
  BoardIo io;
  stats.backlight_found = crowpanel43_board::start(io);
  size_t before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  stats.ok = gfx.init();
  gfx.initDMA();
  gfx.startWrite();
  gfx.fillScreen(TFT_BLACK);
  stats.psram_used = (uint32_t)(before - heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  if (gfx.getStartCount() > 0) gfx.endWrite();  // pushImage runs outside a write transaction
  Serial.printf("LCD %s, 800x480 RGB565, pixel clock 16 MHz, panel framebuffer %u B PSRAM\n",
                stats.ok ? "ready" : "INIT FAILED", (unsigned)stats.psram_used);
  return stats.ok;
}

static void push_rows(const uint16_t *fb, int x1, int x2, int y1, int y2) {
  if (x1 == 0 && x2 == W) {  // whole rows are contiguous: one call
    gfx.pushImage(0, y1, W, y2 - y1, (lgfx::rgb565_t *)(fb + (size_t)y1 * W));
  } else {
    for (int y = y1; y < y2; ++y)
      gfx.pushImage(x1, y, x2 - x1, 1, (lgfx::rgb565_t *)(fb + (size_t)y * W + x1));
  }
  stats.rows += (uint32_t)(y2 - y1);
  stats.pixels += (uint32_t)((x2 - x1) * (y2 - y1));
}

void lcd_push_all(const uint16_t *fb) {
  if (!stats.ok) return;
  push_rows(fb, 0, W, 0, H);
}

// Damage being pushed: rectangles taken from the core, worked through row by row.
static xr_rect pending[X11_DAMAGE_RECTS];
static int npending = 0, current = 0, next_row = 0;

void lcd_flush(x11_server *s, uint32_t budget_us) {
  if (!stats.ok) return;
  int64_t t0 = esp_timer_get_time();
  if (current >= npending) {
    npending = x11_take_damage(s, pending, X11_DAMAGE_RECTS);
    current = 0;
    next_row = npending ? pending[0].y1 : 0;
    if (!npending) {
      stats.pending_rows = 0;
      return;
    }
    ++stats.flushes;
    stats.rects += (uint32_t)npending;
  }
  const uint16_t *fb = (const uint16_t *)s->fb.pix;
  while (current < npending) {
    const xr_rect &r = pending[current];
    int x1 = r.x1 < 0 ? 0 : r.x1, x2 = r.x2 > W ? W : r.x2;
    int y_end = r.y2 > H ? H : r.y2;
    if (next_row < 0) next_row = 0;
    // a slice: 16 rows (or fewer); more slices while the budget lasts
    int y2 = next_row + 16 < y_end ? next_row + 16 : y_end;
    if (x1 < x2 && next_row < y2) push_rows(fb, x1, x2, next_row, y2);
    next_row = y2;
    if (next_row >= y_end) {
      ++current;
      if (current < npending) next_row = pending[current].y1;
    }
    if ((uint32_t)(esp_timer_get_time() - t0) >= budget_us) break;
  }
  uint32_t left = 0;
  for (int i = current; i < npending; ++i) left += (uint32_t)(pending[i].y2 - (i == current ? next_row : pending[i].y1));
  stats.pending_rows = left;
  uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
  stats.last_flush_us = dt;
  if (dt > stats.max_slice_us) stats.max_slice_us = dt;
  ++stats.slices;
  stats.busy_us += dt;
}

const LcdStats &lcd_stats() { return stats; }

// ---------------------------------------------------------------- touch

int lcd_touch_read(int &x, int &y) {
  if (!stats.ok) return 0;  TOUCH_DIAG_HOOK(x, y)  // expands to no tokens in normal builds
  lgfx::touch_point_t tp[2];
  int count = (int)gfx.getTouch(tp, 2);
  if (count > 0) {
    x = tp[0].x;
    y = tp[0].y;
  }
  return count;
}

static bool gt911_read(uint16_t reg, uint8_t *buf, size_t n) {  // GT911 register read (datasheet)
  Wire.beginTransmission(0x5D);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint16_t)0x5D, n, true) != n) return false;
  for (size_t i = 0; i < n; ++i) buf[i] = (uint8_t)Wire.read();
  return true;
}

bool lcd_touch_info(TouchInfo &o) {
  uint8_t id[6], cfg[6];
  if (!gt911_read(0x8140, id, sizeof(id)) || !gt911_read(0x8047, cfg, sizeof(cfg))) return false;
  for (int i = 0; i < 4; ++i) o.product[i] = (id[i] >= 0x20 && id[i] < 0x7F) ? (char)id[i] : '\0';
  o.product[4] = '\0';
  o.fw_version = (uint16_t)(id[4] | id[5] << 8);
  o.config_version = cfg[0];
  o.x_res = (uint16_t)(cfg[1] | cfg[2] << 8);
  o.y_res = (uint16_t)(cfg[3] | cfg[4] << 8);
  o.max_touch = cfg[5] & 0x0F;
  return true;
}

// ---------------------------------------------------------------- test pattern

static void text(const xr_surface *s, const xr_region *clip, int x, int y, const char *str, uint16_t color) {
  xr_paint p = {3, 0xFFFFFFFFu, color, 0};
  for (int i = 0; str[i]; ++i) {
    int c = (unsigned char)str[i];
    if (c < X11_FONT_FIRST || c > X11_FONT_LAST) c = ' ';
    const uint8_t *rows = x11_font_fixed_rows[c - X11_FONT_FIRST];
    for (int ry = 0; ry < X11_FONT_H; ++ry)
      for (int rx = 0; rx < X11_FONT_W; ++rx)
        if (rows[ry] & (0x80 >> rx)) {  // 2x scale for readability on the panel
          xr_fill_rect(s, clip, &p, x + (i * X11_FONT_W + rx) * 2, y + ry * 2, 2, 2);
        }
  }
}

static uint16_t rgb(int r5, int g6, int b5) { return (uint16_t)(r5 << 11 | g6 << 5 | b5); }

void lcd_test_pattern(uint16_t *fb) {
  xr_surface s = {fb, W, H, W, 16, nullptr};
  xr_region all;
  xr_region_set(&all, 0, 0, W, H);
  xr_paint p = {3, 0xFFFFFFFFu, 0, 0};
  xr_fill_rect(&s, &all, &p, 0, 0, W, H);  // black
  // 8 colour bars (white, yellow, cyan, green, magenta, red, blue, black), 160 px high
  const uint16_t bars[8] = {0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000};
  for (int i = 0; i < 8; ++i) {
    p.fg = bars[i];
    xr_fill_rect(&s, &all, &p, i * 100, 40, 100, 160);
  }
  // ramps: red 0..31, green 0..63, blue 0..31, grey; 32 px each
  for (int x = 0; x < W; ++x) {
    int r5 = x * 32 / W, g6 = x * 64 / W, b5 = x * 32 / W;
    p.fg = rgb(r5, 0, 0);
    xr_fill_rect(&s, &all, &p, x, 210, 1, 32);
    p.fg = rgb(0, g6, 0);
    xr_fill_rect(&s, &all, &p, x, 246, 1, 32);
    p.fg = rgb(0, 0, b5);
    xr_fill_rect(&s, &all, &p, x, 282, 1, 32);
    p.fg = rgb(r5, g6, b5);
    xr_fill_rect(&s, &all, &p, x, 318, 1, 32);
  }
  // 1-pixel checkerboard (shows jitter and timing problems clearly) and line pairs
  for (int y = 360; y < 460; ++y)
    for (int x = 20; x < 220; ++x) fb[y * W + x] = ((x + y) & 1) ? 0xFFFF : 0x0000;
  p.fg = 0xFFFF;
  for (int i = 0; i < 10; ++i) xr_fill_rect(&s, &all, &p, 240 + i * 8, 360, 1, 100);  // vertical lines
  for (int i = 0; i < 10; ++i) xr_fill_rect(&s, &all, &p, 340, 360 + i * 10, 100, 1);  // horizontal lines
  // corner markers (orientation and colour order) and a 1-pixel white frame on the edges
  p.fg = 0xF800;
  xr_fill_rect(&s, &all, &p, 0, 0, 30, 30);
  p.fg = 0x07E0;
  xr_fill_rect(&s, &all, &p, W - 30, 0, 30, 30);
  p.fg = 0x001F;
  xr_fill_rect(&s, &all, &p, 0, H - 30, 30, 30);
  p.fg = 0xFFFF;
  xr_fill_rect(&s, &all, &p, W - 30, H - 30, 30, 30);
  xr_fill_rect(&s, &all, &p, 0, 0, W, 1);
  xr_fill_rect(&s, &all, &p, 0, H - 1, W, 1);
  xr_fill_rect(&s, &all, &p, 0, 0, 1, H);
  xr_fill_rect(&s, &all, &p, W - 1, 0, 1, H);
  text(&s, &all, 40, 8, "WOODLAND X11 EXPERIMENT - LCD TEST", 0xFFFF);
  text(&s, &all, 460, 370, "TL RED  TR GREEN", 0xFFFF);
  text(&s, &all, 460, 400, "BL BLUE BR WHITE", 0xFFFF);
  text(&s, &all, 460, 430, "BARS W Y C G M R B K", 0xFFFF);
  lcd_push_all(fb);
}

#ifdef X11_TOUCH_DIAG
// The multi-touch chain (mt/x11_touchchain: tracker, recogniser, gate, clicks), the same code
// the host server runs with simulated frames. Rotation and calibration stay in their usual
// places (LovyanGFX rotation 0; the pointer filter's calibration), so the chain maps 1:1 here.
#include "src/x11_touchchain.h"
static x11_tc tc;
static bool tc_ready = false;

int touch_diag_read(int &x, int &y) {
  if (!stats.ok) return 0;
  if (!tc_ready) {
    x11_tc_config c;
    x11_tc_default_config(&c, 800, 480);
    x11_tc_init(&tc, &c);
    tc_ready = true;
  }
  lgfx::touch_point_t tp[X11_MT_MAX];
  int count = (int)gfx.getTouch(tp, X11_MT_MAX);
  x11_mt_point p[X11_MT_MAX];
  for (int i = 0; i < count && i < X11_MT_MAX; ++i)
    p[i].id = (uint8_t)tp[i].id, p[i].x = (int16_t)tp[i].x, p[i].y = (int16_t)tp[i].y, p[i].size = (uint16_t)tp[i].size;
  int64_t t0 = esp_timer_get_time();
  // the driver cannot tell a failed read from no touch: every read counts as ok
  int pts = x11_tc_frame(&tc, millis(), 1, p, count, &x, &y);
  uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
  if (dt > tc.frame_us_max) tc.frame_us_max = dt;
  return pts;
}

void touch_gesture_apply(x11_server *s, int button1_down) {
  if (tc_ready) x11_tc_apply(&tc, s, button1_down);
}

size_t touch_diag_text(char *out, size_t cap) {
  if (!tc_ready || !cap) return 0;
  return x11_tc_text(&tc, out, cap);
}
#endif

#ifdef X11_PCLK
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>

#include "src/x11_internal.h"  // the root window's properties (read only)
#include "src/x11_pclk.h"

namespace {
struct NvsStore : pclk::Store {  // NVS "wl_pclk", the keys Woodland Lab uses
  Preferences prefs;
  bool open = false;
  bool get(const char *k, uint8_t &v) override {
    if (!open || !prefs.isKey(k)) return false;
    v = prefs.getUChar(k, 0);
    return true;
  }
  bool set(const char *k, uint8_t v) override { return open && prefs.putUChar(k, v) == 1; }
};
NvsStore pstore;
x11pclk::Boot pboot = {x11pclk::BASELINE_MHZ, x11pclk::BASELINE_MHZ, 0, false, false, pclk::BootNote::Normal};
bool ptrial = false;
esp_timer_handle_t pdeadline = nullptr;
uint32_t preq_hash = 0, preq_handled = 0, preq_rejected = 0, prestart_at = 0, plast_poll = 0;
bool prestart = false;
const char *plast = "none";

void pdeadline_cb(void *) { esp_restart(); }  // esp_timer task; no NVS write: the next boot reverts

// 6x12 text at scale s straight into the RGB565 framebuffer
void ptext(uint16_t *fb, int x, int y, int s, const char *str, uint16_t fg) {
  for (int i = 0; str[i]; ++i) {
    int c = (unsigned char)str[i];
    if (c < X11_FONT_FIRST || c > X11_FONT_LAST) c = ' ';
    const uint8_t *rows = x11_font_fixed_rows[c - X11_FONT_FIRST];
    for (int ry = 0; ry < X11_FONT_H; ++ry)
      for (int rx = 0; rx < X11_FONT_W; ++rx)
        if (rows[ry] & (0x80 >> rx))
          for (int dy = 0; dy < s; ++dy)
            for (int dx = 0; dx < s; ++dx) {
              int px = x + (i * X11_FONT_W + rx) * s + dx, py = y + ry * s + dy;
              if (px >= 0 && px < 800 && py >= 0 && py < 480) fb[py * 800 + px] = fg;
            }
  }
}
void pfill(uint16_t *fb, int x0, int y0, int x1, int y1, uint16_t c) {
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x) fb[y * 800 + x] = c;
}
void pframe(uint16_t *fb, int x0, int y0, int x1, int y1, uint16_t c) {
  pfill(fb, x0, y0, x1, y0 + 3, c), pfill(fb, x0, y1 - 3, x1, y1, c);
  pfill(fb, x0, y0, x0 + 3, y1, c), pfill(fb, x1 - 3, y0, x1, y1, c);
}

// The dialog's buttons
constexpr int KEEP_L = 190, KEEP_R = 380, BTN_T = 250, BTN_B = 330, REV_L = 420, REV_R = 610;

void pdraw(uint16_t *fb, int left) {
  pfill(fb, 150, 120, 650, 360, 0x0000);
  pframe(fb, 150, 120, 650, 360, 0xFFFF);
  char line[48];
  snprintf(line, sizeof(line), "Pixel clock %u MHz", (unsigned)pboot.apply_mhz);
  ptext(fb, 190, 140, 3, line, 0xFFFF);
  snprintf(line, sizeof(line), "Keep it? Reverting to %u MHz in %d s", (unsigned)pboot.confirmed_mhz, left);
  ptext(fb, 190, 200, 2, line, 0xFFE0);
  pfill(fb, KEEP_L, BTN_T, KEEP_R, BTN_B, 0x07E0), ptext(fb, KEEP_L + 47, BTN_T + 22, 3, "KEEP", 0x0000);
  pfill(fb, REV_L, BTN_T, REV_R, BTN_B, 0xF800), ptext(fb, REV_L + 23, BTN_T + 22, 3, "REVERT", 0xFFFF);
}

bool ota_trial_running() {
  esp_ota_img_states_t st;
  const esp_partition_t *run = esp_ota_get_running_partition();
  return run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
}
}  // namespace

void pclk_before_lcd() {
  pstore.open = pstore.prefs.begin("wl_pclk", false);
  pboot = x11pclk::decide(pstore);
  auto c = gfx._bus_instance.config();
  c.freq_write = (uint32_t)pboot.apply_mhz * 1000000u;
  gfx._bus_instance.config(c);
  Serial.printf("PCLK %u MHz (%s%s%s; confirmed %u, stored %u)\n", (unsigned)pboot.apply_mhz,
                pclk::boot_note_name(pboot.note), pboot.trial ? ", TRIAL" : "", pboot.restricted ? ", restricted" : "",
                (unsigned)pboot.confirmed_mhz, (unsigned)pboot.stored_confirmed);
  if (!pboot.trial) return;
  ptrial = true;
  esp_timer_create_args_t a = {};
  a.callback = pdeadline_cb;
  a.name = "pclk_deadline";
  if (esp_timer_create(&a, &pdeadline) == ESP_OK) esp_timer_start_once(pdeadline, (uint64_t)pclk::HARD_DEADLINE_MS * 1000u);
  enableLoopWDT();  // a hung setup()/loop() during the trial resets the chip (then the next boot reverts)
}

void pclk_trial_dialog(uint16_t *fb) {
  if (!ptrial) return;
  uint32_t t0 = millis();
  int shown = -1;
  for (;;) {
    esp_task_wdt_reset();
    int left = (int)x11pclk::CONFIRM_SECONDS - (int)((millis() - t0) / 1000);
    if (left <= 0) break;  // no answer: revert
    if (left != shown) {
      pdraw(fb, left);
      lcd_push_all(fb);
      shown = left;
    }
    lgfx::touch_point_t tp;
    if (gfx.getTouch(&tp, 1) > 0 && tp.y >= BTN_T && tp.y < BTN_B) {
      if (tp.x >= KEEP_L && tp.x < KEEP_R) {  // Keep: confirmed first, then the trial ends
        bool ok = pclk::keep(pstore, pboot.apply_mhz);
        if (pdeadline) esp_timer_stop(pdeadline);
        disableLoopWDT();
        ptrial = false, pboot.trial = false, pboot.confirmed_mhz = pboot.apply_mhz;
        plast = ok ? "kept" : "keep write failed (next boot reverts)";
        lcd_test_pattern(fb);  // the dialog off; the boot continues
        return;
      }
      if (tp.x >= REV_L && tp.x < REV_R) break;
    }
    delay(15);
  }
  pfill(fb, 150, 120, 650, 360, 0x0000);
  ptext(fb, 190, 220, 3, "Reverting...", 0xFFFF);
  lcd_push_all(fb);
  pclk::revert(pstore);
  delay(300);
  esp_restart();
}

void pclk_poll(x11_server *s) {
  uint32_t now = millis();
  if (prestart) {
    if ((int32_t)(now - prestart_at) >= 0) esp_restart();
    return;
  }
  if (now - plast_poll < 250 || !s || !s->root) return;
  plast_poll = now;
  for (x11_prop *p = s->root->props; p; p = p->next) {
    const char *n = x11_atom_name(s, p->name);
    if (!n || strcmp(n, x11pclk::REQUEST_ATOM)) continue;
    uint32_t h = 2166136261u;  // each distinct value is handled once
    for (uint32_t i = 0; i < p->nbytes; ++i) h = (h ^ p->data[i]) * 16777619u;
    h ^= p->nbytes;
    if (h == preq_hash) return;
    preq_hash = h;
    x11pclk::Parsed r = p->format == 8 ? x11pclk::parse_request((const char *)p->data, p->nbytes)
                                       : x11pclk::Parsed{x11pclk::Cmd::Invalid, 0, 0};
    if (r.cmd == x11pclk::Cmd::None) return;
    if (r.cmd == x11pclk::Cmd::Invalid) {
      ++preq_rejected, plast = "invalid request";
      return;
    }
    if (ota_trial_running()) {
      ++preq_rejected, plast = "refused: OTA trial running";
      return;
    }
    if (r.cmd == x11pclk::Cmd::Request && r.mhz == pboot.apply_mhz && r.mhz == pboot.confirmed_mhz) {
      ++preq_handled, plast = "already at this clock";
      return;
    }
    bool ok = r.cmd == x11pclk::Cmd::Request ? pclk::request(pstore, r.mhz) : pclk::reset_to_baseline(pstore);
    ++preq_handled;
    if (!ok) {
      plast = "NVS write failed";
      return;
    }
    plast = r.cmd == x11pclk::Cmd::Request ? "restarting for a trial" : "reset to 16 MHz, restarting";
    Serial.printf("PCLK: %s\n", plast);
    prestart = true, prestart_at = now + 500;  // let the client's request finish
    return;
  }
}

size_t pclk_diag_text(char *out, size_t cap) {
  int n = snprintf(out, cap,
                   "pclk.applied_mhz=%u\npclk.confirmed_mhz=%u\npclk.stored_confirmed=%u\npclk.trial=%d\n"
                   "pclk.restricted=%d\npclk.boot_note=%s\npclk.requests=%lu\npclk.rejected=%lu\npclk.last=%s\n"
                   "lcd.fps_nominal=%.1f\n",
                   (unsigned)pboot.apply_mhz, (unsigned)pboot.confirmed_mhz, (unsigned)pboot.stored_confirmed,
                   ptrial ? 1 : 0, pboot.restricted ? 1 : 0, pclk::boot_note_name(pboot.note),
                   (unsigned long)preq_handled, (unsigned long)preq_rejected, plast,
                   (double)x11pclk::nominal_fps((uint32_t)pboot.apply_mhz * 1000000u));
  if (n < 0) return 0;
  return (size_t)n < cap ? (size_t)n : cap ? cap - 1 : 0;
}
#endif

#ifdef X11_BACKLIGHT
// ---------------------------------------------------------------- runtime backlight level
// Writes from loop() only, the task that also reads touch on this I2C port.
namespace {
crowpanel43_board::Backlight backlight;
struct BacklightIo {
  bool write(uint8_t addr, uint8_t byte) {
    Wire.beginTransmission(addr);
    Wire.write(byte);
    return Wire.endTransmission() == 0;
  }
};
}  // namespace

bool backlight_serial(const char *line) {
  if (strncmp(line, "BACKLIGHT", 9) || (line[9] && line[9] != ' ')) return false;
  const char *arg = line[9] ? line + 10 : nullptr;
  if (!arg) {
    Serial.printf("backlight %u%% (command %u), writes %lu, errors %lu%s\n", (unsigned)backlight.applied_pct,
                  (unsigned)crowpanel43_board::backlight_command(backlight.applied_pct), (unsigned long)backlight.writes,
                  (unsigned long)backlight.errors, backlight.pending ? ", a change is pending" : "");
    return true;
  }
  char *end = nullptr;
  long pct = strtol(arg, &end, 10);
  while (end && (*end == ' ' || *end == '%')) ++end;
  if (end == arg || (end && *end) || !crowpanel43_board::backlight_request(backlight, pct)) {
    Serial.printf("BACKLIGHT <%d..%d> (percent; not stored, every boot starts at 100)\n",
                  crowpanel43_board::BACKLIGHT_MIN_PCT, crowpanel43_board::BACKLIGHT_MAX_PCT);
    return true;
  }
  Serial.printf("backlight %ld%% requested (command %u)\n", pct, (unsigned)backlight.cmd);
  return true;
}

void backlight_poll() {
  BacklightIo io;
  if (crowpanel43_board::backlight_service(backlight, io, millis()))
    Serial.printf("backlight: command %u %s\n", (unsigned)backlight.cmd,
                  backlight.failed ? "not acknowledged, retrying in 1 s" : "acknowledged");
}

size_t backlight_diag_text(char *out, size_t cap) {
  int n = snprintf(out, cap,
                   "lcd.backlight_pct=%u\nlcd.backlight_cmd=%u\nlcd.backlight_requested_pct=%u\n"
                   "lcd.backlight_pending=%d\nlcd.backlight_writes=%lu\nlcd.backlight_errors=%lu\n",
                   (unsigned)backlight.applied_pct, (unsigned)crowpanel43_board::backlight_command(backlight.applied_pct),
                   (unsigned)backlight.pct, backlight.pending ? 1 : 0, (unsigned long)backlight.writes,
                   (unsigned long)backlight.errors);
  if (n < 0) return 0;
  return (size_t)n < cap ? (size_t)n : cap ? cap - 1 : 0;
}
#endif
