/* Touchscreen to X11 pointer (Milestone 3A). Portable C99, no I/O: the platform reads the
 * controller (GT911 on the ESP32) and passes each sample in with its time; the filter
 * says where the pointer is and whether Button1 is down. One finger only: the first
 * reported point; no gestures.
 *
 *   finger down           -> pointer moves there, Button1 pressed
 *   finger moves          -> pointer follows (moves under jitter_px are ignored)
 *   no touch reported for release_ms (finger lifted, a dropped sample, a failed read)
 *                         -> Button1 released at the last position
 *
 * The release timer makes a single missing sample harmless (no false release/press pair)
 * and guarantees the button never stays down when the controller stops reporting.
 * Coordinates are mapped by the optional calibration and clamped to the screen. */
#ifndef X11_TOUCH_H
#define X11_TOUCH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint16_t width, height;         /* screen: x in 0..width-1, y in 0..height-1 */
  uint16_t release_ms;            /* no touch for this long: the finger is up */
  uint8_t jitter_px;              /* while down, moves of at most this many pixels are ignored */
  /* Calibration: raw x0..x1 maps to 0..width-1 (likewise y). x0 == x1: no mapping (raw
   * coordinates are already screen coordinates, as the production firmware uses them). */
  int16_t raw_x0, raw_x1, raw_y0, raw_y1;
} x11_touch_config;

typedef struct {
  x11_touch_config cfg;
  uint8_t down;                   /* Button1 */
  int16_t x, y;                   /* pointer position */
  uint32_t last_touch_ms;         /* time of the last sample with a touch */
  /* diagnostics */
  uint32_t samples, touched, failed, presses, releases;
  int16_t raw_x, raw_y;           /* last raw point */
} x11_touch;

void x11_touch_default_config(x11_touch_config *cfg, uint16_t width, uint16_t height);
void x11_touch_init(x11_touch *t, const x11_touch_config *cfg);

/* One controller read at now_ms. ok = 0: the read failed. points: touches reported;
 * (raw_x, raw_y) the first one. Returns 1 if x, y or down changed. */
int x11_touch_sample(x11_touch *t, uint32_t now_ms, int ok, int points, int raw_x, int raw_y);

/* Ends a touch at once (touch polling switched off): returns 1 if Button1 was down. */
int x11_touch_cancel(x11_touch *t);

#ifdef __cplusplus
}
#endif
#endif
