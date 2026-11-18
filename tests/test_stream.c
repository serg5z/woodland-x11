/* Bounded GetImage reply producer: framing, byte order, padding, drawable kinds and sizes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/x11_internal.h"

static x11_server srv;
static uint8_t out[X11_REPLY_MAX];
static int failures;
#define CHECK(v) do { if (!(v)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #v); ++failures; } } while (0)

static void p16(uint8_t *p, unsigned v, int msb) { if (msb) p[0] = v >> 8, p[1] = v; else p[0] = v, p[1] = v >> 8; }
static void p32(uint8_t *p, uint32_t v, int msb) { if (msb) p[0] = v >> 24, p[1] = v >> 16, p[2] = v >> 8, p[3] = v; else p[0] = v, p[1] = v >> 8, p[2] = v >> 16, p[3] = v >> 24; }
static uint32_t g32(const uint8_t *p, int msb) { return msb ? (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3] : (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }
static void init(int msb, int *c);
static size_t get_image(int c, int msb, uint8_t format, uint32_t drawable, uint16_t w, uint16_t h,
                        uint32_t planes, x11_reply_stream *stream);

static int client(int msb) {
  int c = x11_client_open(&srv);
  uint8_t q[12] = {(uint8_t)(msb ? 'B' : 'l'), 0};
  p16(q + 2, 11, msb);
  uint8_t rep[512]; x11_setup_result result;
  x11_client_setup(&srv, c, q, sizeof(q), rep, sizeof(rep), &result);
  CHECK(result.status == X11_SETUP_SUCCESS);
  return c;
}

static size_t request(int c, int msb, uint8_t op, uint8_t data, const uint8_t *body, size_t n,
                      x11_reply_stream *stream) {
  uint8_t q[128] = {0}; q[0] = op; q[1] = data; p16(q + 2, (unsigned)((4 + n) / 4), msb);
  if (n) memcpy(q + 4, body, n);
  int close_after = 0;
  return x11_client_request_stream(&srv, c, q, 4 + n, out, sizeof(out), &close_after, stream);
}

static void test_compact_bitmap_protocol(int msb) {
  int c; init(msb, &c); const int w = 13, h = 5;
  uint32_t base = srv.clients[c].id_base, pmid = base | 0x51, copyid = base | 0x52;
  uint32_t gcid = base | 0x53, gc2 = base | 0x54, rootgc = base | 0x55, clipgc = base | 0x56;
  uint8_t body[64] = {0}; x11_reply_stream st = {0};
  p32(body, pmid, msb); p32(body + 4, X11_ID_ROOT_WINDOW, msb); p16(body + 8, w, msb); p16(body + 10, h, msb);
  CHECK(request(c, msb, 53, 1, body, 12, &st) == 0);
  p32(body, copyid, msb); p32(body + 4, X11_ID_ROOT_WINDOW, msb);
  CHECK(request(c, msb, 53, 1, body, 12, &st) == 0);
  x11_pixmap *pm = (x11_pixmap *)x_find(&srv, pmid)->obj;
  x11_pixmap *pc = (x11_pixmap *)x_find(&srv, copyid)->obj;
  CHECK(pm->surf.packed1 && pm->surf.stride == 2 && xr_surface_storage_bytes(&pm->surf) == 10);
  CHECK(pc->surf.packed1 && pc->surf.stride == 2);
  p32(body, gcid, msb); p32(body + 4, pmid, msb); p32(body + 8, 0, msb);
  CHECK(request(c, msb, 55, 0, body, 12, &st) == 0);
  p32(body, gc2, msb); p32(body + 4, copyid, msb);
  CHECK(request(c, msb, 55, 0, body, 12, &st) == 0);

  /* ZPixmap depth 1 uses 32-bit-padded scanlines on the wire, even though internal rows are
   * two bytes. Its bits are LSB-first; the third byte of each row is padding and must stay zero. */
  uint8_t expected[5][13] = {{0}}, image[20] = {0};
  for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
    int v = ((x * 3 + y) % 4) < 2; expected[y][x] = (uint8_t)v;
    if (v) image[y * 4 + x / 8] |= (uint8_t)(1u << (x & 7));
  }
  memset(body, 0, sizeof(body)); p32(body, pmid, msb); p32(body + 4, gcid, msb);
  p16(body + 8, w, msb); p16(body + 10, h, msb); body[17] = 1;
  memcpy(body + 20, image, sizeof(image));
  CHECK(request(c, msb, 72, 2, body, 40, &st) == 0);
  CHECK(get_image(c, msb, 2, pmid, w, h, 1, &st) == 52 && !st.active);
  CHECK(out[1] == 1 && memcmp(out + 32, image, sizeof(image)) == 0);
  CHECK(pm->surf.pix && ((((uint8_t *)pm->surf.pix)[1] & 0xE0u) == 0));  /* unused tail bits */

  /* CopyArea to another compact pixmap, then overlapping CopyArea within it. */
  memset(body, 0, sizeof(body)); p32(body, pmid, msb); p32(body + 4, copyid, msb); p32(body + 8, gc2, msb);
  p16(body + 20, w, msb); p16(body + 22, h, msb);
  CHECK(request(c, msb, 62, 0, body, 24, &st) == 0);
  CHECK(get_image(c, msb, 2, copyid, w, h, 1, &st) == 52);
  CHECK(memcmp(out + 32, image, sizeof(image)) == 0);
  memset(body, 0, sizeof(body)); p32(body, copyid, msb); p32(body + 4, copyid, msb); p32(body + 8, gc2, msb);
  p16(body + 16, 3, msb); p16(body + 20, 10, msb); p16(body + 22, h, msb);
  CHECK(request(c, msb, 62, 0, body, 24, &st) == 0);
  uint8_t copy_expected[5][13]; memcpy(copy_expected, expected, sizeof(copy_expected));
  for (int y = 0; y < h; ++y) for (int x = 9; x >= 0; --x) copy_expected[y][x + 3] = copy_expected[y][x];
  CHECK(get_image(c, msb, 2, copyid, w, h, 1, &st) == 52);
  for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
    CHECK(((out[32 + y * 4 + x / 8] >> (x & 7)) & 1u) == copy_expected[y][x]);

  /* GXxor with plane mask 1 must toggle just the requested bits. A zero plane mask is a no-op. */
  memset(body, 0, sizeof(body)); p32(body, gcid, msb); p32(body + 4, 7, msb);
  p32(body + 8, 6, msb); p32(body + 12, 1, msb); p32(body + 16, 1, msb);
  CHECK(request(c, msb, 56, 0, body, 20, &st) == 0);
  memset(body, 0, sizeof(body)); p32(body, pmid, msb); p32(body + 4, gcid, msb);
  p16(body + 8, 2, msb); p16(body + 10, 1, msb); p16(body + 12, 5, msb); p16(body + 14, 3, msb);
  CHECK(request(c, msb, 70, 0, body, 16, &st) == 0);
  for (int y = 1; y < 4; ++y) for (int x = 2; x < 7; ++x) expected[y][x] ^= 1;
  p32(body, gcid, msb); p32(body + 4, 6, msb); p32(body + 8, 1, msb); p32(body + 12, 6, msb);
  CHECK(request(c, msb, 56, 0, body, 16, &st) == 0);
  memset(body, 0, sizeof(body)); p32(body, pmid, msb); p32(body + 4, gcid, msb);
  p16(body + 12, w, msb); p16(body + 14, h, msb);
  CHECK(request(c, msb, 70, 0, body, 16, &st) == 0);
  CHECK(get_image(c, msb, 2, pmid, w, h, 1, &st) == 52);
  for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
    CHECK(((out[32 + y * 4 + x / 8] >> (x & 7)) & 1u) == expected[y][x]);

  /* A compact source bitmap is also used by CopyPlane and by a GC clip mask. */
  p32(body, rootgc, msb); p32(body + 4, X11_ID_ROOT_WINDOW, msb); p32(body + 8, 0, msb);
  CHECK(request(c, msb, 55, 0, body, 12, &st) == 0);
  p32(body, rootgc, msb); p32(body + 4, (1u << 2) | (1u << 3), msb);
  p32(body + 8, 0xF800, msb); p32(body + 12, 0x001F, msb);
  CHECK(request(c, msb, 56, 0, body, 16, &st) == 0);
  memset(body, 0, sizeof(body)); p32(body, pmid, msb); p32(body + 4, X11_ID_ROOT_WINDOW, msb); p32(body + 8, rootgc, msb);
  p16(body + 16, 100, msb); p16(body + 18, 100, msb); p16(body + 20, w, msb); p16(body + 22, h, msb); p32(body + 24, 1, msb);
  CHECK(request(c, msb, 63, 0, body, 28, &st) == 0);
  for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
    CHECK(((uint16_t *)srv.fb.pix)[(100 + y) * srv.fb.stride + 100 + x] == (expected[y][x] ? 0xF800 : 0x001F));

  p32(body, clipgc, msb); p32(body + 4, X11_ID_ROOT_WINDOW, msb); p32(body + 8, 0, msb);
  CHECK(request(c, msb, 55, 0, body, 12, &st) == 0);
  p32(body, clipgc, msb); p32(body + 4, 1u << 19, msb); p32(body + 8, pmid, msb);
  CHECK(request(c, msb, 56, 0, body, 12, &st) == 0);
  p32(body, clipgc, msb); p32(body + 4, (1u << 2) | (1u << 17) | (1u << 18), msb);
  p32(body + 8, 0xFFFF, msb); p32(body + 12, 150, msb); p32(body + 16, 150, msb);
  CHECK(request(c, msb, 56, 0, body, 20, &st) == 0);
  uint16_t before[5][13];
  for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) before[y][x] = ((uint16_t *)srv.fb.pix)[(150 + y) * srv.fb.stride + 150 + x];
  memset(body, 0, sizeof(body)); p32(body, X11_ID_ROOT_WINDOW, msb); p32(body + 4, clipgc, msb);
  p16(body + 8, 150, msb); p16(body + 10, 150, msb); p16(body + 12, w, msb); p16(body + 14, h, msb);
  CHECK(request(c, msb, 70, 0, body, 16, &st) == 0);
  for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
    CHECK(((uint16_t *)srv.fb.pix)[(150 + y) * srv.fb.stride + 150 + x] == (expected[y][x] ? 0xFFFF : before[y][x]));
  x11_server_free(&srv);
}

static size_t get_image(int c, int msb, uint8_t format, uint32_t drawable, uint16_t w, uint16_t h,
                        uint32_t planes, x11_reply_stream *stream) {
  /* Request body starts at byte 4: drawable, x/y, width/height, plane mask. */
  uint8_t body[16] = {0};
  p32(body + 0, drawable, msb);
  p16(body + 4, 0, msb); p16(body + 6, 0, msb);
  p16(body + 8, w, msb); p16(body + 10, h, msb); p32(body + 12, planes, msb);
  return request(c, msb, 73, format, body, sizeof(body), stream);
}

static uint16_t pattern(int x, int y) { return (uint16_t)((x * 977u + y * 313u + 0x1234u) & 0xFFFFu); }

static void init(int msb, int *c) {
  x11_server_config cfg; x11_default_config(&cfg); CHECK(x11_server_init(&srv, &cfg) == 0);
  for (int y = 0; y < srv.fb.h; ++y) for (int x = 0; x < srv.fb.w; ++x)
    ((uint16_t *)srv.fb.pix)[y * srv.fb.stride + x] = pattern(x, y);
  *c = client(msb);
}

static void verify_rgb_stream(x11_reply_stream *st, int sx, int sy, int w, int h, size_t row_bytes) {
  uint8_t chunk[4096]; size_t row = 0;
  while (st->active) {
    size_t n = x11_reply_stream_read(st, chunk, sizeof(chunk));
    CHECK(n != 0);
    for (size_t i = 0; i < n; ++i, ++row) {
      size_t y = row / row_bytes, col = row % row_bytes;
      uint8_t expected = 0;
      if (y < (size_t)h && col < (size_t)w * 2) {
        uint16_t v = pattern(sx + (int)(col / 2), sy + (int)y);
        expected = (uint8_t)(col & 1 ? v >> 8 : v); /* server setup advertises LSBFirst */
      }
      CHECK(chunk[i] == expected);
    }
  }
  CHECK(row == row_bytes * (size_t)h);
}

static void verify_xy_stream(x11_reply_stream *st, int sx, int sy, int w, int h, size_t bitmap_row, int depth) {
  uint8_t chunk[3072]; size_t offset = 0, plane_bytes = bitmap_row * (size_t)h;
  while (st->active) {
    size_t n = x11_reply_stream_read(st, chunk, sizeof(chunk));
    CHECK(n != 0);
    for (size_t i = 0; i < n; ++i, ++offset) {
      size_t plane = offset / plane_bytes, within = offset % plane_bytes;
      size_t y = within / bitmap_row, col = within % bitmap_row;
      int bit = depth - 1 - (int)plane;
      uint8_t expected = 0;
      if (col < ((size_t)w + 7) / 8)
        for (int b = 0; b < 8; ++b) {
          size_t x = col * 8 + (size_t)b;
          if (x < (size_t)w && (pattern(sx + (int)x, sy + (int)y) & (1u << bit)))
            expected |= (uint8_t)(1u << b);
        }
      CHECK(chunk[i] == expected);
    }
  }
  CHECK(offset == plane_bytes * (size_t)depth);
}

static void test_boundaries_and_fullscreen(int msb) {
  int c; init(msb, &c); x11_reply_stream st = {0};
  size_t n = get_image(c, msb, 2, X11_ID_ROOT_WINDOW, 180, 180, 0xFFFF, &st);
  CHECK(n == 32 + 64800 && !st.active); /* below previous 65,568-byte cap */
  CHECK(g32(out + 4, msb) == 64800 / 4);
  CHECK(out[32] == (uint8_t)pattern(0, 0));
  n = get_image(c, msb, 2, X11_ID_ROOT_WINDOW, 256, 128, 0xFFFF, &st);
  CHECK(n == X11_REPLY_MAX && !st.active); /* exactly 65,568 bytes */
  CHECK(g32(out + 4, msb) == 65536 / 4);
  n = get_image(c, msb, 2, X11_ID_ROOT_WINDOW, 256, 129, 0xFFFF, &st);
  CHECK(n == 32 && st.active && st.total == 66048);
  CHECK(g32(out + 4, msb) == 66048 / 4 && g32(out + 8, msb) == X11_ID_VISUAL_TRUECOLOR16);
  verify_rgb_stream(&st, 0, 0, 256, 129, 512);
  n = get_image(c, msb, 2, X11_ID_ROOT_WINDOW, 800, 480, 0xFFFF, &st);
  CHECK(n == 32 && st.active && st.total == 768000);
  CHECK(g32(out + 4, msb) == 192000);
  verify_rgb_stream(&st, 0, 0, 800, 480, 1600);
  n = get_image(c, msb, 1, X11_ID_ROOT_WINDOW, 800, 480, 0xFFFF, &st);
  CHECK(n == 32 && st.active && st.total == 768000);
  verify_xy_stream(&st, 0, 0, 800, 480, 100, 16);
  x11_server_free(&srv);
}

static void test_odd_width_pixmap(void) {
  int c; init(1, &c); uint32_t pmid = srv.clients[c].id_base | 0x44;
  uint8_t create[16] = {53, 16, 0, 4};
  p32(create + 4, pmid, 1); p32(create + 8, X11_ID_ROOT_WINDOW, 1);
  p16(create + 12, 7, 1); p16(create + 14, 5000, 1);
  int close_after = 0; CHECK(x11_client_request(&srv, c, create, sizeof(create), out, sizeof(out), &close_after) == 0);
  x11_resource *r = x_find(&srv, pmid); CHECK(r && r->type == X11_RES_PIXMAP);
  x11_pixmap *pm = (x11_pixmap *)r->obj;
  for (int y = 0; y < pm->surf.h; ++y) for (int x = 0; x < pm->surf.w; ++x)
    ((uint16_t *)pm->surf.pix)[y * pm->surf.stride + x] = pattern(x, y);
  x11_reply_stream st = {0};
  size_t n = get_image(c, 1, 2, pmid, 7, 5000, 0xFFFF, &st);
  CHECK(n == 32 && st.active && st.total == 80000 && st.row_bytes == 16);
  CHECK(g32(out + 4, 1) == 20000);
  verify_rgb_stream(&st, 0, 0, 7, 5000, 16);
  x11_server_free(&srv);
}

static void test_property_stream(int msb, uint8_t format) {
  int c; init(msb, &c);
  const uint32_t atom = 39; /* WM_NAME */
  const size_t bytes = format == 8 ? 5001 : format == 16 ? 5000 : 5004;
  static uint8_t q[24 + 6000], scratch[4096];
  size_t row = x_pad4(bytes), put_len = 24 + row;
  memset(q, 0, put_len); q[0] = 18; q[1] = 0; p16(q + 2, (unsigned)(put_len / 4), msb);
  p32(q + 4, X11_ID_ROOT_WINDOW, msb); p32(q + 8, atom, msb); p32(q + 12, 31, msb);
  q[16] = format; p32(q + 20, (uint32_t)(bytes / (format / 8)), msb);
  for (size_t i = 0; i < bytes; ++i) q[24 + i] = (uint8_t)(i * 73u + (i >> 3) + 19u);
  int close_after = 0;
  size_t pr = x11_client_request(&srv, c, q, put_len, out, sizeof(out), &close_after);
  CHECK(pr == 0 && !close_after);

  uint8_t get[24] = {20, 0}; p16(get + 2, 6, msb); p32(get + 4, X11_ID_ROOT_WINDOW, msb); p32(get + 8, atom, msb);
  p32(get + 12, 0, msb); p32(get + 16, 0, msb); p32(get + 20, 2000, msb);
  x11_reply_stream st = {0};
  size_t n = x11_client_request_stream(&srv, c, get, sizeof(get), scratch, sizeof(scratch), &close_after, &st);
  CHECK(n == 32 && st.active && st.kind == 2 && st.total == x_pad4(bytes));
  CHECK(scratch[0] == 1 && scratch[1] == format && g32(scratch + 4, msb) == x_pad4(bytes) / 4);
  CHECK(g32(scratch + 8, msb) == 31 && g32(scratch + 12, msb) == 0);
  CHECK(g32(scratch + 16, msb) == bytes / (format / 8));
  size_t off = 0;
  while (st.active) {
    size_t k = x11_reply_stream_read(&st, scratch, sizeof(scratch)); CHECK(k != 0);
    for (size_t i = 0; i < k; ++i, ++off) {
      uint8_t expected = off < bytes ? q[24 + off] : 0;
      CHECK(scratch[i] == expected);
    }
  }
  CHECK(off == x_pad4(bytes));

  /* Delete=True with a streamed, complete value removes the property before the payload drains. */
  get[1] = 1; memset(&st, 0, sizeof(st));
  n = x11_client_request_stream(&srv, c, get, sizeof(get), scratch, sizeof(scratch), &close_after, &st);
  CHECK(n == 32 && st.active);
  while (st.active) CHECK(x11_reply_stream_read(&st, scratch, sizeof(scratch)) != 0);
  get[1] = 0;
  n = x11_client_request_stream(&srv, c, get, sizeof(get), scratch, sizeof(scratch), &close_after, &st);
  CHECK(n == 32 && scratch[0] == 1 && scratch[1] == 0 && !st.active);
  x11_server_free(&srv);
}

static void test_depth1_xy_pixmap(void) {
  int c; init(0, &c); uint32_t pmid = srv.clients[c].id_base | 0x45;
  uint8_t create[16] = {53, 1, 4, 0};
  p32(create + 4, pmid, 0); p32(create + 8, X11_ID_ROOT_WINDOW, 0);
  p16(create + 12, 7, 0); p16(create + 14, 20000, 0);
  int close_after = 0; CHECK(x11_client_request(&srv, c, create, sizeof(create), out, sizeof(out), &close_after) == 0);
  x11_resource *r = x_find(&srv, pmid); CHECK(r && r->type == X11_RES_PIXMAP);
  x11_pixmap *pm = (x11_pixmap *)r->obj;
  CHECK(pm->surf.packed1 && pm->surf.stride == 1);
  CHECK(xr_surface_storage_bytes(&pm->surf) == 20000);
  for (int y = 0; y < pm->surf.h; ++y) for (int x = 0; x < pm->surf.w; ++x)
    if ((x + y) & 1) ((uint8_t *)pm->surf.pix)[(size_t)y * pm->surf.stride + (unsigned)x / 8] |= (uint8_t)(1u << (x & 7));
  x11_reply_stream st = {0};
  size_t n = get_image(c, 0, 1, pmid, 7, 20000, 1, &st);
  CHECK(n == 32 && st.active && st.total == 80000 && st.bitmap_row == 4);
  uint8_t chunk[17]; size_t off = 0;
  while (st.active) {
    size_t k = x11_reply_stream_read(&st, chunk, sizeof(chunk)); CHECK(k != 0);
    for (size_t i = 0; i < k; ++i, ++off) {
      size_t y = off / 4, col = off % 4; uint8_t expected = 0;
      if (col == 0) for (int b = 0; b < 7; ++b) if ((b + (int)y) & 1) expected |= (uint8_t)(1u << b);
      CHECK(chunk[i] == expected);
    }
  }
  CHECK(off == 80000);
  x11_server_free(&srv);
}

int main(void) {
  test_compact_bitmap_protocol(0);
  test_compact_bitmap_protocol(1);
  test_boundaries_and_fullscreen(0);
  test_boundaries_and_fullscreen(1);
  test_odd_width_pixmap();
  test_depth1_xy_pixmap();
  test_property_stream(0, 8);
  test_property_stream(1, 16);
  test_property_stream(0, 32);
  if (failures) return 1;
  puts("bounded GetImage streaming tests OK");
  return 0;
}
