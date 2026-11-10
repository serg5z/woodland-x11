/* Unit tests for core/x11_server.c: every implemented request in both byte orders, its
 * errors (code, sequence number, bad value, major opcode), framing, resource IDs across
 * clients, atoms, and cleanup on close. Run with ASan/UBSan (Makefile). */
#include <stdio.h>
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

static uint16_t g16(const uint8_t *p, int msb) { return msb ? (uint16_t)(p[0] << 8 | p[1]) : (uint16_t)(p[1] << 8 | p[0]); }
static uint32_t g32(const uint8_t *p, int msb) {
  return msb ? (uint32_t)g16(p, 1) << 16 | g16(p + 2, 1) : (uint32_t)g16(p + 2, 0) << 16 | g16(p, 0);
}
static void p16(uint8_t *p, uint16_t v, int msb) {
  if (msb) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; } else { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
}
static void p32(uint8_t *p, uint32_t v, int msb) {
  if (msb) { p16(p, (uint16_t)(v >> 16), 1); p16(p + 2, (uint16_t)v, 1); }
  else { p16(p, (uint16_t)v, 0); p16(p + 2, (uint16_t)(v >> 16), 0); }
}

/* Opens a client and completes setup in the given byte order. */
static int connect_client(int msb) {
  int c = x11_client_open(&srv);
  uint8_t req[12] = {(uint8_t)(msb ? 'B' : 'l'), 0};
  p16(req + 2, 11, msb);
  uint8_t rep[512];
  x11_setup_result r;
  x11_client_setup(&srv, c, req, sizeof(req), rep, sizeof(rep), &r);
  CHECK(r.status == X11_SETUP_SUCCESS);
  return c;
}

/* Request builder: header + body words. */
typedef struct {
  uint8_t b[256];
  size_t n;
  int msb;
} req;

static void begin(req *q, int msb, uint8_t op, uint8_t data) {
  memset(q, 0, sizeof(*q));
  q->msb = msb;
  q->b[0] = op;
  q->b[1] = data;
  q->n = 4;
}
static void add32(req *q, uint32_t v) { p32(q->b + q->n, v, q->msb); q->n += 4; }
static void add16pair(req *q, uint16_t a, uint16_t b) { p16(q->b + q->n, a, q->msb); p16(q->b + q->n + 2, b, q->msb); q->n += 4; }
static void addstr(req *q, const char *s) {
  size_t n = strlen(s);
  memcpy(q->b + q->n, s, n);
  q->n += (n + 3) & ~(size_t)3;
}
static size_t send_req(int c, req *q, int *close_after) {
  p16(q->b + 2, (uint16_t)(q->n / 4), q->msb);
  CHECK(x11_request_size(&srv, c, q->b) == q->n);
  return x11_client_request(&srv, c, q->b, q->n, out, sizeof(out), close_after);
}
static size_t run(int c, req *q) {
  int close_after;
  size_t n = send_req(c, q, &close_after);
  CHECK(!close_after);
  return n;
}
static void expect_error(size_t n, int msb, uint8_t code, uint16_t seq, uint32_t bad, uint8_t major) {
  CHECK(n == 32);
  CHECK(out[0] == 0 && out[1] == code);
  CHECK(g16(out + 2, msb) == seq);
  CHECK(g32(out + 4, msb) == bad);
  CHECK(out[10] == major);
}
static void expect_reply(size_t n, int msb, uint16_t seq, size_t extra) {
  CHECK(n == 32 + extra);
  CHECK(out[0] == 1);
  CHECK(g16(out + 2, msb) == seq);
  CHECK(g32(out + 4, msb) == extra / 4);
}

static void test_xdpyinfo_sequence(int msb) {
  x11_server_config cfg;
  x11_default_config(&cfg);
  x11_server_init(&srv, &cfg);
  int c = connect_client(msb);
  uint32_t base = srv.clients[c].id_base;
  req q;
  begin(&q, msb, 98, 0); add16pair(&q, 12, 0); addstr(&q, "BIG-REQUESTS");   /* QueryExtension */
  expect_reply(run(c, &q), msb, 1, 0);
  CHECK(out[8] == 0);                                                         /* not present */
  begin(&q, msb, 55, 0); add32(&q, base); add32(&q, X11_ID_ROOT_WINDOW); add32(&q, 1u << 16); add32(&q, 0);
  CHECK(run(c, &q) == 0);                                                     /* CreateGC: no reply */
  CHECK(x11_resources_used(&srv) == 1);
  begin(&q, msb, 20, 0); add32(&q, X11_ID_ROOT_WINDOW); add32(&q, 23); add32(&q, 31); add32(&q, 0); add32(&q, 100000000);
  expect_reply(run(c, &q), msb, 3, 0);                                        /* GetProperty */
  CHECK(out[1] == 0 && g32(out + 8, msb) == 0 && g32(out + 12, msb) == 0 && g32(out + 16, msb) == 0);
  begin(&q, msb, 43, 0);
  expect_reply(run(c, &q), msb, 4, 0);                                        /* GetInputFocus */
  CHECK(out[1] == 0 && g32(out + 8, msb) == 1);                               /* PointerRoot */
  begin(&q, msb, 99, 0);
  expect_reply(run(c, &q), msb, 5, 0);                                        /* ListExtensions */
  CHECK(out[1] == 0);
  begin(&q, msb, 97, 0); add32(&q, X11_ID_ROOT_WINDOW); add16pair(&q, 65535, 65535);
  expect_reply(run(c, &q), msb, 6, 0);                                        /* QueryBestSize cursor */
  CHECK(g16(out + 8, msb) == 32 && g16(out + 10, msb) == 32);
  begin(&q, msb, 97, 1); add32(&q, X11_ID_ROOT_WINDOW); add16pair(&q, 17, 9);
  expect_reply(run(c, &q), msb, 7, 0);                                        /* Tile: as asked */
  CHECK(g16(out + 8, msb) == 17 && g16(out + 10, msb) == 9);
  begin(&q, msb, 60, 0); add32(&q, base);
  CHECK(run(c, &q) == 0);                                                     /* FreeGC */
  CHECK(x11_resources_used(&srv) == 0);
  begin(&q, msb, 127, 0); add32(&q, 0);
  CHECK(run(c, &q) == 0);                                                     /* NoOperation, any length */
  CHECK(srv.clients[c].seq == 9 && srv.clients[c].errors == 0);
  x11_server_free(&srv);
}

static void test_errors(int msb) {
  x11_server_config cfg;
  x11_default_config(&cfg);
  x11_server_init(&srv, &cfg);
  int c = connect_client(msb);
  uint32_t base = srv.clients[c].id_base;
  req q;
  uint16_t seq = 0;
  /* CreateGC: id outside the client range, on a non-existent drawable, duplicate id,
   * a font or pixmap reference, an invalid enum value, a wrong length */
  begin(&q, msb, 55, 0); add32(&q, 0x123); add32(&q, X11_ID_ROOT_WINDOW); add32(&q, 0);
  expect_error(run(c, &q), msb, X11_BAD_IDCHOICE, ++seq, 0x123, 55);
  begin(&q, msb, 55, 0); add32(&q, base); add32(&q, 0x99); add32(&q, 0);
  expect_error(run(c, &q), msb, X11_BAD_DRAWABLE, ++seq, 0x99, 55);
  begin(&q, msb, 55, 0); add32(&q, base); add32(&q, X11_ID_ROOT_WINDOW); add32(&q, 0);
  CHECK(run(c, &q) == 0); ++seq;
  begin(&q, msb, 55, 0); add32(&q, base); add32(&q, X11_ID_ROOT_WINDOW); add32(&q, 0);
  expect_error(run(c, &q), msb, X11_BAD_IDCHOICE, ++seq, base, 55);
  begin(&q, msb, 55, 0); add32(&q, base + 1); add32(&q, X11_ID_ROOT_WINDOW); add32(&q, 1u << 14); add32(&q, 0x4242);
  expect_error(run(c, &q), msb, X11_BAD_FONT, ++seq, 0x4242, 55);
  begin(&q, msb, 55, 0); add32(&q, base + 1); add32(&q, X11_ID_ROOT_WINDOW); add32(&q, 1u << 10); add32(&q, 7);
  expect_error(run(c, &q), msb, X11_BAD_PIXMAP, ++seq, 7, 55);
  begin(&q, msb, 55, 0); add32(&q, base + 1); add32(&q, X11_ID_ROOT_WINDOW); add32(&q, 1); add32(&q, 16);
  expect_error(run(c, &q), msb, X11_BAD_VALUE, ++seq, 16, 55);              /* function > 15 */
  begin(&q, msb, 55, 0); add32(&q, base + 1); add32(&q, X11_ID_ROOT_WINDOW); add32(&q, 3); add32(&q, 3);
  expect_error(run(c, &q), msb, X11_BAD_LENGTH, ++seq, 0, 55);              /* 2 values announced, 1 sent */
  /* FreeGC of an unknown GC */
  begin(&q, msb, 60, 0); add32(&q, base + 5);
  expect_error(run(c, &q), msb, X11_BAD_GC, ++seq, base + 5, 60);
  /* GetProperty: wrong window, unknown atom, bad delete flag */
  begin(&q, msb, 20, 0); add32(&q, 0x77); add32(&q, 23); add32(&q, 0); add32(&q, 0); add32(&q, 1);
  expect_error(run(c, &q), msb, X11_BAD_WINDOW, ++seq, 0x77, 20);
  begin(&q, msb, 20, 0); add32(&q, X11_ID_ROOT_WINDOW); add32(&q, 999); add32(&q, 0); add32(&q, 0); add32(&q, 1);
  expect_error(run(c, &q), msb, X11_BAD_ATOM, ++seq, 999, 20);
  begin(&q, msb, 20, 2); add32(&q, X11_ID_ROOT_WINDOW); add32(&q, 23); add32(&q, 0); add32(&q, 0); add32(&q, 1);
  expect_error(run(c, &q), msb, X11_BAD_VALUE, ++seq, 2, 20);
  /* QueryBestSize: bad class; QueryExtension: inconsistent length */
  begin(&q, msb, 97, 3); add32(&q, X11_ID_ROOT_WINDOW); add16pair(&q, 1, 1);
  expect_error(run(c, &q), msb, X11_BAD_VALUE, ++seq, 3, 97);
  begin(&q, msb, 98, 0); add16pair(&q, 40, 0); addstr(&q, "XKEYBOARD");
  expect_error(run(c, &q), msb, X11_BAD_LENGTH, ++seq, 0, 98);
  /* GetInputFocus with extra words */
  begin(&q, msb, 43, 0); add32(&q, 0);
  expect_error(run(c, &q), msb, X11_BAD_LENGTH, ++seq, 0, 43);
  /* ChangeHosts (no client may change host access), not a core request at all */
  begin(&q, msb, 109, 0); add32(&q, 0);
  expect_error(run(c, &q), msb, X11_BAD_ACCESS, ++seq, 0, 109);
  begin(&q, msb, 140, 0);
  expect_error(run(c, &q), msb, X11_BAD_REQUEST, ++seq, 0, 140);
  begin(&q, msb, 121, 0);
  expect_error(run(c, &q), msb, X11_BAD_REQUEST, ++seq, 0, 121);
  CHECK(srv.clients[c].errors == (uint32_t)(seq - 1));
  x11_server_free(&srv);
}

static void test_framing(void) {
  x11_server_config cfg;
  x11_default_config(&cfg);
  x11_server_init(&srv, &cfg);
  int c = connect_client(0);
  uint8_t hdr[4] = {43, 0, 0, 0};                     /* length 0: BIG-REQUESTS form, not offered */
  CHECK(x11_request_size(&srv, c, hdr) == 0);
  int close_after;
  size_t n = x11_client_request(&srv, c, hdr, 4, out, sizeof(out), &close_after);
  CHECK(!close_after && n == 32 && out[0] == 0 && out[1] == X11_BAD_LENGTH && g16(out + 2, 0) == 1);
  uint8_t big[4] = {72, 0, 0x01, 0x10};               /* 4097 units: above maximum-request-length */
  CHECK(x11_request_size(&srv, c, big) == 0);
  uint8_t ok[4] = {72, 0, 0x00, 0x10};                /* exactly 4096 units: allowed */
  CHECK(x11_request_size(&srv, c, ok) == 4 * 4096);
  /* too long: BadLength with the next sequence number and the major opcode; the transport
   * discards all 4097 units and the client stays */
  size_t skip = 1;
  n = x11_client_request_too_long(&srv, c, big, out, sizeof(out), &skip);
  CHECK(n == 32 && out[0] == 0 && out[1] == X11_BAD_LENGTH && g16(out + 2, 0) == 2 && out[10] == 72);
  CHECK(skip == 4 * 4097);
  CHECK(x11_client_request_too_long(&srv, c, ok, out, sizeof(out), &skip) == 0 && skip == 0);
  CHECK(srv.clients[c].seq == 2);
  CHECK(x11_client_request(&srv, c, hdr, 4, out, 40, &close_after) == 0);  /* output too small: refuse */
  x11_server_free(&srv);
}

static void test_clients_and_atoms(void) {
  x11_server_config cfg;
  x11_default_config(&cfg);
  x11_server_init(&srv, &cfg);
  int a = connect_client(0), b = connect_client(1);
  CHECK(a != b && srv.clients[a].id_base != srv.clients[b].id_base);
  req q;
  /* each client creates a GC with the same low bits in its own range */
  begin(&q, 0, 55, 0); add32(&q, srv.clients[a].id_base | 7); add32(&q, X11_ID_ROOT_WINDOW); add32(&q, 0);
  CHECK(run(a, &q) == 0);
  begin(&q, 1, 55, 0); add32(&q, srv.clients[b].id_base | 7); add32(&q, X11_ID_ROOT_WINDOW); add32(&q, 0);
  CHECK(run(b, &q) == 0);
  /* b may not create an ID in a's range */
  begin(&q, 1, 55, 0); add32(&q, srv.clients[a].id_base | 8); add32(&q, X11_ID_ROOT_WINDOW); add32(&q, 0);
  expect_error(run(b, &q), 1, X11_BAD_IDCHOICE, 2, srv.clients[a].id_base | 8, 55);
  CHECK(x11_resources_used(&srv) == 2);
  /* independent sequence numbers */
  CHECK(srv.clients[a].seq == 1 && srv.clients[b].seq == 2);
  /* closing a frees only a's resources */
  x11_client_close(&srv, a);
  CHECK(x11_resources_used(&srv) == 1);
  int a2 = connect_client(0);
  CHECK(a2 == a && srv.clients[a2].seq == 0);
  /* atoms: predefined lookup, new atom, only-if-exists, GetAtomName */
  begin(&q, 0, 16, 1); add16pair(&q, 6, 0); addstr(&q, "STRING");
  expect_reply(run(a2, &q), 0, 1, 0);
  CHECK(g32(out + 8, 0) == 31);
  begin(&q, 0, 16, 1); add16pair(&q, 9, 0); addstr(&q, "_WOODLAND");
  expect_reply(run(a2, &q), 0, 2, 0);
  CHECK(g32(out + 8, 0) == 0);                                  /* None: only if exists */
  begin(&q, 0, 16, 0); add16pair(&q, 9, 0); addstr(&q, "_WOODLAND");
  expect_reply(run(a2, &q), 0, 3, 0);
  uint32_t atom = g32(out + 8, 0);
  CHECK(atom == 69);
  begin(&q, 1, 16, 0); add16pair(&q, 9, 0); addstr(&q, "_WOODLAND");  /* atoms are server-wide */
  expect_reply(run(b, &q), 1, 3, 0);
  CHECK(g32(out + 8, 1) == 69);
  begin(&q, 1, 17, 0); add32(&q, 69);
  expect_reply(run(b, &q), 1, 4, 12);
  CHECK(g16(out + 8, 1) == 9 && memcmp(out + 32, "_WOODLAND\0\0\0", 12) == 0);
  begin(&q, 0, 17, 0); add32(&q, 68);
  expect_reply(run(a2, &q), 0, 4, 16);
  CHECK(memcmp(out + 32, "WM_TRANSIENT_FOR", 16) == 0);
  begin(&q, 0, 17, 0); add32(&q, 0);
  expect_error(run(a2, &q), 0, X11_BAD_ATOM, 5, 0, 17);
  CHECK(x11_atom_name(&srv, 23) && !strcmp(x11_atom_name(&srv, 23), "RESOURCE_MANAGER"));
  /* slots run out */
  int n = 2;
  while (x11_client_open(&srv) >= 0) ++n;
  CHECK(n == X11_SERVER_CLIENTS);
}

/* RecolorCursor (96): a glyph cursor from the "cursor" font is recoloured; a wrong length, an
 * unknown id, a non-cursor resource and a freed cursor are errors (both byte orders). */
static void test_recolor_cursor(int msb) {
  x11_server_config cfg;
  x11_default_config(&cfg);
  x11_server_init(&srv, &cfg);
  int c = connect_client(msb), d = connect_client(msb);
  uint32_t base = srv.clients[c].id_base, font = base + 1, cur = base + 2, gc = base + 3;
  req q;
  uint16_t seq = 0;
  begin(&q, msb, 45, 0); add32(&q, font); add16pair(&q, 6, 0); addstr(&q, "cursor");
  CHECK(run(c, &q) == 0); ++seq;
  /* CreateGlyphCursor: XC_xterm (152) with its mask (153), black on white */
  begin(&q, msb, 94, 0); add32(&q, cur); add32(&q, font); add32(&q, font); add16pair(&q, 152, 153);
  add16pair(&q, 0, 0); add16pair(&q, 0, 0xFFFF); add16pair(&q, 0xFFFF, 0xFFFF);
  CHECK(run(c, &q) == 0); ++seq;
  /* recolour: red on blue; values spanning both bytes catch a byte-order mistake */
  begin(&q, msb, 96, 0); add32(&q, cur); add16pair(&q, 0xFF00, 0x0012); add16pair(&q, 0x3400, 0x0000);
  add16pair(&q, 0x0056, 0xFF78);
  CHECK(run(c, &q) == 0); ++seq;
  CHECK(srv.clients[c].errors == 0);
  /* another client may recolour it too (cursors are shared resources) */
  begin(&q, msb, 96, 0); add32(&q, cur); add16pair(&q, 1, 2); add16pair(&q, 3, 4); add16pair(&q, 5, 6);
  CHECK(run(d, &q) == 0);
  CHECK(srv.clients[d].errors == 0);
  /* wrong lengths: 4 and 6 words */
  begin(&q, msb, 96, 0); add32(&q, cur); add16pair(&q, 0, 0); add16pair(&q, 0, 0);
  expect_error(run(c, &q), msb, X11_BAD_LENGTH, ++seq, 0, 96);
  begin(&q, msb, 96, 0); add32(&q, cur); add16pair(&q, 0, 0); add16pair(&q, 0, 0); add16pair(&q, 0, 0); add32(&q, 0);
  expect_error(run(c, &q), msb, X11_BAD_LENGTH, ++seq, 0, 96);
  /* an unknown id, the None id, a GC, a font */
  begin(&q, msb, 96, 0); add32(&q, base + 50); add16pair(&q, 0, 0); add16pair(&q, 0, 0); add16pair(&q, 0, 0);
  expect_error(run(c, &q), msb, X11_BAD_CURSOR, ++seq, base + 50, 96);
  begin(&q, msb, 96, 0); add32(&q, 0); add16pair(&q, 0, 0); add16pair(&q, 0, 0); add16pair(&q, 0, 0);
  expect_error(run(c, &q), msb, X11_BAD_CURSOR, ++seq, 0, 96);
  begin(&q, msb, 55, 0); add32(&q, gc); add32(&q, X11_ID_ROOT_WINDOW); add32(&q, 0);
  CHECK(run(c, &q) == 0); ++seq;
  begin(&q, msb, 96, 0); add32(&q, gc); add16pair(&q, 0, 0); add16pair(&q, 0, 0); add16pair(&q, 0, 0);
  expect_error(run(c, &q), msb, X11_BAD_CURSOR, ++seq, gc, 96);
  begin(&q, msb, 96, 0); add32(&q, font); add16pair(&q, 0, 0); add16pair(&q, 0, 0); add16pair(&q, 0, 0);
  expect_error(run(c, &q), msb, X11_BAD_CURSOR, ++seq, font, 96);
  /* the cursor still works as one (a window may use it) and can be freed; then it is gone */
  begin(&q, msb, 2, 0); add32(&q, X11_ID_ROOT_WINDOW); add32(&q, 1u << 14); add32(&q, cur);
  CHECK(run(c, &q) == 0); ++seq;
  begin(&q, msb, 95, 0); add32(&q, cur);
  CHECK(run(c, &q) == 0); ++seq;
  begin(&q, msb, 96, 0); add32(&q, cur); add16pair(&q, 0, 0); add16pair(&q, 0, 0); add16pair(&q, 0, 0);
  expect_error(run(c, &q), msb, X11_BAD_CURSOR, ++seq, cur, 96);
  CHECK(srv.clients[c].errors == 7);
  x11_server_free(&srv);
}

/* SetScreenSaver (107), GetScreenSaver (108), ForceScreenSaver (115): values are checked,
 * kept and reported; -1 and Default restore the defaults (XTS calls ForceScreenSaver at the
 * start of every test). */
static void test_screen_saver(int msb) {
  x11_server_config cfg;
  x11_default_config(&cfg);
  x11_server_init(&srv, &cfg);
  int c = connect_client(msb);
  req q;
  uint16_t seq = 0;
  begin(&q, msb, 108, 0);
  expect_reply(run(c, &q), msb, ++seq, 0);
  CHECK(g16(out + 8, msb) == 0 && g16(out + 10, msb) == 0 && out[12] == 1 && out[13] == 1);
  begin(&q, msb, 107, 0); add16pair(&q, 300, 0x1234); add16pair(&q, 0, 0);  /* No, No */
  CHECK(run(c, &q) == 0); ++seq;
  begin(&q, msb, 108, 0);
  expect_reply(run(c, &q), msb, ++seq, 0);
  CHECK(g16(out + 8, msb) == 300 && g16(out + 10, msb) == 0x1234 && out[12] == 0 && out[13] == 0);
  begin(&q, msb, 107, 0); add16pair(&q, 0xFFFF, 0xFFFF); add16pair(&q, 2 | 2 << 8, 0);  /* -1, -1, Default */
  CHECK(run(c, &q) == 0); ++seq;
  begin(&q, msb, 108, 0);
  expect_reply(run(c, &q), msb, ++seq, 0);
  CHECK(g16(out + 8, msb) == 0 && g16(out + 10, msb) == 0 && out[12] == 1 && out[13] == 1);
  /* errors: timeout -2, interval -5, prefer-blanking 3, allow-exposures 3, wrong lengths */
  begin(&q, msb, 107, 0); add16pair(&q, 0xFFFE, 0); add16pair(&q, 0, 0);
  expect_error(run(c, &q), msb, X11_BAD_VALUE, ++seq, 0xFFFFFFFEu, 107);
  begin(&q, msb, 107, 0); add16pair(&q, 0, 0xFFFB); add16pair(&q, 0, 0);
  expect_error(run(c, &q), msb, X11_BAD_VALUE, ++seq, 0xFFFFFFFBu, 107);
  begin(&q, msb, 107, 0); add16pair(&q, 0, 0); q.b[8] = 3; q.n += 4;
  expect_error(run(c, &q), msb, X11_BAD_VALUE, ++seq, 3, 107);
  begin(&q, msb, 107, 0); add16pair(&q, 0, 0); q.b[9] = 3; q.n += 4;
  expect_error(run(c, &q), msb, X11_BAD_VALUE, ++seq, 3, 107);
  begin(&q, msb, 107, 0); add16pair(&q, 0, 0);
  expect_error(run(c, &q), msb, X11_BAD_LENGTH, ++seq, 0, 107);
  begin(&q, msb, 108, 0); add32(&q, 0);
  expect_error(run(c, &q), msb, X11_BAD_LENGTH, ++seq, 0, 108);
  /* ForceScreenSaver: Reset and Activate; mode 2 and a wrong length are errors */
  begin(&q, msb, 115, 0);
  CHECK(run(c, &q) == 0); ++seq;
  begin(&q, msb, 115, 1);
  CHECK(run(c, &q) == 0); ++seq;
  begin(&q, msb, 115, 2);
  expect_error(run(c, &q), msb, X11_BAD_VALUE, ++seq, 2, 115);
  begin(&q, msb, 115, 0); add32(&q, 0);
  expect_error(run(c, &q), msb, X11_BAD_LENGTH, ++seq, 0, 115);
  CHECK(srv.clients[c].errors == 8);
  x11_server_free(&srv);
}

/* An undefined value-mask bit has its own value word: the length is right, so it is BadValue
 * (XTS CreateWindow-6, ChangeWindowAttributes-4, ConfigureWindow-4); without the word it is
 * BadLength. */
static void test_undefined_mask_bits(int msb) {
  x11_server_config cfg;
  x11_default_config(&cfg);
  x11_server_init(&srv, &cfg);
  int c = connect_client(msb);
  uint32_t wid = srv.clients[c].id_base + 1;
  req q;
  uint16_t seq = 0;
  /* CreateWindow: mask bit 15 with one value */
  begin(&q, msb, 1, 0); add32(&q, wid); add32(&q, X11_ID_ROOT_WINDOW); add16pair(&q, 0, 0); add16pair(&q, 10, 10);
  add16pair(&q, 0, 1); add32(&q, 0); add32(&q, 0x8000); add32(&q, 0);
  expect_error(run(c, &q), msb, X11_BAD_VALUE, ++seq, 0x8000, 1);
  begin(&q, msb, 1, 0); add32(&q, wid); add32(&q, X11_ID_ROOT_WINDOW); add16pair(&q, 0, 0); add16pair(&q, 10, 10);
  add16pair(&q, 0, 1); add32(&q, 0); add32(&q, 0x8000);
  expect_error(run(c, &q), msb, X11_BAD_LENGTH, ++seq, 0, 1);
  CHECK(x11_resources_used(&srv) == 0);
  /* a real window for the other two */
  begin(&q, msb, 1, 0); add32(&q, wid); add32(&q, X11_ID_ROOT_WINDOW); add16pair(&q, 0, 0); add16pair(&q, 10, 10);
  add16pair(&q, 0, 1); add32(&q, 0); add32(&q, 0);
  CHECK(run(c, &q) == 0); ++seq;
  /* ChangeWindowAttributes: background-pixel and bit 16 */
  begin(&q, msb, 2, 0); add32(&q, wid); add32(&q, 0x10002); add32(&q, 5); add32(&q, 0);
  expect_error(run(c, &q), msb, X11_BAD_VALUE, ++seq, 0x10002, 2);
  /* ConfigureWindow: x and bit 7 */
  begin(&q, msb, 12, 0); add32(&q, wid); add16pair(&q, 0x81, 0); add32(&q, 3); add32(&q, 0);
  expect_error(run(c, &q), msb, X11_BAD_VALUE, ++seq, 0x81, 12);
  begin(&q, msb, 12, 0); add32(&q, wid); add16pair(&q, 0x81, 0); add32(&q, 3);
  expect_error(run(c, &q), msb, X11_BAD_LENGTH, ++seq, 0, 12);
  CHECK(srv.clients[c].errors == 5);
  x11_server_free(&srv);
}

/* QueryKeymap, Bell, Change/GetKeyboardControl, Change/GetPointerControl, GetPointerMapping,
 * Set/GetFontPath, UnmapSubwindows: replies, kept values, defaults and errors. */
static void test_controls(int msb) {
  x11_server_config cfg;
  x11_default_config(&cfg);
  x11_server_init(&srv, &cfg);
  int c = connect_client(msb);
  req q;
  uint16_t seq = 0;
  int errors = 0;
  begin(&q, msb, 44, 0);                                            /* QueryKeymap: nothing down */
  expect_reply(run(c, &q), msb, ++seq, 8);
  for (int i = 8; i < 40; ++i) CHECK(out[i] == 0);
  begin(&q, msb, 104, (uint8_t)-100); CHECK(run(c, &q) == 0); ++seq;  /* Bell */
  begin(&q, msb, 104, 100); CHECK(run(c, &q) == 0); ++seq;
  begin(&q, msb, 104, 101);
  expect_error(run(c, &q), msb, X11_BAD_VALUE, ++seq, 101, 104); ++errors;
  begin(&q, msb, 104, (uint8_t)-101);
  expect_error(run(c, &q), msb, X11_BAD_VALUE, ++seq, 0xFFFFFF9Bu, 104); ++errors;
  /* GetKeyboardControl defaults */
  begin(&q, msb, 103, 0);
  expect_reply(run(c, &q), msb, ++seq, 20);
  CHECK(out[1] == 1 && g32(out + 8, msb) == 0 && out[12] == 0 && out[13] == 50);
  CHECK(g16(out + 14, msb) == 400 && g16(out + 16, msb) == 100 && out[20] == 0xFF && out[51] == 0xFF);
  /* bell-percent 70, bell-pitch 880, led 3 on, key 10 auto-repeat off */
  begin(&q, msb, 102, 0); add32(&q, 0x02 | 0x04 | 0x10 | 0x20 | 0x40 | 0x80);
  add32(&q, 70); add32(&q, 880); add32(&q, 3); add32(&q, 1); add32(&q, 10); add32(&q, 0);
  CHECK(run(c, &q) == 0); ++seq;
  begin(&q, msb, 103, 0);
  expect_reply(run(c, &q), msb, ++seq, 20);
  CHECK(out[13] == 70 && g16(out + 14, msb) == 880 && g32(out + 8, msb) == 4 && out[21] == (0xFF & ~4));
  /* global auto-repeat off; then -1 restores bell-percent */
  begin(&q, msb, 102, 0); add32(&q, 0x80 | 0x02); add32(&q, (uint32_t)-1); add32(&q, 0);
  CHECK(run(c, &q) == 0); ++seq;
  begin(&q, msb, 103, 0);
  expect_reply(run(c, &q), msb, ++seq, 20);
  CHECK(out[1] == 0 && out[13] == 50);
  /* errors: click 101, led 33, key without auto-repeat-mode, led without led-mode, bit 8, length */
  begin(&q, msb, 102, 0); add32(&q, 0x01); add32(&q, 101);
  expect_error(run(c, &q), msb, X11_BAD_VALUE, ++seq, 101, 102); ++errors;
  begin(&q, msb, 102, 0); add32(&q, 0x30); add32(&q, 33); add32(&q, 1);
  expect_error(run(c, &q), msb, X11_BAD_VALUE, ++seq, 33, 102); ++errors;
  begin(&q, msb, 102, 0); add32(&q, 0x40); add32(&q, 10);
  expect_error(run(c, &q), msb, X11_BAD_MATCH, ++seq, 0, 102); ++errors;
  begin(&q, msb, 102, 0); add32(&q, 0x10); add32(&q, 1);
  expect_error(run(c, &q), msb, X11_BAD_MATCH, ++seq, 0, 102); ++errors;
  begin(&q, msb, 102, 0); add32(&q, 0x100); add32(&q, 0);
  expect_error(run(c, &q), msb, X11_BAD_VALUE, ++seq, 0x100, 102); ++errors;
  begin(&q, msb, 102, 0); add32(&q, 0x03); add32(&q, 0);
  expect_error(run(c, &q), msb, X11_BAD_LENGTH, ++seq, 0, 102); ++errors;
  /* pointer control: defaults 2/1/4; set 3/2 and 7; -1 restores; errors */
  begin(&q, msb, 106, 0);
  expect_reply(run(c, &q), msb, ++seq, 0);
  CHECK(g16(out + 8, msb) == 2 && g16(out + 10, msb) == 1 && g16(out + 12, msb) == 4);
  begin(&q, msb, 105, 0); add16pair(&q, 3, 2); add16pair(&q, 7, 0x0101);
  CHECK(run(c, &q) == 0); ++seq;
  begin(&q, msb, 106, 0);
  expect_reply(run(c, &q), msb, ++seq, 0);
  CHECK(g16(out + 8, msb) == 3 && g16(out + 10, msb) == 2 && g16(out + 12, msb) == 7);
  begin(&q, msb, 105, 0); add16pair(&q, 0xFFFF, 0xFFFF); add16pair(&q, 0, 0); q.b[10] = 1;  /* accel only */
  CHECK(run(c, &q) == 0); ++seq;
  begin(&q, msb, 106, 0);
  expect_reply(run(c, &q), msb, ++seq, 0);
  CHECK(g16(out + 8, msb) == 2 && g16(out + 10, msb) == 1 && g16(out + 12, msb) == 7);
  begin(&q, msb, 105, 0); add16pair(&q, 1, 0); add16pair(&q, 0, 0); q.b[10] = 1;  /* denominator 0 */
  expect_error(run(c, &q), msb, X11_BAD_VALUE, ++seq, 0, 105); ++errors;
  begin(&q, msb, 105, 0); add16pair(&q, 1, 1); add16pair(&q, 0, 0); q.b[10] = 2;  /* do-accel 2 */
  expect_error(run(c, &q), msb, X11_BAD_VALUE, ++seq, 2, 105); ++errors;
  /* GetPointerMapping: 1 2 3 4 5 */
  begin(&q, msb, 117, 0);
  expect_reply(run(c, &q), msb, ++seq, 8);
  CHECK(out[1] == 5 && out[32] == 1 && out[36] == 5);
  /* font path: "built-ins"; setting it (or nothing) is fine, another path is BadValue */
  begin(&q, msb, 52, 0);
  expect_reply(run(c, &q), msb, ++seq, 12);
  CHECK(g16(out + 8, msb) == 1 && out[32] == 9 && !memcmp(out + 33, "built-ins", 9));
  begin(&q, msb, 51, 0); add16pair(&q, 1, 0); q.b[q.n] = 9; memcpy(q.b + q.n + 1, "built-ins", 9); q.n += 12;
  CHECK(run(c, &q) == 0); ++seq;
  begin(&q, msb, 51, 0); add16pair(&q, 0, 0);
  CHECK(run(c, &q) == 0); ++seq;
  begin(&q, msb, 51, 0); add16pair(&q, 1, 0); q.b[q.n] = 4; memcpy(q.b + q.n + 1, "/tmp", 4); q.n += 8;
  expect_error(run(c, &q), msb, X11_BAD_VALUE, ++seq, 0, 51); ++errors;
  /* a second string whose length (5) runs past the request */
  begin(&q, msb, 51, 0); add16pair(&q, 2, 0); q.b[q.n] = 9; memcpy(q.b + q.n + 1, "built-ins", 9); q.n += 12;
  q.b[18] = 5;
  expect_error(run(c, &q), msb, X11_BAD_LENGTH, ++seq, 0, 51); ++errors;
  /* UnmapSubwindows: both mapped children are unmapped, the parent stays mapped */
  uint32_t base = srv.clients[c].id_base, par = base + 1;
  for (uint32_t i = 0; i < 3; ++i) {
    begin(&q, msb, 1, 0); add32(&q, par + i); add32(&q, i ? par : X11_ID_ROOT_WINDOW); add16pair(&q, 0, 0);
    add16pair(&q, 20, 20); add16pair(&q, 0, 1); add32(&q, 0); add32(&q, 0);
    CHECK(run(c, &q) == 0); ++seq;
    begin(&q, msb, 8, 0); add32(&q, par + i);
    CHECK(run(c, &q) == 0); ++seq;
  }
  begin(&q, msb, 11, 0); add32(&q, par);
  CHECK(run(c, &q) == 0); ++seq;
  for (uint32_t i = 0; i < 3; ++i) {
    begin(&q, msb, 3, 0); add32(&q, par + i);
    expect_reply(run(c, &q), msb, ++seq, 12);
    CHECK(out[26] == (i ? 0 : 2));                                   /* Unmapped / Viewable */
  }
  begin(&q, msb, 11, 0); add32(&q, base + 50);
  expect_error(run(c, &q), msb, X11_BAD_WINDOW, ++seq, base + 50, 11); ++errors;
  CHECK(srv.clients[c].errors == (uint32_t)errors);
  x11_server_free(&srv);
}

/* InternAtom (16) / GetAtomName (17): names of any length, including empty ones and ones with
 * NUL bytes; only-if-exists; one identity for all clients; BadValue, BadLength, BadAtom;
 * BadAlloc when the name pool or the table is full, while existing atoms keep working. */
static uint8_t big[16384];
static size_t intern_raw(int c, int msb, const uint8_t *name, size_t n, int only) {
  size_t len = 8 + ((n + 3) & ~(size_t)3);
  memset(big, 0, len);
  big[0] = 16, big[1] = (uint8_t)only;
  p16(big + 2, (uint16_t)(len / 4), msb);
  p16(big + 4, (uint16_t)n, msb);
  memcpy(big + 8, name, n);
  int close_after;
  return x11_client_request(&srv, c, big, len, out, sizeof(out), &close_after);
}
static uint32_t intern_ok(int c, int msb, const uint8_t *name, size_t n, int only) {
  size_t r = intern_raw(c, msb, name, n, only);
  CHECK(r == 32 && out[0] == 1);
  return g32(out + 8, msb);
}
static void test_atoms(int msb) {
  x11_server_config cfg;
  x11_default_config(&cfg);
  x11_server_init(&srv, &cfg);
  int c = connect_client(msb), d = connect_client(!msb);
  req q;
  /* empty name: an atom like any other */
  CHECK(intern_ok(c, msb, (const uint8_t *)"", 0, 1) == 0);
  uint32_t empty = intern_ok(c, msb, (const uint8_t *)"", 0, 0);
  CHECK(empty > 68 && intern_ok(c, msb, (const uint8_t *)"", 0, 1) == empty);
  begin(&q, msb, 17, 0); add32(&q, empty);
  CHECK(run(c, &q) == 32 && out[0] == 1 && g16(out + 8, msb) == 0 && g32(out + 4, msb) == 0);
  /* NUL bytes: "a\0b" and "a" are different atoms */
  uint32_t anb = intern_ok(c, msb, (const uint8_t *)"a\0b", 3, 0), a1 = intern_ok(c, msb, (const uint8_t *)"a", 1, 0);
  CHECK(anb != a1 && anb != empty);
  begin(&q, msb, 17, 0); add32(&q, anb);
  CHECK(run(c, &q) == 36 && g16(out + 8, msb) == 3 && !memcmp(out + 32, "a\0b", 3));
  /* predefined names, duplicates, another client in the other byte order */
  CHECK(intern_ok(c, msb, (const uint8_t *)"WM_NAME", 7, 1) == 39);
  CHECK(intern_ok(d, !msb, (const uint8_t *)"a", 1, 1) == a1);
  /* the longest name a request can carry: 16376 bytes */
  static uint8_t name[16376];
  for (size_t i = 0; i < sizeof(name); ++i) name[i] = (uint8_t)('A' + i % 26);
  uint32_t longest = intern_ok(c, msb, name, sizeof(name) - 1000, 0);
  begin(&q, msb, 17, 0); add32(&q, longest);
  CHECK(run(c, &q) == 32 + 15376 && g16(out + 8, msb) == 15376 && !memcmp(out + 32, name, 15376));
  /* the pool is nearly full: a 1000-byte name does not fit (BadAlloc), short ones still do */
  CHECK(intern_raw(c, msb, name + 1, 1000, 0) == 32 && out[0] == 0 && out[1] == X11_BAD_ALLOC);
  CHECK(intern_raw(c, msb, name + 1, 1000, 1) == 32 && out[0] == 1 && g32(out + 8, msb) == 0);
  CHECK(intern_ok(c, msb, (const uint8_t *)"short", 5, 0) > 0);
  CHECK(intern_ok(c, msb, name, sizeof(name) - 1000, 0) == longest);  /* existing: same atom */
  /* errors: only-if-exists 2, inconsistent length, GetAtomName of None, unknown, wrong length */
  CHECK(intern_raw(c, msb, (const uint8_t *)"x", 1, 2) == 32 && out[0] == 0 && out[1] == X11_BAD_VALUE);
  begin(&q, msb, 16, 0); add16pair(&q, 9, 0); addstr(&q, "abcd");
  CHECK(run(c, &q) == 32 && out[0] == 0 && out[1] == X11_BAD_LENGTH);
  begin(&q, msb, 17, 0); add32(&q, 0);
  CHECK(run(c, &q) == 32 && out[0] == 0 && out[1] == X11_BAD_ATOM);
  begin(&q, msb, 17, 0); add32(&q, 69 + 4000);
  CHECK(run(c, &q) == 32 && out[0] == 0 && out[1] == X11_BAD_ATOM && g32(out + 4, msb) == 69 + 4000);
  begin(&q, msb, 17, 0); add32(&q, empty); add32(&q, 0);
  CHECK(run(c, &q) == 32 && out[0] == 0 && out[1] == X11_BAD_LENGTH);
  x11_server_free(&srv);
  /* the table: 512 run-time atoms, then BadAlloc; all of them still resolve */
  x11_server_init(&srv, &cfg);
  c = connect_client(msb);
  char nm[16];
  uint32_t first = 0, last = 0;
  for (int i = 0; i < X11_MAX_DYNAMIC_ATOMS; ++i) {
    int n = snprintf(nm, sizeof(nm), "A%d", i);
    last = intern_ok(c, msb, (const uint8_t *)nm, (size_t)n, 0);
    if (!i) first = last;
  }
  CHECK(last == first + X11_MAX_DYNAMIC_ATOMS - 1);
  CHECK(intern_raw(c, msb, (const uint8_t *)"one more", 8, 0) == 32 && out[0] == 0 && out[1] == X11_BAD_ALLOC);
  CHECK(intern_ok(c, msb, (const uint8_t *)"A511", 4, 0) == last && intern_ok(c, msb, (const uint8_t *)"A0", 2, 1) == first);
  x11_server_free(&srv);
}

/* CreateCursor (93): source and mask of depth 1 and the same size, the hotspot inside the
 * source; otherwise BadMatch (a source or mask that is no pixmap: BadPixmap). */
static size_t create_pixmap_cursor(int c, int msb, uint32_t cid, uint32_t src, uint32_t mask, uint16_t hx, uint16_t hy) {
  req q;
  begin(&q, msb, 93, 0); add32(&q, cid); add32(&q, src); add32(&q, mask);
  add16pair(&q, 0, 0); add16pair(&q, 0, 0xFFFF); add16pair(&q, 0xFFFF, 0xFFFF); add16pair(&q, hx, hy);
  return run(c, &q);
}
static void test_create_cursor(int msb) {
  x11_server_config cfg;
  x11_default_config(&cfg);
  x11_server_init(&srv, &cfg);
  int c = connect_client(msb);
  uint32_t base = srv.clients[c].id_base, s16 = base + 1, m16 = base + 2, m8 = base + 3, deep = base + 4;
  req q;
  uint32_t pm[4][3] = {{s16, 16, 1}, {m16, 16, 1}, {m8, 8, 1}, {deep, 16, 16}};
  for (int i = 0; i < 4; ++i) {
    begin(&q, msb, 53, (uint8_t)pm[i][2]); add32(&q, pm[i][0]); add32(&q, X11_ID_ROOT_WINDOW);
    add16pair(&q, (uint16_t)pm[i][1], (uint16_t)pm[i][1]);
    CHECK(run(c, &q) == 0);
  }
  CHECK(create_pixmap_cursor(c, msb, base + 10, s16, m16, 15, 15) == 0);       /* fine */
  CHECK(create_pixmap_cursor(c, msb, base + 11, s16, 0, 0, 0) == 0);           /* no mask */
  CHECK(create_pixmap_cursor(c, msb, base + 12, deep, 0, 0, 0) == 32 && out[1] == X11_BAD_MATCH);
  CHECK(create_pixmap_cursor(c, msb, base + 12, s16, deep, 0, 0) == 32 && out[1] == X11_BAD_MATCH);
  CHECK(create_pixmap_cursor(c, msb, base + 12, s16, m8, 0, 0) == 32 && out[1] == X11_BAD_MATCH);
  /* the hotspot may be at the width/height itself, as X.Org (Xvfb) accepts: GDK's blank cursor is a
   * 1x1 bitmap with its hotspot at 1,1; one past that is BadMatch */
  CHECK(create_pixmap_cursor(c, msb, base + 13, s16, m16, 16, 16) == 0);
  CHECK(create_pixmap_cursor(c, msb, base + 12, s16, m16, 17, 0) == 32 && out[1] == X11_BAD_MATCH);
  CHECK(create_pixmap_cursor(c, msb, base + 12, s16, m16, 0, 17) == 32 && out[1] == X11_BAD_MATCH);
  begin(&q, msb, 53, 1); add32(&q, base + 5); add32(&q, X11_ID_ROOT_WINDOW); add16pair(&q, 1, 1);
  CHECK(run(c, &q) == 0);
  CHECK(create_pixmap_cursor(c, msb, base + 14, base + 5, base + 5, 1, 1) == 0);
  CHECK(create_pixmap_cursor(c, msb, base + 12, base + 5, base + 5, 2, 2) == 32 && out[1] == X11_BAD_MATCH);
  CHECK(create_pixmap_cursor(c, msb, base + 12, base + 40, 0, 0, 0) == 32 && out[1] == X11_BAD_PIXMAP);
  CHECK(create_pixmap_cursor(c, msb, base + 12, s16, base + 40, 0, 0) == 32 && out[1] == X11_BAD_PIXMAP);
  CHECK(create_pixmap_cursor(c, msb, base + 10, s16, 0, 0, 0) == 32 && out[1] == X11_BAD_IDCHOICE);
  x11_server_free(&srv);
}

/* backing-planes and backing-pixel: kept and reported by GetWindowAttributes (defaults: all
 * planes, pixel 0), at CreateWindow and ChangeWindowAttributes. */
static void test_backing_attributes(int msb) {
  x11_server_config cfg;
  x11_default_config(&cfg);
  x11_server_init(&srv, &cfg);
  int c = connect_client(msb);
  uint32_t a = srv.clients[c].id_base + 1, b = a + 1;
  req q;
  begin(&q, msb, 1, 0); add32(&q, a); add32(&q, X11_ID_ROOT_WINDOW); add16pair(&q, 0, 0); add16pair(&q, 5, 5);
  add16pair(&q, 0, 1); add32(&q, 0); add32(&q, 0);
  CHECK(run(c, &q) == 0);
  begin(&q, msb, 1, 0); add32(&q, b); add32(&q, X11_ID_ROOT_WINDOW); add16pair(&q, 0, 0); add16pair(&q, 5, 5);
  add16pair(&q, 0, 1); add32(&q, 0); add32(&q, 0x180); add32(&q, 0x00FF00FFu); add32(&q, 0x1234);
  CHECK(run(c, &q) == 0);
  begin(&q, msb, 3, 0); add32(&q, a);
  CHECK(run(c, &q) == 44 && g32(out + 16, msb) == 0xFFFFFFFFu && g32(out + 20, msb) == 0);
  begin(&q, msb, 3, 0); add32(&q, b);
  CHECK(run(c, &q) == 44 && g32(out + 16, msb) == 0x00FF00FFu && g32(out + 20, msb) == 0x1234);
  begin(&q, msb, 2, 0); add32(&q, a); add32(&q, 0x100); add32(&q, 7);
  CHECK(run(c, &q) == 0);
  begin(&q, msb, 3, 0); add32(&q, a);
  CHECK(run(c, &q) == 44 && g32(out + 16, msb) == 0xFFFFFFFFu && g32(out + 20, msb) == 7);
  begin(&q, msb, 3, 0); add32(&q, X11_ID_ROOT_WINDOW);
  CHECK(run(c, &q) == 44 && g32(out + 16, msb) == 0xFFFFFFFFu);
  x11_server_free(&srv);
}

/* Server reset when the last client leaves: atoms (not the predefined ones), root properties,
 * keyboard/pointer control and the focus go back; nothing resets while another client is
 * connected, or when a connection that never finished setup goes. */
static void test_server_reset(int msb) {
  x11_server_config cfg;
  x11_default_config(&cfg);
  /* by default (the panel) nothing resets */
  x11_server_init(&srv, &cfg);
  int k = connect_client(msb);
  uint32_t kept = intern_ok(k, msb, (const uint8_t *)"STAYS", 5, 0);
  x11_client_close(&srv, k);
  k = connect_client(msb);
  CHECK(intern_ok(k, msb, (const uint8_t *)"STAYS", 5, 1) == kept);
  x11_server_free(&srv);
  cfg.reset_on_last_close = 1;
  x11_server_init(&srv, &cfg);
  int a = connect_client(msb), b = connect_client(msb);
  req q;
  uint32_t atom = intern_ok(a, msb, (const uint8_t *)"RESET_ME", 8, 0);
  begin(&q, msb, 18, 0); add32(&q, X11_ID_ROOT_WINDOW); add32(&q, atom); add32(&q, 31); q.b[q.n] = 8; q.n += 4;
  add32(&q, 4); addstr(&q, "abcd");
  CHECK(run(a, &q) == 0);                                            /* ChangeProperty on the root */
  begin(&q, msb, 104 - 2, 0); add32(&q, 0x02); add32(&q, 90);
  CHECK(run(a, &q) == 0);                                            /* bell-percent 90 */
  begin(&q, msb, 42, 0); add32(&q, 0); add32(&q, 0);
  CHECK(run(a, &q) == 0);                                            /* focus None */
  x11_client_close(&srv, a);                                         /* b is still there: no reset */
  CHECK(intern_ok(b, msb, (const uint8_t *)"RESET_ME", 8, 1) == atom && srv.root->props && srv.focus == 0);
  int pending = x11_client_open(&srv);                               /* never finishes setup */
  x11_client_close(&srv, b);
  CHECK(srv.atom_count == 1 || srv.atom_count == 0);                 /* reset happened at b */
  x11_client_close(&srv, pending);
  int c = connect_client(msb);
  CHECK(intern_ok(c, msb, (const uint8_t *)"RESET_ME", 8, 1) == 0);  /* gone */
  CHECK(intern_ok(c, msb, (const uint8_t *)"WM_NAME", 7, 1) == 39);  /* predefined stays */
  CHECK(!srv.root->props && srv.focus == 1);
  begin(&q, msb, 103, 0);
  CHECK(run(c, &q) == 52 && out[13] == 50);                         /* bell-percent default */
  /* a pending connection closing alone does not reset */
  uint32_t again = intern_ok(c, msb, (const uint8_t *)"KEEP", 4, 0);
  int p2 = x11_client_open(&srv);
  x11_client_close(&srv, p2);
  CHECK(intern_ok(c, msb, (const uint8_t *)"KEEP", 4, 1) == again);
  x11_server_free(&srv);
}

/* QueryBestSize (97): Tile and Stipple on an InputOnly window are BadMatch; Cursor is fine. */
static void test_query_best_size_input_only(int msb) {
  x11_server_config cfg;
  x11_default_config(&cfg);
  x11_server_init(&srv, &cfg);
  int c = connect_client(msb);
  uint32_t io = srv.clients[c].id_base + 1;
  req q;
  begin(&q, msb, 1, 0); add32(&q, io); add32(&q, X11_ID_ROOT_WINDOW); add16pair(&q, 0, 0); add16pair(&q, 5, 5);
  add16pair(&q, 0, 2); add32(&q, 0); add32(&q, 0);  /* InputOnly */
  CHECK(run(c, &q) == 0);
  for (uint8_t cls = 0; cls < 3; ++cls) {
    begin(&q, msb, 97, cls); add32(&q, io); add16pair(&q, 8, 8);
    size_t n = run(c, &q);
    if (cls == 0) CHECK(n == 32 && out[0] == 1);
    else CHECK(n == 32 && out[0] == 0 && out[1] == X11_BAD_MATCH);
  }
  x11_server_free(&srv);
}

/* ClearArea (61): exposures is a BOOL, other values are BadValue (bad value: the flag), checked
 * after BadWindow and the InputOnly BadMatch (as X.Org); 0 and 1 are fine. */
static void test_clear_area_exposures(int msb) {
  x11_server_config cfg;
  x11_default_config(&cfg);
  x11_server_init(&srv, &cfg);
  int c = connect_client(msb);
  uint32_t w = srv.clients[c].id_base + 1, io = w + 1;
  req q;
  begin(&q, msb, 1, 0); add32(&q, w); add32(&q, X11_ID_ROOT_WINDOW); add16pair(&q, 0, 0); add16pair(&q, 20, 20);
  add16pair(&q, 0, 1); add32(&q, 0); add32(&q, 0);
  CHECK(run(c, &q) == 0);
  begin(&q, msb, 1, 0); add32(&q, io); add32(&q, X11_ID_ROOT_WINDOW); add16pair(&q, 0, 0); add16pair(&q, 20, 20);
  add16pair(&q, 0, 2); add32(&q, 0); add32(&q, 0);
  CHECK(run(c, &q) == 0);
  for (unsigned e = 0; e < 4; ++e) {
    uint8_t flag = e < 2 ? (uint8_t)e : e == 2 ? 2 : 0xFF;
    begin(&q, msb, 61, flag); add32(&q, w); add16pair(&q, 0, 0); add16pair(&q, 5, 5);
    size_t n = run(c, &q);
    if (flag <= 1) CHECK(n == 0);
    else CHECK(n == 32 && out[0] == 0 && out[1] == X11_BAD_VALUE && g32(out + 4, msb) == flag && out[10] == 61);
  }
  begin(&q, msb, 61, 2); add32(&q, io); add16pair(&q, 0, 0); add16pair(&q, 5, 5);
  CHECK(run(c, &q) == 32 && out[1] == X11_BAD_MATCH);   /* InputOnly first */
  begin(&q, msb, 61, 2); add32(&q, w + 50); add16pair(&q, 0, 0); add16pair(&q, 5, 5);
  CHECK(run(c, &q) == 32 && out[1] == X11_BAD_WINDOW);  /* the window first */
  x11_server_free(&srv);
}

int main(void) {
  test_clear_area_exposures(0);
  test_clear_area_exposures(1);
  test_query_best_size_input_only(0);
  test_query_best_size_input_only(1);
  test_server_reset(0);
  test_server_reset(1);
  test_backing_attributes(0);
  test_backing_attributes(1);
  test_create_cursor(0);
  test_create_cursor(1);
  test_atoms(0);
  test_atoms(1);
  test_controls(0);
  test_controls(1);
  test_undefined_mask_bits(0);
  test_undefined_mask_bits(1);
  test_screen_saver(0);
  test_screen_saver(1);
  test_xdpyinfo_sequence(0);
  test_xdpyinfo_sequence(1);
  test_errors(0);
  test_errors(1);
  test_recolor_cursor(0);
  test_recolor_cursor(1);
  test_framing();
  test_clients_and_atoms();
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  puts("x11 request tests OK");
  return 0;
}
