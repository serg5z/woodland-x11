/* Pointer gate (P6 touch polish): decides when a single finger becomes the
 * X11 pointer, so that a two-finger gesture whose fingers land a few reads apart never sends
 * Button1 first. Portable C99, no I/O. It sits between the contact tracker / gesture
 * recogniser and the single-touch pointer filter (input/x11_touch), whose input it produces.
 *
 *   one finger lands           -> PENDING: the pointer sees no touch yet
 *   it lifts within hold_ms    -> a tap: one sample at the landing point (the filter presses
 *                                 Button1 now and releases it release_ms later)
 *   still alone after hold_ms, -> ACTIVE: one sample at the landing point (press there: a
 *   having moved >= move_px,      selection starts where the finger touched), then the
 *   or after still_ms anyway      finger's positions as they come. A finger that stays
 *                                 still waits longer (still_ms), because a second finger
 *                                 often lands 30-100 ms after the first: pressing at
 *                                 hold_ms gave a false Button1 click before a two-finger
 *                                 tap or scroll (input audit B6)
 *   a second finger, or the gesture recogniser owns the touch
 *                              -> BLOCKED until every finger is up (pending: no press at
 *                                 all; active: the filter releases Button1 as before)
 * Input: the contacts down after each read (the tracker's, so a single missing report is not
 * a lift). Time is the read time, so late reads only make the decision later, never wrong. */
#ifndef X11_GATE_H
#define X11_GATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { X11_GATE_IDLE, X11_GATE_PENDING, X11_GATE_ACTIVE, X11_GATE_BLOCKED };

typedef struct {
  uint16_t hold_ms;   /* earliest press, for a finger that moved >= move_px */
  uint16_t still_ms;  /* press of a finger that stayed still (>= hold_ms) */
  uint16_t move_px;   /* 0: every finger presses at hold_ms (the old behaviour) */
  uint8_t state;
  uint32_t t0;
  int16_t x0, y0;
  /* counters */
  uint32_t taps, commits, discards, blocked_active;
  uint32_t latency_ms_last, latency_ms_max;  /* finger down -> the pointer's first sample */
} x11_gate;

/* still_ms = hold_ms and move_px = 0 (one hold for every finger); x11_gate_set_still sets them. */
void x11_gate_init(x11_gate *g, uint16_t hold_ms);
void x11_gate_set_still(x11_gate *g, uint16_t still_ms, uint16_t move_px);

/* One read: n contacts down, (x, y) the single contact when n == 1; owns = the gesture
 * recogniser owns the touch. Returns the points to give the pointer filter (0 or 1), with
 * the position in *ox, *oy. */
int x11_gate_step(x11_gate *g, uint32_t now_ms, int n, int x, int y, int owns, int *ox, int *oy);

#ifdef __cplusplus
}
#endif
#endif
