/* Portable raster backend, see x11_render.h. */
#include "x11_render.h"
#ifdef X11_LATENCY_TRACE
#include "../net/x11_latency_trace.h"
#endif

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define XR_MAX_W 2048
#define FX 16  /* polygon coordinates in 1/16 pixel */

/* ---- region scratch pool ------------------------------------------------------------------ */

static xr_pool *pool;

void xr_pool_use(xr_pool *p) { pool = p; }

xr_pool *xr_pool_current(void) { return pool; }

xr_region *xr_pool_get(void) {
  if (!pool) return 0;
  ++pool->gets;
  if (pool->top >= XR_POOL_REGIONS) {
    ++pool->failures;
    return 0;
  }
  xr_region *g = &pool->slot[pool->top++];
  if (pool->top > pool->peak) pool->peak = pool->top;
  g->n = 0;
  g->overflow = 0;
  return g;
}

int xr_pool_mark(void) { return pool ? pool->top : 0; }

void xr_pool_release(int mark) {
  if (pool && mark >= 0 && mark <= pool->top) pool->top = mark;
}

/* ---- regions ----------------------------------------------------------------------------- */

void xr_region_set(xr_region *g, int x1, int y1, int x2, int y2) {
  g->overflow = 0;
  g->n = 0;
  if (x2 > x1 && y2 > y1) {
    g->r[0].x1 = (int16_t)x1, g->r[0].y1 = (int16_t)y1, g->r[0].x2 = (int16_t)x2, g->r[0].y2 = (int16_t)y2;
    g->n = 1;
  }
}

void xr_region_clear(xr_region *g) { g->n = 0; g->overflow = 0; }

static int isect(xr_rect *o, const xr_rect *a, const xr_rect *b) {
  o->x1 = a->x1 > b->x1 ? a->x1 : b->x1;
  o->y1 = a->y1 > b->y1 ? a->y1 : b->y1;
  o->x2 = a->x2 < b->x2 ? a->x2 : b->x2;
  o->y2 = a->y2 < b->y2 ? a->y2 : b->y2;
  return o->x1 < o->x2 && o->y1 < o->y2;
}

void xr_region_intersect(xr_region *g, const xr_rect *r) {
  int k = 0;
  for (int i = 0; i < g->n; ++i) {
    xr_rect o;
    if (isect(&o, &g->r[i], r)) g->r[k++] = o;
  }
  g->n = k;
}

/* A region temporary is unavailable (pool exhausted): the result becomes empty, which is
 * never more than the exact result. */
static void region_lost(xr_region *g) {
  g->n = 0;
  g->overflow = 1;
}

static void region_copy(xr_region *g, const xr_region *from) {
  g->n = from->n;
  g->overflow = from->overflow;
  memcpy(g->r, from->r, sizeof(xr_rect) * (size_t)from->n);
}

void xr_region_intersect_region(xr_region *g, const xr_region *other) {
  int m = xr_pool_mark();
  xr_region *out = xr_pool_get();
  if (!out) {
    region_lost(g);
    return;
  }
  out->overflow = g->overflow | other->overflow;
  for (int i = 0; i < g->n; ++i)
    for (int j = 0; j < other->n; ++j) {
      xr_rect o;
      if (isect(&o, &g->r[i], &other->r[j])) {
        if (out->n == XR_MAX_RECTS) { out->overflow = 1; break; }
        out->r[out->n++] = o;
      }
    }
  region_copy(g, out);
  xr_pool_release(m);
}

void xr_region_subtract(xr_region *g, const xr_rect *r) {
  int i0 = 0;
  for (; i0 < g->n; ++i0) {  /* nothing to cut: no temporary needed */
    xr_rect o;
    if (isect(&o, &g->r[i0], r)) break;
  }
  if (i0 == g->n) return;
  int m = xr_pool_mark();
  xr_region *outp = xr_pool_get();
  if (!outp) {
    region_lost(g);
    return;
  }
  outp->overflow = g->overflow;
  for (int i = 0; i < g->n; ++i) {
    const xr_rect *a = &g->r[i];
    xr_rect o;
    if (!isect(&o, a, r)) {
      if (outp->n < XR_MAX_RECTS) outp->r[outp->n++] = *a; else outp->overflow = 1;
      continue;
    }
    xr_rect parts[4];
    int np = 0;
    if (a->y1 < o.y1) { parts[np] = *a; parts[np].y2 = o.y1; ++np; }               /* above */
    if (o.y2 < a->y2) { parts[np] = *a; parts[np].y1 = o.y2; ++np; }               /* below */
    if (a->x1 < o.x1) { parts[np] = o; parts[np].x1 = a->x1; parts[np].x2 = o.x1; ++np; }  /* left */
    if (o.x2 < a->x2) { parts[np] = o; parts[np].x1 = o.x2; parts[np].x2 = a->x2; ++np; }  /* right */
    for (int k = 0; k < np; ++k) {
      if (outp->n < XR_MAX_RECTS) outp->r[outp->n++] = parts[k];
      else outp->overflow = 1;  /* dropped: the region only gets smaller, never paints too much */
    }
  }
  region_copy(g, outp);
  xr_pool_release(m);
}

void xr_region_translate(xr_region *g, int dx, int dy) {
  for (int i = 0; i < g->n; ++i) {
    g->r[i].x1 = (int16_t)(g->r[i].x1 + dx), g->r[i].x2 = (int16_t)(g->r[i].x2 + dx);
    g->r[i].y1 = (int16_t)(g->r[i].y1 + dy), g->r[i].y2 = (int16_t)(g->r[i].y2 + dy);
  }
}

int xr_region_empty(const xr_region *g) { return g->n == 0; }

long xr_region_area(const xr_region *g) {
  long a = 0;
  for (int i = 0; i < g->n; ++i) a += (long)(g->r[i].x2 - g->r[i].x1) * (g->r[i].y2 - g->r[i].y1);
  return a;
}

/* ---- pixels ------------------------------------------------------------------------------ */

void xr_damage_reset(xr_damage *d) { d->x1 = d->y1 = 0x7FFF, d->x2 = d->y2 = -0x7FFF; }

static void mark(const xr_surface *s, int x1, int y, int x2) {
  xr_damage *d = s->dmg;
  if (!d || x1 >= x2) return;
  if (x1 < d->x1) d->x1 = x1;
  if (x2 > d->x2) d->x2 = x2;
  if (y < d->y1) d->y1 = y;
  if (y + 1 > d->y2) d->y2 = y + 1;
}

static uint32_t rop(uint8_t f, uint32_t s, uint32_t d) {
  switch (f & 15) {
    case 0: return 0;
    case 1: return s & d;
    case 2: return s & ~d;
    case 3: return s;
    case 4: return ~s & d;
    case 5: return d;
    case 6: return s ^ d;
    case 7: return s | d;
    case 8: return ~(s | d);
    case 9: return ~s ^ d;
    case 10: return ~d;
    case 11: return s | ~d;
    case 12: return ~s;
    case 13: return ~s | d;
    case 14: return ~(s & d);
    default: return 0xFFFFFFFFu;
  }
}

uint32_t xr_get(const xr_surface *s, int x, int y) {
  if (s->depth == 1) return ((const uint8_t *)s->pix)[y * s->stride + x];
  return ((const uint16_t *)s->pix)[y * s->stride + x];
}

static const xr_fill *fill;  /* xr_fill_use; NULL: solid */
void xr_fill_use(const xr_fill *f) { fill = f && f->style ? f : 0; }

static int fill_mod(int a, int m) {
  int r = a % m;
  return r < 0 ? r + m : r;
}

/* The fill style's source at (x, y) for a solid source src; 0 when a stipple leaves it out. */
static int fill_source(const xr_paint *p, int x, int y, uint32_t *src) {
  const xr_surface *t = fill->pattern;
  if (fill->style == 1) {
    if (t) *src = xr_get(t, fill_mod(x - fill->x, t->w), fill_mod(y - fill->y, t->h));
    return 1;
  }
  int bit = t ? (int)xr_get(t, fill_mod(x - fill->x, t->w), fill_mod(y - fill->y, t->h)) : 1;
  if (fill->style == 2) return bit;  /* Stippled: the foreground where the stipple is 1 */
  *src = bit ? p->fg : p->bg;        /* OpaqueStippled */
  return 1;
}

static void put(const xr_surface *s, const xr_paint *p, int x, int y, uint32_t src) {
  if (fill && !fill_source(p, x, y, &src)) return;
  mark(s, x, y, x + 1);
  uint32_t full = s->depth == 1 ? 1u : 0xFFFFu, pm = p->plane_mask & full;
  if (s->depth == 1) {
    uint8_t *d = (uint8_t *)s->pix + y * s->stride + x;
    uint32_t v = (p->function == 3 && pm == full) ? src : ((*d & ~pm) | (rop(p->function, src, *d) & pm));
    *d = (uint8_t)(v & 1u);
  } else {
    uint16_t *d = (uint16_t *)s->pix + y * s->stride + x;
    uint32_t v = (p->function == 3 && pm == full) ? src : ((*d & ~pm) | (rop(p->function, src, *d) & pm));
    *d = (uint16_t)v;
  }
}

/* Span [x1, x2) on row y in colour src, clipped. */
static void span(const xr_surface *s, const xr_region *clip, const xr_paint *p, int y, int x1, int x2, uint32_t src) {
  if (y < 0 || y >= s->h) return;
  for (int i = 0; i < clip->n; ++i) {
    const xr_rect *r = &clip->r[i];
    if (y < r->y1 || y >= r->y2) continue;
    int a = x1 > r->x1 ? x1 : r->x1, b = x2 < r->x2 ? x2 : r->x2;
    if (a < 0) a = 0;
    if (b > s->w) b = s->w;
#ifdef X11_LATENCY_TRACE
    if (a < b) x11_latency_trace_polyfill_pixels((uint64_t)(b - a));
#endif
    if (!fill && s->depth == 16 && p->function == 3 && (p->plane_mask & 0xFFFFu) == 0xFFFFu) {
      mark(s, a, y, b);
      uint16_t *d = (uint16_t *)s->pix + y * s->stride;
      for (int x = a; x < b; ++x) d[x] = (uint16_t)src;
    } else {
      for (int x = a; x < b; ++x) put(s, p, x, y, src);
    }
  }
}

static int in_clip(const xr_surface *s, const xr_region *clip, int x, int y) {
  if (x < 0 || y < 0 || x >= s->w || y >= s->h) return 0;
  for (int i = 0; i < clip->n; ++i)
    if (x >= clip->r[i].x1 && x < clip->r[i].x2 && y >= clip->r[i].y1 && y < clip->r[i].y2) return 1;
  return 0;
}

void xr_point_set(const xr_surface *s, const xr_region *clip, const xr_paint *p, int x, int y) {
  if (in_clip(s, clip, x, y)) put(s, p, x, y, p->fg);
}

void xr_fill_rect(const xr_surface *s, const xr_region *clip, const xr_paint *p, int x, int y, int w, int h) {
  if (w <= 0 || h <= 0) return;
  for (int yy = y; yy < y + h; ++yy) span(s, clip, p, yy, x, x + w, p->fg);
}

/* ---- polygons (1/16 pixel coordinates) ---------------------------------------------------- */

#define MAX_POLY XR_MAX_POLY
static xr_point fxbuf[MAX_POLY + 4];

static void fill_polygon_fx(const xr_surface *s, const xr_region *clip, const xr_paint *p, const xr_point *pt, int n,
                            int rule) {
  if (n < 3) return;
  int32_t ymin = pt[0].y, ymax = pt[0].y;
  for (int i = 1; i < n; ++i) {
    if (pt[i].y < ymin) ymin = pt[i].y;
    if (pt[i].y > ymax) ymax = pt[i].y;
  }
  int row0 = (int)floorf((float)ymin / FX), row1 = (int)ceilf((float)ymax / FX);
  if (row0 < 0) row0 = 0;
  if (row1 > s->h) row1 = s->h;
  static float xs[MAX_POLY];
  static int dirs[MAX_POLY];
  for (int row = row0; row < row1; ++row) {
    float yc = (row + 0.5f) * FX;  /* pixel centre */
    int k = 0;
    for (int i = 0; i < n && k < MAX_POLY; ++i) {
      const xr_point *a = &pt[i], *b = &pt[(i + 1) % n];
      if (a->y == b->y) continue;
      int up = a->y < b->y;
      float ya = (float)(up ? a->y : b->y), yb = (float)(up ? b->y : a->y);
      if (yc < ya || yc >= yb) continue;  /* half-open in y: shared vertices count once */
      float xa = (float)(up ? a->x : b->x), xb = (float)(up ? b->x : a->x);
      xs[k] = xa + (xb - xa) * (yc - ya) / (yb - ya);
      dirs[k] = up ? 1 : -1;
      ++k;
    }
    for (int i = 1; i < k; ++i)  /* insertion sort (few crossings per row) */
      for (int j = i; j > 0 && xs[j] < xs[j - 1]; --j) {
        float t = xs[j]; xs[j] = xs[j - 1]; xs[j - 1] = t;
        int d = dirs[j]; dirs[j] = dirs[j - 1]; dirs[j - 1] = d;
      }
    int wind = 0;
    for (int i = 0; i + 1 < k; ++i) {
      wind += dirs[i];
      int inside = rule ? wind != 0 : (i % 2 == 0);
      if (!inside) continue;
      /* pixels whose centre x+0.5 lies in [xs[i], xs[i+1]) */
      int a = (int)ceilf(xs[i] / FX - 0.5f), b = (int)ceilf(xs[i + 1] / FX - 0.5f);
      span(s, clip, p, row, a, b, p->fg);
    }
  }
}

void xr_fill_polygon(const xr_surface *s, const xr_region *clip, const xr_paint *p, const xr_point *pts, int n,
                     int rule) {
  if (n > MAX_POLY) n = MAX_POLY;
  for (int i = 0; i < n; ++i) fxbuf[i].x = pts[i].x * FX, fxbuf[i].y = pts[i].y * FX;
  fill_polygon_fx(s, clip, p, fxbuf, n, rule);
}

/* ---- lines ------------------------------------------------------------------------------- */

static void thin_line(const xr_surface *s, const xr_region *clip, const xr_paint *p, int x0, int y0, int x1, int y1,
                      int skip_last) {
  int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0;
  int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
  for (;;) {
    int last = x0 == x1 && y0 == y1;
    if (!(last && skip_last)) xr_point_set(s, clip, p, x0, y0);
    if (last) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

static void disc(const xr_surface *s, const xr_region *clip, const xr_paint *p, float cx, float cy, float r);

void xr_line(const xr_surface *s, const xr_region *clip, const xr_paint *p, int x0, int y0, int x1, int y1, int width,
             int skip_last) {
  if (width <= 1) {
    thin_line(s, clip, p, x0, y0, x1, y1, skip_last);
    return;
  }
  float dx = (float)(x1 - x0), dy = (float)(y1 - y0), len = sqrtf(dx * dx + dy * dy);
  if (len == 0) {
    disc(s, clip, p, x0 + 0.5f, y0 + 0.5f, width / 2.0f);
    return;
  }
  /* centred on pixel centres; butt caps: a quad of the line's width */
  float nx = -dy / len * width / 2.0f, ny = dx / len * width / 2.0f;
  float ax = x0 + 0.5f, ay = y0 + 0.5f, bx = x1 + 0.5f, by = y1 + 0.5f;
  xr_point q[4] = {{(int32_t)lroundf((ax + nx) * FX), (int32_t)lroundf((ay + ny) * FX)},
                   {(int32_t)lroundf((bx + nx) * FX), (int32_t)lroundf((by + ny) * FX)},
                   {(int32_t)lroundf((bx - nx) * FX), (int32_t)lroundf((by - ny) * FX)},
                   {(int32_t)lroundf((ax - nx) * FX), (int32_t)lroundf((ay - ny) * FX)}};
  fill_polygon_fx(s, clip, p, q, 4, 1);
}

static void disc(const xr_surface *s, const xr_region *clip, const xr_paint *p, float cx, float cy, float r) {
  int n = 24;
  xr_point q[24];
  for (int i = 0; i < n; ++i) {
    float t = (float)i * 6.2831853f / n;
    q[i].x = (int32_t)lroundf((cx + r * cosf(t)) * FX), q[i].y = (int32_t)lroundf((cy + r * sinf(t)) * FX);
  }
  fill_polygon_fx(s, clip, p, q, n, 1);
}

/* ---- arcs -------------------------------------------------------------------------------- */

static int arc_points(xr_point *out, int max, float cx, float cy, float rx, float ry, int a1, int a2) {
  float start = (float)a1 / 64.0f * 3.14159265f / 180.0f, ext = (float)a2 / 64.0f * 3.14159265f / 180.0f;
  if (ext > 6.2831853f) ext = 6.2831853f;
  if (ext < -6.2831853f) ext = -6.2831853f;
  int steps = (int)(fabsf(ext) * 180.0f / 3.14159265f * 2.0f);  /* 0.5 degree steps */
  if (steps < 4) steps = 4;
  if (steps > max - 2) steps = max - 2;
  for (int i = 0; i <= steps; ++i) {
    float t = start + ext * (float)i / (float)steps;
    out[i].x = (int32_t)lroundf((cx + rx * cosf(t)) * FX);
    out[i].y = (int32_t)lroundf((cy - ry * sinf(t)) * FX);  /* y grows downwards */
  }
  return steps + 1;
}

void xr_fill_arc(const xr_surface *s, const xr_region *clip, const xr_paint *p, int x, int y, int w, int h, int a1,
                 int a2, int arc_mode) {
  if (w <= 0 || h <= 0 || a2 == 0) return;
  float cx = x + w / 2.0f, cy = y + h / 2.0f;
  int n = arc_points(fxbuf, MAX_POLY - 1, cx, cy, w / 2.0f, h / 2.0f, a1, a2);
  int full = a2 >= 360 * 64 || a2 <= -360 * 64;
  if (arc_mode == 1 && !full) {  /* PieSlice: close through the centre */
    fxbuf[n].x = (int32_t)lroundf(cx * FX), fxbuf[n].y = (int32_t)lroundf(cy * FX);
    ++n;
  }
  fill_polygon_fx(s, clip, p, fxbuf, n, 1);
}

void xr_arc(const xr_surface *s, const xr_region *clip, const xr_paint *p, int x, int y, int w, int h, int a1, int a2,
            int width) {
  if (w < 0 || h < 0 || a2 == 0) return;
  float cx = x + w / 2.0f, cy = y + h / 2.0f;
  if (width <= 1) {
    /* thin: the ellipse sampled finely, reduced to an 8-connected chain of pixels (duplicates
       and redundant L-corners removed) and drawn once per pixel (XOR-safe). Streamed through
       a three-point window: no buffer proportional to the arc length. */
    float start = (float)a1 / 64.0f * 3.14159265f / 180.0f, ext = (float)a2 / 64.0f * 3.14159265f / 180.0f;
    if (ext > 6.2831853f) ext = 6.2831853f;
    if (ext < -6.2831853f) ext = -6.2831853f;
    float rx = w / 2.0f, ry = h / 2.0f;
    int steps = (int)(fabsf(ext) * (rx > ry ? rx : ry) * 4.0f) + 8;
    int full = a2 >= 360 * 64 || a2 <= -360 * 64;
    xr_point win[3], first = {0, 0};
    int k = 0, emitted = 0;
    for (int i = 0; i <= steps; ++i) {
      float t = start + ext * (float)i / (float)steps;
      int px = (int)floorf(cx + rx * cosf(t)), py = (int)floorf(cy - ry * sinf(t));
      if (px == x + w && w) px = x + w - 1;  /* the right/bottom extremes stay on the last pixel */
      if (py == y + h && h) py = y + h - 1;
      if (k && win[k - 1].x == px && win[k - 1].y == py) continue;
      win[k].x = px, win[k].y = py;
      ++k;
      if (k == 3 && abs(win[0].x - win[2].x) <= 1 && abs(win[0].y - win[2].y) <= 1) {
        win[1] = win[2];  /* drop the middle of an L */
        k = 2;
      }
      if (k == 3) {
        if (!emitted++) first = win[0];
        xr_point_set(s, clip, p, win[0].x, win[0].y);
        win[0] = win[1], win[1] = win[2];
        k = 2;
      }
    }
    for (int i = 0; i < k; ++i) {
      if (full && emitted && win[i].x == first.x && win[i].y == first.y) continue;  /* closed: once */
      if (!emitted++) first = win[i];
      xr_point_set(s, clip, p, win[i].x, win[i].y);
    }
    return;
  }
  /* wide: the ring between two ellipses, as one polygon (outer forward, inner back) */
  float hw = width / 2.0f;
  static xr_point outer[MAX_POLY / 2], inner[MAX_POLY / 2];
  int n = arc_points(outer, MAX_POLY / 2, cx, cy, w / 2.0f + hw, h / 2.0f + hw, a1, a2);
  float irx = w / 2.0f - hw, iry = h / 2.0f - hw;
  if (irx < 0) irx = 0;
  if (iry < 0) iry = 0;
  int m = arc_points(inner, MAX_POLY / 2, cx, cy, irx, iry, a1, a2);
  int k = 0;
  for (int i = 0; i < n; ++i) fxbuf[k++] = outer[i];
  for (int i = m - 1; i >= 0; --i) fxbuf[k++] = inner[i];
  fill_polygon_fx(s, clip, p, fxbuf, k, 1);
}

/* ---- copies ------------------------------------------------------------------------------ */

static uint16_t linebuf[XR_MAX_W];
xr_counters xr_count;
int xr_copy_fast_enabled = 1;

static void copy_rows(const xr_surface *dst, const xr_region *clip, const xr_paint *p, const xr_surface *src, int sx,
                      int sy, int w, int h, int dx, int dy, int plane, int opaque) {
  /* clamp the source rectangle to the source surface */
  if (sx < 0) { dx -= sx; w += sx; sx = 0; }
  if (sy < 0) { dy -= sy; h += sy; sy = 0; }
  if (sx + w > src->w) w = src->w - sx;
  if (sy + h > src->h) h = src->h - sy;
  if (w <= 0 || h <= 0 || w > XR_MAX_W) return;
  int same = src->pix == dst->pix;
  int bottom_up = same && dy > sy;
  /* Fast path: a plain copy (GXcopy, all planes) between surfaces of the same depth moves
   * each clipped span with one memcpy and marks the damage once per span. Within one
   * surface the source row is staged in linebuf first and rows go bottom-up when moving
   * down, exactly like the per-pixel path below (which stays the reference for every other
   * function, plane mask and for CopyPlane). */
  uint32_t full = dst->depth == 1 ? 1u : 0xFFFFu;
  if (xr_copy_fast_enabled && !plane && src->depth == dst->depth && p->function == 3 &&
      (p->plane_mask & full) == full) {
    size_t bpp = dst->depth == 1 ? 1 : 2;
    for (int i = 0; i < h; ++i) {
      int r = bottom_up ? h - 1 - i : i;
      int y = dy + r;
      if (y < 0 || y >= dst->h) continue;
      const uint8_t *srow = (const uint8_t *)src->pix + ((size_t)(sy + r) * (size_t)src->stride + (size_t)sx) * bpp;
      if (same) {  /* the row may overlap itself: whole row first */
        memcpy(linebuf, srow, (size_t)w * bpp);
        srow = (const uint8_t *)linebuf;
      }
      uint8_t *drow = (uint8_t *)dst->pix + (size_t)y * (size_t)dst->stride * bpp;
      for (int c = 0; c < clip->n; ++c) {
        const xr_rect *cr = &clip->r[c];
        if (y < cr->y1 || y >= cr->y2) continue;
        int a = dx > cr->x1 ? dx : cr->x1, b = dx + w < cr->x2 ? dx + w : cr->x2;
        if (a < 0) a = 0;
        if (b > dst->w) b = dst->w;
        if (a >= b) continue;
        memcpy(drow + (size_t)a * bpp, srow + (size_t)(a - dx) * bpp, (size_t)(b - a) * bpp);
        mark(dst, a, y, b);
        xr_count.copy_fast_px += (unsigned long)(b - a);
      }
    }
    return;
  }
  for (int i = 0; i < h; ++i) {
    int r = bottom_up ? h - 1 - i : i;
    for (int x = 0; x < w; ++x) linebuf[x] = (uint16_t)xr_get(src, sx + x, sy + r);  /* whole row first */
    int y = dy + r;
    if (y < 0 || y >= dst->h) continue;
    for (int c = 0; c < clip->n; ++c) {
      const xr_rect *cr = &clip->r[c];
      if (y < cr->y1 || y >= cr->y2) continue;
      int a = dx > cr->x1 ? dx : cr->x1, b = dx + w < cr->x2 ? dx + w : cr->x2;
      if (a < 0) a = 0;
      if (b > dst->w) b = dst->w;
      if (b > a) xr_count.copy_slow_px += (unsigned long)(b - a);
      for (int x = a; x < b; ++x) {
        uint32_t v = linebuf[x - dx];
        if (plane) {
          if (v) put(dst, p, x, y, p->fg);
          else if (opaque) put(dst, p, x, y, p->bg);
        } else {
          put(dst, p, x, y, v);
        }
      }
    }
  }
}

void xr_copy(const xr_surface *dst, const xr_region *clip, const xr_paint *p, const xr_surface *src, int sx, int sy,
             int w, int h, int dx, int dy) {
  copy_rows(dst, clip, p, src, sx, sy, w, h, dx, dy, 0, 0);
}

void xr_copy_plane(const xr_surface *dst, const xr_region *clip, const xr_paint *p, const xr_surface *src, int sx,
                   int sy, int w, int h, int dx, int dy, int opaque) {
  copy_rows(dst, clip, p, src, sx, sy, w, h, dx, dy, 1, opaque);
}

/* ---- tiles -------------------------------------------------------------------------------- */

static int mod(int a, int m) {
  int r = a % m;
  return r < 0 ? r + m : r;
}

void xr_fill_tile(const xr_surface *dst, const xr_region *clip, const xr_paint *p, const xr_surface *tile, int ox,
                  int oy, int x, int y, int w, int h) {
  if (w <= 0 || h <= 0 || tile->w <= 0 || tile->h <= 0) return;
  for (int i = 0; i < clip->n; ++i) {
    xr_rect r = clip->r[i], a = {(int16_t)x, (int16_t)y, (int16_t)(x + w), (int16_t)(y + h)}, o;
    xr_rect bounds = {0, 0, (int16_t)dst->w, (int16_t)dst->h};
    if (!isect(&o, &r, &a) || !isect(&o, &o, &bounds)) continue;
    for (int yy = o.y1; yy < o.y2; ++yy) {
      int ty = mod(yy - oy, tile->h);
      for (int xx = o.x1; xx < o.x2; ++xx) put(dst, p, xx, yy, xr_get(tile, mod(xx - ox, tile->w), ty));
    }
  }
}
