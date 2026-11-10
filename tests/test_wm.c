/* Unit tests for Milestone 2C: what window managers need from the request core.
 * SubstructureRedirect (MapRequest, ConfigureRequest, exclusive selection), ReparentWindow,
 * the save-set, stacking with a sibling, SendEvent between byte orders, selections,
 * GrabServer, KillClient, focus revert, background tiles, GetImage, clip masks, the colour
 * database, the cursor font and the FONT property. Run with ASan/UBSan (Makefile). */
#include <stdio.h>
#include <string.h>

#include "../core/x11_font.h"
#include "../core/x11_internal.h"

/* Region pool use across the whole file: never exhausted, and the peak for the record. */
static int pool_peak_seen;
static unsigned long pool_failures_seen;
static void pool_note(const x11_server *s) {
  if (s->pool.peak > pool_peak_seen) pool_peak_seen = s->pool.peak;
  pool_failures_seen += s->pool.failures;
}
#define x11_server_free(s) (pool_note(s), (x11_server_free)(s))

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
static uint8_t events[128][32];
static int ev_client[128], nev = 0;

static void deliver(void *ctx, int client, const uint8_t *data, size_t len) {
  (void)ctx;
  if (nev < 128 && len == 32) {
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
  return c;
}

typedef struct {
  uint8_t b[512];
  size_t n;
  int msb;
} rq;
static int MSB[X11_SERVER_CLIENTS];
static void beg(rq *q, int c, uint8_t op, uint8_t d) {
  memset(q, 0, sizeof(*q));
  q->msb = MSB[c];
  q->b[0] = op, q->b[1] = d, q->n = 4;
}
static void w32(rq *q, uint32_t v) { x_wr32(q->b + q->n, v, q->msb); q->n += 4; }
static void w16x2(rq *q, uint16_t a, uint16_t b) { x_wr16(q->b + q->n, a, q->msb); x_wr16(q->b + q->n + 2, b, q->msb); q->n += 4; }
static void wbytes(rq *q, const void *p, size_t n) { memcpy(q->b + q->n, p, n); q->n += x_pad4(n); }
static size_t send_q(int c, rq *q) {
  x_wr16(q->b + 2, (uint16_t)(q->n / 4), q->msb);
  int close_after;
  size_t n = x11_client_request(&srv, c, q->b, q->n, out, sizeof(out), &close_after);
  CHECK(!close_after);
  return n;
}
static int is_error(size_t n, uint8_t code) { return n == 32 && out[0] == 0 && out[1] == code; }
static uint16_t fbpx(int x, int y) { return ((uint16_t *)srv.fb.pix)[y * 800 + x]; }

static int count_events(int client, uint8_t code) {
  int n = 0;
  for (int i = 0; i < nev; ++i) n += ev_client[i] == client && (events[i][0] & 0x7F) == code;
  return n;
}
static const uint8_t *find_event(int client, uint8_t code) {
  for (int i = 0; i < nev; ++i)
    if (ev_client[i] == client && (events[i][0] & 0x7F) == code) return events[i];
  return 0;
}

/* CreateWindow (InputOutput, CopyFromParent) with background pixel and optional attributes. */
static void create(int c, uint32_t wid, uint32_t parent, int x, int y, int w, int h, uint32_t bg, int override) {
  rq q;
  beg(&q, c, 1, 0);
  w32(&q, wid); w32(&q, parent); w16x2(&q, (uint16_t)x, (uint16_t)y); w16x2(&q, (uint16_t)w, (uint16_t)h);
  w16x2(&q, 0, 1); w32(&q, 0);
  w32(&q, (1u << 1) | (override ? 1u << 9 : 0));
  w32(&q, bg);
  if (override) w32(&q, 1);
  CHECK(send_q(c, &q) == 0);
}
static size_t win_op(int c, uint8_t op, uint32_t wid) {
  rq q;
  beg(&q, c, op, 0);
  w32(&q, wid);
  return send_q(c, &q);
}
static size_t select_events(int c, uint32_t wid, uint32_t mask) {
  rq q;
  beg(&q, c, 2, 0);
  w32(&q, wid); w32(&q, 1u << 11); w32(&q, mask);
  return send_q(c, &q);
}
static x11_window *window(uint32_t id) { return (x11_window *)x_find(&srv, id)->obj; }

#define SUB_REDIRECT 0x00100000u
#define SUB_NOTIFY 0x00080000u
#define STRUCT_NOTIFY 0x00020000u

static void test_redirect(void) {
  start();
  int wm = open_client(0), app = open_client(1), other = open_client(0);
  MSB[wm] = 0, MSB[app] = 1, MSB[other] = 0;
  uint32_t aw = srv.clients[app].id_base + 1;
  CHECK(select_events(wm, X11_ID_ROOT_WINDOW, SUB_REDIRECT | SUB_NOTIFY) == 0);
  CHECK(is_error(select_events(other, X11_ID_ROOT_WINDOW, SUB_REDIRECT), X11_BAD_ACCESS));  /* one at a time */
  CHECK(select_events(wm, X11_ID_ROOT_WINDOW, SUB_REDIRECT) == 0);  /* the holder may change its own mask */
  create(app, aw, X11_ID_ROOT_WINDOW, 10, 20, 100, 50, 0xFFFF, 0);
  nev = 0;
  CHECK(win_op(app, 8, aw) == 0);  /* MapWindow: redirected */
  CHECK(!window(aw)->mapped && count_events(wm, 19) == 0);
  const uint8_t *ev = find_event(wm, 20);  /* MapRequest */
  CHECK(ev && x_rd32(ev + 4, 0) == X11_ID_ROOT_WINDOW && x_rd32(ev + 8, 0) == aw);
  /* ConfigureWindow x and width: a ConfigureRequest with the given and current values */
  rq q;
  nev = 0;
  beg(&q, app, 12, 0); w32(&q, aw); w16x2(&q, 1 | 4, 0); w32(&q, 5); w32(&q, 77);
  CHECK(send_q(app, &q) == 0);
  ev = find_event(wm, 23);
  CHECK(ev && x_rd32(ev + 8, 0) == aw && (int16_t)x_rd16(ev + 16, 0) == 5 && x_rd16(ev + 18, 0) == 20);
  CHECK(ev && x_rd16(ev + 20, 0) == 77 && x_rd16(ev + 22, 0) == 50 && x_rd16(ev + 26, 0) == (1 | 4));
  CHECK(window(aw)->x == 10 && window(aw)->w == 100);  /* unchanged */
  /* the window manager's own requests are not redirected */
  CHECK(win_op(wm, 8, aw) == 0 && window(aw)->mapped);
  /* override-redirect windows map directly */
  uint32_t pop = aw + 1;
  create(app, pop, X11_ID_ROOT_WINDOW, 300, 300, 20, 20, 0xF800, 1);
  nev = 0;
  CHECK(win_op(app, 8, pop) == 0 && window(pop)->mapped && count_events(wm, 20) == 0 && fbpx(305, 305) == 0xF800);
  x11_server_free(&srv);
}

static void test_reparent_and_save_set(void) {
  start();
  int wm = open_client(0), app = open_client(0);
  MSB[wm] = MSB[app] = 0;
  uint32_t aw = srv.clients[app].id_base + 1, frame = srv.clients[wm].id_base + 1;
  create(app, aw, X11_ID_ROOT_WINDOW, 10, 20, 100, 50, 0xFFFF, 0);
  CHECK(select_events(app, aw, STRUCT_NOTIFY) == 0);
  create(wm, frame, X11_ID_ROOT_WINDOW, 200, 100, 120, 90, 0x001F, 0);
  CHECK(select_events(wm, frame, SUB_NOTIFY | SUB_REDIRECT) == 0);
  CHECK(win_op(wm, 8, frame) == 0);
  rq q;
  beg(&q, wm, 6, 0); w32(&q, frame);
  CHECK(is_error(send_q(wm, &q), X11_BAD_MATCH));  /* not its own window */
  beg(&q, wm, 6, 0); w32(&q, aw);
  CHECK(send_q(wm, &q) == 0 && srv.save_sets[0].window == aw && srv.save_sets[0].client == wm);
  /* ReparentWindow into the frame at (4, 30) */
  nev = 0;
  beg(&q, wm, 7, 0); w32(&q, aw); w32(&q, frame); w16x2(&q, 4, 30);
  CHECK(send_q(wm, &q) == 0);
  const uint8_t *ev = find_event(app, 21);
  CHECK(ev && x_rd32(ev + 12, 0) == frame && x_rd16(ev + 16, 0) == 4 && x_rd16(ev + 18, 0) == 30);
  CHECK(count_events(wm, 21) == 1);  /* to the new parent's SubstructureNotify */
  CHECK(window(aw)->parent == window(frame));
  beg(&q, wm, 7, 0); w32(&q, frame); w32(&q, aw); w16x2(&q, 0, 0);
  CHECK(is_error(send_q(wm, &q), X11_BAD_MATCH));  /* into its own inferior */
  CHECK(win_op(wm, 8, aw) == 0 && xw_viewable(window(aw)));
  CHECK(fbpx(200 + 4 + 10, 100 + 30 + 10) == 0xFFFF);
  /* the application maps a child of the frame-managed window normally: no redirect there */
  /* the window manager goes away: the window returns to the root at its screen position */
  nev = 0;
  x11_client_close(&srv, wm);
  x11_window *w = window(aw);
  CHECK(w->parent == srv.root && w->x == 204 && w->y == 130 && xw_viewable(w));
  CHECK(fbpx(210, 140) == 0xFFFF && !x_find(&srv, frame));
  CHECK(count_events(app, 21) == 1);  /* ReparentNotify to the application */
  x11_server_free(&srv);
}

static void query_tree(int c, uint32_t parent, uint32_t *kids, int *n) {
  rq q;
  beg(&q, c, 15, 0); w32(&q, parent);
  CHECK(send_q(c, &q) >= 32);
  *n = x_rd16(out + 16, 0);
  for (int i = 0; i < *n; ++i) kids[i] = x_rd32(out + 32 + 4 * i, 0);
}

static size_t configure_stack(int c, uint32_t wid, uint32_t sib, int mode) {
  rq q;
  beg(&q, c, 12, 0);
  w32(&q, wid);
  if (sib && mode >= 0) {
    w16x2(&q, 0x20 | 0x40, 0); w32(&q, sib); w32(&q, (uint32_t)mode);
  } else if (sib) {
    w16x2(&q, 0x20, 0); w32(&q, sib);  /* a sibling without a stack-mode */
  } else {
    w16x2(&q, 0x40, 0); w32(&q, (uint32_t)mode);
  }
  return send_q(c, &q);
}

static void test_stacking(void) {
  start();
  int c = open_client(0);
  MSB[c] = 0;
  uint32_t b = srv.clients[c].id_base, A = b + 1, B = b + 2, C = b + 3, D = b + 4, kids[8];
  int n;
  create(c, A, X11_ID_ROOT_WINDOW, 0, 0, 50, 50, 0xF800, 0);
  create(c, B, X11_ID_ROOT_WINDOW, 20, 20, 50, 50, 0x07E0, 0);
  create(c, C, X11_ID_ROOT_WINDOW, 400, 400, 50, 50, 0x001F, 0);
  create(c, D, A, 0, 0, 5, 5, 0, 0);
  for (uint32_t w = A; w <= C; ++w) win_op(c, 8, w);
  CHECK(configure_stack(c, A, C, 0) == 0);  /* A just above C: B, C, A */
  query_tree(c, X11_ID_ROOT_WINDOW, kids, &n);
  CHECK(n == 3 && kids[0] == B && kids[1] == C && kids[2] == A);
  CHECK(fbpx(30, 30) == 0xF800);  /* A now covers B */
  CHECK(configure_stack(c, A, B, 1) == 0);  /* A just below B: A, B, C */
  query_tree(c, X11_ID_ROOT_WINDOW, kids, &n);
  CHECK(kids[0] == A && kids[1] == B && kids[2] == C && fbpx(30, 30) == 0x07E0);
  CHECK(configure_stack(c, A, 0, 2) == 0);  /* TopIf: B occludes A, so A goes to the top */
  query_tree(c, X11_ID_ROOT_WINDOW, kids, &n);
  CHECK(kids[2] == A);
  CHECK(configure_stack(c, C, 0, 2) == 0);  /* TopIf: nothing occludes C: no change */
  query_tree(c, X11_ID_ROOT_WINDOW, kids, &n);
  CHECK(kids[2] == A);
  CHECK(configure_stack(c, A, B, 3) == 0);  /* BottomIf: A occludes B, so A goes to the bottom */
  query_tree(c, X11_ID_ROOT_WINDOW, kids, &n);
  CHECK(kids[0] == A);
  CHECK(configure_stack(c, A, 0, 4) == 0);  /* Opposite: occluded, so raised */
  query_tree(c, X11_ID_ROOT_WINDOW, kids, &n);
  CHECK(kids[2] == A);
  CHECK(is_error(configure_stack(c, A, B, -1), X11_BAD_MATCH));  /* sibling without stack-mode */
  CHECK(is_error(configure_stack(c, B, D, 0), X11_BAD_MATCH));   /* not a sibling */
  x11_server_free(&srv);
}

static void test_send_event(void) {
  start();
  int lsb = open_client(0), msb = open_client(1);
  MSB[lsb] = 0, MSB[msb] = 1;
  uint32_t w = srv.clients[lsb].id_base + 1;
  create(lsb, w, X11_ID_ROOT_WINDOW, 0, 0, 10, 10, 0, 0);
  /* ClientMessage, format 32, from a big-endian client to the window's creator (mask 0) */
  rq q;
  beg(&q, msb, 25, 0);
  w32(&q, w); w32(&q, 0);
  uint8_t ev[32] = {33, 32};
  x_wr32(ev + 4, w, 1), x_wr32(ev + 8, 39, 1), x_wr32(ev + 12, 0x11223344u, 1), x_wr32(ev + 28, 7, 1);
  wbytes(&q, ev, 32);
  nev = 0;
  CHECK(send_q(msb, &q) == 0);
  const uint8_t *got = find_event(lsb, 33);
  CHECK(got && (got[0] & 0x80) && x_rd32(got + 4, 0) == w && x_rd32(got + 8, 0) == 39);
  CHECK(got && x_rd32(got + 12, 0) == 0x11223344u && x_rd32(got + 28, 0) == 7);
  /* ConfigureNotify by event mask: 16-bit fields converted too */
  CHECK(select_events(lsb, w, STRUCT_NOTIFY) == 0);
  uint8_t cn[32] = {22};
  x_wr32(cn + 4, w, 1), x_wr32(cn + 8, w, 1), x_wr16(cn + 16, 0x0102, 1), x_wr16(cn + 20, 300, 1);
  beg(&q, msb, 25, 0);
  w32(&q, w); w32(&q, STRUCT_NOTIFY); wbytes(&q, cn, 32);
  nev = 0;
  CHECK(send_q(msb, &q) == 0);
  got = find_event(lsb, 22);
  CHECK(got && x_rd16(got + 16, 0) == 0x0102 && x_rd16(got + 20, 0) == 300);
  uint8_t bad[32] = {70};  /* not a core event */
  beg(&q, msb, 25, 0);
  w32(&q, w); w32(&q, 0); wbytes(&q, bad, 32);
  CHECK(is_error(send_q(msb, &q), X11_BAD_VALUE));
  x11_server_free(&srv);
}

static void test_selections_grab_kill(void) {
  start();
  int a = open_client(0), b = open_client(0);
  MSB[a] = MSB[b] = 0;
  uint32_t wa = srv.clients[a].id_base + 1, wb = srv.clients[b].id_base + 1;
  create(a, wa, X11_ID_ROOT_WINDOW, 0, 0, 10, 10, 0, 0);
  create(b, wb, X11_ID_ROOT_WINDOW, 0, 0, 10, 10, 0, 0);
  rq q;
  beg(&q, a, 22, 0); w32(&q, wa); w32(&q, 1); w32(&q, 0);  /* PRIMARY, CurrentTime */
  CHECK(send_q(a, &q) == 0);
  beg(&q, b, 23, 0); w32(&q, 1);
  CHECK(send_q(b, &q) == 32 && x_rd32(out + 8, 0) == wa);
  nev = 0;
  beg(&q, b, 22, 0); w32(&q, wb); w32(&q, 1); w32(&q, 0);
  CHECK(send_q(b, &q) == 0);
  const uint8_t *ev = find_event(a, 29);  /* SelectionClear to the previous owner */
  CHECK(ev && x_rd32(ev + 8, 0) == wa && x_rd32(ev + 12, 0) == 1);
  nev = 0;
  beg(&q, a, 24, 0); w32(&q, wa); w32(&q, 1); w32(&q, 31); w32(&q, 1); w32(&q, 0);  /* ConvertSelection */
  CHECK(send_q(a, &q) == 0);
  ev = find_event(b, 30);  /* SelectionRequest to the owner */
  CHECK(ev && x_rd32(ev + 8, 0) == wb && x_rd32(ev + 12, 0) == wa && x_rd32(ev + 20, 0) == 31);
  CHECK(win_op(b, 4, wb) == 0);  /* owner window destroyed: owner None */
  beg(&q, a, 23, 0); w32(&q, 1);
  CHECK(send_q(a, &q) == 32 && x_rd32(out + 8, 0) == 0);
  /* GrabServer: only the grabbing client may run until it ungrabs or leaves */
  CHECK(x11_client_may_run(&srv, b));
  beg(&q, a, 36, 0);
  CHECK(send_q(a, &q) == 0 && !x11_client_may_run(&srv, b) && x11_client_may_run(&srv, a));
  beg(&q, a, 37, 0);
  CHECK(send_q(a, &q) == 0 && x11_client_may_run(&srv, b));
  beg(&q, a, 36, 0);
  send_q(a, &q);
  x11_client_close(&srv, a);
  CHECK(x11_client_may_run(&srv, b));
  /* KillClient: the transport is told to close the resource's creator */
  int c = open_client(0);
  MSB[c] = 0;
  uint32_t wc = srv.clients[c].id_base + 1;
  create(c, wc, X11_ID_ROOT_WINDOW, 0, 0, 10, 10, 0, 0);
  beg(&q, b, 113, 0); w32(&q, wc);
  CHECK(send_q(b, &q) == 0 && x11_take_kill(&srv) == c && x11_take_kill(&srv) == -1);
  beg(&q, b, 113, 0); w32(&q, 0);  /* AllTemporary: nothing retained, nothing to do */
  CHECK(send_q(b, &q) == 0 && x11_take_kill(&srv) == -1);
  beg(&q, b, 113, 0); w32(&q, 0x12345);
  CHECK(is_error(send_q(b, &q), X11_BAD_VALUE));
  x11_server_free(&srv);
}

static void test_focus_revert(void) {
  start();
  int c = open_client(0);
  MSB[c] = 0;
  uint32_t parent = srv.clients[c].id_base + 1, child = parent + 1;
  create(c, parent, X11_ID_ROOT_WINDOW, 0, 0, 100, 100, 0, 0);
  create(c, child, parent, 10, 10, 20, 20, 0, 0);
  win_op(c, 8, parent);
  win_op(c, 8, child);
  rq q;
  beg(&q, c, 42, 2); w32(&q, child); w32(&q, 0);  /* revert-to Parent */
  CHECK(send_q(c, &q) == 0 && srv.focus == child);
  win_op(c, 10, child);
  CHECK(srv.focus == parent && srv.focus_revert == 0);  /* to the parent, then revert-to None */
  beg(&q, c, 42, 1); w32(&q, parent); w32(&q, 0);  /* revert-to PointerRoot */
  send_q(c, &q);
  win_op(c, 10, parent);
  CHECK(srv.focus == 1);
  x11_server_free(&srv);
}

static void test_tiles_image_clip_mask(void) {
  start();
  int c = open_client(0);
  MSB[c] = 0;
  uint32_t base = srv.clients[c].id_base, pm = base + 1, gc = base + 2, w = base + 3, mask = base + 4, mgc = base + 5;
  rq q;
  beg(&q, c, 53, 16); w32(&q, pm); w32(&q, X11_ID_ROOT_WINDOW); w16x2(&q, 2, 2);  /* 2x2 tile */
  CHECK(send_q(c, &q) == 0);
  beg(&q, c, 55, 0); w32(&q, gc); w32(&q, pm); w32(&q, 0);
  CHECK(send_q(c, &q) == 0);
  uint8_t img[8] = {0x00, 0xF8, 0xE0, 0x07, 0x1F, 0x00, 0xFF, 0xFF};  /* red green / blue white */
  beg(&q, c, 72, 2); w32(&q, pm); w32(&q, gc); w16x2(&q, 2, 2); w16x2(&q, 0, 0); w16x2(&q, 0 | 16 << 8, 0);
  wbytes(&q, img, 8);
  CHECK(send_q(c, &q) == 0);
  /* window with that background pixmap; the pixmap is freed at once (the window keeps a copy) */
  beg(&q, c, 1, 0);
  w32(&q, w); w32(&q, X11_ID_ROOT_WINDOW); w16x2(&q, 101, 51); w16x2(&q, 10, 10); w16x2(&q, 0, 1); w32(&q, 0);
  w32(&q, 1u << 0); w32(&q, pm);
  CHECK(send_q(c, &q) == 0);
  CHECK(win_op(c, 54, pm) == 0);
  win_op(c, 8, w);
  /* the tile's origin is the window's origin (101, 51) */
  CHECK(fbpx(101, 51) == 0xF800 && fbpx(102, 51) == 0x07E0 && fbpx(101, 52) == 0x001F && fbpx(102, 52) == 0xFFFF);
  CHECK(fbpx(103, 53) == 0xF800 && fbpx(110, 60) == 0xFFFF);  /* offsets (2,2) and (9,9) */
  /* GetImage reads it back (ZPixmap, LSBFirst) */
  beg(&q, c, 73, 2); w32(&q, w); w16x2(&q, 0, 0); w16x2(&q, 2, 2); w32(&q, 0xFFFFFFFFu);
  CHECK(send_q(c, &q) == 32 + 8 && out[1] == 16 && memcmp(out + 32, img, 8) == 0);
  beg(&q, c, 73, 2); w32(&q, w); w16x2(&q, 9, 9); w16x2(&q, 2, 2); w32(&q, 0xFFFFFFFFu);
  CHECK(is_error(send_q(c, &q), X11_BAD_MATCH));  /* outside the window */
  win_op(c, 10, w);
  beg(&q, c, 73, 2); w32(&q, w); w16x2(&q, 0, 0); w16x2(&q, 2, 2); w32(&q, 0xFFFFFFFFu);
  CHECK(is_error(send_q(c, &q), X11_BAD_MATCH));  /* not viewable */
  /* clip mask: a 4x4 bitmap with only its diagonal set limits a fill to those 4 pixels */
  beg(&q, c, 53, 1); w32(&q, mask); w32(&q, X11_ID_ROOT_WINDOW); w16x2(&q, 4, 4);
  CHECK(send_q(c, &q) == 0);
  beg(&q, c, 55, 0); w32(&q, mgc); w32(&q, mask); w32(&q, 1u << 2); w32(&q, 0);
  send_q(c, &q);
  beg(&q, c, 70, 0); w32(&q, mask); w32(&q, mgc); w16x2(&q, 0, 0); w16x2(&q, 4, 4);
  send_q(c, &q);
  beg(&q, c, 56, 0); w32(&q, mgc); w32(&q, 1u << 2); w32(&q, 1);
  send_q(c, &q);
  for (int i = 0; i < 4; ++i) {
    beg(&q, c, 70, 0); w32(&q, mask); w32(&q, mgc); w16x2(&q, (uint16_t)i, (uint16_t)i); w16x2(&q, 1, 1);
    send_q(c, &q);
  }
  uint32_t draw = base + 6;
  beg(&q, c, 55, 0); w32(&q, draw); w32(&q, X11_ID_ROOT_WINDOW); w32(&q, (1u << 2) | (1u << 17) | (1u << 18) | (1u << 19));
  w32(&q, 0xFFE0); w32(&q, 300); w32(&q, 200); w32(&q, mask);  /* fg yellow, clip origin (300,200) */
  CHECK(send_q(c, &q) == 0);
  beg(&q, c, 70, 0); w32(&q, X11_ID_ROOT_WINDOW); w32(&q, draw); w16x2(&q, 290, 190); w16x2(&q, 30, 30);
  CHECK(send_q(c, &q) == 0);
  int yellow = 0;
  for (int y = 190; y < 220; ++y)
    for (int x = 290; x < 320; ++x) yellow += fbpx(x, y) == 0xFFE0;
  CHECK(yellow == 4 && fbpx(300, 200) == 0xFFE0 && fbpx(303, 203) == 0xFFE0 && fbpx(301, 200) != 0xFFE0);
  x11_server_free(&srv);
}

static void test_colors_and_fonts(void) {
  start();
  int c = open_client(0);
  MSB[c] = 0;
  static const struct { const char *name; uint8_t r, g, b; } want[] = {
      {"grey40", 102, 102, 102}, {"Gray85", 217, 217, 217}, {"slategrey", 112, 128, 144},
      {"Slate Gray", 112, 128, 144}, {"gray100", 255, 255, 255}, {"navy blue", 0, 0, 128}};
  rq q;
  for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); ++i) {
    size_t n = strlen(want[i].name);
    beg(&q, c, 92, 0); w32(&q, X11_ID_DEFAULT_COLORMAP); w16x2(&q, (uint16_t)n, 0); wbytes(&q, want[i].name, n);
    CHECK(send_q(c, &q) == 32);
    CHECK(x_rd16(out + 8, 0) == want[i].r * 257 && x_rd16(out + 10, 0) == want[i].g * 257 &&
          x_rd16(out + 12, 0) == want[i].b * 257);
  }
  beg(&q, c, 92, 0); w32(&q, X11_ID_DEFAULT_COLORMAP); w16x2(&q, 7, 0); wbytes(&q, "gray101", 7);
  CHECK(is_error(send_q(c, &q), X11_BAD_NAME));
  /* the cursor font: glyph 68 (left_ptr) exists, 200 does not */
  uint32_t base = srv.clients[c].id_base, cf = base + 1, cur = base + 2, ff = base + 3;
  beg(&q, c, 45, 0); w32(&q, cf); w16x2(&q, 6, 0); wbytes(&q, "cursor", 6);
  CHECK(send_q(c, &q) == 0);
  beg(&q, c, 94, 0); w32(&q, cur); w32(&q, cf); w32(&q, cf); w16x2(&q, 68, 69); w16x2(&q, 0, 0); w16x2(&q, 0, 0xFFFF);
  w16x2(&q, 0xFFFF, 0xFFFF);
  CHECK(send_q(c, &q) == 0 && x_find(&srv, cur)->type == X11_RES_CURSOR);
  beg(&q, c, 94, 0); w32(&q, cur + 10); w32(&q, cf); w32(&q, 0); w16x2(&q, 200, 0); w16x2(&q, 0, 0); w16x2(&q, 0, 0);
  w16x2(&q, 0, 0);
  CHECK(is_error(send_q(c, &q), X11_BAD_VALUE));
  /* "fixed" reports its full name as the FONT property */
  beg(&q, c, 45, 0); w32(&q, ff); w16x2(&q, 5, 0); wbytes(&q, "fixed", 5);
  CHECK(send_q(c, &q) == 0);
  beg(&q, c, 47, 0); w32(&q, ff);
  CHECK(send_q(c, &q) > 68 && x_rd16(out + 46, 0) == 1 && x_rd32(out + 60, 0) == 18);
  const char *name = x11_atom_name(&srv, x_rd32(out + 64, 0));
  CHECK(name && !strcmp(name, X11_FONT_XLFD));
  /* ListFonts "*" lists both fonts' names, nothing else */
  beg(&q, c, 49, 0); w16x2(&q, 100, 1); wbytes(&q, "*", 1);
  CHECK(send_q(c, &q) >= 32 && x_rd16(out + 8, 0) == 4);
  x11_server_free(&srv);
}

/* CirculateWindow (13): RaiseLowest / LowerHighest pick the child by overlap, restack it,
 * send CirculateNotify (window and parent selections), repaint; nothing to do: no event;
 * redirected: CirculateRequest only; bad direction or window: errors. */
static void test_circulate(void) {
  start();
  int app = open_client(0), wm = open_client(1);
  MSB[app] = 0, MSB[wm] = 1;
  uint32_t base = srv.clients[app].id_base, p = base + 1, a = base + 2, b = base + 3, c = base + 4;
  uint32_t q2 = base + 5, d = base + 6, e = base + 7;
  create(app, p, X11_ID_ROOT_WINDOW, 0, 0, 200, 200, 0x1111, 0);
  create(app, a, p, 10, 10, 50, 50, 0xF800, 0);   /* bottom */
  create(app, b, p, 30, 30, 50, 50, 0x07E0, 0);   /* over a */
  create(app, c, p, 150, 150, 20, 20, 0x001F, 0); /* top, overlaps nothing */
  CHECK(win_op(app, 9, p) == 0 && win_op(app, 8, p) == 0);  /* MapSubwindows, MapWindow */
  CHECK(fbpx(40, 40) == 0x07E0);
  CHECK(select_events(app, p, X_EV_SUBSTRUCTURE) == 0);
  CHECK(select_events(app, a, X_EV_STRUCTURE | X_EV_EXPOSURE) == 0);
  nev = 0;
  CHECK(win_op(app, 13, p) == 0);  /* RaiseLowest: a (overlapped by b) goes to the top */
  CHECK(window(p)->last == window(a) && fbpx(40, 40) == 0xF800);
  CHECK(count_events(app, 26) == 2 && count_events(app, 12) >= 1);  /* CirculateNotify to a and p; Expose */
  const uint8_t *ev = find_event(app, 26);
  CHECK(ev && x_rd32(ev + 8, 0) == a && ev[16] == 0);
  nev = 0;
  rq q;
  beg(&q, app, 13, 1); w32(&q, p);  /* LowerHighest: a (top, over b) goes to the bottom */
  CHECK(send_q(app, &q) == 0);
  CHECK(window(p)->first == window(a) && fbpx(40, 40) == 0x07E0);
  ev = find_event(app, 26);
  CHECK(ev && x_rd32(ev + 8, 0) == a && ev[16] == 1 && count_events(app, 26) == 2);
  /* no child overlaps another: nothing happens */
  create(app, q2, X11_ID_ROOT_WINDOW, 300, 0, 100, 100, 0, 0);
  create(app, d, q2, 0, 0, 10, 10, 0, 0);
  create(app, e, q2, 50, 50, 10, 10, 0, 0);
  CHECK(win_op(app, 9, q2) == 0 && select_events(app, q2, X_EV_SUBSTRUCTURE) == 0);
  nev = 0;
  CHECK(win_op(app, 13, q2) == 0 && count_events(app, 26) == 0 && window(q2)->last == window(e));
  /* another client redirects: CirculateRequest (in its byte order), no restack */
  CHECK(select_events(wm, p, X_EV_SUBSTRUCTURE_REDIRECT) == 0);
  nev = 0;
  CHECK(win_op(app, 13, p) == 0);
  ev = find_event(wm, 27);
  CHECK(ev && x_rd32(ev + 4, 1) == p && x_rd32(ev + 8, 1) == a && ev[16] == 0);
  CHECK(count_events(app, 26) == 0 && window(p)->first == window(a) && fbpx(40, 40) == 0x07E0);
  /* the window manager's own CirculateWindow is not redirected */
  CHECK(win_op(wm, 13, p) == 0 && window(p)->last == window(a));
  /* errors */
  beg(&q, app, 13, 2); w32(&q, p);
  CHECK(is_error(send_q(app, &q), X11_BAD_VALUE));
  CHECK(is_error(win_op(app, 13, base + 50), X11_BAD_WINDOW));
  x11_server_free(&srv);
}

/* RotateProperties (114): values move to (i + delta) mod N, both directions, a full turn changes
 * nothing; PropertyNotify per listed atom in list order; BadAtom / BadMatch (repeated atom,
 * missing property) change nothing; BadWindow, BadLength; an empty list is fine. */
static void set_prop(int c, uint32_t w, uint32_t atom, const char *v) {
  rq q;
  beg(&q, c, 18, 0);  /* ChangeProperty Replace, STRING, format 8 */
  w32(&q, w); w32(&q, atom); w32(&q, 31);
  q.b[q.n] = 8, q.n += 4;  /* format, then 3 unused bytes */
  w32(&q, (uint32_t)strlen(v));
  wbytes(&q, v, strlen(v));
  CHECK(send_q(c, &q) == 0);
}
static char get_prop(int c, uint32_t w, uint32_t atom) {
  rq q;
  beg(&q, c, 20, 0); w32(&q, w); w32(&q, atom); w32(&q, 0); w32(&q, 0); w32(&q, 16);
  size_t n = send_q(c, &q);
  return n > 32 && out[0] == 1 ? (char)out[32] : 0;
}
static size_t rotate(int c, uint32_t w, int16_t delta, const uint32_t *atoms, int n) {
  rq q;
  beg(&q, c, 114, 0);
  w32(&q, w); w16x2(&q, (uint16_t)n, (uint16_t)delta);
  for (int i = 0; i < n; ++i) w32(&q, atoms[i]);
  return send_q(c, &q);
}
static void test_rotate_properties(void) {
  start();
  int c = open_client(1);
  MSB[c] = 1;
  uint32_t w = srv.clients[c].id_base + 1;
  create(c, w, X11_ID_ROOT_WINDOW, 0, 0, 10, 10, 0, 0);
  const uint32_t A = 1, B = 2, C = 3, D = 4;  /* PRIMARY, SECONDARY, ARC, ATOM: any atoms */
  set_prop(c, w, A, "a"), set_prop(c, w, B, "b"), set_prop(c, w, C, "c");
  CHECK(select_events(c, w, 1u << 22) == 0);  /* PropertyChange */
  uint32_t abc[3] = {A, B, C};
  nev = 0;
  CHECK(rotate(c, w, 1, abc, 3) == 0);  /* A -> B, B -> C, C -> A */
  CHECK(get_prop(c, w, A) == 'c' && get_prop(c, w, B) == 'a' && get_prop(c, w, C) == 'b');
  CHECK(nev == 3);
  for (int i = 0; i < 3 && i < nev; ++i)
    CHECK(events[i][0] == 28 && x_rd32(events[i] + 8, 1) == abc[i] && events[i][16] == 0);
  CHECK(rotate(c, w, -1, abc, 3) == 0);  /* back */
  CHECK(get_prop(c, w, A) == 'a' && get_prop(c, w, B) == 'b' && get_prop(c, w, C) == 'c');
  CHECK(rotate(c, w, -7, abc, 3) == 0);  /* -7 mod 3 = 2: A -> C, B -> A, C -> B */
  CHECK(get_prop(c, w, A) == 'b' && get_prop(c, w, B) == 'c' && get_prop(c, w, C) == 'a');
  nev = 0;
  CHECK(rotate(c, w, 6, abc, 3) == 0 && nev == 0);  /* a full turn: no change, no events */
  CHECK(get_prop(c, w, A) == 'b');
  /* four properties, delta 2: two cycles */
  set_prop(c, w, D, "d");
  uint32_t abcd[4] = {A, B, C, D};  /* values b c a d */
  CHECK(rotate(c, w, 2, abcd, 4) == 0);
  CHECK(get_prop(c, w, A) == 'a' && get_prop(c, w, B) == 'd' && get_prop(c, w, C) == 'b' && get_prop(c, w, D) == 'c');
  /* errors change nothing */
  nev = 0;
  uint32_t dup[3] = {A, B, A}, missing[2] = {A, 5}, badatom[2] = {A, 99999};
  CHECK(is_error(rotate(c, w, 1, dup, 3), X11_BAD_MATCH) && x_rd32(out + 4, 1) == A);
  CHECK(is_error(rotate(c, w, 1, missing, 2), X11_BAD_MATCH) && x_rd32(out + 4, 1) == 5);
  CHECK(is_error(rotate(c, w, 1, badatom, 2), X11_BAD_ATOM) && x_rd32(out + 4, 1) == 99999);
  CHECK(is_error(rotate(c, X11_ID_ROOT_WINDOW + 999, 1, abc, 3), X11_BAD_WINDOW));
  CHECK(get_prop(c, w, A) == 'a' && get_prop(c, w, B) == 'd' && nev == 0);
  CHECK(rotate(c, w, 1, abc, 0) == 0);  /* empty list */
  rq q;
  beg(&q, c, 114, 0); w32(&q, w); w16x2(&q, 2, 1); w32(&q, A);  /* says 2 atoms, has 1 */
  CHECK(is_error(send_q(c, &q), X11_BAD_LENGTH));
  x11_server_free(&srv);
}

/* ChangeWindowAttributes border-pixel repaints the border at once (only where visible: not under
 * a sibling above), and GetImage over the border returns the new colour (XTS Xlib9
 * XGetImage-7 / XGetSubImage-7); a background change does not repaint the inside. */
static void test_border_repaint(void) {
  start();
  int c = open_client(1);
  MSB[c] = 1;
  uint32_t w = srv.clients[c].id_base + 1, top = w + 1;
  rq q;
  beg(&q, c, 1, 0);  /* 10x10 at 20,20, border 2: background 0x1234, border red */
  w32(&q, w); w32(&q, X11_ID_ROOT_WINDOW); w16x2(&q, 20, 20); w16x2(&q, 10, 10); w16x2(&q, 2, 1); w32(&q, 0);
  w32(&q, 0x2 | 0x8); w32(&q, 0x1234); w32(&q, 0xF800);
  CHECK(send_q(c, &q) == 0);
  create(c, top, X11_ID_ROOT_WINDOW, 18, 18, 3, 3, 0x0001, 0);  /* covers the top-left border corner */
  CHECK(win_op(c, 8, w) == 0 && win_op(c, 8, top) == 0);
  /* outer 20..33, inside 22..31: border columns/rows 20-21 and 32-33 */
  CHECK(fbpx(33, 33) == 0xF800 && fbpx(18, 18) == 0x0001 && fbpx(22, 22) == 0x1234);
  beg(&q, c, 2, 0); w32(&q, w); w32(&q, 0x8); w32(&q, 0x07E0);  /* border-pixel green */
  CHECK(send_q(c, &q) == 0);
  CHECK(fbpx(33, 33) == 0x07E0 && fbpx(20, 25) == 0x07E0 && fbpx(33, 20) == 0x07E0 && fbpx(21, 21) == 0x07E0);
  CHECK(fbpx(18, 18) == 0x0001 && fbpx(20, 20) == 0x0001);  /* under the sibling above: untouched */
  CHECK(fbpx(22, 22) == 0x1234);
  beg(&q, c, 73, 2); w32(&q, w); w16x2(&q, (uint16_t)-2, 9); w16x2(&q, 1, 1); w32(&q, 0xFFFFFFFFu);  /* GetImage */
  CHECK(send_q(c, &q) == 36 && x_rd16(out + 32, 0) == 0x07E0);  /* image byte order LSBFirst */
  beg(&q, c, 2, 0); w32(&q, w); w32(&q, 0x2); w32(&q, 0x001F);  /* background: shows at the next exposure */
  CHECK(send_q(c, &q) == 0 && fbpx(22, 22) == 0x1234);
  x11_server_free(&srv);
}

int main(void) {
  test_border_repaint();
  test_rotate_properties();
  test_circulate();
  test_redirect();
  test_reparent_and_save_set();
  test_stacking();
  test_send_event();
  test_selections_grab_kill();
  test_focus_revert();
  test_tiles_image_clip_mask();
  test_colors_and_fonts();
  CHECK(pool_failures_seen == 0);
  printf("region pool: peak %d of %d, failures %lu\n", pool_peak_seen, XR_POOL_REGIONS, pool_failures_seen);
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  puts("x11 window manager tests OK");
  return 0;
}
