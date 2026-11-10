/* Unit tests for Milestone 2A: the raster backend (core/x11_render.c) and windows, events,
 * properties and drawing through the request core. Run with ASan/UBSan (Makefile). */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../core/x11_internal.h"

/* Region pool use across the whole file: never exhausted, and the peak for the record. */
static int pool_peak_seen;
static unsigned long pool_failures_seen;
static void pool_note(const x11_server *s) {
  if (s->pool.peak > pool_peak_seen) pool_peak_seen = s->pool.peak;
  pool_failures_seen += s->pool.failures;
}
#define x11_server_free(s) (pool_note(s), (x11_server_free)(s))

static int failures = 0;
#define CHECK(c)                                                             \
  do {                                                                       \
    if (!(c)) {                                                              \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);  \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

/* ---- raster ------------------------------------------------------------------------------ */

static uint16_t px[64 * 64];
static xr_surface S = {px, 64, 64, 64, 16, 0};
static xr_region ALL;
static xr_paint COPY = {3, 0xFFFFFFFFu, 0xFFFF, 0};

static long count(uint16_t v) {
  long n = 0;
  for (int i = 0; i < 64 * 64; ++i) n += px[i] == v;
  return n;
}
static void clear(void) { memset(px, 0, sizeof(px)); }

static void test_regions(void) {
  xr_region g;
  xr_region_set(&g, 0, 0, 10, 10);
  xr_rect hole = {3, 3, 6, 6};
  xr_region_subtract(&g, &hole);
  CHECK(xr_region_area(&g) == 100 - 9 && !g.overflow);
  xr_rect cut = {0, 0, 5, 10};
  xr_region_intersect(&g, &cut);
  CHECK(xr_region_area(&g) == 50 - 6);
  xr_region_translate(&g, 10, 0);
  for (int i = 0; i < g.n; ++i) CHECK(g.r[i].x1 >= 10 && g.r[i].x2 <= 15);
  xr_region_set(&g, 5, 5, 5, 9);  /* empty */
  CHECK(xr_region_empty(&g));
}

static void test_fill_and_clip(void) {
  clear();
  xr_fill_rect(&S, &ALL, &COPY, 2, 3, 4, 5);
  CHECK(count(0xFFFF) == 20 && px[3 * 64 + 2] == 0xFFFF && px[7 * 64 + 5] == 0xFFFF && px[8 * 64 + 5] == 0);
  clear();
  xr_region half;
  xr_region_set(&half, 0, 0, 32, 64);
  xr_fill_rect(&S, &half, &COPY, -10, -10, 100, 100);  /* clipped to the region and the surface */
  CHECK(count(0xFFFF) == 32 * 64);
}

static void test_polygons(void) {
  clear();
  xr_point sq[4] = {{0, 0}, {4, 0}, {4, 4}, {0, 4}};
  xr_fill_polygon(&S, &ALL, &COPY, sq, 4, 0);
  CHECK(count(0xFFFF) == 16);  /* pixel centres inside: exactly the 4x4 block */
  /* a self-overlapping square drawn twice in one path: even-odd leaves a hole, winding not */
  xr_point twice[8] = {{0, 0}, {8, 0}, {8, 8}, {0, 8}, {0, 0}, {8, 0}, {8, 8}, {0, 8}};
  clear();
  xr_fill_polygon(&S, &ALL, &COPY, twice, 8, 0);
  long eo = count(0xFFFF);
  clear();
  xr_fill_polygon(&S, &ALL, &COPY, twice, 8, 1);
  CHECK(eo == 0 && count(0xFFFF) == 64);
  /* adjacent polygons share an edge without overlap or gap */
  xr_paint x = COPY;
  x.function = 6;  /* GXxor: overlap would cancel out */
  clear();
  xr_point a[3] = {{0, 0}, {20, 0}, {0, 20}}, b[3] = {{20, 0}, {20, 20}, {0, 20}};
  xr_fill_polygon(&S, &ALL, &x, a, 3, 0);
  xr_fill_polygon(&S, &ALL, &x, b, 3, 0);
  CHECK(count(0xFFFF) == 400);
}

static void test_lines(void) {
  clear();
  xr_line(&S, &ALL, &COPY, 0, 0, 9, 0, 0, 0);
  CHECK(count(0xFFFF) == 10);
  clear();
  xr_line(&S, &ALL, &COPY, 0, 0, 9, 0, 0, 1);  /* CapNotLast / interior joint */
  CHECK(count(0xFFFF) == 9 && px[9] == 0);
  clear();
  xr_line(&S, &ALL, &COPY, 0, 0, 9, 9, 0, 0);
  CHECK(count(0xFFFF) == 10 && px[9 * 64 + 9] == 0xFFFF);
  clear();
  xr_line(&S, &ALL, &COPY, 10, 20, 50, 20, 4, 0);  /* wide: 40 long, 4 wide, butt caps */
  CHECK(count(0xFFFF) == 160);
}

static void test_arcs(void) {
  clear();
  xr_fill_arc(&S, &ALL, &COPY, 2, 2, 40, 40, 0, 360 * 64, 1);
  double area = 3.14159265 * 20 * 20;
  CHECK(fabs((double)count(0xFFFF) - area) < area * 0.03);
  clear();
  xr_fill_arc(&S, &ALL, &COPY, 2, 2, 40, 40, 0, 90 * 64, 1);  /* pie slice: upper right quarter */
  CHECK(fabs((double)count(0xFFFF) - area / 4) < area * 0.03);
  CHECK(px[10 * 64 + 30] == 0xFFFF && px[30 * 64 + 10] == 0);
  clear();
  xr_arc(&S, &ALL, &COPY, 2, 2, 40, 40, 0, 360 * 64, 0);  /* thin outline: about the perimeter */
  long n = count(0xFFFF);
  if (!(n > 100 && n < 140 && px[22 * 64 + 22] == 0)) fprintf(stderr, "thin arc pixels %ld, centre %u\n", n, px[22 * 64 + 22]);
  CHECK(n > 100 && n < 140 && px[22 * 64 + 22] == 0);
}

static void test_copy_and_rop(void) {
  clear();
  for (int i = 0; i < 64; ++i) px[10 * 64 + i] = (uint16_t)i;
  xr_copy(&S, &ALL, &COPY, &S, 0, 10, 64, 1, 0, 11);  /* scroll down by one row */
  CHECK(px[11 * 64 + 63] == 63 && px[10 * 64 + 63] == 63);
  xr_copy(&S, &ALL, &COPY, &S, 0, 10, 60, 1, 4, 10);  /* overlapping to the right */
  CHECK(px[10 * 64 + 4] == 0 && px[10 * 64 + 63] == 59);
  /* XOR twice restores; plane mask keeps the other bits */
  clear();
  xr_paint x = {6, 0xFFFFFFFFu, 0x1234, 0};
  xr_fill_rect(&S, &ALL, &x, 0, 0, 8, 8);
  xr_fill_rect(&S, &ALL, &x, 0, 0, 8, 8);
  CHECK(count(0) == 64 * 64);
  xr_paint pm = {3, 0x00FFu, 0xFFFF, 0};
  xr_fill_rect(&S, &ALL, &pm, 0, 0, 1, 1);
  CHECK(px[0] == 0x00FF);
  /* depth-1 plane drawn opaque */
  uint8_t bits[4] = {1, 0, 1, 0};
  xr_surface b = {bits, 4, 1, 4, 1, 0};
  xr_paint fgbg = {3, 0xFFFFFFFFu, 0xF800, 0x001F};
  xr_copy_plane(&S, &ALL, &fgbg, &b, 0, 0, 4, 1, 0, 5, 1);
  CHECK(px[5 * 64] == 0xF800 && px[5 * 64 + 1] == 0x001F && px[5 * 64 + 2] == 0xF800);
}

/* ---- requests: windows, events, properties, drawing --------------------------------------- */

static x11_server srv;
static uint8_t out[X11_REPLY_MAX];
static uint8_t events[64][32];
static int ev_client[64], nev = 0;

static void deliver(void *ctx, int client, const uint8_t *data, size_t len) {
  (void)ctx;
  if (nev < 64 && len == 32) {
    memcpy(events[nev], data, 32);
    ev_client[nev++] = client;
  }
}

static int open_client(int msb) {
  int c = x11_client_open(&srv);
  uint8_t req[12] = {(uint8_t)(msb ? 'B' : 'l'), 0};
  x_wr16(req + 2, 11, msb);
  x11_setup_result r;
  uint8_t rep[512];
  x11_client_setup(&srv, c, req, 12, rep, sizeof(rep), &r);
  CHECK(r.status == X11_SETUP_SUCCESS);
  return c;
}

typedef struct {
  uint8_t b[512];
  size_t n;
  int msb;
} rq;
static void beg(rq *q, int msb, uint8_t op, uint8_t d) { memset(q, 0, sizeof(*q)); q->msb = msb; q->b[0] = op; q->b[1] = d; q->n = 4; }
static void w32(rq *q, uint32_t v) { x_wr32(q->b + q->n, v, q->msb); q->n += 4; }
static void w16x2(rq *q, uint16_t a, uint16_t b) { x_wr16(q->b + q->n, a, q->msb); x_wr16(q->b + q->n + 2, b, q->msb); q->n += 4; }
static void wbytes(rq *q, const void *p, size_t n) { memcpy(q->b + q->n, p, n); q->n += x_pad4(n); }
static size_t send_q(int c, rq *q) {
  x_wr16(q->b + 2, (uint16_t)(q->n / 4), q->msb);
  int close_after;
  size_t n = x11_client_request(&srv, c, q->b, q->n, out, sizeof(out), &close_after);
  CHECK(!close_after);
  return n;
}

static uint16_t fbpx(int x, int y) { return ((uint16_t *)srv.fb.pix)[y * 800 + x]; }

static void test_window_lifecycle(void) {
  x11_server_config cfg;
  x11_default_config(&cfg);
  x11_platform plat = {0};
  plat.deliver = deliver;
  CHECK(x11_server_init_platform(&srv, &cfg, &plat) == 0);
  int c = open_client(0);
  uint32_t base = srv.clients[c].id_base, win = base + 1;
  rq q;
  /* CreateWindow 100x50 at (10,20), border 2, background white, Exposure|StructureNotify */
  beg(&q, 0, 1, 0);
  w32(&q, win); w32(&q, X11_ID_ROOT_WINDOW); w16x2(&q, 10, 20); w16x2(&q, 100, 50); w16x2(&q, 2, 1);
  w32(&q, 0); w32(&q, (1u << 1) | (1u << 3) | (1u << 11));
  w32(&q, 0xFFFF); w32(&q, 0xF800); w32(&q, X_EV_EXPOSURE | X_EV_STRUCTURE);
  CHECK(send_q(c, &q) == 0);
  CHECK(fbpx(50, 50) != 0xFFFF);  /* not mapped: nothing drawn */
  nev = 0;
  beg(&q, 0, 8, 0); w32(&q, win);  /* MapWindow */
  CHECK(send_q(c, &q) == 0);
  CHECK(nev == 2 && events[0][0] == 19 && events[1][0] == 12);  /* MapNotify, then Expose */
  CHECK(x_rd16(events[1] + 12, 0) == 100 && x_rd16(events[1] + 14, 0) == 50 && x_rd16(events[1] + 16, 0) == 0);
  CHECK(x_rd16(events[1] + 2, 0) == 2);  /* sequence of the MapWindow request */
  CHECK(fbpx(12, 22) == 0xFFFF && fbpx(111, 69) == 0xFFFF);  /* inside: white */
  CHECK(fbpx(10, 20) == 0xF800 && fbpx(113, 71) == 0xF800);  /* border: red */
  /* a child window on top, then GC drawing is clipped by it (ClipByChildren) */
  uint32_t child = base + 2, gc = base + 3;
  beg(&q, 0, 1, 0);
  w32(&q, child); w32(&q, win); w16x2(&q, 10, 10); w16x2(&q, 20, 20); w16x2(&q, 0, 1);
  w32(&q, 0); w32(&q, 1u << 1); w32(&q, 0x07E0);
  send_q(c, &q);
  beg(&q, 0, 8, 0); w32(&q, child);
  send_q(c, &q);
  CHECK(fbpx(12 + 15, 22 + 15) == 0x07E0);
  beg(&q, 0, 55, 0); w32(&q, gc); w32(&q, win); w32(&q, 1u << 2); w32(&q, 0x001F);  /* CreateGC fg blue */
  CHECK(send_q(c, &q) == 0);
  beg(&q, 0, 70, 0); w32(&q, win); w32(&q, gc); w16x2(&q, 0, 0); w16x2(&q, 100, 50);  /* fill all */
  send_q(c, &q);
  CHECK(fbpx(12 + 5, 22 + 5) == 0x001F && fbpx(12 + 15, 22 + 15) == 0x07E0);  /* child untouched */
  /* GC clip rectangle limits drawing */
  beg(&q, 0, 59, 0); w32(&q, gc); w16x2(&q, 0, 0); w16x2(&q, 0, 0); w16x2(&q, 5, 5);  /* 5x5 at origin */
  send_q(c, &q);
  beg(&q, 0, 56, 0); w32(&q, gc); w32(&q, 1u << 2); w32(&q, 0xFFE0);  /* ChangeGC fg yellow */
  send_q(c, &q);
  beg(&q, 0, 70, 0); w32(&q, win); w32(&q, gc); w16x2(&q, 0, 0); w16x2(&q, 100, 50);
  send_q(c, &q);
  CHECK(fbpx(12 + 4, 22 + 4) == 0xFFE0 && fbpx(12 + 5, 22 + 5) == 0x001F);
  /* ImageText8 "A" draws font pixels in fg on bg */
  beg(&q, 0, 56, 0); w32(&q, gc); w32(&q, (1u << 2) | (1u << 3) | (1u << 19)); w32(&q, 0); w32(&q, 0xFFFF); w32(&q, 0);
  send_q(c, &q);
  beg(&q, 1, 76, 1);  /* (built in LSB below) */
  beg(&q, 0, 76, 1); w32(&q, win); w32(&q, gc); w16x2(&q, 40, 30); wbytes(&q, "A", 1);
  CHECK(send_q(c, &q) == 0);
  long black = 0;
  for (int yy = 22 + 20; yy < 22 + 32; ++yy)
    for (int xx = 12 + 40; xx < 12 + 46; ++xx) black += fbpx(xx, yy) == 0;
  CHECK(black > 10 && black < 30);  /* the glyph's pixels; the rest is the white background */
  /* unmapping the child re-exposes the parent there: Expose for the parent, background */
  nev = 0;
  beg(&q, 0, 10, 0); w32(&q, child);
  send_q(c, &q);
  CHECK(fbpx(12 + 15, 22 + 15) == 0xFFFF);
  int got_expose = 0;
  for (int i = 0; i < nev; ++i)
    if (events[i][0] == 12 && x_rd32(events[i] + 4, 0) == win) got_expose = 1;
  CHECK(got_expose);
  /* destroying the window repaints the root there and sends DestroyNotify */
  nev = 0;
  beg(&q, 0, 4, 0); w32(&q, win);
  send_q(c, &q);
  CHECK(fbpx(50, 50) == 0x2945u);
  int destroyed = 0;
  for (int i = 0; i < nev; ++i) destroyed += events[i][0] == 17;
  CHECK(destroyed >= 1);
  CHECK(x11_resources_used(&srv) == 1);  /* only the GC remains */
  x11_server_free(&srv);
}

static void test_properties_across_byte_orders(void) {
  x11_server_config cfg;
  x11_default_config(&cfg);
  x11_platform plat = {0};
  plat.deliver = deliver;
  x11_server_init_platform(&srv, &cfg, &plat);
  int a = open_client(1), b = open_client(0);  /* MSB writer, LSB reader */
  rq q;
  beg(&q, 1, 18, 0);  /* ChangeProperty Replace on root, WM_NAME (39), CARDINAL (6), format 32 */
  w32(&q, X11_ID_ROOT_WINDOW); w32(&q, 39); w32(&q, 6); q.b[q.n] = 32; q.n += 4; w32(&q, 2);
  w32(&q, 0x11223344u); w32(&q, 7);
  CHECK(send_q(a, &q) == 0);
  beg(&q, 1, 18, 2);  /* Append one more */
  w32(&q, X11_ID_ROOT_WINDOW); w32(&q, 39); w32(&q, 6); q.b[q.n] = 32; q.n += 4; w32(&q, 1); w32(&q, 9);
  send_q(a, &q);
  beg(&q, 0, 20, 0); w32(&q, X11_ID_ROOT_WINDOW); w32(&q, 39); w32(&q, 0); w32(&q, 0); w32(&q, 100);
  size_t n = send_q(b, &q);
  CHECK(n == 32 + 12 && out[1] == 32 && x_rd32(out + 8, 0) == 6 && x_rd32(out + 16, 0) == 3);
  CHECK(x_rd32(out + 32, 0) == 0x11223344u && x_rd32(out + 36, 0) == 7 && x_rd32(out + 40, 0) == 9);
  /* partial read: offset 1, length 1 -> one item, 4 bytes after */
  beg(&q, 0, 20, 0); w32(&q, X11_ID_ROOT_WINDOW); w32(&q, 39); w32(&q, 0); w32(&q, 1); w32(&q, 1);
  n = send_q(b, &q);
  CHECK(n == 36 && x_rd32(out + 12, 0) == 4 && x_rd32(out + 32, 0) == 7);
  /* type mismatch: type reported, no data */
  beg(&q, 0, 20, 0); w32(&q, X11_ID_ROOT_WINDOW); w32(&q, 39); w32(&q, 31); w32(&q, 0); w32(&q, 100);
  n = send_q(b, &q);
  CHECK(n == 32 && x_rd32(out + 8, 0) == 6 && x_rd32(out + 12, 0) == 12);
  /* Append with another format: BadMatch */
  beg(&q, 1, 18, 2);
  w32(&q, X11_ID_ROOT_WINDOW); w32(&q, 39); w32(&q, 6); q.b[q.n] = 8; q.n += 4; w32(&q, 1); w32(&q, 0);
  CHECK(send_q(a, &q) == 32 && out[1] == X11_BAD_MATCH);
  /* GetProperty with delete removes it */
  beg(&q, 0, 20, 1); w32(&q, X11_ID_ROOT_WINDOW); w32(&q, 39); w32(&q, 0); w32(&q, 0); w32(&q, 100);
  send_q(b, &q);
  beg(&q, 0, 20, 0); w32(&q, X11_ID_ROOT_WINDOW); w32(&q, 39); w32(&q, 0); w32(&q, 0); w32(&q, 100);
  CHECK(send_q(b, &q) == 32 && x_rd32(out + 8, 0) == 0);
  /* colours: AllocColor quantises to RGB565 and reports the exact visual value */
  beg(&q, 0, 84, 0); w32(&q, X11_ID_DEFAULT_COLORMAP); w16x2(&q, 0xFFFF, 0x8000); w16x2(&q, 0, 0);
  CHECK(send_q(b, &q) == 32 && x_rd32(out + 16, 0) == ((31u << 11) | (32u << 5)));
  beg(&q, 0, 85, 0); w32(&q, X11_ID_DEFAULT_COLORMAP); w16x2(&q, 5, 0); wbytes(&q, "White", 5);
  CHECK(send_q(b, &q) == 32 && x_rd32(out + 8, 0) == 0xFFFF);
  beg(&q, 0, 85, 0); w32(&q, X11_ID_DEFAULT_COLORMAP); w16x2(&q, 7, 0); wbytes(&q, "nocolor", 7);
  CHECK(send_q(b, &q) == 32 && out[0] == 0 && out[1] == X11_BAD_NAME);
  /* fonts: the built-in font opens under its names; others are BadName */
  beg(&q, 0, 45, 0); w32(&q, srv.clients[b].id_base + 9); w16x2(&q, 5, 0); wbytes(&q, "fixed", 5);
  CHECK(send_q(b, &q) == 0);
  beg(&q, 0, 45, 0); w32(&q, srv.clients[b].id_base + 10); w16x2(&q, 24, 0); wbytes(&q, "-*-*-*-R-*-*-*-120-*-*-*", 24);
  CHECK(send_q(b, &q) == 0);
  beg(&q, 0, 45, 0); w32(&q, srv.clients[b].id_base + 11); w16x2(&q, 9, 0); wbytes(&q, "helvetica", 9);
  CHECK(send_q(b, &q) == 32 && out[1] == X11_BAD_NAME);
  beg(&q, 0, 47, 0); w32(&q, srv.clients[b].id_base + 9);
  n = send_q(b, &q);
  CHECK(n == 32 + 28 + 8 + 12 * 95 && x_rd16(out + 46, 0) == 1 && x_rd32(out + 60, 0) == 18 && x_rd16(out + 52, 0) == 10 && x_rd32(out + 56, 0) == 95);
  /* closing a client frees its fonts */
  x11_client_close(&srv, b);
  CHECK(x11_resources_used(&srv) == 0);
  x11_server_free(&srv);
}

/* Damage: every pixel that changes lies inside a reported rectangle, and a small drawing
 * reports a small area (the display pushes only these rectangles to the LCD). */
static uint16_t before[800 * 480];

static void check_damage_covers(const char *what, long max_area) {
  xr_rect d[X11_DAMAGE_RECTS];
  int n = x11_take_damage(&srv, d, X11_DAMAGE_RECTS);
  const uint16_t *now = (const uint16_t *)srv.fb.pix;
  long changed = 0, uncovered = 0, area = 0;
  for (int i = 0; i < n; ++i) area += (long)(d[i].x2 - d[i].x1) * (d[i].y2 - d[i].y1);
  for (int y = 0; y < 480; ++y)
    for (int x = 0; x < 800; ++x) {
      if (now[y * 800 + x] == before[y * 800 + x]) continue;
      ++changed;
      int in = 0;
      for (int i = 0; i < n && !in; ++i) in = x >= d[i].x1 && x < d[i].x2 && y >= d[i].y1 && y < d[i].y2;
      uncovered += !in;
    }
  if (uncovered || area > max_area) fprintf(stderr, "%s: changed %ld uncovered %ld area %ld (max %ld)\n", what, changed,
                                            uncovered, area, max_area);
  CHECK(uncovered == 0);
  CHECK(area <= max_area);
  memcpy(before, now, sizeof(before));
}

static void test_damage(void) {
  x11_server_config cfg;
  x11_default_config(&cfg);
  x11_platform plat = {0};
  plat.deliver = deliver;
  x11_server_init_platform(&srv, &cfg, &plat);
  xr_rect d[X11_DAMAGE_RECTS];
  CHECK(x11_take_damage(&srv, d, X11_DAMAGE_RECTS) == 1 && d[0].x2 == 800 && d[0].y2 == 480);  /* the root */
  CHECK(x11_take_damage(&srv, d, X11_DAMAGE_RECTS) == 0);  /* taken: nothing left */
  memcpy(before, srv.fb.pix, sizeof(before));
  int c = open_client(0);
  uint32_t base = srv.clients[c].id_base, win = base + 1, gc = base + 2;
  rq q;
  beg(&q, 0, 1, 0);  /* 300x200 window at (100, 100), white */
  w32(&q, win); w32(&q, X11_ID_ROOT_WINDOW); w16x2(&q, 100, 100); w16x2(&q, 300, 200); w16x2(&q, 0, 1);
  w32(&q, 0); w32(&q, 1u << 1); w32(&q, 0xFFFF);
  send_q(c, &q);
  check_damage_covers("create (unmapped)", 0);
  beg(&q, 0, 8, 0); w32(&q, win);
  send_q(c, &q);
  check_damage_covers("map", 300 * 200);
  beg(&q, 0, 55, 0); w32(&q, gc); w32(&q, win); w32(&q, 1u << 2); w32(&q, 0x001F);
  send_q(c, &q);
  beg(&q, 0, 70, 0); w32(&q, win); w32(&q, gc); w16x2(&q, 10, 10); w16x2(&q, 10, 10);
  send_q(c, &q);
  CHECK(x11_take_damage(&srv, d, X11_DAMAGE_RECTS) == 1 && d[0].x1 == 110 && d[0].y1 == 110 && d[0].x2 == 120 &&
        d[0].y2 == 120);
  memcpy(before, srv.fb.pix, sizeof(before));
  /* two far apart drawings stay two rectangles; lines, arcs, polygons, text are covered */
  beg(&q, 0, 70, 0); w32(&q, win); w32(&q, gc); w16x2(&q, 0, 0); w16x2(&q, 4, 4); w16x2(&q, 290, 190); w16x2(&q, 4, 4);
  send_q(c, &q);
  CHECK(x11_take_damage(&srv, d, X11_DAMAGE_RECTS) == 2);
  memcpy(before, srv.fb.pix, sizeof(before));
  unsigned seed = 1;
  for (int i = 0; i < 200; ++i) {  /* random primitives, damage checked after each */
    seed = seed * 1103515245u + 12345u;
    int op = (int)(seed >> 16) % 5, x0 = (int)(seed >> 8) % 320 - 10, y0 = (int)(seed >> 4) % 220 - 10;
    beg(&q, 0, 56, 0); w32(&q, gc); w32(&q, (1u << 2) | (1u << 4)); w32(&q, seed & 0xFFFF); w32(&q, (seed >> 20) % 6);
    send_q(c, &q);
    switch (op) {
      case 0: beg(&q, 0, 65, 0); w32(&q, win); w32(&q, gc); w16x2(&q, (uint16_t)x0, (uint16_t)y0); w16x2(&q, (uint16_t)(x0 + 40), (uint16_t)(y0 + 25)); break;
      case 1: beg(&q, 0, 68, 0); w32(&q, win); w32(&q, gc); w16x2(&q, (uint16_t)x0, (uint16_t)y0); w16x2(&q, 30, 20); w16x2(&q, 0, 360 * 64); break;
      case 2: beg(&q, 0, 69, 0); w32(&q, win); w32(&q, gc); w32(&q, 0); w16x2(&q, (uint16_t)x0, (uint16_t)y0); w16x2(&q, (uint16_t)(x0 + 30), (uint16_t)(y0 + 5)); w16x2(&q, (uint16_t)(x0 + 10), (uint16_t)(y0 + 25)); break;
      case 3: beg(&q, 0, 71, 0); w32(&q, win); w32(&q, gc); w16x2(&q, (uint16_t)x0, (uint16_t)y0); w16x2(&q, 20, 30); w16x2(&q, 45 * 64, 200 * 64); break;
      default: beg(&q, 0, 76, 5); w32(&q, win); w32(&q, gc); w16x2(&q, (uint16_t)x0, (uint16_t)(y0 + 12)); wbytes(&q, "Hello", 5); break;
    }
    send_q(c, &q);
    check_damage_covers("random primitive", 300 * 200);
  }
  /* unmap: the root is repainted there */
  beg(&q, 0, 10, 0); w32(&q, win);
  send_q(c, &q);
  check_damage_covers("unmap", 300 * 200);
  /* a full list merges instead of losing areas */
  for (int i = 0; i < 40; ++i) {
    beg(&q, 0, 70, 0); w32(&q, X11_ID_ROOT_WINDOW); w32(&q, gc);
    w16x2(&q, (uint16_t)((i % 8) * 100), (uint16_t)((i / 8) * 90)); w16x2(&q, 3, 3);
    send_q(c, &q);
  }
  check_damage_covers("40 rectangles merged into 16", 800L * 480);
  x11_server_free(&srv);
}

static xr_pool raster_pool;  /* region temporaries before a server exists (it brings its own) */

int main(void) {
  xr_pool_use(&raster_pool);
  xr_region_set(&ALL, 0, 0, 64, 64);
  test_regions();
  test_fill_and_clip();
  test_polygons();
  test_lines();
  test_arcs();
  test_copy_and_rop();
  test_window_lifecycle();
  test_properties_across_byte_orders();
  test_damage();
  CHECK(pool_failures_seen == 0);
  printf("region pool: peak %d of %d, failures %lu\n", pool_peak_seen, XR_POOL_REGIONS, pool_failures_seen);
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  puts("x11 render and window tests OK");
  return 0;
}
