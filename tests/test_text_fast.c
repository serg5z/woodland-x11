/* Row-based glyph drawing (draw_glyphs fast path) against the per-pixel reference path
 * (xr_text_fast_enabled = 0). Run with ASan/UBSan (Makefile); built a second time with the
 * generated XTS test fonts when TEST_FONT_DIR is given (build/test_text_fast_fonts).
 *
 *  - the same random text streams replayed on two servers (fast path on and off), for LSB- and
 *    MSB-first clients: ImageText8/16 and PolyText8/16 (several items, deltas, font shifts,
 *    bad fonts, truncated items, wrong lengths), every font (fixed: byte table; cursor: 16-bit
 *    rows; test fonts: PCF bit rows, some wider than 32 pixels), random and missing
 *    characters (default character), text partly and fully off screen and at extreme
 *    coordinates, into a window partly off screen, a child window, the root, depth-16 and
 *    depth-1 pixmaps, with all 16 functions, plane masks, fill styles (tiled, stippled, opaque
 *    stippled), GC clip rectangles and IncludeInferiors: identical replies and errors,
 *    framebuffer, pixmaps, damage list and fb_rev after every request
 *  - per font, a direct GXcopy check that the fast path was taken and matches */
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

static x11_server sa, sb;
static int cla, clb, msb;
static uint8_t outa[X11_REPLY_MAX], outb[X11_REPLY_MAX];
static void deliver(void *ctx, int client, const uint8_t *d, size_t n) { (void)ctx, (void)client, (void)d, (void)n; }

static void start(x11_server *s, int *cl) {
  x11_server_config cfg;
  x11_default_config(&cfg);
  x11_platform plat = {0};
  plat.deliver = deliver;
  CHECK(x11_server_init_platform(s, &cfg, &plat) == 0);
  *cl = x11_client_open(s);
  uint8_t req[12] = {(uint8_t)(msb ? 'B' : 'l'), 0};
  x_wr16(req + 2, 11, msb);
  x11_setup_result r;
  static uint8_t rep[4096];
  x11_client_setup(s, *cl, req, 12, rep, sizeof(rep), &r);
  CHECK(r.status == X11_SETUP_SUCCESS);
}

typedef struct {
  uint8_t b[2048];
  size_t n;
} rq;
static rq q;
static void beg(uint8_t op, uint8_t d) { memset(q.b, 0, sizeof(q.b)); q.b[0] = op; q.b[1] = d; q.n = 4; }
static void w32(uint32_t v) { x_wr32(q.b + q.n, v, msb); q.n += 4; }
static void w16(uint16_t v) { x_wr16(q.b + q.n, v, msb); q.n += 2; }
static void w8(uint8_t v) { q.b[q.n++] = v; }
static void pad(void) { while (q.n % 4) q.b[q.n++] = 0; }

static int compare_state(const char *what) {
  int ok = memcmp(sa.fb.pix, sb.fb.pix, (size_t)sa.fb.stride * sa.fb.h * 2) == 0 && sa.fb_rev == sb.fb_rev;
  xr_rect da[X11_DAMAGE_RECTS], db[X11_DAMAGE_RECTS];
  int na = x11_take_damage(&sa, da, X11_DAMAGE_RECTS), nb = x11_take_damage(&sb, db, X11_DAMAGE_RECTS);
  if (na != nb || memcmp(da, db, sizeof(xr_rect) * (size_t)na) != 0) ok = 0;
  for (int i = 0; i < sa.max_resources && ok; ++i) {
    x11_resource *ra = &sa.resources[i], *rb = &sb.resources[i];
    if (ra->type != rb->type) ok = 0;
    else if (ra->type == X11_RES_PIXMAP && ra->obj) {
      const xr_surface *pa = &((x11_pixmap *)ra->obj)->surf, *pb = &((x11_pixmap *)rb->obj)->surf;
      if (memcmp(pa->pix, pb->pix, (size_t)pa->stride * pa->h * (pa->depth == 16 ? 2 : 1)) != 0) ok = 0;
    }
  }
  if (!ok) fprintf(stderr, "state differs after %s\n", what);
  return ok;
}

static unsigned long a_fast, a_slow;  /* glyphs drawn by server A (fast path allowed), by path */

static size_t send_both(void) {
  x_wr16(q.b + 2, (uint16_t)(q.n / 4), msb);
  int ca, cb;
  unsigned long f0 = xr_count.text_fast_glyphs, s0 = xr_count.text_slow_glyphs;
  size_t na = x11_client_request(&sa, cla, q.b, q.n, outa, sizeof(outa), &ca);
  a_fast += xr_count.text_fast_glyphs - f0, a_slow += xr_count.text_slow_glyphs - s0;
  xr_text_fast_enabled = 0;
  size_t nb = x11_client_request(&sb, clb, q.b, q.n, outb, sizeof(outb), &cb);
  xr_text_fast_enabled = 1;
  CHECK(na == nb && ca == cb && memcmp(outa, outb, na) == 0);
  return na;
}

#define MAXF 64
static uint32_t fonts[MAXF];
static const x11_font *fontp[MAXF];
static int nfonts;
static uint32_t win, child, top, pix16, pix1, tile, stip, gc16, gc1, gcroot, gcs;

static void open_font(uint32_t id, const char *name) {
  beg(45, 0); w32(id); w16((uint16_t)strlen(name)); w16(0);
  memcpy(q.b + q.n, name, strlen(name)); q.n += strlen(name); pad();
  CHECK(send_both() == 0);
}

static void setup_scene(void) {
  uint32_t base = sa.clients[cla].id_base;
  win = base + 1, child = base + 2, top = base + 3, pix16 = base + 4, pix1 = base + 5;
  tile = base + 6, stip = base + 7, gc16 = base + 8, gc1 = base + 9, gcroot = base + 10, gcs = base + 11;
  beg(1, 16); w32(win); w32(X11_ID_ROOT_WINDOW); w16((uint16_t)-40); w16(30); w16(360); w16(220); w16(0); w16(1);
  w32(0); w32(1u << 1); w32(0x1111); CHECK(send_both() == 0);
  beg(1, 16); w32(child); w32(win); w16(120); w16(40); w16(80); w16(60); w16(0); w16(1); w32(0); w32(1u << 1); w32(0x2222);
  CHECK(send_both() == 0);
  beg(1, 16); w32(top); w32(X11_ID_ROOT_WINDOW); w16(250); w16(150); w16(100); w16(120); w16(0); w16(1);
  w32(0); w32(1u << 1); w32(0x3333); CHECK(send_both() == 0);
  beg(8, 0); w32(child); CHECK(send_both() == 0);
  beg(8, 0); w32(win); CHECK(send_both() == 0);
  beg(8, 0); w32(top); CHECK(send_both() == 0);
  beg(53, 16); w32(pix16); w32(win); w16(97); w16(61); CHECK(send_both() == 0);
  beg(53, 1); w32(pix1); w32(win); w16(71); w16(45); CHECK(send_both() == 0);
  beg(53, 16); w32(tile); w32(win); w16(8); w16(8); CHECK(send_both() == 0);
  beg(53, 1); w32(stip); w32(win); w16(8); w16(8); CHECK(send_both() == 0);
  beg(55, 0); w32(gc16); w32(win); w32(0); CHECK(send_both() == 0);
  beg(55, 0); w32(gc1); w32(pix1); w32(0); CHECK(send_both() == 0);
  beg(55, 0); w32(gcroot); w32(X11_ID_ROOT_WINDOW); w32(0); CHECK(send_both() == 0);
  beg(55, 0); w32(gcs); w32(stip); w32(0); CHECK(send_both() == 0);
  /* a two-colour tile and a checkered stipple */
  beg(70, 0); w32(tile); w32(gc16); w16(0); w16(0); w16(8); w16(8); CHECK(send_both() == 0);
  beg(56, 0); w32(gc16); w32(1u << 2); w32(0xF81F); CHECK(send_both() == 0);
  beg(70, 0); w32(tile); w32(gc16); w16(0); w16(0); w16(4); w16(4); w16(4); w16(4); w16(4); w16(4); CHECK(send_both() == 0);
  beg(56, 0); w32(gcs); w32(1u << 2); w32(0); CHECK(send_both() == 0);
  beg(70, 0); w32(stip); w32(gcs); w16(0); w16(0); w16(8); w16(8); CHECK(send_both() == 0);
  beg(56, 0); w32(gcs); w32(1u << 2); w32(1); CHECK(send_both() == 0);
  beg(70, 0); w32(stip); w32(gcs); w16(0); w16(0); w16(3); w16(5); w16(5); w16(2); w16(3); w16(6); CHECK(send_both() == 0);
  nfonts = 0;
  for (int k = 0; k < 2; ++k) fontp[nfonts] = x11_fonts[k], fonts[nfonts] = base + 20 + (uint32_t)nfonts, ++nfonts;
#ifdef X11_TEST_FONTS
  for (int k = 0; k < x11_test_font_count && nfonts < MAXF; ++k)
    fontp[nfonts] = x11_test_fonts[k], fonts[nfonts] = base + 20 + (uint32_t)nfonts, ++nfonts;
#endif
  for (int k = 0; k < nfonts; ++k) open_font(fonts[k], fontp[k]->name);
}

/* function, plane mask, fg/bg, fill style + tile/stipple/origin, font, subwindow mode, no clip;
 * sometimes clip rectangles */
static void random_gc(uint32_t gc, int depth) {
  int r = rr(0, 9);
  uint32_t function = r < 6 ? 3 : (uint32_t)rr(0, 15);
  uint32_t planes = rr(0, 3) ? 0xFFFFFFFFu : (uint32_t)rnd();
  uint32_t fs = rr(0, 2) ? 0 : (uint32_t)rr(1, 3);
  if (depth == 1 && fs == 1) fs = 0;  /* no depth-1 tile in this scene */
  uint32_t mask = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 8) | (1u << 11) | (1u << 12) | (1u << 13) |
                  (1u << 14) | (1u << 15) | (1u << 19);
  if (depth == 16) mask |= 1u << 10;  /* tile: same depth as the drawable */
  beg(56, 0); w32(gc); w32(mask);
  w32(function); w32(planes); w32(rnd() & 0xFFFF); w32(rnd() & 0xFFFF); w32(fs);
  if (depth == 16) w32(tile);
  w32(stip); w32((uint32_t)rr(-9, 9) & 0xFFFF); w32((uint32_t)rr(-9, 9) & 0xFFFF);
  w32(fonts[rr(0, nfonts - 1)]); w32((uint32_t)(rr(0, 3) == 0)); w32(0);
  CHECK(send_both() == 0);
  if (rr(0, 4) == 0) {
    beg(59, 0); w32(gc); w16((uint16_t)rr(-20, 20)); w16((uint16_t)rr(-20, 20));
    int n = rr(0, 6);
    for (int i = 0; i < n; ++i) { w16((uint16_t)rr(-30, 300)); w16((uint16_t)rr(-30, 200)); w16((uint16_t)rr(1, 200)); w16((uint16_t)rr(1, 120)); }
    CHECK(send_both() == 0);
  }
}

static void put_char(int wide) {
  if (!wide) { w8((uint8_t)(rr(0, 3) ? rr(32, 126) : rr(0, 255))); return; }
  int b1 = rr(0, 5) == 0 ? rr(0, 255) : rr(0, 2);
  w8((uint8_t)b1); w8((uint8_t)(rr(0, 3) ? rr(32, 126) : rr(0, 255)));
}

static int coord(int lo, int hi) { return rr(0, 40) == 0 ? (rr(0, 1) ? -32000 : 32000) : rr(lo, hi); }

static unsigned long text_ok, text_err[32];

static void random_text(void) {
  uint32_t targets[5] = {win, child, X11_ID_ROOT_WINDOW, pix16, pix1};
  int ti = rr(0, 4);
  uint32_t gc = ti == 4 ? gc1 : ti == 2 ? gcroot : gc16;
  int image = rr(0, 1), wide = rr(0, 2) == 0;
  int x = coord(-80, 820), y = coord(-20, 500);
  if (image) {
    int n = rr(0, 6) == 0 ? rr(0, 3) : rr(1, 60);
    beg((uint8_t)(wide ? 77 : 76), (uint8_t)n); w32(targets[ti]); w32(gc); w16((uint16_t)x); w16((uint16_t)y);
    for (int i = 0; i < n; ++i) put_char(wide);
    pad();
    if (rr(0, 40) == 0) q.n += 4;  /* wrong length: BadLength */
  } else {
    beg((uint8_t)(wide ? 75 : 74), 0); w32(targets[ti]); w32(gc); w16((uint16_t)x); w16((uint16_t)y);
    int items = rr(1, 5);
    for (int it = 0; it < items; ++it) {
      if (rr(0, 6) == 0) {  /* font shift: 255, then the font ID most significant byte first */
        uint32_t f = rr(0, 15) == 0 ? 0x7777777u : fonts[rr(0, nfonts - 1)];
        w8(255); w8((uint8_t)(f >> 24)); w8((uint8_t)(f >> 16)); w8((uint8_t)(f >> 8)); w8((uint8_t)f);
        continue;
      }
      int n = rr(0, 8) == 0 ? 0 : rr(1, 30);
      w8((uint8_t)n); w8((uint8_t)rr(-128, 127));
      for (int i = 0; i < n; ++i) put_char(wide);
    }
    if (rr(0, 30) == 0) { w8(20); w8(0); w8('x'); }  /* a truncated item: BadLength */
    pad();
  }
  size_t n = send_both();
  if (!n) ++text_ok;
  else if (outa[0] == 0) ++text_err[outa[1] & 31];
}

static void per_font_fast(void) {
  /* each font in GXcopy, characters from its own ranges (8- and 16-bit): a font whose glyphs are
   * all row-drawable must take only the fast path, and must take it when it has a glyph there */
  for (int k = 0; k < nfonts; ++k) {
    const x11_font *f = fontp[k];
    beg(56, 0); w32(gc16); w32((1u << 0) | (1u << 1) | (1u << 8) | (1u << 14) | (1u << 19));
    w32(3); w32(0xFFFFFFFFu); w32(0); w32(fonts[k]); w32(0); CHECK(send_both() == 0);
    unsigned long f0 = a_fast, s0 = a_slow;
    int nb1 = f->max_byte1 - f->min_byte1 + 1, nb2 = f->last - f->first + 1, exists = 0;
    for (int line = 0; line < 4; ++line) {
      beg(76, 60); w32(win); w32(gc16); w16((uint16_t)(5 + 3 * line)); w16((uint16_t)(40 + 40 * line));
      for (int i = 0; i < 60; ++i) {
        int c = f->first + (i * 7 + line * 13) % nb2;
        w8((uint8_t)c);
        if (!f->min_byte1 && x11_font_glyph(f, c)) exists = 1;
      }
      pad(); send_both();
      beg(77, 30); w32(win); w32(gc16); w16((uint16_t)(-7 + line)); w16((uint16_t)(60 + 40 * line));
      for (int i = 0; i < 30; ++i) {
        int b1 = f->min_byte1 + (i + line) % nb1, b2 = f->first + (i * 11 + line) % nb2;
        w8((uint8_t)b1); w8((uint8_t)b2);
        if (x11_font_glyph(f, b1 << 8 | b2)) exists = 1;
      }
      pad(); send_both();
    }
    CHECK(compare_state(f->name));
    if (x11_font_rows_ok(f, f->max_bounds.rsb - f->min_bounds.lsb)) {
      if (a_slow != s0) fprintf(stderr, "%s: slow glyphs in a row-drawable font\n", f->name);
      CHECK(a_slow == s0);
      if (exists) CHECK(a_fast > f0);
    } else {
      CHECK(a_slow > s0 || !exists);  /* wide glyphs: the reference path */
    }
  }
}

static void run(int big_endian) {
  msb = big_endian;
  start(&sa, &cla);
  start(&sb, &clb);
  setup_scene();
  CHECK(compare_state("setup"));
  unsigned long fast0 = a_fast, slow0 = a_slow;
  int ok = 1;
  for (int i = 0; i < 3000 && ok; ++i) {
    if (rr(0, 3) == 0) random_gc(gc16, 16), random_gc(gc1, 1), random_gc(gcroot, 16);
    random_text();
    ok = compare_state("text request");
    CHECK(ok);
  }
  CHECK(a_fast > fast0 && a_slow > slow0);  /* server A used both paths */
  per_font_fast();
  x11_server_free(&sa);
  x11_server_free(&sb);
}

int main(void) {
  run(0);
  run(1);
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  printf("x11 glyph fast path tests OK (%d fonts; fast-path server: %lu glyphs by rows, %lu point by point; random text requests: %lu drawn,"
         " errors: length %lu, font %lu, other %lu)\n", nfonts, a_fast, a_slow,
         text_ok, text_err[X11_BAD_LENGTH], text_err[X11_BAD_FONT],
         text_err[1] + text_err[2] + text_err[3] + text_err[8] + text_err[9] + text_err[11] + text_err[13]);
  return 0;
}
