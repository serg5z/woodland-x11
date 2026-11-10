/* Text and font requests: PolyText8/16, ImageText8/16, QueryTextExtents, QueryFont fields,
 * CloseFont while a GC uses the font. The "fixed" rendering is compared with a framebuffer
 * hash recorded from the code before the font model became two-byte capable (bce69ee), so
 * host and ESP32 keep drawing the same pixels. Both byte orders. Run with ASan/UBSan. */
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

static x11_server srv;
static uint8_t out[X11_REPLY_MAX];
static int MSB;

typedef struct {
  uint8_t b[1024];
  size_t n;
} rq;
static void beg(rq *q, uint8_t op, uint8_t d) {
  memset(q, 0, sizeof(*q));
  q->b[0] = op, q->b[1] = d, q->n = 4;
}
static void w32(rq *q, uint32_t v) { x_wr32(q->b + q->n, v, MSB); q->n += 4; }
static void w16x2(rq *q, uint16_t a, uint16_t b) { x_wr16(q->b + q->n, a, MSB); x_wr16(q->b + q->n + 2, b, MSB); q->n += 4; }
static void raw(rq *q, const void *p, size_t n) { memcpy(q->b + q->n, p, n); q->n += n; }
static void pad(rq *q) { while (q->n % 4) q->b[q->n++] = 0; }
static size_t run(int c, rq *q) {
  x_wr16(q->b + 2, (uint16_t)(q->n / 4), MSB);
  int close_after;
  size_t n = x11_client_request(&srv, c, q->b, q->n, out, sizeof(out), &close_after);
  CHECK(!close_after);
  return n;
}
static int is_error(size_t n, uint8_t code) { return n == 32 && out[0] == 0 && out[1] == code; }

static uint32_t fb_hash(int x0, int y0, int w, int h) {
  uint32_t hsh = 2166136261u;
  for (int y = y0; y < y0 + h; ++y)
    for (int x = x0; x < x0 + w; ++x) {
      uint16_t p = ((uint16_t *)srv.fb.pix)[y * 800 + x];
      hsh = (hsh ^ (p & 0xFF)) * 16777619u;
      hsh = (hsh ^ (p >> 8)) * 16777619u;
    }
  return hsh;
}
static uint16_t px(int x, int y) { return ((uint16_t *)srv.fb.pix)[y * 800 + x]; }

static int setup(uint32_t *win, uint32_t *gc) {
  x11_server_config cfg;
  x11_default_config(&cfg);
  CHECK(x11_server_init(&srv, &cfg) == 0);
  int c = x11_client_open(&srv);
  uint8_t req[12] = {(uint8_t)(MSB ? 'B' : 'l'), 0};
  x_wr16(req + 2, 11, MSB);
  x11_setup_result r;
  uint8_t rep[512];
  x11_client_setup(&srv, c, req, 12, rep, sizeof(rep), &r);
  uint32_t base = srv.clients[c].id_base;
  *win = base + 1, *gc = base + 2;
  rq q;
  beg(&q, 1, 0);  /* CreateWindow 800x480, background black */
  w32(&q, *win); w32(&q, X11_ID_ROOT_WINDOW); w16x2(&q, 0, 0); w16x2(&q, 800, 480); w16x2(&q, 0, 1); w32(&q, 0);
  w32(&q, 2); w32(&q, 0);
  CHECK(run(c, &q) == 0);
  beg(&q, 8, 0); w32(&q, *win);
  CHECK(run(c, &q) == 0);
  beg(&q, 55, 0);  /* CreateGC: foreground white, background 0x1234 */
  w32(&q, *gc); w32(&q, *win); w32(&q, 4 | 8); w32(&q, 0xFFFF); w32(&q, 0x1234);
  CHECK(run(c, &q) == 0);
  return c;
}

/* Every "fixed" character with PolyText8 (rows of 32) and ImageText8: the same pixels as
 * before the font refactor. */
static void test_fixed_golden(void) {
  uint32_t win, gc;
  int c = setup(&win, &gc);
  rq q;
  for (int row = 0; row < 3; ++row) {
    uint8_t s[32];
    int n = 0;
    for (int ch = 32 + 32 * row; ch < 32 + 32 * (row + 1) && ch <= 126; ++ch) s[n++] = (uint8_t)ch;
    beg(&q, 74, 0); w32(&q, win); w32(&q, gc); w16x2(&q, 10, (uint16_t)(20 + 14 * row));
    uint8_t item[2] = {(uint8_t)n, 0};
    raw(&q, item, 2); raw(&q, s, (size_t)n); pad(&q);
    CHECK(run(c, &q) == 0);
    beg(&q, 76, (uint8_t)n); w32(&q, win); w32(&q, gc); w16x2(&q, 10, (uint16_t)(80 + 14 * row));
    raw(&q, s, (size_t)n); pad(&q);
    CHECK(run(c, &q) == 0);
  }
  uint32_t h = fb_hash(0, 0, 220, 130);
  if (getenv("PRINT_GOLDEN")) printf("golden %08x\n", h);
  CHECK(h == 0xb60709bbu);  /* bce69ee, before the two-byte font model */
  x11_server_free(&srv);
}

/* PolyText8: a zero-length item only moves the pen, and the items after it are drawn. */
static void test_poly_text_items(void) {
  uint32_t win, gc;
  int c = setup(&win, &gc);
  rq q;
  beg(&q, 74, 0); w32(&q, win); w32(&q, gc); w16x2(&q, 100, 50);
  uint8_t items[] = {0, 30, 1, 0, 'I'};  /* move 30, then "I" (no move) */
  raw(&q, items, sizeof(items)); pad(&q);
  CHECK(run(c, &q) == 0);
  uint32_t h1 = fb_hash(90, 30, 60, 30);
  beg(&q, 74, 0); w32(&q, win); w32(&q, gc); w16x2(&q, 100, 50);
  uint8_t direct[] = {1, 30, 'I'};
  raw(&q, direct, sizeof(direct)); pad(&q);
  CHECK(run(c, &q) == 0);
  CHECK(fb_hash(90, 30, 60, 30) == h1);  /* drawing it again in place changes nothing */
  int lit = 0;
  for (int y = 38; y < 52; ++y)
    for (int x = 130; x < 136; ++x) lit += px(x, y) == 0xFFFF;
  CHECK(lit > 0);
  x11_server_free(&srv);
}

/* PolyText16 / ImageText16 with byte1 0 draw as the 8-bit requests; a character outside
 * the font (byte1 1) is the default character (' ' in "fixed": nothing drawn, pen moves). */
static void test_text16(void) {
  uint32_t win, gc;
  int c = setup(&win, &gc);
  rq q;
  beg(&q, 74, 0); w32(&q, win); w32(&q, gc); w16x2(&q, 10, 30);
  uint8_t i8[] = {3, 0, 'W', 'x', '7'};
  raw(&q, i8, sizeof(i8)); pad(&q);
  CHECK(run(c, &q) == 0);
  beg(&q, 76, 3); w32(&q, win); w32(&q, gc); w16x2(&q, 10, 60); raw(&q, "Wx7", 3); pad(&q);
  CHECK(run(c, &q) == 0);
  uint32_t poly8 = fb_hash(0, 10, 60, 25), image8 = fb_hash(0, 40, 60, 25);
  beg(&q, 75, 0); w32(&q, win); w32(&q, gc); w16x2(&q, 10, 130);
  uint8_t i16[] = {3, 0, 0, 'W', 0, 'x', 0, '7'};
  raw(&q, i16, sizeof(i16)); pad(&q);
  CHECK(run(c, &q) == 0);
  beg(&q, 77, 3); w32(&q, win); w32(&q, gc); w16x2(&q, 10, 160);
  uint8_t s16[] = {0, 'W', 0, 'x', 0, '7'};
  raw(&q, s16, sizeof(s16)); pad(&q);
  CHECK(run(c, &q) == 0);
  CHECK(fb_hash(0, 110, 60, 25) == poly8 && fb_hash(0, 140, 60, 25) == image8);
  /* (1, 'W') does not exist: the default character ' ' takes its place */
  beg(&q, 75, 0); w32(&q, win); w32(&q, gc); w16x2(&q, 300, 30);
  uint8_t gap[] = {3, 0, 0, 'W', 1, 'W', 0, '7'};
  raw(&q, gap, sizeof(gap)); pad(&q);
  CHECK(run(c, &q) == 0);
  beg(&q, 74, 0); w32(&q, win); w32(&q, gc); w16x2(&q, 300, 60);
  uint8_t ref[] = {3, 0, 'W', ' ', '7'};
  raw(&q, ref, sizeof(ref)); pad(&q);
  CHECK(run(c, &q) == 0);
  CHECK(fb_hash(290, 10, 40, 25) == fb_hash(290, 40, 40, 25));
  /* lengths: ImageText16 2 characters in a 4-character body; PolyText16 item past the end */
  beg(&q, 77, 2); w32(&q, win); w32(&q, gc); w16x2(&q, 0, 0); raw(&q, "abcdefgh", 8);
  CHECK(is_error(run(c, &q), X11_BAD_LENGTH));
  beg(&q, 75, 0); w32(&q, win); w32(&q, gc); w16x2(&q, 0, 0);
  uint8_t over[] = {3, 0, 0, 'a'};
  raw(&q, over, sizeof(over));
  CHECK(is_error(run(c, &q), X11_BAD_LENGTH));
  /* a font shift to a font that does not exist */
  beg(&q, 75, 0); w32(&q, win); w32(&q, gc); w16x2(&q, 0, 0);
  uint8_t shift[] = {255, 0, 0, 0, 9, 0, 0, 0};
  raw(&q, shift, sizeof(shift));
  CHECK(is_error(run(c, &q), X11_BAD_FONT));
  x11_server_free(&srv);
}

/* QueryTextExtents on a font and on a GC; odd-length; the empty string; errors. */
static void test_query_text_extents(void) {
  uint32_t win, gc;
  int c = setup(&win, &gc);
  uint32_t font = srv.clients[c].id_base + 3;
  rq q;
  beg(&q, 45, 0); w32(&q, font); w16x2(&q, 5, 0); raw(&q, "fixed", 5); pad(&q);
  CHECK(run(c, &q) == 0);
  uint8_t hello[] = {0, 'H', 0, 'e', 0, 'l', 0, 'l', 0, 'o', 0, 0};  /* 5 characters + padding */
  for (int k = 0; k < 2; ++k) {
    beg(&q, 48, 1); w32(&q, k ? gc : font); raw(&q, hello, sizeof(hello));
    CHECK(run(c, &q) == 32 && out[0] == 1 && out[1] == 0);
    CHECK(x_rd16(out + 8, MSB) == 10 && x_rd16(out + 10, MSB) == 2);           /* font ascent, descent */
    CHECK(x_rd16(out + 12, MSB) == 10 && x_rd16(out + 14, MSB) == 2);          /* overall */
    CHECK(x_rd32(out + 16, MSB) == 30 && x_rd32(out + 20, MSB) == 0 && x_rd32(out + 24, MSB) == 30);
  }
  beg(&q, 48, 0); w32(&q, font);  /* empty: all overall values 0 */
  CHECK(run(c, &q) == 32 && x_rd16(out + 12, MSB) == 0 && x_rd32(out + 16, MSB) == 0 && x_rd32(out + 24, MSB) == 0);
  beg(&q, 48, 1); w32(&q, font);  /* odd-length with no characters */
  CHECK(is_error(run(c, &q), X11_BAD_LENGTH));
  beg(&q, 48, 0); w32(&q, win); raw(&q, hello, 4);  /* a window is not fontable */
  CHECK(is_error(run(c, &q), X11_BAD_FONT));
  beg(&q, 48, 0);
  CHECK(is_error(run(c, &q), X11_BAD_LENGTH));
  x11_server_free(&srv);
}

/* QueryFont fields for the single-byte built-ins; CloseFont leaves a GC's font in place. */
static void test_query_font_and_close(void) {
  uint32_t win, gc;
  int c = setup(&win, &gc);
  uint32_t cur = srv.clients[c].id_base + 4;
  rq q;
  beg(&q, 47, 0); w32(&q, gc);  /* the GC's default font: fixed */
  CHECK(run(c, &q) == 32 + 28 + 8 + 12 * 95);
  CHECK(x_rd16(out + 40, MSB) == 32 && x_rd16(out + 42, MSB) == 126 && out[49] == 0 && out[50] == 0 && out[51] == 1);
  beg(&q, 45, 0); w32(&q, cur); w16x2(&q, 6, 0); raw(&q, "cursor", 6); pad(&q);
  CHECK(run(c, &q) == 0);
  beg(&q, 56, 0); w32(&q, gc); w32(&q, 1u << 14); w32(&q, cur);  /* ChangeGC font */
  CHECK(run(c, &q) == 0);
  beg(&q, 46, 0); w32(&q, cur);  /* CloseFont */
  CHECK(run(c, &q) == 0);
  beg(&q, 47, 0); w32(&q, gc);  /* the GC still uses the cursor font */
  CHECK(run(c, &q) > 32 && x_rd16(out + 40, MSB) == 0 && x_rd16(out + 42, MSB) == 153);
  beg(&q, 47, 0); w32(&q, cur);  /* the ID is gone */
  CHECK(is_error(run(c, &q), X11_BAD_FONT));
  x11_server_free(&srv);
}

/* GC fill styles (Tiled, Stippled, OpaqueStippled) for fills, points, polygons and text; the tile
 * and stipple origin; the GC keeps its own copy (FreePixmap afterwards changes nothing); depth
 * checks; copies and ImageText ignore the fill style; a dashed line style does not stop fills
 * (dashed lines themselves are not implemented yet). */
static uint32_t pixmap_with(int c, uint32_t id, int depth, int w, int h, const uint16_t *px) {
  rq q;
  beg(&q, 53, (uint8_t)depth); w32(&q, id); w32(&q, X11_ID_ROOT_WINDOW); w16x2(&q, (uint16_t)w, (uint16_t)h);
  CHECK(run(c, &q) == 0);
  uint32_t gc = id + 1;
  beg(&q, 55, 0); w32(&q, gc); w32(&q, id); w32(&q, 0);
  CHECK(run(c, &q) == 0);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      beg(&q, 56, 0); w32(&q, gc); w32(&q, 4); w32(&q, px[y * w + x]);
      CHECK(run(c, &q) == 0);
      beg(&q, 64, 0); w32(&q, id); w32(&q, gc); w16x2(&q, (uint16_t)x, (uint16_t)y);  /* PolyPoint */
      CHECK(run(c, &q) == 0);
    }
  beg(&q, 60, 0); w32(&q, gc);
  CHECK(run(c, &q) == 0);
  return id;
}
static size_t change_gc(int c, uint32_t gc, uint32_t mask, const uint32_t *v, int n) {
  rq q;
  beg(&q, 56, 0); w32(&q, gc); w32(&q, mask);
  for (int i = 0; i < n; ++i) w32(&q, v[i]);
  return run(c, &q);
}
static void fill_rect(int c, uint32_t d, uint32_t gc, int x, int y, int w, int h) {
  rq q;
  beg(&q, 70, 0); w32(&q, d); w32(&q, gc); w16x2(&q, (uint16_t)x, (uint16_t)y); w16x2(&q, (uint16_t)w, (uint16_t)h);
  CHECK(run(c, &q) == 0);
}
static void test_fill_styles(void) {
  uint32_t win, gc;
  int c = setup(&win, &gc);
  uint32_t base = srv.clients[c].id_base;
  const uint16_t R = 0xF800, G = 0x07E0, B = 0x001F, W = 0xFFFF;
  uint16_t tile_px[4] = {R, G, B, W}, st_px[2] = {1, 0};
  uint32_t tile = pixmap_with(c, base + 10, 16, 2, 2, tile_px), st = pixmap_with(c, base + 20, 1, 2, 1, st_px);
  /* Tiled, origin (1, 0): pixel (x, y) shows tile ((x - 1) mod 2, y mod 2) */
  uint32_t v[3] = {tile, 1, 0};
  CHECK(change_gc(c, gc, (1u << 10) | (1u << 12) | (1u << 13), (uint32_t[]){tile, 1, 0}, 3) == 0);
  CHECK(change_gc(c, gc, 1u << 8, (uint32_t[]){1}, 1) == 0);  /* fill-style Tiled */
  rq q;
  beg(&q, 54, 0); w32(&q, tile);  /* FreePixmap: the GC keeps its copy */
  CHECK(run(c, &q) == 0);
  fill_rect(c, win, gc, 100, 100, 4, 4);
  CHECK(px(100, 100) == G && px(101, 100) == R && px(100, 101) == W && px(101, 101) == B && px(103, 103) == B);
  (void)v;
  /* the tile also fills a polygon and points; text draws its glyph pixels from the tile */
  beg(&q, 64, 0); w32(&q, win); w32(&q, gc); w16x2(&q, 201, 50);
  CHECK(run(c, &q) == 0 && px(201, 50) == R);
  /* Stippled: the foreground (white) where the stipple has 1, nothing elsewhere */
  CHECK(change_gc(c, gc, (1u << 8) | (1u << 11) | (1u << 12), (uint32_t[]){2, st, 0}, 3) == 0);
  fill_rect(c, win, gc, 300, 300, 4, 2);
  CHECK(px(300, 300) == W && px(301, 300) == 0 && px(302, 301) == W && px(303, 301) == 0);
  /* OpaqueStippled: foreground and background (0x1234) */
  CHECK(change_gc(c, gc, 1u << 8, (uint32_t[]){3}, 1) == 0);
  fill_rect(c, win, gc, 310, 300, 4, 1);
  CHECK(px(310, 300) == W && px(311, 300) == 0x1234 && px(312, 300) == W);
  /* text through the stipple: glyph pixels only on even columns */
  CHECK(change_gc(c, gc, 1u << 8, (uint32_t[]){2}, 1) == 0);
  beg(&q, 74, 0); w32(&q, win); w32(&q, gc); w16x2(&q, 400, 60);
  uint8_t item[] = {1, 0, 'M'};
  raw(&q, item, 3); pad(&q);
  CHECK(run(c, &q) == 0);
  int odd_lit = 0, even_lit = 0;
  for (int y = 48; y < 62; ++y)
    for (int x = 400; x < 406; ++x) {
      if (x % 2) odd_lit += px(x, y) == W;
      else even_lit += px(x, y) == W;
    }
  CHECK(even_lit > 0 && odd_lit == 0);
  /* ImageText ignores the fill style: a solid background box */
  beg(&q, 76, 1); w32(&q, win); w32(&q, gc); w16x2(&q, 500, 60); raw(&q, "M", 1); pad(&q);
  CHECK(run(c, &q) == 0);
  CHECK(px(505, 50) == 0x1234 && px(500, 61) == 0x1234);  /* box corners: background, every column */
  /* CopyArea with this GC copies plainly */
  beg(&q, 62, 0); w32(&q, win); w32(&q, win); w32(&q, gc); w16x2(&q, 100, 100); w16x2(&q, 600, 100); w16x2(&q, 2, 2);
  CHECK(run(c, &q) == 0 && px(600, 100) == G && px(601, 101) == B);
  /* a dashed line style does not stop fills; dashed lines are still BadImplementation */
  CHECK(change_gc(c, gc, (1u << 5) | (1u << 8), (uint32_t[]){1, 0}, 2) == 0);
  fill_rect(c, win, gc, 700, 10, 2, 2);
  CHECK(px(700, 10) == W);
  beg(&q, 65, 0); w32(&q, win); w32(&q, gc); w16x2(&q, 0, 0); w16x2(&q, 10, 10);
  CHECK(is_error(run(c, &q), X11_BAD_IMPLEMENTATION));
  /* depth checks: a depth-1 tile or a depth-16 stipple is BadMatch, the GC is unchanged */
  uint16_t one[1] = {1};
  uint32_t deep = pixmap_with(c, base + 30, 16, 1, 1, tile_px), bit = pixmap_with(c, base + 40, 1, 1, 1, one);
  CHECK(is_error(change_gc(c, gc, 1u << 10, (uint32_t[]){bit}, 1), X11_BAD_MATCH));
  CHECK(is_error(change_gc(c, gc, 1u << 11, (uint32_t[]){deep}, 1), X11_BAD_MATCH));
  CHECK(is_error(change_gc(c, gc, 1u << 10, (uint32_t[]){base + 99}, 1), X11_BAD_PIXMAP));
  /* CopyGC hands over a copy; then both GCs draw the tile */
  uint32_t gc2 = base + 50;
  beg(&q, 55, 0); w32(&q, gc2); w32(&q, win); w32(&q, 0);
  CHECK(run(c, &q) == 0);
  beg(&q, 57, 0); w32(&q, gc); w32(&q, gc2); w32(&q, (1u << 8) | (1u << 10) | (1u << 12) | (1u << 13));
  CHECK(run(c, &q) == 0);
  CHECK(change_gc(c, gc2, 1u << 8, (uint32_t[]){1}, 1) == 0);
  fill_rect(c, win, gc2, 120, 100, 2, 1);
  CHECK(px(120, 100) == R && px(121, 100) == G);
  beg(&q, 60, 0); w32(&q, gc);
  CHECK(run(c, &q) == 0);
  fill_rect(c, win, gc2, 130, 100, 2, 1);  /* still drawn after the source GC is freed */
  CHECK(px(130, 100) == R);
  x11_server_free(&srv);
}

int main(void) {
  for (MSB = 0; MSB < 2; ++MSB) test_fill_styles();
  for (MSB = 0; MSB < 2; ++MSB) {
    test_fixed_golden();
    test_poly_text_items();
    test_text16();
    test_query_text_extents();
    test_query_font_and_close();
  }
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  puts("x11 text and font tests OK");
  return 0;
}
