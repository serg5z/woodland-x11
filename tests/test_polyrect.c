/* PolyRectangle: the outline renderer (core/x11_outline.c) and the request (core/x11_server.c).
 * Reference images are Xvfb's at depth 16 (tools/xts/polyrect_diff.py makes the same comparison
 * live); every case is also drawn with GXxor to check that no pixel is drawn twice. The protocol
 * part covers dash lists, the line styles PolyRectangle accepts, errors and xterm's dashed box
 * for a glyph its font lacks. Both byte orders. Run with ASan/UBSan (Makefile). */
#include <stdio.h>
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

#define FG 0xF81Fu
#define BG 0x07E0u
#define BACK 0x1234u

/* ---- reference images ---------------------------------------------------------------------- */

typedef struct {
  const char *name;
  int w, h, lw, style, cap, join, ndash;
  uint8_t dash[5];
  int offset, nrect;
  int16_t rect[4][4];
  int clip_x, clip_y, nclip;  /* GC clip rectangles (nclip 0: none), relative to the clip origin */
  int16_t clip[2][4];
  const char *img;            /* '.' untouched, 'F' foreground, 'B' background */
} gcase;

static const gcase CASES[] = {
    {"xterm_missing_glyph", 10, 16, 1, 1, 3, 0, 2, {4, 4}, 0, 1, {{2, 3, 4, 10}}, 0, 2, 1, {{0, 0, 6, 12}},
     ".........."
     ".........."
     ".........."
     "..FFFF...."
     "..F......."
     "..F......."
     "..F......."
     "..F......."
     ".........."
     ".........."
     ".........."
     "..F......."
     "..F......."
     "..FFF....."
     ".........."
     ".........."},
    {"thin_onoff", 24, 18, 0, 1, 1, 0, 2, {4, 4}, 0, 1, {{2, 2, 12, 8}}, 0, 0, 0, {{0}},
     "........................"
     "........................"
     "..FFFF....FFFF.........."
     "........................"
     "........................"
     "........................"
     "..............F........."
     "..F...........F........."
     "..F...........F........."
     "..F...........F........."
     "..F....FFFF............."
     "........................"
     "........................"
     "........................"
     "........................"
     "........................"
     "........................"
     "........................"},
    {"thin_double_odd_list", 24, 18, 0, 2, 1, 0, 3, {3, 1, 2}, 4, 1, {{2, 2, 12, 8}}, 0, 0, 0, {{0}},
     "........................"
     "........................"
     "..BFBBBFBBFFFBF........."
     "..B...........F........."
     "..B...........B........."
     "..F...........B........."
     "..F...........B........."
     "..B...........F........."
     "..F...........B........."
     "..F...........B........."
     "..FBBFBBBFFBFFF........."
     "........................"
     "........................"
     "........................"
     "........................"
     "........................"
     "........................"
     "........................"},
    {"thin_degenerate", 24, 18, 0, 1, 0, 0, 2, {2, 1}, 1, 4, {{2, 2, 6, 0}, {10, 2, 0, 5}, {14, 2, 0, 0}, {16, 2, 1, 1}}, 0, 0, 0, {{0}},
     "........................"
     "........................"
     "..FFFFFFF.F.....F......."
     "..........F.....FF......"
     "..........F............."
     "..........F............."
     "..........F............."
     "..........F............."
     "........................"
     "........................"
     "........................"
     "........................"
     "........................"
     "........................"
     "........................"
     "........................"
     "........................"
     "........................"},
    {"lw1_projecting", 24, 18, 1, 1, 3, 0, 2, {4, 4}, 0, 1, {{2, 2, 12, 8}}, 0, 0, 0, {{0}},
     "........................"
     "........................"
     "..FFFFF...FFFF.........."
     "........................"
     "........................"
     "........................"
     "..F...........F........."
     "..F...........F........."
     "..F...........F........."
     "..F...........F........."
     "......FFFFF............."
     "........................"
     "........................"
     "........................"
     "........................"
     "........................"
     "........................"
     "........................"},
    {"lw3_round_caps", 24, 18, 3, 1, 2, 0, 2, {4, 4}, 0, 1, {{3, 3, 14, 9}}, 0, 0, 0, {{0}},
     "........................"
     "........................"
     "..FFFFFFF.FFFFFFF......."
     "..FFFFFFF.FFFFFFF......."
     "..FFFFFFF.FFFFFFFFF....."
     "..FFF...........FFF....."
     "..FFF...........FFF....."
     "..FFF...........FFF....."
     "..FFF...........FFF....."
     "..FFF...........FFF....."
     "..FFF...........FFF....."
     "...FFFFFFF.FFFFFFF......"
     "...FFFFFFF.FFFFFFF......"
     "...FFFFFFF.FFFFFFF......"
     "........................"
     "........................"
     "........................"
     "........................"},
    {"lw3_projecting", 24, 18, 3, 1, 3, 0, 2, {4, 4}, 0, 1, {{3, 3, 14, 9}}, 0, 0, 0, {{0}},
     "........................"
     "........................"
     "..FFFFFFF.FFFFFFF......."
     "..FFFFFFF.FFFFFFF......."
     "..FFFFFFF.FFFFFFFFF....."
     "..FFF...........FFF....."
     "..FFF...........FFF....."
     "..FFF...........FFF....."
     "..FFF...........FFF....."
     "..FFF...........FFF....."
     "..FFF...........FFF....."
     "...FFFFFFF.FFFFFFF......"
     "...FFFFFFF.FFFFFFF......"
     "...FFFFFFF.FFFFFFF......"
     "........................"
     "........................"
     "........................"
     "........................"},
    {"lw2_double_odd_list", 24, 18, 2, 2, 1, 0, 3, {3, 1, 2}, 4, 1, {{3, 3, 14, 9}}, 0, 0, 0, {{0}},
     "........................"
     "........................"
     "..FFFBBBFBBFFFBFFB......"
     "..FFFBBBFBBFFFBFBB......"
     "..FF............BB......"
     "..BB............BB......"
     "..BB............FF......"
     "..FF............BB......"
     "..BB............BB......"
     "..BB............FF......"
     "..BB............FF......"
     "..FFBFFFBBFBBBFFBF......"
     "..FFBFFFBBFBBBFFBB......"
     "........................"
     "........................"
     "........................"
     "........................"
     "........................"},
    {"lw3_double_round_join", 24, 18, 3, 2, 1, 1, 2, {4, 4}, 0, 1, {{3, 3, 14, 10}}, 0, 0, 0, {{0}},
     "........................"
     "........................"
     "..BFFFFBBBBFFFFBBBB....."
     "..BBBFFBBBBFFFFBBBB....."
     "..BBBFFBBBBFFFFBBBB....."
     "..BBB...........FFF....."
     "..BBB...........FFF....."
     "..FFF...........FFF....."
     "..FFF...........FFF....."
     "..FFF...........BBB....."
     "..FFF...........BBB....."
     "..BBB...........BBB....."
     "..BBBFFFFBBBBFFFFBB....."
     "..BBBFFFFBBBBFFFFFF....."
     "..BBBFFFFBBBBFFFFFF....."
     "........................"
     "........................"
     "........................"},
    {"solid_lw3_bevel", 24, 18, 3, 0, 1, 2, 1, {4}, 0, 1, {{3, 3, 12, 8}}, 0, 0, 0, {{0}},
     "........................"
     "........................"
     "...FFFFFFFFFFFFF........"
     "..FFFFFFFFFFFFFFF......."
     "..FFFFFFFFFFFFFFF......."
     "..FFF.........FFF......."
     "..FFF.........FFF......."
     "..FFF.........FFF......."
     "..FFF.........FFF......."
     "..FFF.........FFF......."
     "..FFFFFFFFFFFFFFF......."
     "..FFFFFFFFFFFFFFF......."
     "...FFFFFFFFFFFFF........"
     "........................"
     "........................"
     "........................"
     "........................"
     "........................"},
    {"solid_lw4_round", 24, 18, 4, 0, 1, 1, 1, {4}, 0, 1, {{3, 3, 12, 8}}, 0, 0, 0, {{0}},
     "........................"
     "...FFFFFFFFFFFFF........"
     "..FFFFFFFFFFFFFFF......."
     ".FFFFFFFFFFFFFFFF......."
     ".FFFFFFFFFFFFFFFF......."
     ".FFFF........FFFF......."
     ".FFFF........FFFF......."
     ".FFFF........FFFF......."
     ".FFFF........FFFF......."
     ".FFFFFFFFFFFFFFFF......."
     ".FFFFFFFFFFFFFFFF......."
     ".FFFFFFFFFFFFFFFF......."
     "..FFFFFFFFFFFFFFF......."
     "........................"
     "........................"
     "........................"
     "........................"
     "........................"},
    {"solid_lw3_miter_small", 24, 18, 3, 0, 3, 0, 1, {4}, 0, 4, {{3, 3, 1, 1}, {9, 3, 0, 4}, {14, 3, 5, 0}, {4, 12, 0, 0}}, 0, 0, 0, {{0}},
     "........................"
     "........................"
     "..FFFF........FFFFF....."
     "..FFFF..FFF...FFFFF....."
     "..FFFF..FFF...FFFFF....."
     "..FFFF..FFF............."
     "........FFF............."
     "........................"
     "........................"
     "........................"
     "........................"
     "...FFF.................."
     "...FFF.................."
     "...FFF.................."
     "........................"
     "........................"
     "........................"
     "........................"},
    {"lw5_round_cap_fold", 24, 18, 5, 1, 2, 0, 1, {2}, 15, 1, {{8, 6, 2, 0}}, 0, 0, 0, {{0}},
     "........................"
     "........................"
     "........................"
     "........................"
     "........FFF............."
     ".......FFFF............."
     ".......FFFF............."
     ".......FFFF............."
     "........FFF............."
     "........................"
     "........................"
     "........................"
     "........................"
     "........................"
     "........................"
     "........................"
     "........................"
     "........................"},
    {"lw2_zero_width_cap", 24, 18, 2, 1, 3, 0, 5, {3, 3, 3, 1, 5}, 10, 1, {{10, 3, 0, 12}}, 0, 0, 0, {{0}},
     "........................"
     "........................"
     ".........FF............."
     ".........FF............."
     ".........FF............."
     ".........FF............."
     ".........FF............."
     ".........FF............."
     ".........FF............."
     ".........FF............."
     ".........FF............."
     ".........FF............."
     ".........FF............."
     ".........FF............."
     ".........FF............."
     "........................"
     "........................"
     "........................"},
    {"lw2_double_clipped", 24, 18, 2, 2, 1, 0, 2, {4, 4}, 0, 1, {{2, 2, 18, 12}}, 3, 1, 2, {{0, 0, 10, 8}, {15, 10, 8, 30}},
     "........................"
     "...FFFBBBBFFF..........."
     "...FFFBBBBFFF..........."
     "........................"
     "........................"
     "........................"
     "........................"
     "........................"
     "........................"
     "........................"
     "........................"
     "...................FF..."
     "...................BB..."
     "..................BBB..."
     "..................BBB..."
     "........................"
     "........................"
     "........................"},
};

static uint16_t px[24 * 18];

static void draw(const gcase *c, uint8_t function, uint16_t back) {
  for (int i = 0; i < c->w * c->h; ++i) px[i] = back;
  xr_surface s = {px, c->w, c->h, c->w, 16, 0};
  xr_region clip;
  if (!c->nclip) {
    xr_region_set(&clip, 0, 0, c->w, c->h);
  } else {
    memset(&clip, 0, sizeof(clip));
    for (int i = 0; i < c->nclip; ++i) {
      xr_rect r = {(int16_t)(c->clip_x + c->clip[i][0]), (int16_t)(c->clip_y + c->clip[i][1]),
                   (int16_t)(c->clip_x + c->clip[i][0] + c->clip[i][2]), (int16_t)(c->clip_y + c->clip[i][1] + c->clip[i][3])};
      if (r.x2 > c->w) r.x2 = (int16_t)c->w;
      if (r.y2 > c->h) r.y2 = (int16_t)c->h;
      clip.r[clip.n++] = r;
    }
  }
  xr_paint even = {function, 0xFFFFFFFFu, FG, BG}, odd = even;
  odd.fg = BG;
  xr_line_attr la = {c->lw, (uint8_t)c->style, (uint8_t)c->cap, (uint8_t)c->join, (uint8_t)c->ndash, c->dash, c->offset, 0};
  for (int i = 0; i < c->nrect; ++i)
    xr_rect_outline(&s, &clip, &even, &odd, &la, 0, 0, c->rect[i][0], c->rect[i][1], c->rect[i][2], c->rect[i][3]);
}

static char pixel_char(uint16_t v, uint16_t back) { return v == back ? '.' : v == FG ? 'F' : v == BG ? 'B' : '?'; }

static void test_reference_images(void) {
  for (size_t k = 0; k < sizeof(CASES) / sizeof(CASES[0]); ++k) {
    const gcase *c = &CASES[k];
    draw(c, 3, BACK);
    int bad = 0;
    for (int i = 0; i < c->w * c->h; ++i) bad += pixel_char(px[i], BACK) != c->img[i];
    if (bad) {
      fprintf(stderr, "%s: %d pixels differ from Xvfb (expected | drawn)\n", c->name, bad);
      for (int y = 0; y < c->h; ++y) {
        fprintf(stderr, "  %.*s | ", c->w, c->img + y * c->w);
        for (int x = 0; x < c->w; ++x) fputc(pixel_char(px[y * c->w + x], BACK), stderr);
        fputc('\n', stderr);
      }
    }
    CHECK(bad == 0);
  }
}

/* The protocol draws no pixel twice within one rectangle: with GXxor onto 0 every pixel shows
 * the one colour it got, exactly as GXcopy draws it. */
static void test_each_pixel_once(void) {
  static uint16_t copy[24 * 18];
  for (size_t k = 0; k < sizeof(CASES) / sizeof(CASES[0]); ++k) {
    const gcase *c = &CASES[k];
    if (c->nrect > 1) continue;  /* separate rectangles may overlap; that is drawn twice */
    draw(c, 3, 0);
    memcpy(copy, px, sizeof(copy));
    draw(c, 6, 0);
    CHECK(memcmp(copy, px, (size_t)(c->w * c->h) * 2) == 0);
  }
}

/* Huge rectangles and coordinates near the 16-bit limits: bounded work, no stray pixels. */
static void test_large_coordinates(void) {
  static uint16_t big[64 * 48];
  xr_surface s = {big, 64, 48, 64, 16, 0};
  xr_region all;
  xr_region_set(&all, 0, 0, 64, 48);
  xr_paint even = {3, 0xFFFFFFFFu, FG, BG}, odd = even;
  odd.fg = BG;
  static const uint8_t one[1] = {1};
  const struct { int lw, style, cap, join; } kinds[] = {{0, 0, 1, 0}, {0, 2, 1, 0}, {1, 1, 3, 0}, {7, 2, 2, 1}, {9, 1, 3, 2},
                                                        {70, 1, 2, 1}};
  clock_t t0 = clock();
  for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); ++k) {
    xr_line_attr la = {kinds[k].lw, (uint8_t)kinds[k].style, (uint8_t)kinds[k].cap, (uint8_t)kinds[k].join, 1, one, 0, 0};
    memset(big, 0, sizeof(big));
    xr_rect_outline(&s, &all, &even, &odd, &la, 0, 0, -30000, -30000, 65535, 65535);  /* all sides far outside */
    int n = 0;
    for (int i = 0; i < 64 * 48; ++i) n += big[i] != 0;
    CHECK(n == 0);
    xr_rect_outline(&s, &all, &even, &odd, &la, 0, 0, -20000, 10, 65535, 20);  /* the top crosses the surface */
    n = 0;
    for (int x = 0; x < 64; ++x) n += big[10 * 64 + x] != 0;
    CHECK(n == 64);
    xr_rect_outline(&s, &all, &even, &odd, &la, 0, 0, 32000, 32000, 65535, 65535);  /* corners clamped to 32767 */
  }
  CHECK((double)(clock() - t0) / CLOCKS_PER_SEC < 2.0);
}

/* ---- the request --------------------------------------------------------------------------- */

static x11_server srv;
static uint8_t out[X11_REPLY_MAX];
static int MSB[X11_SERVER_CLIENTS];

static int open_client(int msb) {
  int c = x11_client_open(&srv);
  uint8_t req[12] = {(uint8_t)(msb ? 'B' : 'l'), 0};
  x_wr16(req + 2, 11, msb);
  x11_setup_result r;
  uint8_t rep[512];
  x11_client_setup(&srv, c, req, sizeof(req), rep, sizeof(rep), &r);
  CHECK(r.status == X11_SETUP_SUCCESS);
  MSB[c] = msb;
  return c;
}

typedef struct {
  uint8_t b[256];
  size_t n;
  int msb;
} rq;

static void beg(rq *q, int c, uint8_t op, uint8_t d1) {
  memset(q, 0, sizeof(*q));
  q->msb = MSB[c], q->b[0] = op, q->b[1] = d1, q->n = 4;
}
static void w32(rq *q, uint32_t v) { x_wr32(q->b + q->n, v, q->msb), q->n += 4; }
static void w16x2(rq *q, int a, int b) { x_wr16(q->b + q->n, (uint16_t)a, q->msb), x_wr16(q->b + q->n + 2, (uint16_t)b, q->msb), q->n += 4; }
static void w8(rq *q, uint8_t v) { q->b[q->n++] = v; }
static void pad(rq *q) { while (q->n % 4) q->b[q->n++] = 0; }

/* The error code of the answer (0: none, 1: a reply, 100 + code: an error). */
static int send_q(int c, rq *q) {
  x_wr16(q->b + 2, (uint16_t)(q->n / 4), q->msb);
  int close_after = 0;
  size_t n = x11_client_request(&srv, c, q->b, q->n, out, sizeof(out), &close_after);
  CHECK(!close_after);
  return n == 0 ? 0 : out[0] == 0 ? 100 + out[1] : 1;
}
#define ERR(code) (100 + (code))

static int poly_rect(int c, uint32_t d, uint32_t gc, int x, int y, int w, int h) {
  rq q;
  beg(&q, c, 67, 0);
  w32(&q, d), w32(&q, gc), w16x2(&q, x, y), w16x2(&q, w, h);
  return send_q(c, &q);
}
static int change_gc(int c, uint32_t gc, uint32_t mask, const uint32_t *v, int n) {
  rq q;
  beg(&q, c, 56, 0);
  w32(&q, gc), w32(&q, mask);
  for (int i = 0; i < n; ++i) w32(&q, v[i]);
  return send_q(c, &q);
}
static int set_dashes(int c, uint32_t gc, int offset, const uint8_t *d, int n) {
  rq q;
  beg(&q, c, 58, 0);
  w32(&q, gc), w16x2(&q, offset, n);
  for (int i = 0; i < n; ++i) w8(&q, d[i]);
  pad(&q);
  return send_q(c, &q);
}
static void clear_pixmap(int c, uint32_t pm, uint32_t gc, int w, int h) {
  const uint32_t v[2] = {3, BACK};  /* GXcopy, foreground */
  CHECK(change_gc(c, gc, 0x1 | 0x4, v, 2) == 0);
  rq q;
  beg(&q, c, 70, 0);
  w32(&q, pm), w32(&q, gc), w16x2(&q, 0, 0), w16x2(&q, w, h);
  CHECK(send_q(c, &q) == 0);
  const uint32_t f[1] = {FG};
  CHECK(change_gc(c, gc, 0x4, f, 1) == 0);
}
/* The pixmap as characters ('.' BACK, 'F', 'B'); returns the number written. */
static int read_pixmap(int c, uint32_t pm, int w, int h, char *s) {
  rq q;
  beg(&q, c, 73, 2);
  w32(&q, pm), w16x2(&q, 0, 0), w16x2(&q, w, h), w32(&q, 0xFFFFFFFFu);
  if (send_q(c, &q) != 1) return 0;
  int stride = (w * 2 + 3) & ~3;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) s[y * w + x] = pixel_char(x_rd16(out + 32 + y * stride + 2 * x, 0), BACK);  /* LSBFirst */
  return w * h;
}

static void test_request(int msb) {
  x11_server_config cfg;
  x11_default_config(&cfg);
  CHECK(x11_server_init(&srv, &cfg) == 0);
  int c = open_client(msb);
  uint32_t base = srv.clients[c].id_base, pm = base + 1, gc = base + 2, gc2 = base + 3;
  const gcase *xt = &CASES[0];  /* xterm's box for a missing glyph */
  rq q;
  beg(&q, c, 53, 16);  /* CreatePixmap depth 16 */
  w32(&q, pm), w32(&q, X11_ID_ROOT_WINDOW), w16x2(&q, xt->w, xt->h);
  CHECK(send_q(c, &q) == 0);
  beg(&q, c, 55, 0);  /* CreateGC: foreground, background */
  w32(&q, gc), w32(&q, pm), w32(&q, 0x4 | 0x8), w32(&q, FG), w32(&q, BG);
  CHECK(send_q(c, &q) == 0);

  /* xterm (UTF-8, a glyph the font lacks): clip to the cell, LineOnOffDash width 1, projecting caps */
  clear_pixmap(c, pm, gc, xt->w, xt->h);
  beg(&q, c, 59, 0);  /* SetClipRectangles */
  w32(&q, gc), w16x2(&q, xt->clip_x, xt->clip_y);
  w16x2(&q, xt->clip[0][0], xt->clip[0][1]), w16x2(&q, xt->clip[0][2], xt->clip[0][3]);
  CHECK(send_q(c, &q) == 0);
  const uint32_t dashed[3] = {1, 1, 3};  /* line width 1, LineOnOffDash, CapProjecting */
  CHECK(change_gc(c, gc, 0x10 | 0x20 | 0x40, dashed, 3) == 0);
  CHECK(poly_rect(c, pm, gc, xt->rect[0][0], xt->rect[0][1], xt->rect[0][2], xt->rect[0][3]) == 0);
  char img[24 * 18];
  CHECK(read_pixmap(c, pm, xt->w, xt->h, img) && memcmp(img, xt->img, (size_t)(xt->w * xt->h)) == 0);
  const uint32_t no_clip[1] = {0};
  CHECK(change_gc(c, gc, 0x80000, no_clip, 1) == 0);

  /* other line requests still refuse dashed lines */
  beg(&q, c, 65, 0);  /* PolyLine */
  w32(&q, pm), w32(&q, gc), w16x2(&q, 1, 1), w16x2(&q, 8, 8);
  CHECK(send_q(c, &q) == ERR(X11_BAD_IMPLEMENTATION));
  beg(&q, c, 66, 0);  /* PolySegment */
  w32(&q, pm), w32(&q, gc), w16x2(&q, 1, 1), w16x2(&q, 8, 8);
  CHECK(send_q(c, &q) == ERR(X11_BAD_IMPLEMENTATION));

  /* dash lists: a full list (odd length, offset) through SetDashes; ChangeGC dashes = n is [n] */
  const gcase *odd = &CASES[2];  /* thin_double_odd_list: [3, 1, 2] offset 4, LineDoubleDash */
  CHECK(odd->w == 24 && xt->w * xt->h <= 24 * 18);
  uint32_t pm2 = base + 4;
  beg(&q, c, 53, 16);
  w32(&q, pm2), w32(&q, X11_ID_ROOT_WINDOW), w16x2(&q, odd->w, odd->h);
  CHECK(send_q(c, &q) == 0);
  clear_pixmap(c, pm2, gc, odd->w, odd->h);
  const uint32_t thin_double[4] = {BG, 0, 2, 1};  /* background, width 0, LineDoubleDash, CapButt */
  CHECK(change_gc(c, gc, 0x8 | 0x10 | 0x20 | 0x40, thin_double, 4) == 0);
  CHECK(set_dashes(c, gc, odd->offset, odd->dash, odd->ndash) == 0);
  CHECK(poly_rect(c, pm2, gc, odd->rect[0][0], odd->rect[0][1], odd->rect[0][2], odd->rect[0][3]) == 0);
  CHECK(read_pixmap(c, pm2, odd->w, odd->h, img) && memcmp(img, odd->img, (size_t)(odd->w * odd->h)) == 0);
  beg(&q, c, 57, 0);  /* CopyGC with the dash list and offset */
  w32(&q, gc), w32(&q, gc2), w32(&q, 0);
  CHECK(send_q(c, &q) == ERR(X11_BAD_GC));  /* gc2 does not exist yet */
  beg(&q, c, 55, 0);
  w32(&q, gc2), w32(&q, pm2), w32(&q, 0);
  CHECK(send_q(c, &q) == 0);
  beg(&q, c, 57, 0);
  w32(&q, gc), w32(&q, gc2), w32(&q, 0x7FFFFFu & ~(0x80000u | 0x400u | 0x800u));  /* all but clip mask, tile, stipple */
  CHECK(send_q(c, &q) == 0);
  clear_pixmap(c, pm2, gc, odd->w, odd->h);
  CHECK(poly_rect(c, pm2, gc2, odd->rect[0][0], odd->rect[0][1], odd->rect[0][2], odd->rect[0][3]) == 0);
  CHECK(read_pixmap(c, pm2, odd->w, odd->h, img) && memcmp(img, odd->img, (size_t)(odd->w * odd->h)) == 0);
  const gcase *on = &CASES[1];  /* thin_onoff: [4, 4] */
  const uint32_t onoff[3] = {1, 0, 4};  /* LineOnOffDash, dash offset 0, dashes 4 (the list [4]) */
  CHECK(change_gc(c, gc, 0x20 | 0x100000 | 0x200000, onoff, 3) == 0);
  clear_pixmap(c, pm2, gc, on->w, on->h);
  CHECK(poly_rect(c, pm2, gc, on->rect[0][0], on->rect[0][1], on->rect[0][2], on->rect[0][3]) == 0);
  CHECK(read_pixmap(c, pm2, on->w, on->h, img) && memcmp(img, on->img, (size_t)(on->w * on->h)) == 0);

  /* lists longer than XR_MAX_DASHES are kept for SetDashes but dashed drawing with them is not done */
  uint8_t many[XR_MAX_DASHES + 1];
  for (int i = 0; i < XR_MAX_DASHES + 1; ++i) many[i] = (uint8_t)(1 + i % 3);
  CHECK(set_dashes(c, gc, 0, many, XR_MAX_DASHES + 1) == 0);
  CHECK(poly_rect(c, pm2, gc, 2, 2, 10, 10) == ERR(X11_BAD_IMPLEMENTATION));
  const uint32_t solid[1] = {0};
  CHECK(change_gc(c, gc, 0x20, solid, 1) == 0);
  CHECK(poly_rect(c, pm2, gc, 2, 2, 10, 10) == 0);  /* solid lines do not use the list */

  /* errors */
  const uint8_t zero[2] = {4, 0};
  CHECK(set_dashes(c, gc, 0, zero, 2) == ERR(X11_BAD_VALUE));
  CHECK(set_dashes(c, gc, 0, zero, 0) == ERR(X11_BAD_VALUE));
  const uint32_t no_dash[1] = {0};
  CHECK(change_gc(c, gc, 0x200000, no_dash, 1) == ERR(X11_BAD_VALUE));
  const uint32_t bad_style[1] = {3};
  CHECK(change_gc(c, gc, 0x20, bad_style, 1) == ERR(X11_BAD_VALUE));
  CHECK(poly_rect(c, pm2, base + 99, 1, 1, 2, 2) == ERR(X11_BAD_GC));
  CHECK(poly_rect(c, base + 99, gc, 1, 1, 2, 2) == ERR(X11_BAD_DRAWABLE));
  beg(&q, c, 67, 0);  /* no rectangles: nothing to do */
  w32(&q, pm2), w32(&q, gc);
  CHECK(send_q(c, &q) == 0);
  beg(&q, c, 67, 0);  /* half a rectangle */
  w32(&q, pm2), w32(&q, gc), w16x2(&q, 1, 1);
  CHECK(send_q(c, &q) == ERR(X11_BAD_LENGTH));
  x11_server_free(&srv);
}

int main(void) {
  test_reference_images();
  test_each_pixel_once();
  test_large_coordinates();
  for (int msb = 0; msb < 2; ++msb) test_request(msb);
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  printf("PolyRectangle tests OK (%d reference images)\n", (int)(sizeof(CASES) / sizeof(CASES[0])));
  return 0;
}
