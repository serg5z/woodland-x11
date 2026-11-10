/* The multi-touch chain, see x11_touchchain.h. */
#include "x11_touchchain.h"

#include <stdio.h>
#include <string.h>

#include "../core/x11_server.h"

void x11_tc_default_config(x11_tc_config *c, uint16_t width, uint16_t height) {
  memset(c, 0, sizeof(*c));
  c->width = c->raw_w = width;
  c->height = c->raw_h = height;
  c->hold_ms = 50;
  c->still_ms = 150;
  c->move_px = 10;
  c->natural_scroll = 1;
}

void x11_tc_init(x11_tc *t, const x11_tc_config *c) {
  memset(t, 0, sizeof(*t));
  t->cfg = *c;
  x11_mt_init(&t->mt, c->width, c->height);
  x11_gesture_init(&t->gest, NULL);
  x11_gate_init(&t->gate, c->hold_ms);
  x11_gate_set_still(&t->gate, c->still_ms, c->move_px);
}

static int scale(int v, int r0, int r1, int size) {  /* raw r0..r1 -> 0..size-1, as input/x11_touch */
  if (r0 != r1) v = (int)(((long)(v - r0) * (size - 1) + (r1 - r0) / 2) / (r1 - r0));
  return v < 0 ? 0 : v >= size ? size - 1 : v;
}

int x11_tc_map(const x11_tc_config *c, int rx, int ry, int *x, int *y) {
  if (rx < 0 || ry < 0 || rx >= c->raw_w || ry >= c->raw_h) return 0;
  int ux = scale(rx, c->cal_x0, c->cal_x1, c->raw_w), uy = scale(ry, c->cal_y0, c->cal_y1, c->raw_h);
  int sx, sy;
  switch (c->rotation & 3) {
    case 1: sx = c->raw_h - 1 - uy, sy = ux; break;                  /* 90 degrees clockwise */
    case 2: sx = c->raw_w - 1 - ux, sy = c->raw_h - 1 - uy; break;
    case 3: sx = uy, sy = c->raw_w - 1 - ux; break;
    default: sx = ux, sy = uy; break;
  }
  *x = sx < 0 ? 0 : sx >= c->width ? c->width - 1 : sx;
  *y = sy < 0 ? 0 : sy >= c->height ? c->height - 1 : sy;
  return 1;
}

static void log_transition(x11_tc *t, uint32_t now, int kind, int slot, const x11_mt_contact *k) {
  x11_tc_transition *e = &t->log[t->log_total++ % X11_TC_LOG];
  e->frame = t->mt.reads, e->t_ms = now, e->kind = (uint8_t)kind, e->id = k->id, e->slot = (uint8_t)slot;
  e->x = kind == X11_TC_BEGIN ? k->x0 : k->x, e->y = kind == X11_TC_BEGIN ? k->y0 : k->y;
}

static void queue_click(x11_tc *t, const x11_gesture_click *k) {
  if (t->click_n == X11_TC_CLICKS) {
    ++t->clicks_dropped;
    return;
  }
  t->clicks[(t->click_head + t->click_n++) % X11_TC_CLICKS] = *k;
}

int x11_tc_frame(x11_tc *t, uint32_t now, int ok, const x11_mt_point *p, int n, int *x, int *y) {
  ++t->frames;
  x11_mt_point m[X11_MT_MAX];
  int k = 0;
  if (!ok) {
    ++t->failed_frames;
  } else {
    /* a count the controller cannot send (the status nibble goes to 15): malformed, and only
     * the first X11_MT_MAX points are read */
    if (n < 0 || n > X11_MT_MAX) ++t->mt.malformed;
    int take = n < 0 ? 0 : n > X11_MT_MAX ? X11_MT_MAX : n;
    for (int i = 0; i < take; ++i) {
      ++t->points_in;
      int sx, sy;
      if (!x11_tc_map(&t->cfg, p[i].x, p[i].y, &sx, &sy)) {
        ++t->out_of_range;
        continue;
      }
      m[k] = p[i];
      m[k].x = (int16_t)sx, m[k].y = (int16_t)sy;
      ++k;
    }
  }
  x11_mt_sample(&t->mt, now, ok, m, k);
  /* transitions and the contacts down, in slot order */
  x11_gesture_contact c[X11_MT_MAX];
  int down = 0;
  for (int i = 0; i < X11_MT_MAX; ++i) {
    const x11_mt_contact *q = &t->mt.c[i];
    if (q->state == X11_MT_NEW) log_transition(t, now, X11_TC_BEGIN, i, q);
    if (q->state == X11_MT_UP) log_transition(t, now, q->failed_end ? X11_TC_LOST : X11_TC_END, i, q);
    if (q->state == X11_MT_NEW || q->state == X11_MT_MOVE || q->state == X11_MT_STILL)
      c[down].id = q->id, c[down].x = q->x, c[down].y = q->y, ++down;
  }
  x11_gesture_feed(&t->gest, now, c, down);
  x11_gesture_event e;
  while (x11_gesture_pop(&t->gest, &e)) {
    if (e.type == X11_GE_UPDATE) t->last_update = e, ++t->updates;
    else t->ev[t->events++ % 6] = e;
    char pt[X11_PINCH_TEXT_MAX];
    int pn = x11_pinch_text(&t->pinch, &e, pt, sizeof(pt));
    if (pn > 0) memcpy(t->pinch_text, pt, (size_t)pn + 1), t->pinch_len = pn;
    x11_gesture_click ck[X11_GESTURE_MAX_CLICKS_PER_EVENT];
    int nk = x11_gesture_clicks(&e, t->cfg.natural_scroll, ck, X11_GESTURE_MAX_CLICKS_PER_EVENT);
    for (int i = 0; i < nk; ++i) queue_click(t, &ck[i]);
  }
  int px = down ? c[0].x : 0, py = down ? c[0].y : 0;
  return x11_gate_step(&t->gate, now, down, px, py, x11_gesture_owns_touch(&t->gest), x, y);
}

int x11_tc_count(const x11_tc *t) { return x11_mt_count(&t->mt); }

const char *x11_tc_kind_name(int kind) {
  return kind == X11_TC_BEGIN ? "BEGIN" : kind == X11_TC_END ? "END" : kind == X11_TC_LOST ? "LOST" : "?";
}

void x11_tc_apply(x11_tc *t, struct x11_server *s, int button1_down) {
  if (t->pinch_len) {
    if (x11_root_property_set(s, X11_GESTURE_ATOM, (const uint8_t *)t->pinch_text, (size_t)t->pinch_len) == 0)
      ++t->pinch_set;
    else
      ++t->pinch_failed;
    t->pinch_len = 0;
  }
  if (!t->click_n) return;
  if (button1_down) {  /* the pointer filter still releases Button1 from a first finger */
    ++t->clicks_deferred;
    return;
  }
  while (t->click_n) {
    const x11_gesture_click *c = &t->clicks[t->click_head];
    t->click_head = (uint8_t)((t->click_head + 1) % X11_TC_CLICKS), --t->click_n;
    unsigned bit = 1u << (c->button - 1);
    x11_pointer_input(s, c->x, c->y, 0);
    x11_pointer_input(s, c->x, c->y, bit);
    x11_pointer_input(s, c->x, c->y, 0);
    if (c->button < 6) ++t->clicks_x11[c->button];
  }
}

static size_t put(size_t cap, size_t used, int n) {
  if (n < 0) return used;
  return used + (size_t)n < cap ? used + (size_t)n : cap ? cap - 1 : 0;
}

size_t x11_tc_text(const x11_tc *t, char *out, size_t cap) {
  if (!cap) return 0;
  size_t used = x11_mt_text(&t->mt, out, cap);
  uint32_t first = t->log_total > X11_TC_LOG ? t->log_total - X11_TC_LOG : 0;
  for (uint32_t i = first; i < t->log_total; ++i) {
    const x11_tc_transition *e = &t->log[i % X11_TC_LOG];
    used = put(cap, used, snprintf(out + used, cap - used, "tc.ev%lu=%lu,%lu,%s,%u,%u,%d,%d\n", (unsigned long)(i - first),
                                        (unsigned long)e->frame, (unsigned long)e->t_ms, x11_tc_kind_name(e->kind),
                                        (unsigned)e->id, (unsigned)e->slot, e->x, e->y));
  }
  const x11_gesture *g = &t->gest;
  used = put(cap, used,
             snprintf(out + used, cap - used,
                      "tc.frames=%lu\ntc.failed_frames=%lu\ntc.points_in=%lu\ntc.out_of_range=%lu\ntc.transitions=%lu\n"
                      "tc.rotation=%u\ntc.calibrated=%u\ntc.frame_us_max=%lu\n"
                      "g.pairs=%lu\ng.scroll_v=%lu\ng.scroll_h=%lu\ng.pan=%lu\ng.pinch=%lu\ng.taps=%lu\ng.ends=%lu\n"
                      "g.cancels=%lu\ng.dropped=%lu\ng.events=%lu\ng.updates=%lu\n"
                      "g.x11_button3=%lu\ng.x11_button4=%lu\ng.x11_button5=%lu\ng.x11_dropped=%lu\ng.x11_deferred=%lu\n"
                      "g.pinch_texts=%lu\ng.pinch_set=%lu\ng.pinch_failed=%lu\n"
                      "gate.hold_ms=%u\ngate.still_ms=%u\ngate.move_px=%u\ngate.taps=%lu\ngate.commits=%lu\ngate.discards=%lu\ngate.blocked_active=%lu\n"
                      "gate.latency_ms_last=%lu\ngate.latency_ms_max=%lu\n",
                      (unsigned long)t->frames, (unsigned long)t->failed_frames, (unsigned long)t->points_in,
                      (unsigned long)t->out_of_range, (unsigned long)t->log_total, (unsigned)(t->cfg.rotation & 3),
                      (unsigned)(t->cfg.cal_x0 != t->cfg.cal_x1), (unsigned long)t->frame_us_max,
                      (unsigned long)g->pairs, (unsigned long)g->starts[X11_G_SCROLL_V], (unsigned long)g->starts[X11_G_SCROLL_H],
                      (unsigned long)g->starts[X11_G_PAN], (unsigned long)g->starts[X11_G_PINCH], (unsigned long)g->taps,
                      (unsigned long)g->ends, (unsigned long)g->cancels, (unsigned long)g->dropped, (unsigned long)t->events,
                      (unsigned long)t->updates, (unsigned long)t->clicks_x11[3], (unsigned long)t->clicks_x11[4],
                      (unsigned long)t->clicks_x11[5], (unsigned long)t->clicks_dropped, (unsigned long)t->clicks_deferred,
                      (unsigned long)t->pinch.published, (unsigned long)t->pinch_set, (unsigned long)t->pinch_failed,
                      (unsigned)t->gate.hold_ms, (unsigned)t->gate.still_ms, (unsigned)t->gate.move_px,
                      (unsigned long)t->gate.taps, (unsigned long)t->gate.commits, (unsigned long)t->gate.discards,
                      (unsigned long)t->gate.blocked_active, (unsigned long)t->gate.latency_ms_last,
                      (unsigned long)t->gate.latency_ms_max));
  /* the gesture history (as the panel always printed it): the last 6 events, then the latest UPDATE */
  for (uint32_t k = t->events > 6 ? t->events - 6 : 0; k <= t->events && used + 1 < cap; ++k) {
    int upd = k == t->events;
    if (upd && !t->updates) break;
    const x11_gesture_event *e = upd ? &t->last_update : &t->ev[k % 6];
    char key[16];
    if (upd) snprintf(key, sizeof(key), "g.update");
    else snprintf(key, sizeof(key), "g.ev%lu", (unsigned long)k);
    used = put(cap, used, snprintf(out + used, cap - used, "%s=%lu,%s,%s,%d,%d,%ld,%ld,%.2f,%d\n", key,
                                   (unsigned long)e->t_ms, x11_gesture_event_name(e->type), x11_gesture_kind_name(e->kind),
                                   e->cx, e->cy, (long)e->total_dx, (long)e->total_dy, (double)e->scale, e->steps));
  }
  return used;
}
