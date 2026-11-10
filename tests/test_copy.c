/* Robustness milestone: the region pool (clip regions off the loop task's stack) and the
 * CopyArea span fast path. Run with ASan/UBSan (Makefile).
 *
 *  - pool: LIFO allocation, peak, exhaustion -> empty (never larger) regions, counted
 *  - copy exactness: the fast path against the per-pixel reference path (xr_copy_fast_enabled
 *    = 0), byte for byte and damage box, over random sizes, overlaps in every direction,
 *    multi-rectangle clips with holes, sources and destinations partly outside, depth 16 and 1,
 *    and every case where the fast path must not apply (functions, plane masks, CopyPlane)
 *  - the 810x656 regression (gtk3-demo): window -> pixmap ->
 *    window through the request core: identical pixels and GraphicsExpose events with and
 *    without the fast path, no per-pixel copying, pool balanced after every request, and a
 *    speed ratio check
 *  - a request that finds the pool exhausted answers BadAlloc and draws nothing */
#define _POSIX_C_SOURCE 200809L  /* clock_gettime with -std=c99 on Linux */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../core/x11_internal.h"

static int failures = 0;
#define CHECK(c)                                                             \
  do {                                                                       \
    if (!(c)) {                                                              \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);  \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

static uint32_t rng = 12345;
static uint32_t rnd(void) {
  rng ^= rng << 13, rng ^= rng >> 17, rng ^= rng << 5;
  return rng;
}
static int rr(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }

static double now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* ---- pool -------------------------------------------------------------------------------- */

static xr_pool test_pool;

static void test_pool_basics(void) {
  memset(&test_pool, 0, sizeof(test_pool));
  xr_pool_use(&test_pool);
  int m = xr_pool_mark();
  xr_region *a = xr_pool_get(), *b = xr_pool_get();
  CHECK(a && b && a != b && test_pool.top == 2 && test_pool.peak == 2);
  CHECK(a->n == 0 && !a->overflow);
  xr_pool_release(m);
  CHECK(test_pool.top == 0 && test_pool.peak == 2);
  /* exhaustion: every slot taken */
  for (int i = 0; i < XR_POOL_REGIONS; ++i) CHECK(xr_pool_get() != NULL);
  CHECK(xr_pool_get() == NULL && test_pool.failures == 1);
  /* a region operation that needs a temporary then yields an empty region (never larger) */
  xr_region g;
  xr_region_set(&g, 0, 0, 10, 10);
  xr_rect hole = {3, 3, 6, 6};
  xr_region_subtract(&g, &hole);
  CHECK(g.n == 0 && g.overflow && test_pool.failures == 2);
  xr_region_set(&g, 0, 0, 10, 10);
  xr_rect away = {20, 20, 30, 30};
  xr_region_subtract(&g, &away);  /* nothing to cut: no temporary, unchanged */
  CHECK(g.n == 1 && !g.overflow && test_pool.failures == 2);
  xr_region other;
  xr_region_set(&other, 5, 5, 20, 20);
  xr_region_intersect_region(&g, &other);
  CHECK(g.n == 0 && g.overflow && test_pool.failures == 3);
  xr_pool_release(0);
  /* with room again: exact results, temporaries returned */
  xr_region_set(&g, 0, 0, 10, 10);
  xr_region_subtract(&g, &hole);
  CHECK(xr_region_area(&g) == 91 && !g.overflow && test_pool.top == 0);
  xr_region_set(&other, 0, 0, 5, 10);
  xr_region_intersect_region(&g, &other);
  CHECK(xr_region_area(&g) == 50 - 6 && test_pool.top == 0);
  /* no pool at all: operations needing a temporary are conservative */
  xr_pool_use(NULL);
  CHECK(xr_pool_get() == NULL);
  xr_region_set(&g, 0, 0, 10, 10);
  xr_region_subtract(&g, &hole);
  CHECK(g.n == 0 && g.overflow);
  xr_pool_use(&test_pool);
}

/* ---- copy exactness ------------------------------------------------------------------------ */

#define SW 97
#define SH 61
static uint16_t a16[SW * SH], b16[SW * SH], ref16[SW * SH], src16[SW * SH];
static uint8_t a1[SW * SH], b1[SW * SH], ref1[SW * SH], src1[SW * SH];

static void random_clip(xr_region *g) {
  xr_region_set(g, rr(-5, 20), rr(-5, 15), rr(40, SW + 5), rr(30, SH + 5));
  int holes = rr(0, 4);
  for (int i = 0; i < holes; ++i) {
    xr_rect h = {(int16_t)rr(0, SW), (int16_t)rr(0, SH), 0, 0};
    h.x2 = (int16_t)(h.x1 + rr(1, 30)), h.y2 = (int16_t)(h.y1 + rr(1, 20));
    xr_region_subtract(g, &h);
  }
}

/* One random copy, run twice (fast path allowed / reference), compared. */
static int copy_case(int depth, int same, int function, uint32_t planes, int plane_copy) {
  void *dpix = depth == 16 ? (void *)a16 : (void *)a1, *rpix = depth == 16 ? (void *)ref16 : (void *)ref1;
  void *spix = depth == 16 ? (void *)src16 : (void *)src1;
  size_t bytes = (size_t)SW * SH * (depth == 16 ? 2 : 1);
  for (size_t i = 0; i < (size_t)SW * SH; ++i) {
    if (depth == 16) a16[i] = (uint16_t)rnd(), src16[i] = (uint16_t)rnd();
    else a1[i] = (uint8_t)(rnd() & 1), src1[i] = (uint8_t)(rnd() & 1);
  }
  memcpy(rpix, dpix, bytes);
  if (plane_copy) for (size_t i = 0; i < (size_t)SW * SH; ++i) src1[i] = (uint8_t)(rnd() & 1);
  xr_region clip;
  random_clip(&clip);
  int w = rr(1, SW + 10), h = rr(1, SH + 10);
  int sx = rr(-15, SW - 1), sy = rr(-15, SH - 1), dx = sx + rr(-20, 20), dy = sy + rr(-20, 20);
  if (rr(0, 3) == 0) dx = sx;  /* pure vertical move */
  if (rr(0, 3) == 0) dy = sy;  /* pure horizontal move (overlapping within the row) */
  xr_paint p = {(uint8_t)function, planes, (uint32_t)rnd() & 0xFFFF, (uint32_t)rnd() & 0xFFFF};
  if (depth == 1) p.fg &= 1, p.bg &= 1;
  xr_damage da, dr;
  xr_surface D = {dpix, SW, SH, SW, depth, &da}, R = {rpix, SW, SH, SW, depth, &dr};
  xr_surface Sd = {spix, SW, SH, SW, depth, 0}, Sp = {src1, SW, SH, SW, 1, 0};
  xr_damage_reset(&da);
  xr_damage_reset(&dr);
  int opaque = (int)(rnd() & 1);
  unsigned long fast0 = xr_count.copy_fast_px;
  xr_copy_fast_enabled = 1;
  if (plane_copy) xr_copy_plane(&D, &clip, &p, &Sp, sx, sy, w, h, dx, dy, opaque);
  else xr_copy(&D, &clip, &p, same ? &D : &Sd, sx, sy, w, h, dx, dy);
  int used_fast = xr_count.copy_fast_px != fast0;
  xr_copy_fast_enabled = 0;
  if (plane_copy) xr_copy_plane(&R, &clip, &p, &Sp, sx, sy, w, h, dx, dy, opaque);
  else xr_copy(&R, &clip, &p, same ? &R : &Sd, sx, sy, w, h, dx, dy);
  xr_copy_fast_enabled = 1;
  int ok = memcmp(dpix, rpix, bytes) == 0 &&
           (da.x1 >= da.x2 ? dr.x1 >= dr.x2 : (da.x1 == dr.x1 && da.y1 == dr.y1 && da.x2 == dr.x2 && da.y2 == dr.y2));
  if (!ok)
    fprintf(stderr, "copy mismatch: depth %d same %d f %d planes %x  %dx%d %d,%d -> %d,%d clip n=%d\n", depth, same,
            function, planes, w, h, sx, sy, dx, dy, clip.n);
  CHECK(ok);
  return used_fast;
}

static void test_copy_exact(void) {
  int fast = 0, total = 0;
  for (int i = 0; i < 3000; ++i) {
    int depth = i % 3 == 0 ? 1 : 16, same = rnd() & 1;
    fast += copy_case(depth, same, 3, 0xFFFFFFFFu, 0);
    ++total;
  }
  CHECK(fast > total / 2);  /* most cases have something inside the clip */
  /* cases that must take the per-pixel path, and still agree with it */
  unsigned long f0 = xr_count.copy_fast_px;
  for (int i = 0; i < 600; ++i) {
    int depth = i % 2 ? 16 : 1, f = rr(0, 14);
    copy_case(depth, rnd() & 1, f >= 3 ? f + 1 : f, 0xFFFFFFFFu, 0);  /* every function but GXcopy */
    copy_case(depth, rnd() & 1, 3, depth == 16 ? 0x07E0u : 0u, 0);   /* partial plane mask */
  }
  for (int i = 0; i < 200; ++i) copy_case(16, 0, 3, 0xFFFFFFFFu, 1);  /* CopyPlane (opaque or not) */
  CHECK(xr_count.copy_fast_px == f0);  /* none of these may take the fast path */
  /* explicit overlap directions on one surface, full clip, every direction and distance */
  for (int ddx = -3; ddx <= 3; ++ddx)
    for (int ddy = -3; ddy <= 3; ++ddy) {
      for (size_t i = 0; i < (size_t)SW * SH; ++i) a16[i] = (uint16_t)(i * 2654435761u >> 16);
      memcpy(ref16, a16, sizeof(a16));
      xr_region all;
      xr_region_set(&all, 0, 0, SW, SH);
      xr_paint p = {3, 0xFFFFFFFFu, 0, 0};
      xr_surface D = {a16, SW, SH, SW, 16, 0}, R = {ref16, SW, SH, SW, 16, 0};
      xr_copy_fast_enabled = 1;
      xr_copy(&D, &all, &p, &D, 10, 10, 60, 40, 10 + ddx, 10 + ddy);
      xr_copy_fast_enabled = 0;
      xr_copy(&R, &all, &p, &R, 10, 10, 60, 40, 10 + ddx, 10 + ddy);
      xr_copy_fast_enabled = 1;
      CHECK(memcmp(a16, ref16, sizeof(a16)) == 0);
      /* and against the definition: dst = old src */
      int bad = 0;
      for (int y = 0; y < 40 && !bad; ++y)
        for (int x = 0; x < 60; ++x) {
          uint16_t want = (uint16_t)((size_t)((10 + y) * SW + 10 + x) * 2654435761u >> 16);
          if (a16[(10 + ddy + y) * SW + 10 + ddx + x] != want) { bad = 1; break; }
        }
      CHECK(!bad);
    }
  (void)b16;
  (void)b1;
}

/* ---- the 810x656 regression through the request core ------------------------------------- */

static x11_server srv;
static uint8_t out[X11_REPLY_MAX];
static uint8_t evs[512][32];
static int nev;

static void deliver(void *ctx, int client, const uint8_t *data, size_t len) {
  (void)ctx, (void)client;
  if (nev < 512 && len == 32) memcpy(evs[nev++], data, 32);
}

typedef struct {
  uint8_t b[256];
  size_t n;
} rq;
static void beg(rq *q, uint8_t op, uint8_t d) { memset(q, 0, sizeof(*q)); q->b[0] = op; q->b[1] = d; q->n = 4; }
static void w32(rq *q, uint32_t v) { x_wr32(q->b + q->n, v, 0); q->n += 4; }
static void w16x2(rq *q, uint16_t a, uint16_t b) { x_wr16(q->b + q->n, a, 0); x_wr16(q->b + q->n + 2, b, 0); q->n += 4; }
static int cl;
static int pool_balanced = 1, pool_peak;
static size_t send_q(rq *q) {
  x_wr16(q->b + 2, (uint16_t)(q->n / 4), 0);
  int close_after;
  size_t n = x11_client_request(&srv, cl, q->b, q->n, out, sizeof(out), &close_after);
  if (srv.pool.top != 0) pool_balanced = 0;
  return n;
}

static void start_server(void) {
  x11_server_config cfg;
  x11_default_config(&cfg);
  x11_platform plat = {0};
  plat.deliver = deliver;
  CHECK(x11_server_init_platform(&srv, &cfg, &plat) == 0);
  cl = x11_client_open(&srv);
  uint8_t req[12] = {'l', 0, 11, 0};
  x11_setup_result r;
  uint8_t rep[512];
  x11_client_setup(&srv, cl, req, 12, rep, sizeof(rep), &r);
  CHECK(r.status == X11_SETUP_SUCCESS);
}

/* gtk3-demo's sequence: an 810x656 window at 0,0 (larger than the 800x480 screen), a pixmap
 * of that size, CopyArea window -> pixmap, CopyArea pixmap -> window (a pattern drawn first).
 * Returns the time of the two copies; *fb and *pm receive the results. */
static double run_810(int fast, uint16_t *fb_out, uint16_t *pm_out, int *events_out) {
  start_server();
  xr_copy_fast_enabled = fast;
  uint32_t base = srv.clients[cl].id_base, win = base + 1, pix = base + 2, gc = base + 3;
  rq q;
  beg(&q, 1, 16);  /* CreateWindow depth 16, background pixel, Exposure */
  w32(&q, win); w32(&q, X11_ID_ROOT_WINDOW); w16x2(&q, 0, 0); w16x2(&q, 810, 656); w16x2(&q, 0, 1);
  w32(&q, 0); w32(&q, (1u << 1) | (1u << 11)); w32(&q, 0x1234); w32(&q, X_EV_EXPOSURE);
  CHECK(send_q(&q) == 0);
  beg(&q, 8, 0); w32(&q, win); CHECK(send_q(&q) == 0);  /* MapWindow */
  beg(&q, 53, 16); w32(&q, pix); w32(&q, win); w16x2(&q, 810, 656); CHECK(send_q(&q) == 0);  /* CreatePixmap */
  beg(&q, 55, 0); w32(&q, gc); w32(&q, win); w32(&q, 0); CHECK(send_q(&q) == 0);  /* CreateGC (exposures on) */
  /* a pattern in the window: stripes of PolyFillRectangle in GXxor (per-pixel path, both runs) */
  beg(&q, 56, 0); w32(&q, gc); w32(&q, (1u << 0) | (1u << 2)); w32(&q, 6); w32(&q, 0xA5A5); CHECK(send_q(&q) == 0);
  for (int i = 0; i < 20; ++i) {
    beg(&q, 70, 0); w32(&q, win); w32(&q, gc); w16x2(&q, (uint16_t)(i * 37), (uint16_t)(i * 23)); w16x2(&q, 300, 17);
    CHECK(send_q(&q) == 0);
  }
  beg(&q, 56, 0); w32(&q, gc); w32(&q, 1u << 0); w32(&q, 3); CHECK(send_q(&q) == 0);  /* back to GXcopy */
  nev = 0;
  unsigned long slow0 = xr_count.copy_slow_px, fast0 = xr_count.copy_fast_px;
  double t0 = now_ms();
  beg(&q, 62, 0); w32(&q, win); w32(&q, pix); w32(&q, gc); w16x2(&q, 0, 0); w16x2(&q, 0, 0); w16x2(&q, 810, 656);
  CHECK(send_q(&q) == 0);  /* window -> pixmap */
  beg(&q, 62, 0); w32(&q, pix); w32(&q, win); w32(&q, gc); w16x2(&q, 5, 3); w16x2(&q, 0, 0); w16x2(&q, 810, 656);
  CHECK(send_q(&q) == 0);  /* pixmap -> window, shifted */
  double dt = now_ms() - t0;
  if (fast) CHECK(xr_count.copy_slow_px == slow0 && xr_count.copy_fast_px - fast0 == 800UL * 480 + 800UL * 480);
  else CHECK(xr_count.copy_fast_px == fast0);
  memcpy(fb_out, srv.fb.pix, 800 * 480 * 2);
  x11_resource *r = x_find(&srv, pix);
  CHECK(r && r->type == X11_RES_PIXMAP);
  if (r) memcpy(pm_out, ((x11_pixmap *)r->obj)->surf.pix, 810 * 656 * 2);
  *events_out = nev;
  pool_peak = srv.pool.peak;
  x11_server_free(&srv);
  xr_copy_fast_enabled = 1;
  return dt;
}

static uint16_t fb_a[800 * 480], fb_b[800 * 480], pm_a[810 * 656], pm_b[810 * 656];
static uint8_t ev_a[512][32];

static void test_copy_810(void) {
  int na, nb;
  double t_ref = run_810(0, fb_b, pm_b, &nb);
  memcpy(ev_a, evs, sizeof(evs));
  int na_events_snapshot = nb;
  double t_fast = run_810(1, fb_a, pm_a, &na);
  CHECK(memcmp(fb_a, fb_b, sizeof(fb_a)) == 0);
  CHECK(memcmp(pm_a, pm_b, sizeof(pm_a)) == 0);
  CHECK(na == na_events_snapshot && memcmp(ev_a, evs, (size_t)na * 32) == 0);
  /* GraphicsExpose for the off-screen part of the source window (exposures on): x >= 800 or y >= 480 */
  CHECK(na >= 2 && evs[0][0] == 13);
  CHECK(pool_balanced);
  printf("810x656 CopyArea window->pixmap + pixmap->window (host, this build): reference %.2f ms, fast %.2f ms (%.1fx); "
         "events %d; pool peak %d of %d\n", t_ref, t_fast, t_ref / (t_fast > 0.001 ? t_fast : 0.001), na,
         pool_peak, XR_POOL_REGIONS);
  CHECK(pool_peak >= 2 && pool_peak <= 6);
  CHECK(t_fast * 3 < t_ref);  /* memcpy spans against per-pixel puts: a large margin even under ASan */
}

/* ---- pool exhaustion inside a request ------------------------------------------------------ */

static void test_request_bad_alloc(void) {
  start_server();
  uint32_t base = srv.clients[cl].id_base, win = base + 1, gc = base + 2;
  rq q;
  beg(&q, 1, 16);
  w32(&q, win); w32(&q, X11_ID_ROOT_WINDOW); w16x2(&q, 10, 10); w16x2(&q, 100, 100); w16x2(&q, 0, 1);
  w32(&q, 0); w32(&q, 1u << 1); w32(&q, 0);
  CHECK(send_q(&q) == 0);
  beg(&q, 8, 0); w32(&q, win); CHECK(send_q(&q) == 0);
  beg(&q, 55, 0); w32(&q, gc); w32(&q, win); w32(&q, 1u << 2); w32(&q, 0xFFFF); CHECK(send_q(&q) == 0);
  uint16_t before = ((uint16_t *)srv.fb.pix)[50 * 800 + 50];
  srv.pool.top = XR_POOL_REGIONS;  /* as if every slot were in use */
  uint32_t f0 = srv.pool.failures;
  beg(&q, 70, 0); w32(&q, win); w32(&q, gc); w16x2(&q, 0, 0); w16x2(&q, 100, 100);
  size_t n = send_q(&q);
  CHECK(n == 32 && out[0] == 0 && out[1] == X11_BAD_ALLOC && srv.pool.failures == f0 + 1);
  CHECK(((uint16_t *)srv.fb.pix)[50 * 800 + 50] == before);  /* nothing drawn */
  srv.pool.top = 0;
  n = send_q(&q);  /* the same request with room: drawn */
  CHECK(n == 0 && ((uint16_t *)srv.fb.pix)[50 * 800 + 50] == 0xFFFF);
  char text[2048];
  x11_server_stats(&srv, text, sizeof(text));
  CHECK(strstr(text, "core.region_pool_failures=1\n") != NULL);
  x11_server_free(&srv);
}

int main(void) {
  test_pool_basics();
  test_copy_exact();
  test_copy_810();
  test_request_bad_alloc();
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  puts("x11 region pool and CopyArea tests OK");
  return 0;
}
