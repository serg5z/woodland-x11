/* Unit tests for core/x11_setup.c (and the host request-name table).
 * The reply is decoded here with an independent reader, field by field, as the protocol
 * specification lays it out (section 8 and the "Connection Setup" encoding). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/x11_setup.h"
#include "../host/x11_names.h"

static int failures = 0;
#define CHECK(c)                                                             \
  do {                                                                       \
    if (!(c)) {                                                              \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);  \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

/* ---- request builder ------------------------------------------------------------------- */

static size_t pad4(size_t n) { return (n + 3) & ~(size_t)3; }

static size_t build_request(uint8_t *b, char order, uint16_t major, uint16_t minor, const char *name,
                            const uint8_t *data, uint16_t dlen) {
  int msb = order == 'B';
  uint16_t n = (uint16_t)(name ? strlen(name) : 0);
  memset(b, 0, 12 + pad4(n) + pad4(dlen));
  b[0] = (uint8_t)order;
#define W16(off, v) do { if (msb) { b[off] = (uint8_t)((v) >> 8); b[off + 1] = (uint8_t)(v); } \
                         else { b[off] = (uint8_t)(v); b[off + 1] = (uint8_t)((v) >> 8); } } while (0)
  W16(2, major);
  W16(4, minor);
  W16(6, n);
  W16(8, dlen);
  if (n) memcpy(b + 12, name, n);
  if (dlen) memcpy(b + 12 + pad4(n), data, dlen);
  return 12 + pad4(n) + pad4(dlen);
}

/* ---- reply reader ---------------------------------------------------------------------- */

typedef struct {
  const uint8_t *p;
  size_t n, off;
  int msb;
} rd;

static uint8_t u8(rd *r) { return r->off < r->n ? r->p[r->off++] : 0xEE; }
static uint16_t u16(rd *r) {
  uint8_t a = u8(r), b = u8(r);
  return r->msb ? (uint16_t)(a << 8 | b) : (uint16_t)(b << 8 | a);
}
static uint32_t u32(rd *r) {
  uint32_t a = u16(r), b = u16(r);
  return r->msb ? a << 16 | b : b << 16 | a;
}
static void skip(rd *r, size_t k) { r->off += k; }

static void check_success(const uint8_t *rep, size_t len, int msb, const x11_server_config *cfg, uint32_t base) {
  rd r = {rep, len, 0, msb};
  CHECK(len % 4 == 0);
  CHECK(u8(&r) == 1);           /* Success */
  skip(&r, 1);
  CHECK(u16(&r) == 11);
  CHECK(u16(&r) == 0);
  uint16_t units = u16(&r);
  CHECK(8 + (size_t)units * 4 == len);
  CHECK(u32(&r) == cfg->release);
  uint32_t id_base = u32(&r), id_mask = u32(&r);
  CHECK(id_base == base);
  CHECK((id_base & id_mask) == 0);                            /* base and mask disjoint */
  CHECK(id_mask == 0x1FFFFF);                                 /* contiguous, >= 18 bits */
  CHECK((id_base & 0xE0000000u) == 0);                        /* top three bits zero */
  CHECK(u32(&r) == 0);                                        /* motion buffer */
  uint16_t vlen = u16(&r);
  CHECK(vlen == strlen(cfg->vendor));
  uint16_t max_req = u16(&r);
  CHECK(max_req >= 4096);
  uint8_t screens = u8(&r), nformats = u8(&r);
  CHECK(screens == 1 && nformats == 2);
  CHECK(u8(&r) == 0);                                         /* image byte order LSBFirst */
  CHECK(u8(&r) == 0);                                         /* bitmap bit order */
  uint8_t unit = u8(&r), spad = u8(&r);
  CHECK(unit == 32 && spad == 32);
  uint8_t kmin = u8(&r), kmax = u8(&r);
  CHECK(kmin >= 8 && kmax == 255);
  skip(&r, 4);
  CHECK(memcmp(rep + r.off, cfg->vendor, vlen) == 0);
  skip(&r, pad4(vlen));
  int have_depth1_format = 0, have_depth16_format = 0;
  for (int i = 0; i < nformats; ++i) {
    uint8_t depth = u8(&r), bpp = u8(&r), pad = u8(&r);
    skip(&r, 5);
    if (depth == 1) have_depth1_format = bpp == 1 && pad == 32;
    if (depth == 16) have_depth16_format = bpp == 16 && pad == 32;
  }
  CHECK(have_depth1_format && have_depth16_format);
  /* SCREEN */
  uint32_t root = u32(&r), cmap = u32(&r), white = u32(&r), black = u32(&r);
  CHECK(root == X11_ID_ROOT_WINDOW && cmap == X11_ID_DEFAULT_COLORMAP);
  CHECK(root < X11_CLIENT_ID_UNIT && cmap < X11_CLIENT_ID_UNIT);   /* outside every client range */
  CHECK(white == 0xFFFF && black == 0);
  CHECK(u32(&r) == 0);
  CHECK(u16(&r) == 800 && u16(&r) == 480);
  CHECK(u16(&r) == cfg->width_mm && u16(&r) == cfg->height_mm);
  CHECK(u16(&r) == 1 && u16(&r) == 1);                       /* installed maps */
  uint32_t root_visual = u32(&r);
  CHECK(u8(&r) == 0);                                         /* backing stores Never */
  CHECK(u8(&r) == 0);                                         /* save-unders */
  uint8_t root_depth = u8(&r), ndepths = u8(&r);
  CHECK(root_depth == 16 && ndepths == 2);
  int root_visual_found = 0, depth1_listed = 0;
  for (int i = 0; i < ndepths; ++i) {
    uint8_t depth = u8(&r);
    skip(&r, 1);
    uint16_t nvis = u16(&r);
    skip(&r, 4);
    if (depth == 1) { depth1_listed = 1; CHECK(nvis == 0); }
    for (int v = 0; v < nvis; ++v) {
      uint32_t id = u32(&r);
      uint8_t cls = u8(&r), bits = u8(&r);
      uint16_t entries = u16(&r);
      uint32_t rm = u32(&r), gm = u32(&r), bm = u32(&r);
      skip(&r, 4);
      if (id == root_visual) {
        root_visual_found = depth == root_depth;
        CHECK(cls == 4);                                      /* TrueColor */
        CHECK(rm == 0xF800 && gm == 0x07E0 && bm == 0x001F);  /* RGB565 */
        CHECK((rm & gm) == 0 && (gm & bm) == 0 && (rm & bm) == 0);
        CHECK(bits == 6 && entries == 64);
      }
    }
  }
  CHECK(root_visual_found);  /* the root visual is listed under the root depth */
  CHECK(depth1_listed);
  CHECK(r.off == len);       /* every byte accounted for */
}

static void test_success_both_orders(void) {
  x11_server_config cfg;
  x11_default_config(&cfg);
  uint8_t req[64], rep[512];
  for (int o = 0; o < 2; ++o) {
    char order = o ? 'B' : 'l';
    size_t n = build_request(req, order, 11, 0, NULL, NULL, 0);
    x11_setup_result res;
    size_t len = x11_setup_handle(&cfg, req, n, 3, rep, sizeof(rep), &res);
    CHECK(res.status == X11_SETUP_SUCCESS);
    CHECK(res.msb_first == (order == 'B'));
    CHECK(res.consumed == 12);
    CHECK(len == x11_setup_success_len(&cfg));
    check_success(rep, len, order == 'B', &cfg, 4 * X11_CLIENT_ID_UNIT);
  }
}

static void test_partial_input(void) {
  x11_server_config cfg;
  x11_default_config(&cfg);
  uint8_t cookie[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  cfg.cookie = cookie;
  uint8_t req[128], rep[512];
  size_t n = build_request(req, 'l', 11, 0, "MIT-MAGIC-COOKIE-1", cookie, 16);
  CHECK(n == 12 + 20 + 16);
  for (size_t have = 0; have < n; ++have) {  /* any shorter prefix: NEED_MORE, nothing written */
    x11_setup_result res;
    CHECK(x11_setup_handle(&cfg, req, have, 0, rep, sizeof(rep), &res) == 0);
    CHECK(res.status == X11_SETUP_NEED_MORE);
    CHECK(res.needed > have && res.needed <= n);
  }
  x11_setup_result res;
  CHECK(x11_setup_handle(&cfg, req, n, 0, rep, sizeof(rep), &res) > 0 && res.status == X11_SETUP_SUCCESS);
  CHECK(res.consumed == n);
}

static void check_failed(const uint8_t *rep, size_t len, int msb, const char *reason_part) {
  rd r = {rep, len, 0, msb};
  CHECK(u8(&r) == 0);
  uint8_t n = u8(&r);
  CHECK(u16(&r) == 11 && u16(&r) == 0);
  uint16_t units = u16(&r);
  CHECK(units == pad4(n) / 4);
  CHECK(len == 8 + pad4(n));
  char text[256];
  memcpy(text, rep + 8, n);
  text[n] = 0;
  CHECK(strstr(text, reason_part) != NULL);
  for (size_t i = 8 + n; i < len; ++i) CHECK(rep[i] == 0);
}

static int run_fail(const x11_server_config *cfg, char order, uint16_t major, const char *name, const uint8_t *data,
                    uint16_t dlen, const char *reason_part) {
  uint8_t req[1024], rep[512];
  size_t n = build_request(req, order, major, 0, name, data, dlen);
  x11_setup_result res;
  size_t len = x11_setup_handle(cfg, req, n, 0, rep, sizeof(rep), &res);
  CHECK(res.status == X11_SETUP_FAILED);
  if (res.status != X11_SETUP_FAILED) return 0;
  check_failed(rep, len, order == 'B', reason_part);
  return 1;
}

static void test_auth_without_cookie(void) {
  x11_server_config cfg;
  x11_default_config(&cfg);
  uint8_t any[16] = {0};
  /* credentials this server cannot check are rejected, not ignored */
  run_fail(&cfg, 'l', 11, "MIT-MAGIC-COOKIE-1", any, 16, "no MIT-MAGIC-COOKIE-1 is configured");
  run_fail(&cfg, 'B', 11, "XDM-AUTHORIZATION-1", any, 16, "unsupported authorization protocol");
  run_fail(&cfg, 'l', 11, "MIT-KERBEROS-5", NULL, 0, "unsupported");
  run_fail(&cfg, 'l', 11, NULL, any, 8, "unsupported");  /* data without a protocol name */
}

static void test_auth_with_cookie(void) {
  x11_server_config cfg;
  x11_default_config(&cfg);
  uint8_t cookie[16], wrong[16];
  for (int i = 0; i < 16; ++i) cookie[i] = (uint8_t)(0xA0 + i), wrong[i] = cookie[i];
  wrong[15] ^= 1;
  cfg.cookie = cookie;
  run_fail(&cfg, 'l', 11, NULL, NULL, 0, "authorization required");
  run_fail(&cfg, 'B', 11, "MIT-MAGIC-COOKIE-1", wrong, 16, "invalid MIT-MAGIC-COOKIE-1");
  run_fail(&cfg, 'l', 11, "MIT-MAGIC-COOKIE-1", cookie, 15, "invalid MIT-MAGIC-COOKIE-1");
  run_fail(&cfg, 'l', 11, "MIT-MAGIC-COOKIE-", cookie, 16, "unsupported");
  uint8_t req[128], rep[512];
  size_t n = build_request(req, 'B', 11, 0, "MIT-MAGIC-COOKIE-1", cookie, 16);
  x11_setup_result res;
  size_t len = x11_setup_handle(&cfg, req, n, 0, rep, sizeof(rep), &res);
  CHECK(res.status == X11_SETUP_SUCCESS);
  check_success(rep, len, 1, &cfg, X11_CLIENT_ID_UNIT);
}

static void test_malformed(void) {
  x11_server_config cfg;
  x11_default_config(&cfg);
  uint8_t req[64], rep[512];
  build_request(req, 'l', 11, 0, NULL, NULL, 0);
  req[0] = 'x';
  x11_setup_result res;
  size_t len = x11_setup_handle(&cfg, req, 12, 0, rep, sizeof(rep), &res);
  CHECK(res.status == X11_SETUP_FAILED);
  check_failed(rep, len, 0, "byte-order");
  run_fail(&cfg, 'l', 10, NULL, NULL, 0, "unsupported protocol version");
  run_fail(&cfg, 'B', 12, NULL, NULL, 0, "unsupported protocol version");
  /* minor version differences are accepted; the server answers with 11.0 */
  size_t n = build_request(req, 'l', 11, 7, NULL, NULL, 0);
  len = x11_setup_handle(&cfg, req, n, 0, rep, sizeof(rep), &res);
  CHECK(res.status == X11_SETUP_SUCCESS && res.client_minor == 7);
  /* huge auth lengths are refused from the 12-byte header, without waiting for 128 KiB */
  build_request(req, 'l', 11, 0, NULL, NULL, 0);
  req[8] = 0xFF;
  req[9] = 0xFF;
  len = x11_setup_handle(&cfg, req, 12, 0, rep, sizeof(rep), &res);
  CHECK(res.status == X11_SETUP_FAILED && res.consumed == 12);
  check_failed(rep, len, 0, "too large");
  /* too many clients */
  n = build_request(req, 'l', 11, 0, NULL, NULL, 0);
  x11_setup_handle(&cfg, req, n, X11_MAX_CLIENTS, rep, sizeof(rep), &res);
  CHECK(res.status == X11_SETUP_FAILED);
  /* output buffer too small: nothing written, never an overflow */
  CHECK(x11_setup_handle(&cfg, req, n, 0, rep, 40, &res) == 0);
}

static void test_client_ranges(void) {
  x11_server_config cfg;
  x11_default_config(&cfg);
  uint8_t req[64], rep[512];
  size_t n = build_request(req, 'l', 11, 0, NULL, NULL, 0);
  uint32_t prev_end = X11_CLIENT_ID_UNIT;  /* server IDs live below the first client range */
  for (unsigned i = 0; i < X11_MAX_CLIENTS; ++i) {
    x11_setup_result res;
    x11_setup_handle(&cfg, req, n, i, rep, sizeof(rep), &res);
    CHECK(res.status == X11_SETUP_SUCCESS);
    CHECK(res.resource_id_base == prev_end);  /* adjacent, never overlapping */
    prev_end = res.resource_id_base + res.resource_id_mask + 1;
  }
  CHECK(prev_end <= 0x20000000u);
}

static void test_names(void) {
  CHECK(!strcmp(x11_request_name(1), "CreateWindow"));
  CHECK(!strcmp(x11_request_name(20), "GetProperty"));
  CHECK(!strcmp(x11_request_name(43), "GetInputFocus"));
  CHECK(!strcmp(x11_request_name(72), "PutImage"));
  CHECK(!strcmp(x11_request_name(97), "QueryBestSize"));
  CHECK(!strcmp(x11_request_name(98), "QueryExtension"));
  CHECK(!strcmp(x11_request_name(119), "GetModifierMapping"));
  CHECK(!strcmp(x11_request_name(127), "NoOperation"));
  CHECK(!strcmp(x11_request_name(120), "unknown"));
  CHECK(!strcmp(x11_request_name(200), "extension"));
}

int main(void) {
  test_success_both_orders();
  test_partial_input();
  test_auth_without_cookie();
  test_auth_with_cookie();
  test_malformed();
  test_client_ranges();
  test_names();
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  puts("x11 setup tests OK");
  return 0;
}
