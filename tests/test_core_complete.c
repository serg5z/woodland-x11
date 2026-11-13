/* Unit tests for the last core requests (core/x11_server.c): ChangeKeyboardMapping,
 * SetModifierMapping and their Get requests, ListHosts / ChangeHosts / SetAccessControl, and
 * SetCloseDownMode with KillClient and the server reset (retained clients). Both byte orders;
 * the expected answers are Xvfb's (tools/xts/core_diff.py), except where the security model
 * differs (host access: BadAccess). Run with ASan/UBSan (Makefile). */
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

#define MAXEV 256
static x11_server srv;
static uint8_t out[X11_REPLY_MAX];
static uint8_t events[MAXEV][32];
static int ev_client[MAXEV], nev;
static int MSB[X11_SERVER_CLIENTS];

static void deliver(void *ctx, int client, const uint8_t *data, size_t len) {
  (void)ctx;
  if (nev < MAXEV && len == 32) {
    memcpy(events[nev], data, 32);
    ev_client[nev++] = client;
  }
}

static void start(int reset, const uint8_t *cookie) {
  x11_server_config cfg;
  x11_default_config(&cfg);
  cfg.reset_on_last_close = reset;
  cfg.cookie = cookie;
  x11_platform plat = {0};
  plat.deliver = deliver;
  CHECK(x11_server_init_platform(&srv, &cfg, &plat) == 0);
  nev = 0;
}

static int open_client(int msb) {
  int c = x11_client_open(&srv);
  uint8_t req[48] = {(uint8_t)(msb ? 'B' : 'l'), 0};
  x_wr16(req + 2, 11, msb);
  size_t n = 12;
  if (srv.cfg.cookie) {  /* MIT-MAGIC-COOKIE-1 */
    x_wr16(req + 6, 18, msb), x_wr16(req + 8, 16, msb);
    memcpy(req + 12, "MIT-MAGIC-COOKIE-1\0\0", 20);
    memcpy(req + 32, srv.cfg.cookie, 16);
    n = 48;
  }
  x11_setup_result r;
  uint8_t rep[512];
  x11_client_setup(&srv, c, req, n, rep, sizeof(rep), &r);
  CHECK(r.status == X11_SETUP_SUCCESS);
  MSB[c] = msb;
  return c;
}

typedef struct {
  uint8_t b[1100];
  size_t n;
  int msb;
} rq;

static void beg(rq *q, int c, uint8_t op, uint8_t d1) {
  memset(q, 0, sizeof(*q));
  q->msb = MSB[c], q->b[0] = op, q->b[1] = d1, q->n = 4;
}
static void w32(rq *q, uint32_t v) { x_wr32(q->b + q->n, v, q->msb); q->n += 4; }
static void w8x4(rq *q, uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  q->b[q->n] = a, q->b[q->n + 1] = b, q->b[q->n + 2] = c, q->b[q->n + 3] = d;
  q->n += 4;
}
/* Sends q; `units` (if nonzero) overrides the length field. Returns the answer's size. */
static size_t send_len(int c, rq *q, size_t units) {
  if (!units) units = q->n / 4;
  x_wr16(q->b + 2, (uint16_t)units, q->msb);
  int close_after = 0;
  size_t n = x11_client_request(&srv, c, q->b, units * 4, out, sizeof(out), &close_after);
  CHECK(!close_after);
  return n;
}
static size_t send_q(int c, rq *q) { return send_len(c, q, 0); }
/* The error code of the answer (0: none, 1: a reply). */
static int answer(size_t n) { return n == 0 ? 0 : out[0] == 0 ? 100 + out[1] : 1; }
#define ERR(code) (100 + (code))
static uint32_t bad_value(int c) { return x_rd32(out + 4, MSB[c]); }

static int mapping_notifies(int client, int request, int first, int count) {
  int n = 0;
  for (int i = 0; i < nev; ++i)
    n += ev_client[i] == client && (events[i][0] & 0x7F) == 34 && events[i][4] == request &&
         (first < 0 || (events[i][5] == first && events[i][6] == count));
  return n;
}

/* ---- keyboard map ------------------------------------------------------------------------ */

static size_t change_kb(int c, uint8_t count, uint8_t first, uint8_t per, const uint32_t *syms, size_t nsyms) {
  rq q;
  beg(&q, c, 100, count);
  w8x4(&q, first, per, 0, 0);
  for (size_t i = 0; i < nsyms; ++i) w32(&q, syms[i]);
  return send_q(c, &q);
}
static size_t get_kb(int c, uint8_t first, uint8_t count) {
  rq q;
  beg(&q, c, 101, 0);
  w8x4(&q, first, count, 0, 0);
  return send_q(c, &q);
}

static void test_keyboard_map(int msb) {
  start(0, NULL);
  int a = open_client(msb), b = open_client(!msb);
  CHECK(answer(get_kb(a, 200, 2)) == 1 && out[1] == 2 && x_rd32(out + 32, msb) == 0);  /* unassigned: NoSymbol */
  CHECK(!srv.kb_map);
  const uint32_t ab[4] = {0x61, 0x41, 0x62, 0x42};
  nev = 0;
  CHECK(change_kb(a, 2, 200, 2, ab, 4) == 0);
  CHECK(mapping_notifies(a, 1, 200, 2) == 1 && mapping_notifies(b, 1, 200, 2) == 1);  /* every client */
  size_t n = get_kb(b, 199, 3);
  CHECK(answer(n) == 1 && out[1] == 2 && n == 32 + 3 * 2 * 4);   /* widened to 2 */
  CHECK(x_rd32(out + 32, !msb) == 0 && x_rd32(out + 36, !msb) == 0);               /* 199 untouched */
  CHECK(x_rd32(out + 40, !msb) == 0x61 && x_rd32(out + 44, !msb) == 0x41);
  CHECK(x_rd32(out + 48, !msb) == 0x62 && x_rd32(out + 52, !msb) == 0x42);
  const uint32_t c1 = 0x63;
  CHECK(change_kb(a, 1, 255, 1, &c1, 1) == 0);                  /* narrower: NoSymbol-padded */
  CHECK(answer(get_kb(a, 255, 1)) == 1 && out[1] == 2 && x_rd32(out + 32, msb) == 0x63 && x_rd32(out + 36, msb) == 0);
  /* X.Org's checks and order */
  nev = 0;
  CHECK(change_kb(a, 0, 8, 1, NULL, 0) == 0 && nev == 0);       /* no keycodes: no MappingNotify */
  CHECK(answer(change_kb(a, 1, 7, 1, &c1, 1)) == ERR(X11_BAD_VALUE) && bad_value(a) == 7);
  const uint32_t two[2] = {1, 2};
  CHECK(answer(change_kb(a, 2, 255, 1, two, 2)) == ERR(X11_BAD_VALUE) && bad_value(a) == 1);  /* past 255 */
  CHECK(answer(change_kb(a, 1, 200, 0, NULL, 0)) == ERR(X11_BAD_VALUE) && bad_value(a) == 0);
  const uint32_t three[3] = {1, 2, 3};
  CHECK(answer(change_kb(a, 2, 200, 2, three, 3)) == ERR(X11_BAD_LENGTH));
  CHECK(answer(change_kb(a, 1, 7, 1, two, 2)) == ERR(X11_BAD_LENGTH));  /* length before value */
  rq q;
  beg(&q, a, 100, 1);
  CHECK(answer(send_q(a, &q)) == ERR(X11_BAD_LENGTH));          /* header only */
  uint32_t wide[9] = {0};
  CHECK(answer(change_kb(a, 1, 10, 9, wide, 9)) == ERR(X11_BAD_ALLOC));  /* more than X11_KEYSYMS_MAX */
  CHECK(answer(get_kb(a, 200, 1)) == 1 && x_rd32(out + 32, msb) == 0x61);  /* unchanged by the errors */
  x11_server_free(&srv);
}

/* The default core keymap (a US layout in X.Org's keycode numbering, as Xvfb's): mwm resolves its
 * window-menu accelerators (Alt<Key>F5 ...) with XKeysymToKeycode; with no keysyms it dropped every
 * item and posted an empty 16x16 menu. A first change keeps the other defaults. */
static void test_default_keymap(int msb) {
  start(0, NULL);
  int a = open_client(msb);
  struct { int kc; uint32_t s0, s1; } want[] = {
      {9, 0xff1b, 0}, {10, '1', '!'}, {24, 'q', 'Q'}, {36, 0xff0d, 0}, {37, 0xffe3, 0}, {38, 'a', 'A'},
      {50, 0xffe1, 0}, {64, 0xffe9, 0xffe7}, {65, ' ', 0}, {67, 0xffbe, 0}, {71, 0xffc2, 0}, {76, 0xffc7, 0},
      {95, 0xffc8, 0}, {96, 0xffc9, 0}, {111, 0xff52, 0}, {116, 0xff54, 0}, {119, 0xffff, 0}, {200, 0, 0}};
  for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); ++i) {
    CHECK(answer(get_kb(a, (uint8_t)want[i].kc, 1)) == 1 && out[1] == 2);
    CHECK(x_rd32(out + 32, msb) == want[i].s0 && x_rd32(out + 36, msb) == want[i].s1);
  }
  const uint32_t one = 0x1008ff13;  /* XF86AudioRaiseVolume on keycode 123 */
  CHECK(change_kb(a, 1, 123, 1, &one, 1) == 0 && srv.kb_map);
  CHECK(answer(get_kb(a, 71, 1)) == 1 && out[1] == 2 && x_rd32(out + 32, msb) == 0xffc2);  /* F5 kept */
  CHECK(answer(get_kb(a, 123, 1)) == 1 && x_rd32(out + 32, msb) == one && x_rd32(out + 36, msb) == 0);
  x11_server_free(&srv);
}

/* ---- modifier map ------------------------------------------------------------------------ */

static size_t set_mods(int c, uint8_t per, const uint8_t *codes, size_t n) {
  rq q;
  beg(&q, c, 118, per);
  if (n) memcpy(q.b + 4, codes, n);
  q.n = 4 + ((n + 3) & ~(size_t)3);
  return send_q(c, &q);
}

static void test_modifier_map(int msb) {
  start(0, NULL);
  int a = open_client(msb), b = open_client(!msb);
  rq q;
  beg(&q, a, 119, 0);
  /* the default: X.Org's for the default keymap (Xvfb's, without its keycodes 205-207), 2 per modifier */
  const uint8_t def[16] = {50, 62, 66, 0, 37, 105, 64, 108, 77, 0, 0, 0, 133, 134, 0, 0};
  CHECK(send_q(a, &q) == 48 && out[1] == 2 && !memcmp(out + 32, def, 16));
  uint8_t m[16] = {0};
  m[0] = 201, m[1] = 200, m[4] = 202;  /* Shift: 201 200; Control: 202 */
  nev = 0;
  size_t n = set_mods(a, 2, m, 16);
  CHECK(n == 32 && out[0] == 1 && out[1] == 0 && x_rd32(out + 4, msb) == 0);  /* Success */
  CHECK(mapping_notifies(a, 0, -1, 0) == 1 && mapping_notifies(b, 0, -1, 0) == 1);
  beg(&q, b, 119, 0);
  n = send_q(b, &q);
  const uint8_t want[16] = {200, 201, 0, 0, 202, 0};  /* rebuilt as X.Org: sorted, 2 per modifier */
  CHECK(n == 48 && out[1] == 2 && !memcmp(out + 32, want, 16));
  /* conflicts: a keycode twice, in two modifiers or in one; out of range; lengths */
  uint8_t d[8] = {203, 203};
  CHECK(answer(set_mods(a, 1, d, 8)) == ERR(X11_BAD_VALUE) && bad_value(a) == 203);
  uint8_t d2[16] = {204, 204};
  CHECK(answer(set_mods(a, 2, d2, 16)) == ERR(X11_BAD_VALUE));
  uint8_t r7[8] = {200, 7};
  CHECK(answer(set_mods(a, 1, r7, 8)) == ERR(X11_BAD_VALUE) && bad_value(a) == 7);
  uint8_t r7d[8] = {7, 205, 205};  /* X.Org checks duplicates before the range */
  CHECK(answer(set_mods(a, 1, r7d, 8)) == ERR(X11_BAD_VALUE) && bad_value(a) == 205);
  CHECK(answer(set_mods(a, 1, d, 4)) == ERR(X11_BAD_LENGTH));
  CHECK(answer(set_mods(a, 1, (const uint8_t *)"\0\0\0\0\0\0\0\0\0\0\0\0", 12)) == ERR(X11_BAD_LENGTH));
  beg(&q, b, 119, 0);
  CHECK(send_q(b, &q) == 48 && !memcmp(out + 32, want, 16));     /* the errors changed nothing */
  nev = 0;
  CHECK(set_mods(a, 0, NULL, 0) == 32 && out[1] == 0 && mapping_notifies(b, 0, -1, 0) == 1);
  beg(&q, a, 119, 0);
  CHECK(send_q(a, &q) == 40 && out[1] == 1 && !memcmp(out + 32, "\0\0\0\0\0\0\0\0", 8));  /* empty: 1 per modifier */
  x11_server_free(&srv);
}

/* ---- access control ------------------------------------------------------------------------ */

static const uint8_t COOKIE[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

static size_t change_hosts(int c, uint8_t mode, uint8_t family, const uint8_t *addr, uint16_t len, size_t units) {
  rq q;
  beg(&q, c, 109, mode);
  q.b[4] = family;
  x_wr16(q.b + 6, len, q.msb);
  memcpy(q.b + 8, addr, len);
  q.n = 8 + ((len + 3u) & ~3u);
  return send_len(c, &q, units);
}

static void test_access_control(int msb) {
  for (int with_cookie = 0; with_cookie < 2; ++with_cookie) {
    start(0, with_cookie ? COOKIE : NULL);
    int a = open_client(msb);
    rq q;
    beg(&q, a, 110, 0);
    CHECK(send_q(a, &q) == 32 && out[0] == 1 && out[1] == with_cookie && x_rd16(out + 8, msb) == 0);
    w32(&q, 0);
    CHECK(answer(send_q(a, &q)) == ERR(X11_BAD_LENGTH));
    const uint8_t ip[4] = {10, 9, 8, 7};
    CHECK(answer(change_hosts(a, 0, 0, ip, 4, 0)) == ERR(X11_BAD_ACCESS));  /* Insert */
    CHECK(answer(change_hosts(a, 1, 0, ip, 4, 0)) == ERR(X11_BAD_ACCESS));  /* Delete */
    CHECK(answer(change_hosts(a, 0, 0, ip, 3, 0)) == ERR(X11_BAD_ACCESS));  /* family checks come after access */
    CHECK(answer(change_hosts(a, 2, 0, ip, 4, 0)) == ERR(X11_BAD_VALUE) && bad_value(a) == 2);
    CHECK(answer(change_hosts(a, 0, 0, ip, 4, 2)) == ERR(X11_BAD_LENGTH));  /* address past the request */
    CHECK(answer(change_hosts(a, 2, 0, ip, 4, 2)) == ERR(X11_BAD_LENGTH));  /* length before mode */
    for (uint8_t mode = 0; mode < 2; ++mode) {
      beg(&q, a, 111, mode);
      CHECK(answer(send_q(a, &q)) == ERR(X11_BAD_ACCESS));
    }
    beg(&q, a, 111, 2);
    CHECK(answer(send_q(a, &q)) == ERR(X11_BAD_VALUE) && bad_value(a) == 2);
    w32(&q, 0);
    CHECK(answer(send_q(a, &q)) == ERR(X11_BAD_LENGTH));
    beg(&q, a, 110, 0);
    CHECK(send_q(a, &q) == 32 && out[1] == with_cookie);         /* nothing changed */
    if (with_cookie) {  /* the cookie is still required */
      int c = x11_client_open(&srv);
      uint8_t req[12] = {(uint8_t)(msb ? 'B' : 'l'), 0};
      x_wr16(req + 2, 11, msb);
      x11_setup_result r;
      uint8_t rep[512];
      x11_client_setup(&srv, c, req, 12, rep, sizeof(rep), &r);
      CHECK(r.status != X11_SETUP_SUCCESS);
      x11_client_close(&srv, c);
    }
    x11_server_free(&srv);
  }
}

/* ---- close-down modes -------------------------------------------------------------------- */

static uint32_t pixmap(int c) {
  static uint32_t serial = 0;
  uint32_t id = srv.clients[c].id_base + ++serial;
  rq q;
  beg(&q, c, 53, 16);
  w32(&q, id); w32(&q, X11_ID_ROOT_WINDOW); w32(&q, 8 | 8u << 16);
  CHECK(send_q(c, &q) == 0);
  return id;
}
static int exists(uint32_t id) { return x_find(&srv, id) != NULL; }
static size_t simple(int c, uint8_t op, uint8_t d1, uint32_t arg) {
  rq q;
  beg(&q, c, op, d1);
  w32(&q, arg);
  return send_q(c, &q);
}
static void close_down_mode(int c, uint8_t mode) {
  rq q;
  beg(&q, c, 112, mode);
  CHECK(send_q(c, &q) == 0);
}

static void test_close_down(int msb) {
  start(0, NULL);  /* the panel: no server reset */
  int a = open_client(msb), b = open_client(!msb);
  rq q;
  beg(&q, a, 112, 3);
  CHECK(answer(send_q(a, &q)) == ERR(X11_BAD_VALUE) && bad_value(a) == 3);
  w32(&q, 0);
  q.b[1] = 1;
  CHECK(answer(send_q(a, &q)) == ERR(X11_BAD_LENGTH));
  close_down_mode(a, 1);  /* RetainPermanent */
  uint32_t pa = pixmap(a), abase = srv.clients[a].id_base;
  /* a (a window manager) frames b's window and puts it in its save-set: the save-set is
   * processed only when a's resources are destroyed, not at a retained close (as X.Org) */
  uint32_t frame = abase + 50, wb = srv.clients[b].id_base + 60;
  beg(&q, a, 1, 0);  /* CreateWindow frame 0,0 50x50 */
  w32(&q, frame); w32(&q, X11_ID_ROOT_WINDOW); w32(&q, 0); w32(&q, 50 | 50u << 16); w32(&q, 1u << 16); w32(&q, 0); w32(&q, 0);
  CHECK(send_q(a, &q) == 0);
  beg(&q, b, 1, 0);  /* b's window at the root */
  w32(&q, wb); w32(&q, X11_ID_ROOT_WINDOW); w32(&q, 0); w32(&q, 10 | 10u << 16); w32(&q, 1u << 16); w32(&q, 0); w32(&q, 0);
  CHECK(send_q(b, &q) == 0);
  beg(&q, a, 7, 0);  /* ReparentWindow wb into frame at 3,3 */
  w32(&q, wb); w32(&q, frame); w32(&q, 3 | 3u << 16);
  CHECK(send_q(a, &q) == 0);
  CHECK(simple(a, 6, 0, wb) == 0);  /* ChangeSaveSet Insert */
  x11_window *w = (x11_window *)x_find(&srv, wb)->obj;
  x11_client_close(&srv, a);
  CHECK(srv.clients[a].retained == 1 && !srv.clients[a].active && srv.clients[a].id_base == abase);
  CHECK(exists(pa) && exists(frame) && w->parent->id == frame);
  int c = open_client(msb);
  CHECK(c != a && srv.clients[c].id_base != abase);  /* the retained slot's IDs are not reused */
  /* KillClient: retained resources go now (save-set first: wb back to the root, mapped); the
   * ID is then BadValue; the slot is free again */
  CHECK(simple(b, 113, 0, pa) == 0);
  CHECK(!exists(pa) && !exists(frame) && !srv.clients[a].id_base && !srv.clients[a].retained);
  CHECK(exists(wb) && w->parent == srv.root && w->mapped);
  CHECK(answer(simple(b, 113, 0, pa)) == ERR(X11_BAD_VALUE) && bad_value(b) == pa);
  /* RetainTemporary and AllTemporary */
  int t = open_client(msb), p = open_client(msb);
  close_down_mode(t, 2), close_down_mode(p, 1);
  uint32_t pt = pixmap(t), pp = pixmap(p);
  x11_client_close(&srv, t), x11_client_close(&srv, p);
  CHECK(exists(pt) && exists(pp));
  CHECK(simple(b, 113, 0, 0) == 0);
  CHECK(!exists(pt) && exists(pp) && !srv.clients[t].id_base && srv.clients[p].retained == 1);
  /* another client may free a retained resource; with the last one gone the slot is free
   * (X.Org would keep it until a reset) */
  uint32_t pbase = srv.clients[p].id_base;
  CHECK(simple(b, 54, 0, pp) == 0 && !exists(pp) && !srv.clients[p].retained && !srv.clients[p].id_base);
  CHECK(answer(simple(b, 113, 0, pbase)) == ERR(X11_BAD_VALUE));
  /* a live client in a Retain mode killed: its connection closes, its resources stay */
  int l = open_client(msb);
  close_down_mode(l, 1);
  uint32_t pl = pixmap(l);
  CHECK(simple(b, 113, 0, pl) == 0 && x11_take_kill(&srv) == l);
  x11_client_close(&srv, l);  /* the transport closes it */
  CHECK(exists(pl) && srv.clients[l].retained == 1);
  CHECK(simple(b, 113, 0, pl) == 0 && !exists(pl));
  /* a live client in Destroy mode killed: closed, resources destroyed */
  int dcl = open_client(msb);
  uint32_t pd = pixmap(dcl);
  CHECK(simple(b, 113, 0, pd) == 0 && x11_take_kill(&srv) == dcl);
  x11_client_close(&srv, dcl);
  CHECK(!exists(pd));
  /* no reset on this server: the last connection closing keeps retained clients */
  int r = open_client(msb);
  close_down_mode(r, 2);
  uint32_t pr = pixmap(r);
  x11_client_close(&srv, r);
  for (int k = 0; k < X11_SERVER_CLIENTS; ++k)
    if (srv.clients[k].active) x11_client_close(&srv, k);
  CHECK(exists(pr));
  x11_server_free(&srv);  /* frees retained clients too (ASan: no leak) */
}

static void test_close_down_reset(int msb) {
  start(1, NULL);  /* X.Org's reset at the last close */
  int a = open_client(msb), b = open_client(msb);
  close_down_mode(a, 1);
  uint32_t pa = pixmap(a);
  rq q;
  beg(&q, a, 100, 1);
  w8x4(&q, 200, 1, 0, 0); w32(&q, 0x61);
  CHECK(send_q(a, &q) == 0 && srv.kb_map);
  x11_client_close(&srv, a);
  CHECK(exists(pa));  /* b is still connected */
  x11_client_close(&srv, b);  /* the last connection, Destroy mode: reset */
  CHECK(!exists(pa) && !srv.clients[a].id_base && !srv.kb_map);
  /* the last connection closing in a Retain mode: no reset */
  int c = open_client(msb);
  close_down_mode(c, 1);
  uint32_t pc = pixmap(c);
  uint32_t atom;
  beg(&q, c, 16, 0);
  x_wr16(q.b + 4, 4, q.msb); q.n = 8; memcpy(q.b + 8, "KEEP", 4); q.n = 12;
  CHECK(send_q(c, &q) == 32);
  atom = x_rd32(out + 8, msb);
  x11_client_close(&srv, c);
  int d = open_client(msb);
  CHECK(exists(pc));
  beg(&q, d, 17, 0); w32(&q, atom);
  CHECK(answer(send_q(d, &q)) == 1);  /* the atom survived: no reset */
  x11_client_close(&srv, d);           /* now a Destroy-mode last close: reset */
  CHECK(!exists(pc));
  x11_server_free(&srv);
}

static void test_retain_cap(void) {
  start(0, NULL);
  int keep = open_client(0);
  uint32_t px[X11_MAX_RETAINED + 1];
  int cl[X11_MAX_RETAINED + 1];
  for (int i = 0; i <= X11_MAX_RETAINED; ++i) {
    cl[i] = open_client(i & 1);
    close_down_mode(cl[i], 1);
    px[i] = pixmap(cl[i]);
    x11_client_close(&srv, cl[i]);
  }
  for (int i = 0; i < X11_MAX_RETAINED; ++i) CHECK(exists(px[i]) && srv.clients[cl[i]].retained);
  CHECK(!exists(px[X11_MAX_RETAINED]) && srv.retain_refused == 1);  /* destroyed as in Destroy mode */
  char diag[2048];
  x11_server_stats(&srv, diag, sizeof(diag));
  CHECK(strstr(diag, "core.clients_retained=4\n") && strstr(diag, "core.retain_refused=1\n"));
  (void)keep;
  x11_server_free(&srv);
}

/* The diagnostics' resource counts by type, pixmap bytes and properties: they follow what a
 * client creates and return exactly to the start when the client closes. */
static unsigned long stat_value(const char *key) {
  static char text[4096];
  x11_server_stats(&srv, text, sizeof(text));
  char want[64];
  snprintf(want, sizeof(want), "%s=", key);
  const char *at = strstr(text, want);
  CHECK(at != NULL);
  return at ? strtoul(at + strlen(want), NULL, 10) : 0;
}
static void test_resource_counts(int msb) {
  start(0, NULL);
  static const char *const keys[] = {"core.res_windows", "core.res_pixmaps", "core.res_gcs", "core.pixmap_bytes",
                                     "core.properties", "core.property_bytes", "core.resources"};
  unsigned long before[7];
  for (int i = 0; i < 7; ++i) before[i] = stat_value(keys[i]);
  CHECK(before[0] >= 1);  /* the root window */
  int c = open_client(msb);
  uint32_t base = srv.clients[c].id_base;
  rq q;
  for (uint32_t k = 1; k <= 2; ++k) {  /* two windows */
    beg(&q, c, 1, 0);
    w32(&q, base + k); w32(&q, X11_ID_ROOT_WINDOW); w32(&q, 0); w32(&q, 20 | 20u << 16); w32(&q, 1u << 16); w32(&q, 0);
    w32(&q, 0);
    CHECK(send_q(c, &q) == 0);
  }
  beg(&q, c, 18, 0);  /* ChangeProperty WM_NAME (39) on the first window: STRING "abcde" */
  w32(&q, base + 1); w32(&q, 39); w32(&q, 31); w8x4(&q, 8, 0, 0, 0); w32(&q, 5);
  memcpy(q.b + q.n, "abcde\0\0\0", 8), q.n += 8;
  CHECK(send_q(c, &q) == 0);
  beg(&q, c, 53, 16);  /* 30x7 depth 16 and 13x5 depth 1 */
  w32(&q, base + 3); w32(&q, X11_ID_ROOT_WINDOW); w32(&q, 30 | 7u << 16);
  CHECK(send_q(c, &q) == 0);
  beg(&q, c, 53, 1);
  w32(&q, base + 4); w32(&q, X11_ID_ROOT_WINDOW); w32(&q, 13 | 5u << 16);
  CHECK(send_q(c, &q) == 0);
  beg(&q, c, 55, 0);  /* CreateGC */
  w32(&q, base + 5); w32(&q, base + 1); w32(&q, 0);
  CHECK(send_q(c, &q) == 0);
  unsigned long pixbytes = 0;
  for (uint32_t id = base + 3; id <= base + 4; ++id) {
    const xr_surface *sf = &((const x11_pixmap *)x_find(&srv, id)->obj)->surf;
    pixbytes += (unsigned long)sf->stride * sf->h * (sf->depth == 16 ? 2u : 1u);
  }
  CHECK(pixbytes >= 30 * 7 * 2 + 13 * 5);
  CHECK(stat_value("core.res_windows") == before[0] + 2);
  CHECK(stat_value("core.res_pixmaps") == before[1] + 2);
  CHECK(stat_value("core.res_gcs") == before[2] + 1);
  CHECK(stat_value("core.pixmap_bytes") == before[3] + pixbytes);
  CHECK(stat_value("core.properties") == before[4] + 1);
  CHECK(stat_value("core.property_bytes") == before[5] + 5);
  CHECK(stat_value("core.resources") == before[6] + 5);
  x11_client_close(&srv, c);
  for (int i = 0; i < 7; ++i) CHECK(stat_value(keys[i]) == before[i]);
  x11_server_free(&srv);
}

int main(void) {
  for (int msb = 0; msb < 2; ++msb) {
    test_keyboard_map(msb);
    test_default_keymap(msb);
    test_modifier_map(msb);
    test_access_control(msb);
    test_close_down(msb);
    test_close_down_reset(msb);
    test_resource_counts(msb);
  }
  test_retain_cap();
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  printf("x11 keyboard/modifier map, access control and close-down tests OK\n");
  return 0;
}
