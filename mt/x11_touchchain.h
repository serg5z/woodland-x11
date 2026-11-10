/* The multi-touch chain (P6): one controller frame in, the pointer's input
 * and gesture clicks out. Portable C99, no I/O, no allocation: the same code runs in the test
 * firmware (-DX11_TOUCH_DIAG, from the GT9xx read) and in the host server (simulated frames,
 * `--input`), so what the panel does can be reproduced deterministically.
 *
 *   raw frame (up to 5 points: hardware track ID, x, y, size, as the GT9xx reports them)
 *     -> transform: calibration (raw range -> controller range), rotation, bounds; points
 *        outside the controller's range are dropped and counted
 *     -> tracker (x11_mt): contacts by track ID, NEW / MOVE / STILL / UP
 *     -> transitions: BEGIN / END / LOST (released during failed reads) with the frame number,
 *        the latest X11_TC_LOG of them kept for diagnostics
 *     -> recogniser (x11_gesture): two-finger scroll, pan, pinch, tap
 *     -> gate (x11_gate): when a single finger becomes the pointer
 *   out: the single-touch pointer filter's input (0 or 1 point, screen coordinates: the
 *        filter must not calibrate again), and queued gesture clicks / the pinch text, which
 *        x11_tc_apply hands to the X server between requests.
 *
 * Three to five contacts are tracked and shown; the recogniser cancels on a third finger,
 * so they never reach the pointer as anything but "no touch". */
#ifndef X11_TOUCHCHAIN_H
#define X11_TOUCHCHAIN_H

#include <stddef.h>
#include <stdint.h>

#include "x11_gate.h"
#include "x11_gesture.h"
#include "x11_mt.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint16_t width, height;      /* screen (after rotation) */
  uint16_t raw_w, raw_h;       /* the controller's coordinate range: x 0..raw_w-1, y 0..raw_h-1 */
  uint8_t rotation;            /* controller -> screen: 0, 1 = 90 degrees clockwise, 2 = 180, 3 = 270 */
  /* Calibration, in controller coordinates: raw x0..x1 is the controller's 0..raw_w-1 (likewise
   * y). x0 == x1: none (the production default). Applied before the rotation. */
  int16_t cal_x0, cal_x1, cal_y0, cal_y1;
  uint16_t hold_ms;            /* the gate's single-finger hold for a finger that moved (50) */
  uint16_t still_ms;           /* ... for a finger that stayed still (150) */
  uint16_t move_px;            /* ... "moved": this far from where it landed (10) */
  uint8_t natural_scroll;      /* scroll clicks follow the content (1) */
} x11_tc_config;

enum { X11_TC_BEGIN = 1, X11_TC_END, X11_TC_LOST };

typedef struct {
  uint32_t frame;              /* the tracker's read number */
  uint32_t t_ms;
  uint8_t kind, id, slot;      /* X11_TC_*, track ID, tracker slot */
  int16_t x, y;                /* where it began / was last seen */
} x11_tc_transition;

#define X11_TC_LOG 8
#define X11_TC_CLICKS 32

typedef struct {
  x11_tc_config cfg;
  x11_mt mt;
  x11_gesture gest;
  x11_gate gate;
  x11_tc_transition log[X11_TC_LOG];
  uint32_t log_total;          /* transitions so far (log[log_total % X11_TC_LOG] is the next) */
  x11_gesture_click clicks[X11_TC_CLICKS];
  uint8_t click_head, click_n;
  x11_pinch_pub pinch;
  char pinch_text[X11_PINCH_TEXT_MAX];
  int pinch_len;
  x11_gesture_event ev[6], last_update;  /* the latest events other than UPDATE, and the latest UPDATE */
  /* counters */
  uint32_t frames, failed_frames, out_of_range, points_in;
  uint32_t events, updates, clicks_x11[6], clicks_dropped, clicks_deferred, pinch_set, pinch_failed;
  uint32_t frame_us_max;       /* set by the platform: the longest x11_tc_frame (diagnostics) */
} x11_tc;

void x11_tc_default_config(x11_tc_config *c, uint16_t width, uint16_t height);
void x11_tc_init(x11_tc *t, const x11_tc_config *c);

/* Controller -> screen for one point; 0 if it is outside the controller's range. */
int x11_tc_map(const x11_tc_config *c, int raw_x, int raw_y, int *x, int *y);

/* One controller read at now_ms: ok = 0 a failed read (p, n ignored); n points as reported
 * (any order, any count: more than X11_MT_MAX is malformed). Returns the points for the
 * single-touch pointer filter (0 or 1) and the point in *x, *y (screen coordinates). */
int x11_tc_frame(x11_tc *t, uint32_t now_ms, int ok, const x11_mt_point *p, int n, int *x, int *y);

/* Contacts down now. */
int x11_tc_count(const x11_tc *t);

/* Diagnostics: the tracker's "mt.*" lines, the transitions ("tc.ev<k>=frame,ms,BEGIN|END|LOST,
 * id,slot,x,y", newest last), the recogniser, gate and chain counters ("g.*", "gate.*",
 * "tc.*"). Returns the length written (NUL-terminated within cap). */
size_t x11_tc_text(const x11_tc *t, char *out, size_t cap);

const char *x11_tc_kind_name(int kind);

/* Between requests: the pinch text to the root property _WOODLAND_GESTURE, then the queued
 * gesture clicks (pointer to the gesture's position, press, release), unless the pointer
 * filter still holds Button1 (button1_down: they wait). */
struct x11_server;
void x11_tc_apply(x11_tc *t, struct x11_server *s, int button1_down);

#ifdef __cplusplus
}
#endif
#endif
