/* X11 connection setup, see x11_setup.h. Byte layouts follow the protocol specification,
 * section 8 "Connection Setup" and the encoding appendix ("Connection Setup"). */
#include "x11_setup.h"

#include <string.h>

/* ---- screen model: one 800x480 RGB565 screen ------------------------------------------ */

typedef struct {
  uint8_t depth, bits_per_pixel, scanline_pad;
} pixmap_format;

/* Every depth a client may use for pixmaps or windows has a format. Depth 1 (bitmaps) is
 * always required; depth 16 is the panel's native RGB565. Scanline pad 32 as Xorg uses. */
static const pixmap_format FORMATS[] = {{1, 1, 32}, {16, 16, 32}};
#define FORMAT_COUNT (sizeof(FORMATS) / sizeof(FORMATS[0]))

#define ROOT_DEPTH 16
#define TRUECOLOR 4
#define BACKING_STORE_NEVER 0

static size_t pad4(size_t n) { return (n + 3u) & ~(size_t)3u; }

void x11_default_config(x11_server_config *cfg) {
  cfg->width_px = 800;
  cfg->height_px = 480;
  cfg->width_mm = 95;
  cfg->height_mm = 54;
  cfg->release = 1;
  cfg->vendor = "Woodland Lab experimental X11 (ESP32-S3)";
  cfg->max_request_units = 4096; /* 16 KiB: the protocol minimum, sized for ESP32 RAM */
  cfg->cookie = NULL;
  cfg->max_clients = 0;
  cfg->max_resources = 0;
  cfg->max_resources_per_client = 0;
  cfg->reset_on_last_close = 0;
}

/* ---- byte-order aware writer ----------------------------------------------------------- */

typedef struct {
  uint8_t *p;
  size_t n, cap;
  int msb;
  int overflow;
} wbuf;

static void put8(wbuf *w, uint8_t v) {
  if (w->n + 1 > w->cap) { w->overflow = 1; return; }
  w->p[w->n++] = v;
}
static void put16(wbuf *w, uint16_t v) {
  if (w->msb) { put8(w, (uint8_t)(v >> 8)); put8(w, (uint8_t)v); }
  else { put8(w, (uint8_t)v); put8(w, (uint8_t)(v >> 8)); }
}
static void put32(wbuf *w, uint32_t v) {
  if (w->msb) { put16(w, (uint16_t)(v >> 16)); put16(w, (uint16_t)v); }
  else { put16(w, (uint16_t)v); put16(w, (uint16_t)(v >> 16)); }
}
static void zeros(wbuf *w, size_t k) {
  while (k--) put8(w, 0);
}
static void bytes_padded(wbuf *w, const char *s, size_t len) {
  for (size_t i = 0; i < len; ++i) put8(w, (uint8_t)s[i]);
  zeros(w, pad4(len) - len);
}

static uint16_t get16(const uint8_t *p, int msb) {
  return msb ? (uint16_t)(p[0] << 8 | p[1]) : (uint16_t)(p[1] << 8 | p[0]);
}

/* ---- replies --------------------------------------------------------------------------- */

static size_t screen_len(void) {
  /* SCREEN 40 bytes + DEPTH 16 (8 + one 24-byte VISUALTYPE) + DEPTH 1 (8, no visuals) */
  return 40 + (8 + 24) + 8;
}

size_t x11_setup_success_len(const x11_server_config *cfg) {
  return 8 + 32 + pad4(strlen(cfg->vendor)) + 8 * FORMAT_COUNT + screen_len();
}

static size_t write_success(const x11_server_config *cfg, wbuf *w, uint32_t id_base) {
  size_t vlen = strlen(cfg->vendor);
  size_t total = x11_setup_success_len(cfg);
  /* header: success, unused, protocol version, length of the rest in 4-byte units */
  put8(w, 1);
  put8(w, 0);
  put16(w, X11_PROTOCOL_MAJOR);
  put16(w, X11_PROTOCOL_MINOR);
  put16(w, (uint16_t)((total - 8) / 4));
  /* fixed part, 32 bytes */
  put32(w, cfg->release);
  put32(w, id_base);
  put32(w, X11_CLIENT_ID_MASK);
  put32(w, 0);                         /* motion-buffer-size: no motion history */
  put16(w, (uint16_t)vlen);
  put16(w, cfg->max_request_units);
  put8(w, 1);                          /* number of SCREENs */
  put8(w, (uint8_t)FORMAT_COUNT);
  put8(w, 0);                          /* image-byte-order: LSBFirst (ESP32 is little endian) */
  put8(w, 0);                          /* bitmap-format-bit-order: LeastSignificant */
  put8(w, 32);                         /* bitmap-format-scanline-unit */
  put8(w, 32);                         /* bitmap-format-scanline-pad */
  put8(w, 8);                          /* min-keycode */
  put8(w, 255);                        /* max-keycode */
  zeros(w, 4);
  bytes_padded(w, cfg->vendor, vlen);
  for (size_t i = 0; i < FORMAT_COUNT; ++i) {
    put8(w, FORMATS[i].depth);
    put8(w, FORMATS[i].bits_per_pixel);
    put8(w, FORMATS[i].scanline_pad);
    zeros(w, 5);
  }
  /* SCREEN */
  put32(w, X11_ID_ROOT_WINDOW);
  put32(w, X11_ID_DEFAULT_COLORMAP);
  put32(w, 0xFFFFu);                   /* white-pixel (RGB565) */
  put32(w, 0x0000u);                   /* black-pixel */
  put32(w, 0);                         /* current-input-masks of the root window */
  put16(w, cfg->width_px);
  put16(w, cfg->height_px);
  put16(w, cfg->width_mm);
  put16(w, cfg->height_mm);
  put16(w, 1);                         /* min-installed-maps */
  put16(w, 1);                         /* max-installed-maps */
  put32(w, X11_ID_VISUAL_TRUECOLOR16); /* root-visual */
  put8(w, BACKING_STORE_NEVER);
  put8(w, 0);                          /* save-unders: false */
  put8(w, ROOT_DEPTH);
  put8(w, 2);                          /* number of DEPTHs in allowed-depths */
  /* DEPTH 16 with its TrueColor visual */
  put8(w, 16);
  put8(w, 0);
  put16(w, 1);                         /* number of VISUALTYPEs */
  zeros(w, 4);
  put32(w, X11_ID_VISUAL_TRUECOLOR16);
  put8(w, TRUECOLOR);
  put8(w, 6);                          /* bits-per-rgb-value: 6 (green has 6 bits) */
  put16(w, 64);                        /* colormap-entries: 2^6 */
  put32(w, 0xF800u);                   /* red-mask */
  put32(w, 0x07E0u);                   /* green-mask */
  put32(w, 0x001Fu);                   /* blue-mask */
  zeros(w, 4);
  /* DEPTH 1: pixmaps only, no visual */
  put8(w, 1);
  put8(w, 0);
  put16(w, 0);
  zeros(w, 4);
  return w->overflow ? 0 : w->n;
}

static size_t write_failed(wbuf *w, const char *reason) {
  size_t n = strlen(reason);
  if (n > 255) n = 255;
  put8(w, 0);                          /* Failed */
  put8(w, (uint8_t)n);
  put16(w, X11_PROTOCOL_MAJOR);
  put16(w, X11_PROTOCOL_MINOR);
  put16(w, (uint16_t)(pad4(n) / 4));
  bytes_padded(w, reason, n);
  return w->overflow ? 0 : w->n;
}

/* Constant-time comparison: the time taken does not reveal how many bytes matched. */
static int same_bytes(const uint8_t *a, const uint8_t *b, size_t n) {
  uint8_t d = 0;
  for (size_t i = 0; i < n; ++i) d |= (uint8_t)(a[i] ^ b[i]);
  return d == 0;
}

static size_t fail(wbuf *w, x11_setup_result *r, const char *reason, size_t consumed) {
  r->status = X11_SETUP_FAILED;
  r->reason = reason;
  r->consumed = consumed;
  return write_failed(w, reason);
}

size_t x11_setup_refuse(const uint8_t *in, size_t in_len, const char *reason, uint8_t *out, size_t out_cap) {
  if (in_len < 1 || (in[0] != 'B' && in[0] != 'l')) return 0;
  wbuf w = {out, 0, out_cap, in[0] == 'B', 0};
  return write_failed(&w, reason);
}

size_t x11_setup_handle(const x11_server_config *cfg, const uint8_t *in, size_t in_len, unsigned client_index,
                        uint8_t *out, size_t out_cap, x11_setup_result *r) {
  memset(r, 0, sizeof(*r));
  if (in_len < X11_SETUP_HEADER_LEN) {
    r->status = X11_SETUP_NEED_MORE;
    r->needed = X11_SETUP_HEADER_LEN;
    return 0;
  }
  wbuf w = {out, 0, out_cap, 0, 0};
  if (in[0] == 'B') r->msb_first = 1;
  else if (in[0] != 'l') return fail(&w, r, "invalid byte-order byte (expected 'B' or 'l')", X11_SETUP_HEADER_LEN);
  w.msb = r->msb_first;
  r->client_major = get16(in + 2, w.msb);
  r->client_minor = get16(in + 4, w.msb);
  uint16_t n = get16(in + 6, w.msb), d = get16(in + 8, w.msb);
  if (r->client_major != X11_PROTOCOL_MAJOR)
    return fail(&w, r, "unsupported protocol version (this server speaks X11.0)", X11_SETUP_HEADER_LEN);
  if (n > X11_MAX_AUTH_NAME || d > X11_MAX_AUTH_DATA)
    return fail(&w, r, "authorization data too large", X11_SETUP_HEADER_LEN);
  size_t need = X11_SETUP_HEADER_LEN + pad4(n) + pad4(d);
  if (in_len < need) {
    r->status = X11_SETUP_NEED_MORE;
    r->needed = need;
    return 0;
  }
  const uint8_t *name = in + X11_SETUP_HEADER_LEN, *data = name + pad4(n);
  static const char COOKIE_NAME[] = "MIT-MAGIC-COOKIE-1";
  if (n == 0 && d == 0) {
    if (cfg->cookie) return fail(&w, r, "authorization required (MIT-MAGIC-COOKIE-1)", need);
  } else if (n == sizeof(COOKIE_NAME) - 1 && memcmp(name, COOKIE_NAME, n) == 0) {
    if (!cfg->cookie) return fail(&w, r, "no MIT-MAGIC-COOKIE-1 is configured on this server", need);
    if (d != X11_COOKIE_LEN || !same_bytes(data, cfg->cookie, X11_COOKIE_LEN))
      return fail(&w, r, "invalid MIT-MAGIC-COOKIE-1", need);
  } else {
    return fail(&w, r, "unsupported authorization protocol (only MIT-MAGIC-COOKIE-1)", need);
  }
  if (client_index >= X11_MAX_CLIENTS) return fail(&w, r, "too many clients", need);
  r->resource_id_base = (client_index + 1u) * X11_CLIENT_ID_UNIT;
  r->resource_id_mask = X11_CLIENT_ID_MASK;
  size_t len = write_success(cfg, &w, r->resource_id_base);
  if (!len) return 0; /* out_cap too small: caller error (see x11_setup_success_len) */
  r->status = X11_SETUP_SUCCESS;
  r->consumed = need;
  return len;
}
