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
  uint8_t q[20] = {0}; q[0] = op; q[1] = data; p16(q + 2, (unsigned)((4 + n) / 4), msb);
  if (n) memcpy(q + 4, body, n);
  int close_after = 0;
  return x11_client_request_stream(&srv, c, q, 4 + n, out, sizeof(out), &close_after, stream);
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

static void test_depth1_xy_pixmap(void) {
  int c; init(0, &c); uint32_t pmid = srv.clients[c].id_base | 0x45;
  uint8_t create[16] = {53, 1, 4, 0};
  p32(create + 4, pmid, 0); p32(create + 8, X11_ID_ROOT_WINDOW, 0);
  p16(create + 12, 7, 0); p16(create + 14, 20000, 0);
  int close_after = 0; CHECK(x11_client_request(&srv, c, create, sizeof(create), out, sizeof(out), &close_after) == 0);
  x11_resource *r = x_find(&srv, pmid); CHECK(r && r->type == X11_RES_PIXMAP);
  x11_pixmap *pm = (x11_pixmap *)r->obj;
  for (int y = 0; y < pm->surf.h; ++y) for (int x = 0; x < pm->surf.w; ++x)
    ((uint8_t *)pm->surf.pix)[y * pm->surf.stride + x] = (uint8_t)((x + y) & 1);
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
  test_boundaries_and_fullscreen(0);
  test_boundaries_and_fullscreen(1);
  test_odd_width_pixmap();
  test_depth1_xy_pixmap();
  if (failures) return 1;
  puts("bounded GetImage streaming tests OK");
  return 0;
}
