/* Multi-touch contact tracker, see x11_mt.h. */
#include "x11_mt.h"

#include <stdio.h>
#include <string.h>

void x11_mt_init(x11_mt *t, uint16_t width, uint16_t height) {
  memset(t, 0, sizeof(*t));
  t->width = width, t->height = height;
  t->up_after = 2, t->move_px = 1, t->near_px = 40;
}

const char *x11_mt_state_name(int s) {
  static const char *names[] = {"FREE", "NEW", "MOVE", "STILL", "UP"};
  return s >= 0 && s <= X11_MT_UP ? names[s] : "?";
}

static int down(const x11_mt_contact *c) { return c->state == X11_MT_NEW || c->state == X11_MT_MOVE || c->state == X11_MT_STILL; }

int x11_mt_count(const x11_mt *t) {
  int n = 0;
  for (int i = 0; i < X11_MT_MAX; ++i) n += down(&t->c[i]);
  return n;
}

static int dist2(int x1, int y1, int x2, int y2) { return (x1 - x2) * (x1 - x2) + (y1 - y2) * (y1 - y2); }

int x11_mt_sample(x11_mt *t, uint32_t now_ms, int ok, const x11_mt_point *p, int n) {
  int changed = 0;
  ++t->reads;
  /* contacts shown as UP last time are gone now */
  for (int i = 0; i < X11_MT_MAX; ++i)
    if (t->c[i].state == X11_MT_UP) t->c[i].state = X11_MT_FREE, changed = 1;
  uint8_t seen[X11_MT_MAX] = {0};
  uint8_t order[X11_MT_MAX];
  int norder = 0;
  if (!ok) {
    ++t->failed_reads;
    n = 0;
  } else {
    if (n < 0 || n > X11_MT_MAX) {  /* a count the controller cannot send */
      ++t->malformed;
      n = n < 0 ? 0 : X11_MT_MAX;
    }
    int valid = 0;
    for (int k = 0; k < n; ++k) {
      const x11_mt_point *q = &p[k];
      if (q->id > X11_MT_ID_MAX || q->x < 0 || q->y < 0 || q->x >= t->width || q->y >= t->height) {
        ++t->malformed;
        continue;
      }
      int dup = 0;
      for (int j = 0; j < norder; ++j) dup |= order[j] == q->id;
      if (dup) {
        ++t->duplicate_ids;
        continue;
      }
      order[norder++] = q->id;
      ++valid;
      int slot = -1;
      for (int i = 0; i < X11_MT_MAX && slot < 0; ++i)
        if (down(&t->c[i]) && t->c[i].id == q->id) slot = i;
      if (slot >= 0) {
        x11_mt_contact *c = &t->c[slot];
        if (c->missed) ++t->dropouts;
        c->missed = 0;
        int moved = dist2(q->x, q->y, c->x, c->y) > (int)t->move_px * t->move_px;
        uint8_t st = moved ? X11_MT_MOVE : X11_MT_STILL;
        if (moved) ++t->moves;
        if (st != c->state || moved) changed = 1;
        c->state = st, c->x = q->x, c->y = q->y, c->size = q->size, c->last_ms = now_ms, ++c->reports;
        seen[slot] = 1;
      } else {
        for (int i = 0; i < X11_MT_MAX; ++i)
          if (t->c[i].state == X11_MT_FREE) {
            x11_mt_contact *c = &t->c[i];
            memset(c, 0, sizeof(*c));
            c->state = X11_MT_NEW, c->id = q->id, c->x = c->x0 = q->x, c->y = c->y0 = q->y, c->size = q->size;
            c->t_down = c->last_ms = now_ms, c->reports = 1;
            seen[i] = 1, slot = i;
            ++t->news, changed = 1;
            break;
          }
        if (slot < 0) ++t->overflow;  /* every slot taken (one may still be waiting for its release) */
      }
    }
    /* the same IDs in another order than the previous report */
    int common_prev[X11_MT_MAX], cp = 0, common_now[X11_MT_MAX], cn = 0;
    for (int a = 0; a < t->last_n; ++a)
      for (int b = 0; b < norder; ++b)
        if (t->last_order[a] == order[b]) common_prev[cp++] = order[b];
    for (int b = 0; b < norder; ++b)
      for (int a = 0; a < t->last_n; ++a)
        if (t->last_order[a] == order[b]) common_now[cn++] = order[b];
    for (int k = 0; k < cp && k < cn; ++k)
      if (common_prev[k] != common_now[k]) {
        ++t->reorders;
        break;
      }
    memcpy(t->last_order, order, (size_t)norder);
    t->last_n = (uint8_t)norder;
    if (valid) ++t->touched_reads;
    ++t->by_count[valid <= X11_MT_MAX ? valid : X11_MT_MAX + 1];
    if ((uint32_t)valid > t->max_seen) t->max_seen = (uint32_t)valid;
  }
  /* contacts not in this report: released after up_after missing reads */
  for (int i = 0; i < X11_MT_MAX; ++i) {
    x11_mt_contact *c = &t->c[i];
    if (!down(c) || seen[i]) continue;
    if (!ok) c->failed_end = 1;
    if (++c->missed < t->up_after) continue;
    c->state = X11_MT_UP, ++t->ups, changed = 1;
    if (c->failed_end) ++t->lost;
    /* an ID change: another contact appeared after this one was last reported, close to it */
    for (int j = 0; j < X11_MT_MAX; ++j)
      if (j != i && down(&t->c[j]) && t->c[j].t_down > c->last_ms &&
          dist2(t->c[j].x, t->c[j].y, c->x, c->y) <= (int)t->near_px * t->near_px) {
        ++t->id_changes;
        break;
      }
  }
  int downs = x11_mt_count(t);
  if (downs >= 2) {
    if (!t->multi_since) t->multi_since = now_ms ? now_ms : 1;
    uint32_t d = now_ms - t->multi_since;
    if (d > t->multi_ms_max) t->multi_ms_max = d;
  } else {
    t->multi_since = 0;
  }
  if (changed) ++t->seq, t->last_change_ms = now_ms;
  return changed;
}

size_t x11_mt_text(const x11_mt *t, char *out, size_t cap) {
  if (!cap) return 0;
  size_t used = 0;
  int n = snprintf(out, cap,
                   "mt.seq=%lu\nmt.count=%d\nmt.max_seen=%lu\nmt.reads=%lu\nmt.failed_reads=%lu\nmt.touched_reads=%lu\n"
                   "mt.by_count=%lu,%lu,%lu,%lu,%lu,%lu\nmt.new=%lu\nmt.up=%lu\nmt.moves=%lu\nmt.malformed=%lu\nmt.overflow=%lu\n"
                   "mt.duplicate_ids=%lu\nmt.reorders=%lu\nmt.dropouts=%lu\nmt.id_changes=%lu\nmt.lost=%lu\n"
                   "mt.multi_ms_max=%lu\n",
                   (unsigned long)t->seq, x11_mt_count(t), (unsigned long)t->max_seen, (unsigned long)t->reads,
                   (unsigned long)t->failed_reads, (unsigned long)t->touched_reads, (unsigned long)t->by_count[0],
                   (unsigned long)t->by_count[1], (unsigned long)t->by_count[2], (unsigned long)t->by_count[3],
                   (unsigned long)t->by_count[4], (unsigned long)t->by_count[5], (unsigned long)t->news,
                   (unsigned long)t->ups, (unsigned long)t->moves, (unsigned long)t->malformed, (unsigned long)t->overflow,
                   (unsigned long)t->duplicate_ids, (unsigned long)t->reorders, (unsigned long)t->dropouts,
                   (unsigned long)t->id_changes, (unsigned long)t->lost, (unsigned long)t->multi_ms_max);
  if (n < 0) return 0;
  used = (size_t)n < cap ? (size_t)n : cap - 1;
  for (int i = 0; i < X11_MT_MAX && used + 1 < cap; ++i) {
    const x11_mt_contact *c = &t->c[i];
    if (c->state == X11_MT_FREE) continue;
    n = snprintf(out + used, cap - used, "mt.c%d=%u,%d,%d,%u,%s\n", i, c->id, c->x, c->y, c->size,
                 x11_mt_state_name(c->state));
    if (n < 0) break;
    used += (size_t)n < cap - used ? (size_t)n : cap - used - 1;
  }
  return used;
}
