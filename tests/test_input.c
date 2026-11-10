/* Unit tests for Milestone 3A: the core pointer (core/x11_input.c) and the touchscreen
 * filter (input/x11_touch.c). Pointer input goes in through x11_pointer_input(), exactly as
 * the ESP32 touch loop and the host harness feed it; events are captured from
 * plat.deliver and decoded. Run with ASan/UBSan (Makefile). */
#include <stdio.h>
#include <string.h>

#include "../core/x11_internal.h"

/* Region pool use across the whole file: never exhausted, and the peak for the record. */
static int pool_peak_seen;
static unsigned long pool_failures_seen;
static void pool_note(const x11_server *s) {
  if (s->pool.peak > pool_peak_seen) pool_peak_seen = s->pool.peak;
  pool_failures_seen += s->pool.failures;
}
#define x11_server_free(s) (pool_note(s), (x11_server_free)(s))
#include "../input/x11_touch.h"

static int failures = 0;
#define CHECK(c)                                                             \
  do {                                                                       \
    if (!(c)) {                                                              \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);  \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

#define MAXEV 1024
static x11_server srv;
static uint8_t out[X11_REPLY_MAX];
static uint8_t events[MAXEV][32];
static int ev_client[MAXEV], nev = 0;
static int MSB[X11_SERVER_CLIENTS];

static void deliver(void *ctx, int client, const uint8_t *data, size_t len) {
  (void)ctx;
  if (nev < MAXEV && len == 32) {
    memcpy(events[nev], data, 32);
    ev_client[nev++] = client;
  }
}

static void start(void) {
  x11_server_config cfg;
  x11_default_config(&cfg);
  x11_platform plat = {0};
  plat.deliver = deliver;
  CHECK(x11_server_init_platform(&srv, &cfg, &plat) == 0);
  nev = 0;
}

static int open_client(int msb) {
  int c = x11_client_open(&srv);
  uint8_t req[12] = {(uint8_t)(msb ? 'B' : 'l'), 0};
  x_wr16(req + 2, 11, msb);
  x11_setup_result r;
  uint8_t rep[512];
  x11_client_setup(&srv, c, req, 12, rep, sizeof(rep), &r);
  CHECK(r.status == X11_SETUP_SUCCESS);
  MSB[c] = msb;
  return c;
}

typedef struct {
  uint8_t b[256];
  size_t n;
  int msb;
} rq;
static void beg(rq *q, int c, uint8_t op, uint8_t d) {
  memset(q, 0, sizeof(*q));
  q->msb = MSB[c];
  q->b[0] = op, q->b[1] = d, q->n = 4;
}
static void w32(rq *q, uint32_t v) { x_wr32(q->b + q->n, v, q->msb); q->n += 4; }
static void w16x2(rq *q, uint16_t a, uint16_t b) {
  x_wr16(q->b + q->n, a, q->msb);
  x_wr16(q->b + q->n + 2, b, q->msb);
  q->n += 4;
}
static void w16b8b8(rq *q, uint16_t a, uint8_t b, uint8_t c) {
  x_wr16(q->b + q->n, a, q->msb);
  q->b[q->n + 2] = b, q->b[q->n + 3] = c;
  q->n += 4;
}
static size_t send_q(int c, rq *q) {
  x_wr16(q->b + 2, (uint16_t)(q->n / 4), q->msb);
  int close_after;
  size_t n = x11_client_request(&srv, c, q->b, q->n, out, sizeof(out), &close_after);
  CHECK(!close_after);
  return n;
}
static int is_error(size_t n, uint8_t code) { return n == 32 && out[0] == 0 && out[1] == code; }

static void create(int c, uint32_t wid, uint32_t parent, int x, int y, int w, int h, int bw) {
  rq q;
  beg(&q, c, 1, 0);
  w32(&q, wid); w32(&q, parent); w16x2(&q, (uint16_t)x, (uint16_t)y); w16x2(&q, (uint16_t)w, (uint16_t)h);
  w16x2(&q, (uint16_t)bw, 1); w32(&q, 0);
  w32(&q, 1u << 1); w32(&q, 0xFFFF);
  CHECK(send_q(c, &q) == 0);
}
static void map(int c, uint32_t wid) {
  rq q;
  beg(&q, c, 8, 0); w32(&q, wid);
  CHECK(send_q(c, &q) == 0);
}
static void unmap(int c, uint32_t wid) {
  rq q;
  beg(&q, c, 10, 0); w32(&q, wid);
  CHECK(send_q(c, &q) == 0);
}
static void attr(int c, uint32_t wid, uint32_t bit, uint32_t value) {
  rq q;
  beg(&q, c, 2, 0); w32(&q, wid); w32(&q, bit); w32(&q, value);
  CHECK(send_q(c, &q) == 0);
}
static void select_events(int c, uint32_t wid, uint32_t mask) { attr(c, wid, 1u << 11, mask); }

static size_t grab_pointer(int c, uint32_t wid, int owner, uint16_t mask, int pmode, uint32_t confine) {
  rq q;
  beg(&q, c, 26, (uint8_t)owner);
  w32(&q, wid); w16b8b8(&q, mask, (uint8_t)pmode, 1); w32(&q, confine); w32(&q, 0); w32(&q, 0);
  return send_q(c, &q);
}
static size_t grab_button(int c, uint32_t wid, int owner, uint16_t mask, int pmode, int button, uint16_t mods) {
  rq q;
  beg(&q, c, 28, (uint8_t)owner);
  w32(&q, wid); w16b8b8(&q, mask, (uint8_t)pmode, 1); w32(&q, 0); w32(&q, 0);
  x_wr16(q.b + q.n + 2, mods, q.msb);
  q.b[q.n] = (uint8_t)button;
  q.n += 4;
  return send_q(c, &q);
}
static void simple(int c, uint8_t op, uint8_t data1, uint32_t arg) {
  rq q;
  beg(&q, c, op, data1); w32(&q, arg);
  CHECK(send_q(c, &q) == 0);
}
#define allow_events(c, mode) simple(c, 35, mode, 0)
#define ungrab_pointer(c) simple(c, 27, 0, 0)

/* ---- event inspection ---------------------------------------------------------------------- */

typedef struct {
  uint8_t code, detail, mode, flags, same_screen;
  uint32_t root, window, child;
  int rx, ry, ex, ey;
  uint16_t state;
} pev;

static pev decode(int i) {
  const uint8_t *e = events[i];
  int msb = MSB[ev_client[i]];
  pev p = {(uint8_t)(e[0] & 0x7F), e[1], e[30], e[31], e[30],
           x_rd32(e + 8, msb), x_rd32(e + 12, msb), x_rd32(e + 16, msb),
           (int16_t)x_rd16(e + 20, msb), (int16_t)x_rd16(e + 22, msb), (int16_t)x_rd16(e + 24, msb),
           (int16_t)x_rd16(e + 26, msb), x_rd16(e + 28, msb)};
  return p;
}

/* The k-th event (from 0) of `code` for `client` since index `from`, or code 0. */
static pev nth(int client, uint8_t code, int k) {
  for (int i = 0; i < nev; ++i)
    if (ev_client[i] == client && (events[i][0] & 0x7F) == code && k-- == 0) return decode(i);
  pev none = {0};
  return none;
}
static int count(int client, uint8_t code) {
  int n = 0;
  for (int i = 0; i < nev; ++i) n += ev_client[i] == client && (events[i][0] & 0x7F) == code;
  return n;
}
/* Crossing events of one client as "L3:w E0:w" text for order checks. */
static void crossings(int client, char *buf, size_t cap) {
  buf[0] = 0;
  for (int i = 0; i < nev; ++i) {
    uint8_t code = events[i][0] & 0x7F;
    if (ev_client[i] != client || (code != 7 && code != 8)) continue;
    pev p = decode(i);
    snprintf(buf + strlen(buf), cap - strlen(buf), "%s%c%u:%x/%u", buf[0] ? " " : "", code == 7 ? 'E' : 'L',
             p.detail, p.window & 0xFFF, p.mode);
  }
}

enum { PRESS = 4, RELEASE = 5, MOTION = 6, ENTER = 7, LEAVE = 8 };
#define M_PRESS 0x4u
#define M_RELEASE 0x8u
#define M_ENTER 0x10u
#define M_LEAVE 0x20u
#define M_MOTION 0x40u
#define M_HINT 0x80u
#define M_B1MOTION 0x100u
#define M_OWNER_GRAB 0x01000000u
#define ROOT X11_ID_ROOT_WINDOW

/* ---- tests ------------------------------------------------------------------------------- */

/* Motion over the root and a child, coordinates, crossing details and order. */
static void test_motion_and_crossing(void) {
  start();
  int a = open_client(0);
  uint32_t base = srv.clients[a].id_base, w = base + 1, c1 = base + 2, c2 = base + 3;
  create(a, w, ROOT, 100, 100, 200, 100, 2);  /* inside at (102, 102) */
  create(a, c1, w, 10, 10, 50, 50, 0);        /* inside at (112, 112) */
  create(a, c2, w, 100, 10, 50, 50, 0);       /* (202, 112) */
  map(a, c1), map(a, c2), map(a, w);
  uint32_t m = M_MOTION | M_ENTER | M_LEAVE;
  select_events(a, ROOT, m), select_events(a, w, m), select_events(a, c1, m), select_events(a, c2, m);
  nev = 0;
  x11_pointer_input(&srv, 5, 7, 0);
  pev p = nth(a, MOTION, 0);
  CHECK(count(a, MOTION) == 1 && p.window == ROOT && p.rx == 5 && p.ry == 7 && p.ex == 5 && p.child == 0);
  CHECK(p.state == 0 && p.same_screen == 1 && count(a, ENTER) == 0);
  /* root -> c1 (an inferior): Leave(root, Inferior), Enter(w, Virtual), Enter(c1, Ancestor) */
  nev = 0;
  x11_pointer_input(&srv, 150, 130, 0);
  char buf[256];
  crossings(a, buf, sizeof(buf));
  CHECK(!strcmp(buf, "L2:26/0 E1:1/0 E0:2/0"));
  p = nth(a, LEAVE, 0);
  CHECK(p.child == w && p.flags == 3);  /* child toward the pointer; same-screen + focus (PointerRoot) */
  p = nth(a, MOTION, 0);
  CHECK(count(a, MOTION) == 1 && p.window == c1 && p.ex == 38 && p.ey == 18 && p.rx == 150);
  /* c1 -> c2 (siblings): Nonlinear, no virtual events on w */
  nev = 0;
  x11_pointer_input(&srv, 210, 130, 0);
  crossings(a, buf, sizeof(buf));
  CHECK(!strcmp(buf, "L3:2/0 E3:3/0"));
  /* c2 -> w's border (an ancestor): Leave(c2, Ancestor), Enter(w, Inferior) */
  nev = 0;
  x11_pointer_input(&srv, 301, 150, 0);
  crossings(a, buf, sizeof(buf));
  CHECK(!strcmp(buf, "L0:3/0 E2:1/0"));
  /* w -> root over a gap: Leave(w, Ancestor), Enter(root, Inferior) */
  nev = 0;
  x11_pointer_input(&srv, 400, 300, 0);
  crossings(a, buf, sizeof(buf));
  CHECK(!strcmp(buf, "L0:1/0 E2:26/0"));
  /* a window mapped under the pointer: crossing events without motion */
  uint32_t top = base + 4;
  create(a, top, ROOT, 380, 280, 40, 40, 0);
  select_events(a, top, m);
  nev = 0;
  map(a, top);
  crossings(a, buf, sizeof(buf));
  CHECK(!strcmp(buf, "L2:26/0 E0:4/0") && count(a, MOTION) == 0);
  nev = 0;
  unmap(a, top);
  crossings(a, buf, sizeof(buf));
  CHECK(!strcmp(buf, "L0:4/0 E2:26/0"));
  /* the pointer is clamped to the screen */
  x11_pointer_input(&srv, -50, 9999, 0);
  CHECK(srv.ptr_x == 0 && srv.ptr_y == 479);
  x11_server_free(&srv);
}

/* ButtonPress propagation, the automatic grab, release; coordinates in a reparented window. */
static void test_press_release_reparented(void) {
  start();
  int wm = open_client(0), app = open_client(1);  /* the application is MSB-first */
  uint32_t frame = srv.clients[wm].id_base + 1, win = srv.clients[app].id_base + 1;
  uint32_t kid = srv.clients[app].id_base + 2;
  create(app, win, ROOT, 0, 0, 100, 60, 0);
  create(app, kid, win, 20, 10, 30, 30, 0);
  map(app, kid);
  create(wm, frame, ROOT, 300, 200, 110, 90, 1);  /* inside at (301, 201) */
  map(wm, frame);
  rq q;
  beg(&q, wm, 7, 0); w32(&q, win); w32(&q, frame); w16x2(&q, 5, 20);  /* win inside at (306, 221) */
  CHECK(send_q(wm, &q) == 0);
  map(app, win);
  /* the application selects buttons and motion on win (not on kid) */
  select_events(app, win, M_PRESS | M_RELEASE | M_B1MOTION);
  nev = 0;
  x11_pointer_input(&srv, 330, 235, 0);  /* in kid: (326..356, 231..261) */
  CHECK(srv.sprite == (x11_window *)x_find(&srv, kid)->obj);
  CHECK(count(app, MOTION) == 0);       /* Button1Motion only: nothing without a button */
  x11_pointer_input(&srv, 330, 235, 1);
  pev p = nth(app, PRESS, 0);
  CHECK(count(app, PRESS) == 1 && p.detail == 1 && p.window == win && p.child == kid);
  CHECK(p.rx == 330 && p.ry == 235 && p.ex == 24 && p.ey == 14 && p.state == 0 && p.same_screen == 1);
  CHECK(srv.pgrab.client == app && srv.pgrab.kind == X11_GRAB_AUTOMATIC && srv.pgrab.window == win);
  /* drag outside everything: still reported to win (automatic grab), relative to win */
  x11_pointer_input(&srv, 600, 400, 1);
  p = nth(app, MOTION, 0);
  CHECK(count(app, MOTION) == 1 && p.window == win && p.ex == 294 && p.ey == 179 && p.state == 0x100);
  CHECK(p.child == 0);
  x11_pointer_input(&srv, 600, 400, 0);
  p = nth(app, RELEASE, 0);
  CHECK(count(app, RELEASE) == 1 && p.window == win && p.state == 0x100 && p.detail == 1);
  CHECK(srv.pgrab.client < 0 && srv.ptr_buttons == 0);
  /* QueryPointer agrees with the events */
  beg(&q, wm, 38, 0); w32(&q, frame);
  CHECK(send_q(wm, &q) == 32 && out[0] == 1 && out[1] == 1);
  CHECK(x_rd16(out + 16, 0) == 600 && x_rd16(out + 18, 0) == 400 && (int16_t)x_rd16(out + 20, 0) == 299);
  CHECK(x_rd32(out + 12, 0) == 0 && x_rd16(out + 24, 0) == 0);
  /* during a press: the mask, and the child of the frame under the pointer */
  x11_pointer_input(&srv, 310, 230, 1);
  beg(&q, wm, 38, 0); w32(&q, frame);
  CHECK(send_q(wm, &q) == 32 && x_rd32(out + 12, 0) == win && x_rd16(out + 24, 0) == 0x100);
  CHECK(x_rd16(out + 20, 0) == 9 && x_rd16(out + 22, 0) == 29);
  x11_pointer_input(&srv, 310, 230, 0);
  CHECK(srv.ptr_buttons == 0 && srv.pgrab.client < 0);
  x11_server_free(&srv);
}

/* do-not-propagate stops a press; OwnerGrabButton makes the automatic grab owner-events. */
static void test_propagation_masks(void) {
  start();
  int a = open_client(0);
  uint32_t base = srv.clients[a].id_base, w = base + 1, k = base + 2, other = base + 3;
  create(a, w, ROOT, 0, 0, 200, 200, 0);
  create(a, k, w, 50, 50, 50, 50, 0);
  create(a, other, ROOT, 300, 0, 100, 100, 0);
  map(a, k), map(a, w), map(a, other);
  select_events(a, w, M_PRESS | M_RELEASE);
  attr(a, k, 1u << 12, M_PRESS);  /* do-not-propagate ButtonPress */
  nev = 0;
  x11_pointer_input(&srv, 60, 60, 1);
  x11_pointer_input(&srv, 60, 60, 0);
  CHECK(count(a, PRESS) == 0 && count(a, RELEASE) == 1 && srv.pgrab.client < 0);
  /* OwnerGrabButton: while grabbed, events on its other windows go there normally */
  select_events(a, w, M_PRESS | M_RELEASE | M_OWNER_GRAB);
  select_events(a, other, M_RELEASE);
  nev = 0;
  x11_pointer_input(&srv, 10, 10, 1);
  CHECK(srv.pgrab.client == a && srv.pgrab.owner_events == 1);
  x11_pointer_input(&srv, 350, 50, 0);
  CHECK(count(a, RELEASE) == 1 && nth(a, RELEASE, 0).window == other);
  x11_server_free(&srv);
}

/* GrabPointer statuses, owner-events false, confine-to, the grab ends when its client exits
 * or when its window is unmapped; Grab/Ungrab crossing modes. */
static void test_active_grab(void) {
  start();
  int a = open_client(0), b = open_client(0);
  uint32_t aw = srv.clients[a].id_base + 1, bw = srv.clients[b].id_base + 1, hidden = srv.clients[b].id_base + 2;
  create(a, aw, ROOT, 0, 0, 100, 100, 0);
  create(b, bw, ROOT, 200, 200, 100, 100, 0);
  create(b, hidden, ROOT, 0, 300, 10, 10, 0);
  map(a, aw), map(b, bw);
  select_events(a, aw, M_PRESS | M_RELEASE | M_ENTER | M_LEAVE);
  select_events(b, bw, M_ENTER);
  x11_pointer_input(&srv, 50, 50, 0);  /* in aw */
  CHECK(grab_pointer(b, hidden, 0, M_PRESS, 1, 0) == 32 && out[1] == X11_GRAB_NOT_VIEWABLE);
  nev = 0;
  CHECK(grab_pointer(b, bw, 0, M_PRESS | M_RELEASE | M_MOTION | M_ENTER | M_LEAVE, 1, 0) == 32 && out[1] == 0);
  /* the Grab crossing: from aw to bw, delivered as without a grab (a gets its Leave) */
  CHECK(count(a, LEAVE) == 1 && nth(a, LEAVE, 0).mode == 1 && count(b, ENTER) == 1 && nth(b, ENTER, 0).mode == 1);
  CHECK(grab_pointer(a, aw, 0, M_PRESS, 1, 0) == 32 && out[1] == X11_ALREADY_GRABBED);
  nev = 0;
  x11_pointer_input(&srv, 50, 50, 1);  /* over a's window, reported to b's grab window */
  pev p = nth(b, PRESS, 0);
  CHECK(count(a, PRESS) == 0 && count(b, PRESS) == 1 && p.window == bw && p.ex == -150 && p.ey == -150);
  x11_pointer_input(&srv, 50, 50, 0);
  CHECK(srv.pgrab.client == b);  /* an active grab outlives the release */
  /* the client exits: the grab ends (Ungrab crossing to a), a's press is delivered normally */
  nev = 0;
  x11_client_close(&srv, b);
  CHECK(srv.pgrab.client < 0);
  x11_pointer_input(&srv, 51, 50, 1);
  x11_pointer_input(&srv, 51, 50, 0);
  CHECK(count(a, PRESS) == 1 && count(a, RELEASE) == 1);
  /* confine-to: the pointer moves into the window and stays there */
  int c = open_client(0);
  uint32_t cw = srv.clients[c].id_base + 1;
  create(c, cw, ROOT, 400, 100, 50, 40, 0);
  map(c, cw);
  CHECK(grab_pointer(c, cw, 1, M_MOTION, 1, cw) == 32 && out[1] == 0);
  CHECK(srv.ptr_x == 400 && srv.ptr_y == 100);
  x11_pointer_input(&srv, 700, 10, 0);
  CHECK(srv.ptr_x == 449 && srv.ptr_y == 100);
  /* the grab window is unmapped: the grab ends, the pointer is free again */
  unmap(c, cw);
  CHECK(srv.pgrab.client < 0);
  x11_pointer_input(&srv, 701, 11, 0);
  CHECK(srv.ptr_x == 701 && srv.ptr_y == 11);
  /* UngrabPointer */
  map(c, cw);
  CHECK(grab_pointer(c, cw, 0, M_MOTION, 1, 0) == 32 && out[1] == 0 && srv.pgrab.client == c);
  ungrab_pointer(c);
  CHECK(srv.pgrab.client < 0);
  x11_server_free(&srv);
}

/* Passive grabs: the outermost one wins, BadAccess between clients, synchronous freeze,
 * AllowEvents AsyncPointer and ReplayPointer, UngrabButton. */
static void test_passive_grabs(void) {
  start();
  int wm = open_client(0), app = open_client(0), other = open_client(0);
  uint32_t frame = srv.clients[wm].id_base + 1, win = srv.clients[app].id_base + 1;
  create(wm, frame, ROOT, 100, 100, 200, 150, 0);
  create(app, win, frame, 0, 20, 200, 130, 0);
  map(app, win), map(wm, frame);
  select_events(app, win, M_PRESS | M_RELEASE | M_MOTION);
  /* the application grabs too (inner window); the window manager's outer grab wins */
  CHECK(grab_button(app, win, 0, M_PRESS | M_RELEASE, 1, 1, 0) == 0);
  CHECK(grab_button(wm, frame, 0, M_PRESS | M_RELEASE | M_MOTION, 1, 1, 0x8000) == 0);
  CHECK(is_error(grab_button(other, frame, 0, M_PRESS, 1, 0, 0), X11_BAD_ACCESS));  /* AnyButton overlaps */
  nev = 0;
  x11_pointer_input(&srv, 150, 150, 1);
  CHECK(count(wm, PRESS) == 1 && count(app, PRESS) == 0 && srv.pgrab.kind == X11_GRAB_PASSIVE);
  x11_pointer_input(&srv, 160, 150, 1);
  x11_pointer_input(&srv, 160, 150, 0);
  CHECK(count(wm, MOTION) == 1 && count(wm, RELEASE) == 1 && srv.pgrab.client < 0);
  /* synchronous: the pointer freezes after the press; later input waits */
  CHECK(grab_button(wm, frame, 0, M_PRESS | M_RELEASE | M_MOTION, 0, 1, 0x8000) == 0);  /* replaces */
  nev = 0;
  x11_pointer_input(&srv, 150, 150, 1);
  x11_pointer_input(&srv, 170, 160, 1);
  x11_pointer_input(&srv, 170, 160, 0);
  CHECK(srv.frozen && count(wm, PRESS) == 1 && count(wm, MOTION) == 0 && count(wm, RELEASE) == 0);
  CHECK(srv.qlen == 2 && srv.ptr_buttons == 0x100);
  allow_events(wm, 0);  /* AsyncPointer: the queued motion and release go to the grab */
  CHECK(!srv.frozen && count(wm, MOTION) == 1 && count(wm, RELEASE) == 1 && srv.pgrab.client < 0);
  CHECK(srv.ptr_buttons == 0 && srv.qlen == 0);
  /* ReplayPointer: the press is reprocessed without the frame's grab: the application's
   * own passive grab on its window activates, and the rest of the drag goes there */
  nev = 0;
  x11_pointer_input(&srv, 150, 150, 1);
  x11_pointer_input(&srv, 180, 170, 1);
  CHECK(srv.frozen && count(wm, PRESS) == 1);
  allow_events(wm, 2);
  pev p = nth(app, PRESS, 0);
  CHECK(!srv.frozen && count(app, PRESS) == 1 && p.window == win && p.ex == 50 && p.ey == 30);
  CHECK(srv.pgrab.client == app && srv.pgrab.window == win);
  x11_pointer_input(&srv, 180, 170, 0);
  CHECK(count(app, RELEASE) == 1 && count(wm, RELEASE) == 0 && srv.pgrab.client < 0 && srv.ptr_buttons == 0);
  /* SyncPointer: thaw until the next press/release reaches the client */
  nev = 0;
  x11_pointer_input(&srv, 150, 150, 1);
  allow_events(wm, 1);
  CHECK(!srv.frozen);
  x11_pointer_input(&srv, 151, 150, 1);
  CHECK(!srv.frozen && count(wm, MOTION) == 1);
  x11_pointer_input(&srv, 151, 150, 0);
  CHECK(count(wm, RELEASE) == 1 && srv.pgrab.client < 0 && !srv.frozen);  /* passive grab ended: thawed */
  /* UngrabButton (AnyButton, AnyModifier): the application's grab wins next time */
  rq q;
  beg(&q, wm, 29, 0); w32(&q, frame); w16x2(&q, 0x8000, 0);
  CHECK(send_q(wm, &q) == 0);
  nev = 0;
  x11_pointer_input(&srv, 150, 150, 1);
  x11_pointer_input(&srv, 150, 150, 0);
  CHECK(count(wm, PRESS) == 0 && count(app, PRESS) == 1);
  /* a destroyed window's grabs go away */
  rq d;
  beg(&d, app, 4, 0); w32(&d, win);
  CHECK(send_q(app, &d) == 0);
  for (int i = 0; i < X11_MAX_BUTTON_GRABS; ++i) CHECK(srv.button_grabs[i].window != win);
  x11_server_free(&srv);
}

/* Motion hints, WarpPointer, GetMotionEvents, SendEvent to PointerWindow. */
static void test_hints_and_warp(void) {
  start();
  int a = open_client(0);
  uint32_t w = srv.clients[a].id_base + 1;
  create(a, w, ROOT, 0, 0, 300, 300, 0);
  map(a, w);
  select_events(a, w, M_MOTION | M_HINT);
  nev = 0;
  x11_pointer_input(&srv, 10, 10, 0);
  x11_pointer_input(&srv, 20, 10, 0);
  x11_pointer_input(&srv, 30, 10, 0);
  CHECK(count(a, MOTION) == 1 && nth(a, MOTION, 0).detail == 1);
  rq q;
  beg(&q, a, 38, 0); w32(&q, w);
  CHECK(send_q(a, &q) == 32 && x_rd16(out + 20, 0) == 30);
  x11_pointer_input(&srv, 40, 10, 0);
  CHECK(count(a, MOTION) == 2);
  /* WarpPointer to (5, 6) in w: like a user motion */
  beg(&q, a, 41, 0); w32(&q, 0); w32(&q, w); w16x2(&q, 0, 0); w16x2(&q, 0, 0); w16x2(&q, 5, 6);
  CHECK(send_q(a, &q) == 0 && srv.ptr_x == 5 && srv.ptr_y == 6);
  /* relative warp, only when the pointer is inside the source window */
  beg(&q, a, 41, 0); w32(&q, w); w32(&q, 0); w16x2(&q, 0, 0); w16x2(&q, 0, 0); w16x2(&q, 10, (uint16_t)-3);
  CHECK(send_q(a, &q) == 0 && srv.ptr_x == 15 && srv.ptr_y == 3);
  beg(&q, a, 39, 0); w32(&q, w); w32(&q, 0); w32(&q, 0);
  CHECK(send_q(a, &q) == 32 && out[0] == 1 && x_rd32(out + 8, 0) == 0);  /* no motion history */
  /* SendEvent to PointerWindow reaches the window under the pointer */
  select_events(a, w, 1u << 22);
  nev = 0;
  beg(&q, a, 25, 0); w32(&q, 0); w32(&q, 0);
  uint8_t ev[32] = {33, 32};
  memcpy(q.b + q.n, ev, 32);
  q.n += 32;
  CHECK(send_q(a, &q) == 0 && nev == 1 && x_rd32(events[0] + 4, 0) == 0);  /* to the owner: w */
  x11_server_free(&srv);
}

/* A frozen pointer with a flood of taps: the queue stays bounded and the logical button
 * state is consistent afterwards (never stuck down). */
static void test_queue_bound_no_stuck_button(void) {
  start();
  int a = open_client(0);
  uint32_t w = srv.clients[a].id_base + 1;
  create(a, w, ROOT, 0, 0, 800, 480, 0);
  map(a, w);
  CHECK(grab_pointer(a, w, 0, M_PRESS | M_RELEASE | M_MOTION, 0, 0) == 32 && out[1] == 0 && srv.frozen);
  nev = 0;
  for (int i = 0; i < 200; ++i) {
    x11_pointer_input(&srv, 10 + i, 10, 1);
    x11_pointer_input(&srv, 10 + i, 20, 1);
    x11_pointer_input(&srv, 10 + i, 20, 0);
    CHECK(srv.qlen <= X11_INPUT_QUEUE);
  }
  CHECK(nev == 0 && srv.input_dropped > 0);
  allow_events(a, 0);
  CHECK(!srv.frozen && srv.qlen == 0 && srv.ptr_buttons == 0);
  CHECK(count(a, PRESS) == count(a, RELEASE) && count(a, PRESS) > 0);
  /* presses and releases strictly alternate */
  int down = 0, ok = 1;
  for (int i = 0; i < nev; ++i) {
    uint8_t code = events[i][0] & 0x7F;
    if (code == PRESS) ok &= !down, down = 1;
    if (code == RELEASE) ok &= down, down = 0;
  }
  CHECK(ok && !down);
  x11_server_free(&srv);
}

/* Keyboard grabs only record their owner; GrabKey/GrabKeyboard validation. */
static void test_keyboard_grab(void) {
  start();
  int a = open_client(0), b = open_client(0);
  uint32_t w = srv.clients[a].id_base + 1;
  create(a, w, ROOT, 0, 0, 10, 10, 0);
  rq q;
  beg(&q, a, 31, 0); w32(&q, w); w32(&q, 0); w16x2(&q, 0x0101, 0);
  CHECK(send_q(a, &q) == 32 && out[1] == X11_GRAB_NOT_VIEWABLE);
  map(a, w);
  CHECK(send_q(a, &q) == 32 && out[1] == 0 && srv.kgrab_client == a);
  beg(&q, b, 31, 0); w32(&q, ROOT); w32(&q, 0); w16x2(&q, 0x0101, 0);
  CHECK(send_q(b, &q) == 32 && out[1] == X11_ALREADY_GRABBED);
  simple(a, 32, 0, 0);  /* UngrabKeyboard */
  CHECK(srv.kgrab_client < 0);
  x11_server_free(&srv);
}

/* ---- the touch filter --------------------------------------------------------------------- */

static void test_touch_filter(void) {
  x11_touch_config cfg;
  x11_touch_default_config(&cfg, 800, 480);
  x11_touch t;
  x11_touch_init(&t, &cfg);
  CHECK(cfg.release_ms == 60 && cfg.jitter_px == 1);
  CHECK(x11_touch_sample(&t, 0, 1, 0, 0, 0) == 0 && !t.down);
  CHECK(x11_touch_sample(&t, 15, 1, 1, 100, 200) == 1 && t.down && t.x == 100 && t.y == 200);
  CHECK(x11_touch_sample(&t, 30, 1, 1, 101, 199) == 0);           /* jitter */
  CHECK(x11_touch_sample(&t, 45, 1, 1, 105, 200) == 1 && t.x == 105);
  /* a single missing sample, or a failed read, does not release */
  CHECK(x11_touch_sample(&t, 60, 1, 0, 0, 0) == 0 && t.down);
  CHECK(x11_touch_sample(&t, 75, 0, 0, 0, 0) == 0 && t.down);
  CHECK(x11_touch_sample(&t, 90, 1, 2, 300, 300) == 1 && t.x == 300);  /* two points: the first */
  /* lifted: released after release_ms without a touch, at the last position */
  CHECK(x11_touch_sample(&t, 105, 1, 0, 0, 0) == 0 && t.down);
  CHECK(x11_touch_sample(&t, 149, 1, 0, 0, 0) == 0 && t.down);
  CHECK(x11_touch_sample(&t, 150, 1, 0, 0, 0) == 1 && !t.down && t.x == 300);
  CHECK(t.presses == 1 && t.releases == 1);
  /* the controller stops answering while a finger is down: released all the same */
  CHECK(x11_touch_sample(&t, 1000, 1, 1, 10, 10) == 1 && t.down);
  CHECK(x11_touch_sample(&t, 1100, 0, 0, 0, 0) == 1 && !t.down && t.failed == 2);
  /* clamping (LovyanGFX can report 800 or 480 at the far edges) */
  CHECK(x11_touch_sample(&t, 2000, 1, 1, 800, 480) == 1 && t.x == 799 && t.y == 479);
  CHECK(x11_touch_sample(&t, 2010, 1, 1, -5, -1) == 1 && t.x == 0 && t.y == 0);
  CHECK(x11_touch_cancel(&t) == 1 && !t.down && x11_touch_cancel(&t) == 0);
  /* time wraps around */
  x11_touch_init(&t, &cfg);
  CHECK(x11_touch_sample(&t, 0xFFFFFFF0u, 1, 1, 5, 5) == 1);
  CHECK(x11_touch_sample(&t, 0x00000010u, 1, 0, 0, 0) == 0 && t.down);   /* 32 ms later */
  CHECK(x11_touch_sample(&t, 0x00000030u, 1, 0, 0, 0) == 1 && !t.down);  /* 64 ms later */
  /* calibration: raw 20..780 -> 0..799, raw 470..10 (inverted) -> 0..479 */
  cfg.raw_x0 = 20, cfg.raw_x1 = 780, cfg.raw_y0 = 470, cfg.raw_y1 = 10;
  x11_touch_init(&t, &cfg);
  CHECK(x11_touch_sample(&t, 0, 1, 1, 20, 470) == 1 && t.x == 0 && t.y == 0);
  CHECK(x11_touch_sample(&t, 10, 1, 1, 780, 10) == 1 && t.x == 799 && t.y == 479);
  CHECK(x11_touch_sample(&t, 20, 1, 1, 400, 240) == 1 && t.x == 400 && t.y == 240);
  CHECK(x11_touch_sample(&t, 30, 1, 1, 0, 0) == 1 && t.x == 0 && t.y == 479);  /* clamped */
}

/* The filter feeding the server: a tap, a drag, and a lost touch leave no button down. */
static void test_touch_to_pointer(void) {
  start();
  int a = open_client(0);
  uint32_t w = srv.clients[a].id_base + 1;
  create(a, w, ROOT, 0, 0, 800, 480, 0);
  map(a, w);
  select_events(a, w, M_PRESS | M_RELEASE | M_B1MOTION);
  x11_touch_config cfg;
  x11_touch_default_config(&cfg, 800, 480);
  x11_touch t;
  x11_touch_init(&t, &cfg);
  /* raw samples every 15 ms: down at (100, 100), drag to (200, 150), lift */
  int samples[][3] = {{1, 100, 100}, {1, 100, 100}, {1, 150, 120}, {1, 200, 150}, {0, 0, 0}, {0, 0, 0},
                      {0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
  nev = 0;
  for (int i = 0; i < 9; ++i)
    if (x11_touch_sample(&t, (uint32_t)(15 * i), 1, samples[i][0], samples[i][1], samples[i][2]))
      x11_pointer_input(&srv, t.x, t.y, t.down);
  CHECK(count(a, PRESS) == 1 && count(a, MOTION) == 2 && count(a, RELEASE) == 1);
  pev p = nth(a, PRESS, 0);
  CHECK(p.rx == 100 && p.ry == 100);
  p = nth(a, RELEASE, 0);
  CHECK(p.rx == 200 && p.ry == 150 && p.state == 0x100 && srv.ptr_buttons == 0);
  x11_server_free(&srv);
}

/* Stage 4: the platform sets a root property (the pinch, _WOODLAND_GESTURE): PropertyNotify
 * to the clients that select PropertyChange on the root (either byte order), none to others;
 * GetProperty returns the value; the diagnostics atom is refused. */
static void test_root_property_set(void) {
  start();
  int a = open_client(0), b = open_client(1), other = open_client(0);
  select_events(a, ROOT, 1u << 22);   /* PropertyChange */
  select_events(b, ROOT, 1u << 22);
  const char *t1 = "pinch start 1 0 1.00 400 240";
  CHECK(x11_root_property_set(&srv, "_WOODLAND_GESTURE", (const uint8_t *)t1, strlen(t1)) == 0);
  CHECK(count(a, 28) == 1 && count(b, 28) == 1 && count(other, 28) == 0);
  /* the event names the atom; InternAtom (only-if-exists) gives the same one */
  rq q;
  beg(&q, a, 16, 1); w16x2(&q, 17, 0);
  memcpy(q.b + q.n, "_WOODLAND_GESTURE\0\0\0", 20); q.n += 20;
  CHECK(send_q(a, &q) == 32);
  uint32_t atom = x_rd32(out + 8, 0);
  CHECK(atom != 0);
  for (int i = 0; i < nev; ++i)
    if ((events[i][0] & 0x7F) == 28) {
      int msb = MSB[ev_client[i]];
      CHECK(x_rd32(events[i] + 4, msb) == ROOT && x_rd32(events[i] + 8, msb) == atom && events[i][16] == 0);
    }
  /* a new value replaces the old one; GetProperty reads it back as STRING */
  const char *t2 = "pinch end 1 3 1.52 410 250";
  nev = 0;
  CHECK(x11_root_property_set(&srv, "_WOODLAND_GESTURE", (const uint8_t *)t2, strlen(t2)) == 0);
  CHECK(count(a, 28) == 1 && count(b, 28) == 1);
  beg(&q, other, 20, 0); w32(&q, ROOT); w32(&q, atom); w32(&q, 0); w32(&q, 0); w32(&q, 64);
  size_t n = send_q(other, &q);
  CHECK(n == 32 + x_pad4(strlen(t2)) && out[1] == 8 && x_rd32(out + 8, 0) == 31);
  CHECK(x_rd32(out + 16, 0) == strlen(t2) && !memcmp(out + 32, t2, strlen(t2)));
  /* refused: the diagnostics atom, too large a value */
  CHECK(x11_root_property_set(&srv, X11_DIAG_ATOM, (const uint8_t *)"x", 1) == -1);
  CHECK(x11_root_property_set(&srv, "_WOODLAND_GESTURE", (const uint8_t *)t2, X11_MAX_PROPERTY + 1) == -1);
  x11_server_free(&srv);
}

/* SetPointerMapping (116) / GetPointerMapping (117): a 5-element map; BadValue for another length
 * or a repeated nonzero element; Busy (and no change) while a button whose mapping changes is
 * down, Success while only unchanged ones are; MappingNotify (Pointer) to every client; events
 * carry the logical button and its state bit; 0 disables a button; a logical button above 5
 * has no state bit but its press/release pair still arrives and ends the automatic grab. */
static size_t set_map(int c, const uint8_t *m, int n) {
  rq q;
  beg(&q, c, 116, (uint8_t)n);
  memcpy(q.b + 4, m, (size_t)n);
  q.n = 4 + (((size_t)n + 3) & ~(size_t)3);
  return send_q(c, &q);
}
static int count_events(int c, uint8_t code, int detail) {
  int k = 0;
  for (int i = 0; i < nev; ++i) k += ev_client[i] == c && (events[i][0] & 0x7F) == code && (detail < 0 || events[i][1] == detail);
  return k;
}
static void test_pointer_mapping(void) {
  for (int msb = 0; msb < 2; ++msb) {
    start();
    int a = open_client(msb), b = open_client(!msb);
    select_events(a, X11_ID_ROOT_WINDOW, (1u << 2) | (1u << 3));  /* ButtonPress, ButtonRelease */
    rq q;
    beg(&q, a, 117, 0);
    CHECK(send_q(a, &q) == 40 && out[1] == 5 && out[32] == 1 && out[36] == 5);
    /* errors */
    uint8_t four[4] = {1, 2, 3, 4}, dup[5] = {3, 2, 3, 4, 5}, six[6] = {1, 2, 3, 4, 5, 6};
    CHECK(is_error(set_map(a, four, 4), X11_BAD_VALUE) && x_rd32(out + 4, msb) == 4);
    CHECK(is_error(set_map(a, six, 6), X11_BAD_VALUE));
    CHECK(is_error(set_map(a, dup, 5), X11_BAD_VALUE) && x_rd32(out + 4, msb) == 3);
    beg(&q, a, 116, 5);  /* says 5, carries 4 bytes: length */
    q.n = 8;
    CHECK(is_error(send_q(a, &q), X11_BAD_LENGTH));
    /* swap 1 and 3 (zeros may repeat): Success, MappingNotify to both clients */
    uint8_t swap[5] = {3, 2, 1, 0, 0};
    nev = 0;
    CHECK(set_map(a, swap, 5) == 32 && out[0] == 1 && out[1] == 0);
    CHECK(count_events(a, 34, -1) == 1 && count_events(b, 34, -1) == 1);
    for (int i = 0; i < nev; ++i)
      if ((events[i][0] & 0x7F) == 34) CHECK(events[i][4] == 2);  /* request Pointer */
    beg(&q, a, 117, 0);
    CHECK(send_q(a, &q) == 40 && !memcmp(out + 32, swap, 5));
    /* the touch (physical 1) is logical Button3 now; physical 4 is disabled */
    nev = 0;
    x11_pointer_input(&srv, 100, 100, 1);
    CHECK(count_events(a, 4, 3) == 1 && count_events(a, 4, 1) == 0);
    CHECK(srv.ptr_buttons == 0x400);  /* Button3's state bit */
    /* while physical 1 is down: changing its mapping is Busy, changing 5's is fine */
    uint8_t busy[5] = {1, 2, 3, 0, 0}, ok5[5] = {3, 2, 1, 0, 7};
    CHECK(set_map(a, busy, 5) == 32 && out[0] == 1 && out[1] == 1);
    beg(&q, a, 117, 0);
    CHECK(send_q(a, &q) == 40 && out[32] == 3);                 /* unchanged */
    CHECK(set_map(a, ok5, 5) == 32 && out[1] == 0);
    x11_pointer_input(&srv, 100, 100, 0);
    CHECK(count_events(a, 5, 3) == 1 && srv.ptr_buttons == 0);  /* the release matches the press */
    nev = 0;
    x11_pointer_input(&srv, 100, 100, 1u << 3);  /* physical 4: disabled, nothing */
    x11_pointer_input(&srv, 100, 100, 0);
    CHECK(count_events(a, 4, -1) == 0 && count_events(a, 5, -1) == 0);
    /* physical 5 -> logical 7: a press/release pair without a state bit; the automatic grab ends */
    x11_pointer_input(&srv, 100, 100, 1u << 4);
    CHECK(count_events(a, 4, 7) == 1 && srv.ptr_buttons == 0 && srv.hi_down == 1 && srv.pgrab.client == a);
    x11_pointer_input(&srv, 100, 100, 0);
    CHECK(count_events(a, 5, 7) == 1 && srv.hi_down == 0 && srv.pgrab.client == -1);
    /* back to the identity */
    uint8_t id[5] = {1, 2, 3, 4, 5};
    CHECK(set_map(b, id, 5) == 32 && out[1] == 0);
    nev = 0;
    x11_pointer_input(&srv, 100, 100, 1);
    x11_pointer_input(&srv, 100, 100, 0);
    CHECK(count_events(a, 4, 1) == 1 && count_events(a, 5, 1) == 1);
    x11_server_free(&srv);
  }
}

/* SetPointerMapping and the passive-grab fix together (integration, x11-integration-rc1):
 * buttons 1 and 3 swapped, so the touch's physical Button1 is logical Button3; a window
 * manager passive-grabs logical Button3, synchronous, ButtonRelease only (mwm's pattern). The
 * activating press reaches it as Button3, ReplayPointer gives it to the application, click
 * after click; a mapping change of the held button is Busy, of another button Success; the
 * release keeps the logical button it was pressed as; nothing stays down, frozen or grabbed. */
static void test_mapping_with_passive_grab(void) {
  for (int msb = 0; msb < 2; ++msb) {
    start();
    int wm = open_client(msb), app = open_client(!msb);
    uint32_t frame = srv.clients[wm].id_base + 1, win = srv.clients[app].id_base + 1;
    create(wm, frame, ROOT, 100, 100, 200, 150, 0);
    create(app, win, frame, 0, 20, 200, 130, 0);
    map(app, win), map(wm, frame);
    select_events(app, win, M_PRESS | M_RELEASE);
    const uint8_t swap[5] = {3, 2, 1, 4, 5};
    CHECK(set_map(app, swap, 5) == 32 && out[1] == 0);
    CHECK(grab_button(wm, frame, 0, M_RELEASE, 0, 3, 0x8000) == 0);  /* logical Button3, sync */
    for (int click = 0; click < 5; ++click) {
      nev = 0;
      x11_pointer_input(&srv, 150, 150, 1);                           /* physical Button1 */
      CHECK(count_events(wm, PRESS, 3) == 1 && count(wm, PRESS) == 1 && srv.frozen && srv.pgrab.client == wm);
      allow_events(wm, 2);                                            /* ReplayPointer */
      CHECK(!srv.frozen && count_events(app, PRESS, 3) == 1 && count(app, PRESS) == 1 && srv.pgrab.client == app);
      pev p = nth(app, PRESS, 0);
      CHECK(p.window == win && !(p.state & 0x700));                  /* no button down before the press */
      if (click == 2) {  /* while held: changing the held button is Busy, another one is not */
        const uint8_t busy[5] = {1, 2, 3, 4, 5}, other[5] = {3, 2, 1, 5, 4};
        CHECK(set_map(app, busy, 5) == 32 && out[1] == 1 && srv.ptr_map[0] == 3);
        CHECK(set_map(app, other, 5) == 32 && out[1] == 0 && srv.ptr_map[3] == 5);
      }
      x11_pointer_input(&srv, 150, 150, 0);
      CHECK(count_events(app, RELEASE, 3) == 1 && count(app, RELEASE) == 1 && count(wm, RELEASE) == 0);
      pev r = nth(app, RELEASE, 0);
      CHECK(r.state & 0x400);                                         /* Button3Mask: the logical button */
      CHECK(srv.pgrab.client < 0 && srv.ptr_buttons == 0 && !srv.frozen && srv.qlen == 0 && srv.swallowed == 0);
    }
    /* physical Button3 is logical Button1 now: no grab on it, the application gets it directly */
    nev = 0;
    x11_pointer_input(&srv, 150, 150, 4);
    x11_pointer_input(&srv, 150, 150, 0);
    CHECK(count_events(app, PRESS, 1) == 1 && count_events(app, RELEASE, 1) == 1 && count(wm, PRESS) == 0);
    CHECK(srv.pgrab.client < 0 && srv.ptr_buttons == 0 && !srv.frozen);
    x11_server_free(&srv);
  }
}

/* The mwm click-to-focus pattern (Debian mwm + xcalc froze after the first click): the window
 * manager passive-grabs Button1 on the client's frame, synchronous, with an event-mask of
 * ButtonRelease only. The activating ButtonPress is reported to it anyway (protocol GrabButton;
 * X.Org does, checked with Xvfb), so it can answer AllowEvents ReplayPointer; the application
 * then gets every press and release, click after click, and nothing stays frozen or down. */
static void test_passive_grab_reports_activating_press(void) {
  start();
  int wm = open_client(0), app = open_client(1);
  uint32_t frame = srv.clients[wm].id_base + 1, win = srv.clients[app].id_base + 1;
  create(wm, frame, ROOT, 100, 100, 200, 150, 0);
  create(app, win, frame, 0, 20, 200, 130, 0);
  map(app, win), map(wm, frame);
  select_events(app, win, M_PRESS | M_RELEASE);
  CHECK(grab_button(wm, frame, 0, M_RELEASE, 0, 1, 0x8000) == 0);  /* sync, ButtonRelease only */
  for (int click = 0; click < 5; ++click) {
    nev = 0;
    x11_pointer_input(&srv, 150, 150, 1);
    CHECK(count(wm, PRESS) == 1 && srv.frozen && srv.pgrab.client == wm);  /* reported despite the mask */
    pev p = nth(wm, PRESS, 0);
    CHECK(p.window == frame);
    allow_events(wm, 2);  /* ReplayPointer: as if the frame's grab had not existed */
    CHECK(!srv.frozen && count(app, PRESS) == 1 && srv.pgrab.client == app);  /* the automatic grab */
    x11_pointer_input(&srv, 150, 150, 0);
    CHECK(count(app, RELEASE) == 1 && count(wm, RELEASE) == 0);
    CHECK(srv.pgrab.client < 0 && srv.ptr_buttons == 0 && !srv.frozen && srv.qlen == 0);
  }
  /* other events of the grab still follow its mask: with AsyncPointer the release is reported
   * (selected), a motion is not */
  CHECK(grab_button(wm, frame, 0, M_RELEASE, 1, 1, 0x8000) == 0);  /* async now */
  nev = 0;
  x11_pointer_input(&srv, 150, 150, 1);
  x11_pointer_input(&srv, 160, 155, 1);
  x11_pointer_input(&srv, 160, 155, 0);
  CHECK(count(wm, PRESS) == 1 && count(wm, MOTION) == 0 && count(wm, RELEASE) == 1 && count(app, PRESS) == 0);
  CHECK(srv.pgrab.client < 0 && srv.ptr_buttons == 0);
  x11_server_free(&srv);
}

int main(void) {
  test_pointer_mapping();
  test_passive_grab_reports_activating_press();
  test_mapping_with_passive_grab();
  test_root_property_set();
  test_motion_and_crossing();
  test_press_release_reparented();
  test_propagation_masks();
  test_active_grab();
  test_passive_grabs();
  test_hints_and_warp();
  test_queue_bound_no_stuck_button();
  test_keyboard_grab();
  test_touch_filter();
  test_touch_to_pointer();
  CHECK(pool_failures_seen == 0);
  printf("region pool: peak %d of %d, failures %lu\n", pool_peak_seen, XR_POOL_REGIONS, pool_failures_seen);
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  puts("x11 pointer and touch tests OK");
  return 0;
}
