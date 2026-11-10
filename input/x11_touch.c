/* Touchscreen filter, see x11_touch.h. */
#include "x11_touch.h"

#include <string.h>

void x11_touch_default_config(x11_touch_config *cfg, uint16_t width, uint16_t height) {
  memset(cfg, 0, sizeof(*cfg));
  cfg->width = width;
  cfg->height = height;
  cfg->release_ms = 60;  /* about four GT911 polls at 15 ms */
  cfg->jitter_px = 1;
}

void x11_touch_init(x11_touch *t, const x11_touch_config *cfg) {
  memset(t, 0, sizeof(*t));
  t->cfg = *cfg;
}

static int map(int v, int r0, int r1, int size) {
  if (r0 != r1) v = (int)(((long)(v - r0) * (size - 1) + (r1 - r0) / 2) / (r1 - r0));
  return v < 0 ? 0 : v >= size ? size - 1 : v;
}

static int far(int a, int b, int d) { return a - b > d || b - a > d; }

int x11_touch_sample(x11_touch *t, uint32_t now_ms, int ok, int points, int raw_x, int raw_y) {
  ++t->samples;
  if (!ok) ++t->failed;
  if (ok && points > 0) {
    ++t->touched;
    t->raw_x = (int16_t)raw_x, t->raw_y = (int16_t)raw_y;
    int x = map(raw_x, t->cfg.raw_x0, t->cfg.raw_x1, t->cfg.width);
    int y = map(raw_y, t->cfg.raw_y0, t->cfg.raw_y1, t->cfg.height);
    t->last_touch_ms = now_ms;
    if (!t->down) {
      t->down = 1;
      ++t->presses;
      t->x = (int16_t)x, t->y = (int16_t)y;
      return 1;
    }
    if (far(x, t->x, t->cfg.jitter_px) || far(y, t->y, t->cfg.jitter_px)) {
      t->x = (int16_t)x, t->y = (int16_t)y;
      return 1;
    }
    return 0;
  }
  if (t->down && (uint32_t)(now_ms - t->last_touch_ms) >= t->cfg.release_ms) {
    t->down = 0;
    ++t->releases;
    return 1;
  }
  return 0;
}

int x11_touch_cancel(x11_touch *t) {
  if (!t->down) return 0;
  t->down = 0;
  ++t->releases;
  return 1;
}
