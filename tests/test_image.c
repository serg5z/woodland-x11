/* PutImage span fast path (xr_put_image16) against the per-pixel reference path
 * (xr_image_fast_enabled = 0). Run with ASan/UBSan (Makefile).
 *
 *  - render level: random sizes (1 px to 800 wide), strides, source misalignment, clip regions
 *    with holes, partly and fully off-surface positions, extreme coordinates; pixels and the
 *    damage box identical to the per-pixel loop; every case the fast path must refuse
 *    (functions, plane masks, fill styles, depth 1) refused
 *  - request level: the same random PutImage streams replayed on two servers (fast path on
 *    and off): windows partly off screen, a child window (clip with a hole), the root, depth-16
 *    and depth-1 pixmaps, all 16 functions, plane masks, GC clip rectangles, IncludeInferiors,
 *    ZPixmap / XYPixmap / XYBitmap at depths 1 and 16, left pad, LSB- and MSB-first clients,
 *    and malformed requests (lengths, depths, formats, pad, drawables, GCs): identical reply
 *    and error bytes, framebuffer, pixmaps, damage and fb_rev after every request
 *  - explicit edge cases: zero sizes, the 16 KB request limit, odd widths and pads */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/x11_internal.h"

static int failures = 0;
#define CHECK(c)                                                             \
  do {                                                                       \
    if (!(c)) {                                                              \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);  \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

static uint32_t rng = 20261114;
static uint32_t rnd(void) {
  rng ^= rng << 13, rng ^= rng >> 17, rng ^= rng << 5;
  return rng;
}
static int rr(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }

/* ---- render level ------------------------------------------------------------------------- */

#define SW 211
#define SH 67
static uint16_t a16[SW * SH], ref16[SW * SH];
static uint8_t imgbuf[4 + 900 * 2 * 80];

/* The reference: put_image's per-pixel loop for ZPixmap depth 16 before the fast path. */
static void reference(const xr_surface *s, const xr_region *clip, const xr_paint *p, int x0, int y0, int w, int h,
                      const uint8_t *data, size_t stride) {
  for (int yy = 0; yy < h; ++yy) {
    const uint8_t *r = data + stride * (size_t)yy;
    for (int xx = 0; xx < w; ++xx) {
      xr_paint q = *p;
      q.fg = (uint32_t)(r[2 * xx] | r[2 * xx + 1] << 8);
      xr_point_set(s, clip, &q, x0 + xx, y0 + yy);
    }
  }
}

static void random_clip(xr_region *g) {
  xr_region_set(g, rr(-5, 60), rr(-5, 20), rr(80, SW + 5), rr(30, SH + 5));
  int holes = rr(0, 5);
  for (int i = 0; i < holes; ++i) {
    xr_rect hole = {(int16_t)rr(0, SW), (int16_t)rr(0, SH), 0, 0};
    hole.x2 = (int16_t)(hole.x1 + rr(1, 40)), hole.y2 = (int16_t)(hole.y1 + rr(1, 20));
    xr_region_subtract(g, &hole);
  }
  if (rr(0, 9) == 0) g->n = 0;  /* nothing visible */
}

static int same_damage(const xr_damage *a, const xr_damage *b) {
  if (a->x1 >= a->x2) return b->x1 >= b->x2;
  return a->x1 == b->x1 && a->y1 == b->y1 && a->x2 == b->x2 && a->y2 == b->y2;
}

/* One random image drawn both ways; returns 1 if the fast path took it. */
static int image_case(int function, uint32_t planes, int with_fill) {
  for (size_t i = 0; i < (size_t)SW * SH; ++i) a16[i] = (uint16_t)rnd();
  memcpy(ref16, a16, sizeof(a16));
  int w = rr(0, 3) == 0 ? rr(1, 4) : rr(1, SW + 40), h = rr(1, 40);
  if (rr(0, 20) == 0) w = rr(500, 900);
  size_t stride = (size_t)((w * 2 + 3) & ~3) + (size_t)(rr(0, 4) == 0 ? 4 * rr(1, 3) : 0);
  if (stride * (size_t)h + 4 > sizeof(imgbuf)) h = (int)((sizeof(imgbuf) - 4) / stride);
  const uint8_t *data = imgbuf + rr(0, 3);  /* request data is not necessarily aligned */
  for (size_t i = 0; i < stride * (size_t)h; ++i) ((uint8_t *)data)[i] = (uint8_t)rnd();
  int x = rr(-w - 10, SW + 10), y = rr(-h - 5, SH + 5);
  if (rr(0, 30) == 0) x = rr(0, 1) ? -32768 : 32767 - w;
  if (rr(0, 30) == 0) y = rr(0, 1) ? -32768 : 32000;
  xr_region clip;
  random_clip(&clip);
  xr_paint p = {(uint8_t)function, planes, (uint32_t)rnd() & 0xFFFF, (uint32_t)rnd() & 0xFFFF};
  xr_damage da, dr;
  xr_surface D = {a16, SW, SH, SW, 16, &da, 0, 0, 0}, R = {ref16, SW, SH, SW, 16, &dr, 0, 0, 0};
  xr_damage_reset(&da);
  xr_damage_reset(&dr);
  static uint16_t tile_pix[4] = {1, 2, 3, 4};
  xr_surface tile = {tile_pix, 2, 2, 2, 16, 0, 0, 0, 0};
  xr_fill f = {1, &tile, 0, 0};
  if (with_fill) xr_fill_use(&f);
  xr_image_fast_enabled = 1;
  int fast = xr_put_image16(&D, &clip, &p, x, y, w, h, data, stride);
  if (!fast) reference(&D, &clip, &p, x, y, w, h, data, stride);
  reference(&R, &clip, &p, x, y, w, h, data, stride);
  xr_fill_use(0);
  int ok = memcmp(a16, ref16, sizeof(a16)) == 0 && same_damage(&da, &dr);
  if (!ok)
    fprintf(stderr, "image mismatch: f %d planes %x fill %d %dx%d at %d,%d stride %zu clip n=%d\n", function, planes,
            with_fill, w, h, x, y, stride, clip.n);
  CHECK(ok);
  return fast;
}

static void test_render(void) {
  int fast = 0;
  for (int i = 0; i < 4000; ++i) fast += image_case(3, rr(0, 1) ? 0xFFFFFFFFu : 0xFFFFu, 0);
  CHECK(fast == 4000);  /* every eligible case takes the fast path */
  /* cases that must be refused, and still agree with the reference */
  for (int i = 0; i < 800; ++i) {
    int f = rr(0, 14);
    CHECK(image_case(f >= 3 ? f + 1 : f, 0xFFFFFFFFu, 0) == 0);  /* every function but GXcopy */
    CHECK(image_case(3, (uint32_t)rr(0, 0xFFFE), 0) == 0);       /* partial plane masks */
    CHECK(image_case(3, 0xFFFFFFFFu, 1) == 0);                   /* a fill style in effect */
  }
  /* depth-1 surface: refused */
  uint8_t px1[16] = {0};
  xr_surface s1 = {px1, 4, 4, 4, 1, 0, 0, 0, 0};
  xr_region all;
  xr_region_set(&all, 0, 0, 4, 4);
  xr_paint p = {3, 0xFFFFFFFFu, 1, 0};
  CHECK(xr_put_image16(&s1, &all, &p, 0, 0, 2, 2, imgbuf, 4) == 0);
  /* disabled: refused */
  xr_surface s16 = {a16, SW, SH, SW, 16, 0, 0, 0, 0};
  xr_image_fast_enabled = 0;
  CHECK(xr_put_image16(&s16, &all, &p, 0, 0, 2, 2, imgbuf, 4) == 0);
  xr_image_fast_enabled = 1;
  /* a known pattern lands where expected (LSB first), clipped at the surface edges */
  for (int i = 0; i < SW * SH; ++i) a16[i] = 0;
  uint8_t pat[4 * 3 * 2 + 8];
  for (int i = 0; i < 3; ++i)
    for (int k = 0; k < 4; ++k) pat[i * 8 + 2 * k] = (uint8_t)(0x10 * i + k), pat[i * 8 + 2 * k + 1] = 0xA0;
  xr_region full;
  xr_region_set(&full, 0, 0, SW, SH);
  CHECK(xr_put_image16(&s16, &full, &p, SW - 2, SH - 2, 4, 3, pat, 8) == 1);
  CHECK(a16[(SH - 2) * SW + SW - 2] == 0xA000 && a16[(SH - 2) * SW + SW - 1] == 0xA001);
  CHECK(a16[(SH - 1) * SW + SW - 2] == 0xA010 && a16[(SH - 1) * SW + SW - 1] == 0xA011);
  int written = 0;
  for (int i = 0; i < SW * SH; ++i) written += a16[i] != 0;
  CHECK(written == 4);
}

/* ---- request level ------------------------------------------------------------------------ */

static x11_server sa, sb;
static int cla, clb;
static uint8_t outa[X11_REPLY_MAX], outb[X11_REPLY_MAX];
static int msb;

static void deliver(void *ctx, int client, const uint8_t *data, size_t len) { (void)ctx, (void)client, (void)data, (void)len; }

static void start(x11_server *s, int *cl, int big_endian) {
  x11_server_config cfg;
  x11_default_config(&cfg);
  x11_platform plat = {0};
  plat.deliver = deliver;
  CHECK(x11_server_init_platform(s, &cfg, &plat) == 0);
  *cl = x11_client_open(s);
  uint8_t req[12] = {big_endian ? 'B' : 'l', 0, 0, 11, 0, 0, 0, 0, 0, 0, 0, 0};
  if (!big_endian) req[2] = 11, req[3] = 0;
  x11_setup_result r;
  static uint8_t rep[4096];
  x11_client_setup(s, *cl, req, 12, rep, sizeof(rep), &r);
  CHECK(r.status == X11_SETUP_SUCCESS);
}

typedef struct {
  uint8_t b[16 * 1024 + 64];
  size_t n;
} rq;
static rq q;
static void beg(uint8_t op, uint8_t d) { memset(q.b, 0, 32); q.b[0] = op; q.b[1] = d; q.n = 4; }
static void w32(uint32_t v) { x_wr32(q.b + q.n, v, msb); q.n += 4; }
static void w16x2(uint16_t a, uint16_t b) { x_wr16(q.b + q.n, a, msb); x_wr16(q.b + q.n + 2, b, msb); q.n += 4; }
static void w8x4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  q.b[q.n] = a, q.b[q.n + 1] = b, q.b[q.n + 2] = c, q.b[q.n + 3] = d;
  q.n += 4;
}

static int compare_state(const char *what) {
  int ok = 1;
  if (memcmp(sa.fb.pix, sb.fb.pix, (size_t)sa.fb.stride * sa.fb.h * 2) != 0) ok = 0;
  if (sa.fb_rev != sb.fb_rev) ok = 0;
  xr_rect da[X11_DAMAGE_RECTS], db[X11_DAMAGE_RECTS];
  int na = x11_take_damage(&sa, da, X11_DAMAGE_RECTS), nb = x11_take_damage(&sb, db, X11_DAMAGE_RECTS);
  if (na != nb || memcmp(da, db, sizeof(xr_rect) * (size_t)na) != 0) ok = 0;
  for (int i = 0; i < sa.max_resources; ++i) {
    x11_resource *ra = &sa.resources[i], *rb = &sb.resources[i];
    if (ra->type != rb->type) { ok = 0; break; }
    if (ra->type == X11_RES_PIXMAP && ra->obj) {
      const xr_surface *pa = &((x11_pixmap *)ra->obj)->surf, *pb = &((x11_pixmap *)rb->obj)->surf;
      size_t bytes = (size_t)pa->stride * pa->h * (pa->depth == 16 ? 2 : 1);
      if (memcmp(pa->pix, pb->pix, bytes) != 0) ok = 0;
    }
  }
  if (!ok) fprintf(stderr, "state differs after %s\n", what);
  return ok;
}

/* Sends q (length field from q.n unless keep_len) to both servers; replies must agree. */
static size_t send_both(int keep_len) {
  if (!keep_len) x_wr16(q.b + 2, (uint16_t)(q.n / 4), msb);
  int ca, cb;
  size_t na = x11_client_request(&sa, cla, q.b, q.n, outa, sizeof(outa), &ca);
  xr_image_fast_enabled = 0;
  size_t nb = x11_client_request(&sb, clb, q.b, q.n, outb, sizeof(outb), &cb);
  xr_image_fast_enabled = 1;
  CHECK(na == nb && ca == cb && memcmp(outa, outb, na) == 0);
  return na;
}

static uint32_t win, child, top, pix16, pix1, gc16, gc1, gcroot;

static void setup_scene(void) {
  uint32_t base = sa.clients[cla].id_base;
  CHECK(base == sb.clients[clb].id_base);
  win = base + 1, child = base + 2, top = base + 3, pix16 = base + 4, pix1 = base + 5;
  gc16 = base + 6, gc1 = base + 7, gcroot = base + 8;
  /* a window partly off screen (left), a child inside it, a sibling overlapping it on top */
  beg(1, 16); w32(win); w32(X11_ID_ROOT_WINDOW); w16x2((uint16_t)-40, 30); w16x2(360, 220); w16x2(0, 1);
  w32(0); w32(1u << 1); w32(0x1111); CHECK(send_both(0) == 0);
  beg(1, 16); w32(child); w32(win); w16x2(120, 40); w16x2(80, 60); w16x2(0, 1);
  w32(0); w32(1u << 1); w32(0x2222); CHECK(send_both(0) == 0);
  beg(1, 16); w32(top); w32(X11_ID_ROOT_WINDOW); w16x2(250, 150); w16x2(100, 120); w16x2(0, 1);
  w32(0); w32(1u << 1); w32(0x3333); CHECK(send_both(0) == 0);
  beg(8, 0); w32(child); CHECK(send_both(0) == 0);
  beg(8, 0); w32(win); CHECK(send_both(0) == 0);
  beg(8, 0); w32(top); CHECK(send_both(0) == 0);
  beg(53, 16); w32(pix16); w32(win); w16x2(97, 61); CHECK(send_both(0) == 0);
  beg(53, 1); w32(pix1); w32(win); w16x2(71, 45); CHECK(send_both(0) == 0);
  beg(55, 0); w32(gc16); w32(win); w32(0); CHECK(send_both(0) == 0);
  beg(55, 0); w32(gc1); w32(pix1); w32(0); CHECK(send_both(0) == 0);
  beg(55, 0); w32(gcroot); w32(X11_ID_ROOT_WINDOW); w32(0); CHECK(send_both(0) == 0);
}

/* Random GC state: function, plane mask, foreground/background, clip rectangles, subwindow mode. */
static void random_gc(uint32_t gc) {
  int r = rr(0, 9);
  int function = r < 6 ? 3 : rr(0, 15);
  uint32_t planes = rr(0, 4) ? 0xFFFFFFFFu : (uint32_t)rnd();
  beg(56, 0); w32(gc); w32((1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 15) | (1u << 19));
  w32((uint32_t)function); w32(planes); w32(rnd() & 0xFFFF); w32(rnd() & 0xFFFF); w32(rr(0, 3) == 0);
  w32(0);  /* clip mask None */
  CHECK(send_both(0) == 0);
  if (rr(0, 3) == 0) {  /* SetClipRectangles, Unsorted, possibly overlapping */
    beg(59, 0); w32(gc); w16x2((uint16_t)rr(-20, 20), (uint16_t)rr(-20, 20));
    int n = rr(0, 6);
    for (int i = 0; i < n; ++i) { w16x2((uint16_t)rr(-30, 300), (uint16_t)rr(-30, 200)); w16x2((uint16_t)rr(1, 200), (uint16_t)rr(1, 150)); }
    CHECK(send_both(0) == 0);
  }
}

/* One random PutImage: mostly valid, some malformed. */
static void random_put(void) {
  uint32_t targets[5] = {win, child, X11_ID_ROOT_WINDOW, pix16, pix1};
  int ti = rr(0, 4);
  uint32_t drawable = targets[ti];
  int depth1 = ti == 4;
  uint32_t gc = depth1 ? gc1 : (ti == 2 ? gcroot : gc16);
  if (rr(0, 40) == 0) gc = depth1 ? gc16 : gc1;  /* depth mismatch: BadMatch */
  int format, depth;
  int k = rr(0, 9);
  if (depth1) format = k < 4 ? 0 : (k < 7 ? 1 : 2), depth = 1;
  else if (k < 7) format = 2, depth = 16;
  else if (k < 9) format = 0, depth = 1;      /* XYBitmap: fg/bg */
  else format = rr(0, 3), depth = rr(0, 2) ? 16 : rr(0, 24);  /* XYPixmap 16, bad format or depth */
  int w = rr(0, 5) == 0 ? rr(0, 3) : rr(1, 140), h = rr(0, 6) == 0 ? rr(0, 2) : rr(1, 60);
  int left = (format != 2 || depth == 1) ? rr(0, 7) : (rr(0, 30) == 0 ? 1 : 0);
  size_t row = (format == 0 || depth == 1) ? (((size_t)w + (size_t)left + 7) / 8 + 3) & ~(size_t)3
                                           : ((size_t)w * 2 + 3) & ~(size_t)3;
  while (24 + row * (size_t)h > sizeof(q.b) - 16 && h > 1) --h;
  int dx = rr(-160, 420), dy = rr(-80, 300);
  if (rr(0, 50) == 0) dx = rr(0, 1) ? -32768 : 32767;
  beg(72, (uint8_t)format); w32(drawable); w32(gc); w16x2((uint16_t)w, (uint16_t)h); w16x2((uint16_t)dx, (uint16_t)dy);
  w8x4((uint8_t)left, (uint8_t)depth, 0, 0);
  size_t need = row * (size_t)h;
  for (size_t i = 0; i < need; ++i) q.b[q.n + i] = (uint8_t)rnd();
  q.n += need;
  int bad_len = rr(0, 30) == 0;
  if (bad_len) q.n = q.n >= 28 && rr(0, 1) ? q.n - 4 : q.n + 4;  /* too short or too long */
  if (bad_len && q.n > sizeof(q.b)) q.n -= 8;
  send_both(0);
}

static void test_requests(int big_endian) {
  msb = big_endian;
  start(&sa, &cla, big_endian);
  start(&sb, &clb, big_endian);
  setup_scene();
  CHECK(compare_state("setup"));
  unsigned long fast0 = xr_count.image_fast_px, slow0 = xr_count.image_slow_px;
  int ok = 1;
  for (int i = 0; i < 2500 && ok; ++i) {
    if (rr(0, 4) == 0) random_gc(gc16), random_gc(gc1), random_gc(gcroot);
    random_put();
    ok = compare_state("PutImage");
    CHECK(ok);
  }
  CHECK(xr_count.image_fast_px > fast0 && xr_count.image_slow_px > slow0);  /* both paths exercised */
  /* explicit edge cases */
  struct { int format, depth, w, h, left, dx, dy; uint32_t gc, drawable; } cases[] = {
      {2, 16, 0, 10, 0, 5, 5, gc16, win},        /* zero width */
      {2, 16, 10, 0, 0, 5, 5, gc16, win},        /* zero height */
      {2, 16, 1, 1, 0, -40, 30, gc16, win},      /* one pixel, at the window origin, off screen */
      {2, 16, 3, 5, 0, 799, 479, gcroot, X11_ID_ROOT_WINDOW},  /* bottom-right corner of the screen */
      {2, 16, 81, 50, 0, 0, 0, gc16, pix16},     /* odd width, padded rows, into a pixmap */
      {2, 16, 4096, 2, 0, -2000, 10, gcroot, X11_ID_ROOT_WINDOW},  /* a wide image, mostly off screen */
      {2, 16, 10, 10, 1, 0, 0, gc16, win},       /* left pad with ZPixmap: BadMatch */
      {2, 24, 10, 10, 0, 0, 0, gc16, win},       /* depth 24: BadImplementation */
      {1, 16, 10, 10, 0, 0, 0, gc16, win},       /* XYPixmap depth 16: BadImplementation */
      {3, 16, 10, 10, 0, 0, 0, gc16, win},       /* format 3: BadValue */
      {2, 16, 10, 10, 0, 0, 0, gc16, 0x7777777}, /* bad drawable */
      {2, 16, 10, 10, 0, 0, 0, 0x7777777, win},  /* bad GC */
  };
  for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
    beg(72, (uint8_t)cases[c].format); w32(cases[c].drawable); w32(cases[c].gc);
    w16x2((uint16_t)cases[c].w, (uint16_t)cases[c].h); w16x2((uint16_t)cases[c].dx, (uint16_t)cases[c].dy);
    w8x4((uint8_t)cases[c].left, (uint8_t)cases[c].depth, 0, 0);
    size_t need = (((size_t)cases[c].w * 2 + 3) & ~(size_t)3) * (size_t)cases[c].h;
    for (size_t i = 0; i < need; ++i) q.b[q.n + i] = (uint8_t)(i * 13);
    q.n += need;
    send_both(0);
    CHECK(compare_state("edge case"));
  }
  /* a full-screen image as 16 KB strips (what Xlib sends) */
  for (int y = 0; y < 480; y += 10) {
    beg(72, 2); w32(X11_ID_ROOT_WINDOW); w32(gcroot); w16x2(800, 10); w16x2(0, (uint16_t)y); w8x4(0, 16, 0, 0);
    for (size_t i = 0; i < 16000; ++i) q.b[q.n + i] = (uint8_t)(i + (size_t)y);
    q.n += 16000;
    random_gc(gcroot);
    send_both(0);
  }
  CHECK(compare_state("full-screen strips"));
  x11_server_free(&sa);
  x11_server_free(&sb);
}

int main(void) {
  test_render();
  test_requests(0);
  test_requests(1);
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  printf("x11 PutImage fast path tests OK (fast %lu px, reference %lu px)\n", xr_count.image_fast_px,
         xr_count.image_slow_px);
  return 0;
}
