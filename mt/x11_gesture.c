/* Two-finger gesture recogniser, see x11_gesture.h. */
#include "x11_gesture.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

enum { S_IDLE, S_ONE, S_PENDING, S_ACTIVE, S_IGNORE, S_TAPWAIT };

void x11_gesture_default_config(x11_gesture_config *c) {
  c->start_px = 32;
  c->pinch_px = 24;
  c->still_px = 8;
  c->same_dir_cos = 0.5f;
  c->min_spread_px = 40;
  c->axis_ratio = 2.0f;
  c->scroll_step_px = 30;
  c->pinch_step = 1.15f;
  c->tap_ms = 300;
  c->tap_slop_px = 12;
}

void x11_gesture_init(x11_gesture *g, const x11_gesture_config *c) {
  memset(g, 0, sizeof(*g));
  if (c) g->cfg = *c;
  else x11_gesture_default_config(&g->cfg);
}

const char *x11_gesture_kind_name(int k) {
  static const char *n[] = {"none", "scroll_v", "scroll_h", "pan", "pinch"};
  return k >= 0 && k <= X11_G_PINCH ? n[k] : "?";
}

const char *x11_gesture_event_name(int t) {
  static const char *n[] = {"?", "two_down", "start", "update", "end", "tap2", "cancel"};
  return t >= 1 && t <= X11_GE_CANCEL ? n[t] : "?";
}

static void push(x11_gesture *g, uint8_t type, uint32_t now, float cx, float cy) {
  if (g->qn == X11_GESTURE_QUEUE) {
    ++g->dropped;
    return;
  }
  x11_gesture_event *e = &g->q[(g->qh + g->qn++) % X11_GESTURE_QUEUE];
  memset(e, 0, sizeof(*e));
  e->type = type, e->kind = g->kind, e->t_ms = now;
  e->cx = (int16_t)lroundf(cx), e->cy = (int16_t)lroundf(cy);
  e->total_dx = (int32_t)lroundf(cx - g->cx0), e->total_dy = (int32_t)lroundf(cy - g->cy0);
  e->scale = 1.0f;
}

int x11_gesture_pop(x11_gesture *g, x11_gesture_event *out) {
  if (!g->qn) return 0;
  *out = g->q[g->qh];
  g->qh = (g->qh + 1) % X11_GESTURE_QUEUE, --g->qn;
  return 1;
}

static float dist(float ax, float ay, float bx, float by) { return sqrtf((ax - bx) * (ax - bx) + (ay - by) * (ay - by)); }

static const x11_gesture_contact *find(const x11_gesture_contact *c, int n, uint8_t id) {
  for (int i = 0; i < n; ++i)
    if (c[i].id == id) return &c[i];
  return 0;
}

/* Whole steps for position pos (in step units): a step is taken each time pos is a full step
 * away from the last one taken, so turning back needs a full step too (no repeats on wobble). */
static int16_t steps_to(x11_gesture *g, float pos) {
  int16_t out = 0;
  while (pos >= g->step_pos + 1.0f) g->step_pos += 1.0f, ++out;
  while (pos <= g->step_pos - 1.0f) g->step_pos -= 1.0f, --out;
  return out;
}

static void baseline(x11_gesture *g, const x11_gesture_contact *a, const x11_gesture_contact *b, uint32_t now) {
  g->ida = a->id, g->idb = b->id;
  g->ax0 = a->x, g->ay0 = a->y, g->bx0 = b->x, g->by0 = b->y;
  g->cx0 = g->lcx = (a->x + b->x) / 2.0f;
  g->cy0 = g->lcy = (a->y + b->y) / 2.0f;
  g->d0 = dist(a->x, a->y, b->x, b->y);
  g->t_pair = now, g->kind = X11_G_NONE, g->step_pos = 0, g->moved_max = 0;
  ++g->pairs;
  g->state = S_PENDING;
  push(g, X11_GE_TWO_DOWN, now, g->cx0, g->cy0);
}

/* The pair lifted (one or both fingers) or broke up: end what was going on. */
static void finish(x11_gesture *g, uint32_t now, int lifted_both, int cancel) {
  if (cancel) {
    push(g, X11_GE_CANCEL, now, g->lcx, g->lcy);
    ++g->cancels;
  } else if (g->state == S_ACTIVE) {
    push(g, X11_GE_END, now, g->lcx, g->lcy);
    if (g->qn) {
      x11_gesture_event *e = &g->q[(g->qh + g->qn - 1) % X11_GESTURE_QUEUE];
      if (e->type == X11_GE_END) e->scale = g->d0 > 0 ? g->ld / g->d0 : 1.0f;
    }
    ++g->ends;
  } else if (lifted_both && now - g->t_pair <= g->cfg.tap_ms && g->moved_max <= g->cfg.tap_slop_px) {
    push(g, X11_GE_TAP2, now, g->cx0, g->cy0);
    ++g->taps;
  }
  g->kind = X11_G_NONE;
}

static void feed(x11_gesture *g, uint32_t now, const x11_gesture_contact *c, int n, int last_n);

void x11_gesture_feed(x11_gesture *g, uint32_t now, const x11_gesture_contact *c, int n) {
  int last_n = g->last_n;
  g->last_n = (uint8_t)(n < 0 ? 0 : n > 255 ? 255 : n);
  feed(g, now, c, n, last_n);
}

static void feed(x11_gesture *g, uint32_t now, const x11_gesture_contact *c, int n, int last_n) {
  if (n <= 0) {
    if (g->state == S_PENDING || g->state == S_ACTIVE) finish(g, now, 1, 0);
    else if (g->state == S_TAPWAIT && now - g->t_pair <= g->cfg.tap_ms) {  /* the second finger lifted too */
      push(g, X11_GE_TAP2, now, g->cx0, g->cy0);
      ++g->taps;
    }
    g->state = S_IDLE;
    return;
  }
  if (g->state == S_TAPWAIT) {  /* one finger of a possible tap still down */
    if (n == 2) {
      baseline(g, &c[0], &c[1], now);
      return;
    }
    const x11_gesture_contact *k = find(c, n, g->tap_id);
    int moved = k ? (int)dist(k->x, k->y, (float)g->tap_x, (float)g->tap_y) : 0;
    if (n > 1 || !k || moved > g->cfg.tap_slop_px || now - g->t_pair > g->cfg.tap_ms) g->state = S_IGNORE;
    return;
  }
  if (g->state == S_PENDING || g->state == S_ACTIVE) {
    const x11_gesture_contact *a = find(c, n, g->ida), *b = find(c, n, g->idb);
    if (n > 2 || (n == 2 && (!a || !b))) {  /* a third finger, or another pair: not a two-finger gesture */
      finish(g, now, 0, 1);
      g->state = S_IGNORE;
      return;
    }
    if (n == 1) {  /* one finger lifted: the gesture ends; the other one is ignored until up */
      if (g->state == S_PENDING && now - g->t_pair <= g->cfg.tap_ms && g->moved_max <= g->cfg.tap_slop_px) {
        /* fingers rarely lift in the same read: a tap if the other one follows in time */
        const x11_gesture_contact *k = a ? a : b;
        g->tap_id = k->id, g->tap_x = k->x, g->tap_y = k->y;
        g->state = S_TAPWAIT;
        return;
      }
      finish(g, now, 0, 0);
      g->state = S_IGNORE;
      return;
    }
    float cx = (a->x + b->x) / 2.0f, cy = (a->y + b->y) / 2.0f, d = dist(a->x, a->y, b->x, b->y);
    int ma = (int)dist(a->x, a->y, (float)g->ax0, (float)g->ay0), mb = (int)dist(b->x, b->y, (float)g->bx0, (float)g->by0);
    if (ma > g->moved_max) g->moved_max = ma;
    if (mb > g->moved_max) g->moved_max = mb;
    if (g->state == S_PENDING) {
      /* Decided by how each finger moved: the same way -> scroll/pan, opposite -> pinch.
       * Nothing is decided before a finger has moved start_px; while one finger is still
       * (a staggered start), not before the other has moved twice that. */
      float vax = a->x - (float)g->ax0, vay = a->y - (float)g->ay0, vbx = b->x - (float)g->bx0, vby = b->y - (float)g->by0;
      float va = sqrtf(vax * vax + vay * vay), vb = sqrtf(vbx * vbx + vby * vby);
      float mx = cx - g->cx0, my = cy - g->cy0, m = sqrtf(mx * mx + my * my), dd = fabsf(d - g->d0);
      float vmax = va > vb ? va : vb, vmin = va < vb ? va : vb;
      int can_pinch = dd >= g->cfg.pinch_px && g->d0 >= g->cfg.min_spread_px;
      if (vmax < g->cfg.start_px) return;
      int kind;  /* 1 translate, 2 pinch, 0 undecided */
      if (vmin >= g->cfg.still_px) {
        float cosv = (vax * vbx + vay * vby) / (va * vb);
        if (cosv >= g->cfg.same_dir_cos) kind = m >= g->cfg.start_px ? 1 : 0;
        else if (cosv <= -g->cfg.same_dir_cos) kind = can_pinch ? 2 : 0;
        else kind = vmax >= 2.0f * g->cfg.start_px ? (can_pinch && dd / g->cfg.pinch_px >= m / g->cfg.start_px ? 2 : 1) : 0;
      } else {
        kind = vmax >= 2.0f * g->cfg.start_px ? (can_pinch && dd >= m ? 2 : 1) : 0;
      }
      if (!kind) return;
      if (kind == 2) g->kind = X11_G_PINCH;
      else if (fabsf(my) >= g->cfg.axis_ratio * fabsf(mx)) g->kind = X11_G_SCROLL_V;
      else if (fabsf(mx) >= g->cfg.axis_ratio * fabsf(my)) g->kind = X11_G_SCROLL_H;
      else g->kind = X11_G_PAN;
      g->state = S_ACTIVE;
      ++g->starts[g->kind];
      push(g, X11_GE_START, now, g->cx0, g->cy0);
      g->lcx = g->cx0, g->lcy = g->cy0, g->ld = g->d0;
    }
    /* S_ACTIVE: an UPDATE when the centroid (or, pinching, the distance) changed */
    float scale = g->d0 > 0 ? d / g->d0 : 1.0f;
    float ddx = cx - g->lcx, ddy = cy - g->lcy;
    int changed = fabsf(ddx) >= 0.5f || fabsf(ddy) >= 0.5f || (g->kind == X11_G_PINCH && fabsf(d - g->ld) >= 0.5f);
    if (!changed) return;
    int16_t steps = 0;
    if (g->kind == X11_G_SCROLL_V) steps = steps_to(g, (cy - g->cy0) / g->cfg.scroll_step_px);
    else if (g->kind == X11_G_SCROLL_H) steps = steps_to(g, (cx - g->cx0) / g->cfg.scroll_step_px);
    else if (g->kind == X11_G_PINCH) {
      if (scale <= 0) scale = 0.01f;
      steps = steps_to(g, logf(scale) / logf(g->cfg.pinch_step));
    }
    push(g, X11_GE_UPDATE, now, cx, cy);
    if (g->qn) {
      x11_gesture_event *e = &g->q[(g->qh + g->qn - 1) % X11_GESTURE_QUEUE];
      if (e->type == X11_GE_UPDATE) {
        e->dx = (int16_t)lroundf(ddx), e->dy = (int16_t)lroundf(ddy), e->scale = scale, e->steps = steps;
      }
    }
    g->lcx = cx, g->lcy = cy, g->ld = d;
    return;
  }
  if (g->state == S_IGNORE) {
    if (n == 2 && last_n == 1) baseline(g, &c[0], &c[1], now);  /* a second finger again: a new pair */
    return;
  }
  /* S_IDLE / S_ONE */
  if (n == 1) {
    g->state = S_ONE;
  } else if (n == 2) {
    baseline(g, &c[0], &c[1], now);
  } else {
    g->state = S_IGNORE;  /* three or more at once: never a two-finger gesture */
  }
}

int x11_gesture_owns_touch(const x11_gesture *g) { return g->state != S_IDLE && g->state != S_ONE; }

int x11_gesture_clicks(const x11_gesture_event *e, int natural, x11_gesture_click *out, int max) {
  if (max > X11_GESTURE_MAX_CLICKS_PER_EVENT) max = X11_GESTURE_MAX_CLICKS_PER_EVENT;
  int n = 0;
  if (e->type == X11_GE_TAP2 && max > 0) {
    out[n].button = 3, out[n].x = e->cx, out[n].y = e->cy, ++n;
  } else if (e->type == X11_GE_UPDATE && e->kind == X11_G_SCROLL_V && e->steps) {
    int down = e->steps > 0;  /* the fingers moved down */
    uint8_t b = (uint8_t)((down == !!natural) ? 4 : 5);
    for (int k = 0; k < (e->steps > 0 ? e->steps : -e->steps) && n < max; ++k)
      out[n].button = b, out[n].x = e->cx, out[n].y = e->cy, ++n;
  }
  return n;
}

/* ---- Stage 4: pinch text for _WOODLAND_GESTURE ---- */
int x11_pinch_text(x11_pinch_pub *p, const x11_gesture_event *e, char *out, size_t cap) {
  const char *phase = NULL;
  if (e->type == X11_GE_START && e->kind == X11_G_PINCH) {
    ++p->seq, p->steps = 0, p->active = 1, phase = "start";
  } else if (!p->active) {
    return 0;
  } else if (e->type == X11_GE_UPDATE && e->kind == X11_G_PINCH) {
    if (!e->steps) return 0;
    p->steps += e->steps, phase = "update";
  } else if (e->type == X11_GE_END) {
    p->active = 0, phase = "end";
  } else if (e->type == X11_GE_CANCEL) {
    p->active = 0, phase = "cancel";
  } else {
    return 0;
  }
  if (!cap) return 0;
  int sc = (int)(e->scale * 100.0f + (e->scale >= 0 ? 0.5f : -0.5f));
  int n = snprintf(out, cap, "pinch %s %lu %ld %d.%02d %d %d", phase, (unsigned long)p->seq, (long)p->steps,
                   sc / 100, (sc < 0 ? -sc : sc) % 100, e->cx, e->cy);
  if (n < 0) return 0;
  if ((size_t)n >= cap) n = (int)cap - 1;
  ++p->published;
  return n;
}
