// LCD backend for the X11 experiment on the CrowPanel 4.3" (ESP32-S3).
//
// The X11 core draws into its own RGB565 framebuffer in PSRAM. Only the areas it reports
// as changed (x11_take_damage) are pushed to the panel, through LovyanGFX's
// pushImage with lgfx::rgb565_t, which also handles the
// panel's framebuffer cache write-back. The RGB peripheral keeps scanning its own
// framebuffer by DMA independently of this code; pushes are cut into slices of a time
// budget per loop pass so X11 requests and the network are never starved.
#pragma once
#include <stdint.h>

#include "src/x11_server.h"

struct LcdStats {
  bool ok;                  // panel initialised
  bool backlight_found;     // controller 0x30 answered
  uint32_t psram_used;      // PSRAM taken by LovyanGFX's panel framebuffer
  uint32_t flushes, rects, rows, pixels;
  uint32_t last_flush_us, max_slice_us;
  uint32_t pending_rows;    // rows still to push
  uint32_t slices;          // lcd_flush passes that pushed something
  uint64_t busy_us;         // their total time
};

struct TouchInfo {          // GT911 identification and configuration (read-only registers)
  char product[5];
  uint16_t fw_version, x_res, y_res;
  uint8_t config_version, max_touch;
};

// Panel bring-up: the board's I2C devices (crowpanel_board.h: microcontroller 0x30, touch 0x5D,
// bounded wait, full backlight), then LovyanGFX init, DMA and a black screen. Returns false
// if LovyanGFX failed.
bool lcd_begin();

// Draws the static test pattern into `fb` (800x480 RGB565) and pushes all of it.
void lcd_test_pattern(uint16_t *fb);

// Pushes changed areas of the core's framebuffer for at most `budget_us` microseconds.
void lcd_flush(x11_server *s, uint32_t budget_us);

// Pushes a whole framebuffer (boot, test pattern).
void lcd_push_all(const uint16_t *fb);

const LcdStats &lcd_stats();

// GT911 touch read through LovyanGFX (gfx.getTouch, two points, one
// I2C transaction). Returns the number of points (0: none, or the read failed);
// (x, y) is the first point in panel coordinates (crowpanel_lcd.h: 0..800, 0..480).
// I2C_NUM_0 is shared with the board controller (0x30), which only Wire uses, only in
// lcd_begin(); every touch access runs in loop(), so accesses never overlap.
int lcd_touch_read(int &x, int &y);

// GT911 product, firmware, resolution and touch points: Wire register reads on the same
// port, from loop() only (the serial TOUCH command). Never writes.
bool lcd_touch_info(TouchInfo &o);

#ifdef X11_TOUCH_DIAG  // P6 multi-touch diagnostics; test builds only
#include <stddef.h>
#include "src/x11_mt.h"
// The same controller read as lcd_touch_read (one I2C transaction; the driver reads every
// reported point), asking for up to X11_MT_MAX points: all of them go to the contact tracker,
// point 0 is returned as before, so the X11 pointer behaves exactly as without the flag.
int touch_diag_read(int &x, int &y);
size_t touch_diag_text(char *out, size_t cap);  // "mt.*" diagnostics lines
#define TOUCH_DIAG_HOOK(x, y) return touch_diag_read(x, y);  // at the top of lcd_touch_read
#define TOUCH_DIAG_TEXT(out, cap) touch_diag_text(out, cap)
// Phase D: delivers the gesture clicks (scroll -> Button4/5, two-finger tap -> Button3) after
// touch_apply, from touch_poll; held while the pointer still has Button1 down.
void touch_gesture_apply(x11_server *s, int button1_down);
#define TOUCH_GESTURE_APPLY(s, down) touch_gesture_apply(s, down);
#else
#define TOUCH_GESTURE_APPLY(s, down)  // no tokens: normal builds compile exactly as before
#define TOUCH_DIAG_HOOK(x, y)  // no tokens: the normal build compiles exactly as before
#define TOUCH_DIAG_TEXT(out, cap) ((size_t)0)
#endif

#ifdef X11_PCLK  // LCD pixel-clock setting; test builds only
// Before gfx.init(): the NVS decision (12/14/16 MHz) and, on a trial boot, the guards.
void pclk_before_lcd();
// On a trial boot, after the test pattern: the on-panel "keep this clock?" dialog (10 s).
void pclk_trial_dialog(uint16_t *fb);
// From loop(): a request in the root property _WOODLAND_PCLK_REQUEST (restarts to apply).
void pclk_poll(x11_server *s);
size_t pclk_diag_text(char *out, size_t cap);  // "pclk.*" diagnostics lines
#define LCD_BEGIN_HOOK() pclk_before_lcd();
#define PCLK_TRIAL_DIALOG(fb) pclk_trial_dialog(fb);
#define PCLK_POLL(s) pclk_poll(s);
#define PCLK_DIAG_TEXT(out, cap) pclk_diag_text(out, cap)
#else
#define LCD_BEGIN_HOOK()  // no tokens: normal builds compile exactly as before
#define PCLK_TRIAL_DIALOG(fb)
#define PCLK_POLL(s)
#define PCLK_DIAG_TEXT(out, cap) ((size_t)0)
#endif
