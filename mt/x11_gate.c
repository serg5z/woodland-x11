/* Pointer gate, see x11_gate.h. */
#include "x11_gate.h"

#include <stdlib.h>
#include <string.h>

void x11_gate_init(x11_gate *g, uint16_t hold_ms) {
  memset(g, 0, sizeof(*g));
  g->hold_ms = g->still_ms = hold_ms;
}

void x11_gate_set_still(x11_gate *g, uint16_t still_ms, uint16_t move_px) {
  g->still_ms = still_ms < g->hold_ms ? g->hold_ms : still_ms;
  g->move_px = move_px;
}

static int first_sample(x11_gate *g, uint32_t now, int *ox, int *oy) {
  uint32_t lat = now - g->t0;
  g->latency_ms_last = lat;
  if (lat > g->latency_ms_max) g->latency_ms_max = lat;
  *ox = g->x0, *oy = g->y0;
  return 1;
}

int x11_gate_step(x11_gate *g, uint32_t now, int n, int x, int y, int owns, int *ox, int *oy) {
  if (n >= 2 || owns) {  /* two fingers: never the pointer's */
    if (g->state == X11_GATE_PENDING) ++g->discards;
    if (g->state == X11_GATE_ACTIVE) ++g->blocked_active;
    g->state = n ? X11_GATE_BLOCKED : X11_GATE_IDLE;
    return 0;
  }
  switch (g->state) {
    case X11_GATE_BLOCKED:
      if (!n) g->state = X11_GATE_IDLE;
      return 0;
    case X11_GATE_ACTIVE:
      if (!n) {
        g->state = X11_GATE_IDLE;
        return 0;
      }
      *ox = x, *oy = y;
      return 1;
    case X11_GATE_PENDING:
      if (!n) {  /* lifted within the hold: a tap */
        g->state = X11_GATE_IDLE;
        ++g->taps;
        return first_sample(g, now, ox, oy);
      }
      if (now - g->t0 >= g->still_ms ||
          (now - g->t0 >= g->hold_ms && (abs(x - g->x0) >= g->move_px || abs(y - g->y0) >= g->move_px))) {
        g->state = X11_GATE_ACTIVE;
        ++g->commits;
        return first_sample(g, now, ox, oy);
      }
      return 0;
    default:  /* IDLE */
      if (!n) return 0;
      g->state = X11_GATE_PENDING, g->t0 = now, g->x0 = (int16_t)x, g->y0 = (int16_t)y;
      if (!g->hold_ms) {
        g->state = X11_GATE_ACTIVE;
        ++g->commits;
        return first_sample(g, now, ox, oy);
      }
      return 0;
  }
}
