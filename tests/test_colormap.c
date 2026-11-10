/* CreateColormap / FreeColormap (Qt5's xcb plugin creates a colormap for every top-level
 * window, as Qt 5 does). Run with ASan/UBSan (Makefile).
 *
 *  - AllocNone on the TrueColor visual: a client-owned colormap usable wherever the default
 *    one is (colour requests, Install, the window colormap attribute)
 *  - errors: unsupported visual, AllocAll (static class), alloc value, ID outside the client's
 *    range or already in use, unknown window, wrong resource type, quota
 *  - FreeColormap: windows using it get None and ColormapNotify (new = False); every later
 *    use is BadColormap (use-after-free); the default colormap is never freed
 *  - ChangeWindowAttributes colormap: ColormapNotify (new = True, Installed only for the
 *    default colormap)
 *  - the owner's close frees its colormaps the same way; nothing leaks */
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
static uint8_t evs[64][32];
static int ev_client[64], nev;

static void deliver(void *ctx, int client, const uint8_t *data, size_t len) {
  (void)ctx;
  if (nev < 64 && len == 32) {
    memcpy(evs[nev], data, 32);
    ev_client[nev++] = client;
  }
}

typedef struct {
  uint8_t b[256];
  size_t n;
} rq;
static void beg(rq *q, uint8_t op, uint8_t d) { memset(q, 0, sizeof(*q)); q->b[0] = op; q->b[1] = d; q->n = 4; }
static void w32(rq *q, uint32_t v) { x_wr32(q->b + q->n, v, 0); q->n += 4; }
static void w16x2(rq *q, uint16_t a, uint16_t b) { x_wr16(q->b + q->n, a, 0); x_wr16(q->b + q->n + 2, b, 0); q->n += 4; }
static size_t send_q(int c, rq *q) {
  x_wr16(q->b + 2, (uint16_t)(q->n / 4), 0);
  int close_after;
  return x11_client_request(&srv, c, q->b, q->n, out, sizeof(out), &close_after);
}
/* the error code of the last answer, 0 for none/reply */
static int err_of(size_t n) { return n == 32 && out[0] == 0 ? out[1] : 0; }
static uint32_t bad_of(void) { return x_rd32(out + 4, 0); }

static int open_client(void) {
  int c = x11_client_open(&srv);
  uint8_t req[12] = {'l', 0, 11, 0};
  x11_setup_result r;
  uint8_t rep[512];
  x11_client_setup(&srv, c, req, 12, rep, sizeof(rep), &r);
  CHECK(r.status == X11_SETUP_SUCCESS);
  return c;
}

static void start(int quota) {
  x11_server_config cfg;
  x11_default_config(&cfg);
  cfg.max_resources_per_client = (uint16_t)quota;
  x11_platform plat = {0};
  plat.deliver = deliver;
  CHECK(x11_server_init_platform(&srv, &cfg, &plat) == 0);
  nev = 0;
}

static size_t create_cmap(int c, uint8_t alloc, uint32_t mid, uint32_t window, uint32_t visual) {
  rq q;
  beg(&q, 78, alloc);
  w32(&q, mid); w32(&q, window); w32(&q, visual);
  return send_q(c, &q);
}
static size_t free_cmap(int c, uint32_t id) {
  rq q;
  beg(&q, 79, 0); w32(&q, id);
  return send_q(c, &q);
}
static size_t alloc_color(int c, uint32_t cmap) {
  rq q;
  beg(&q, 84, 0); w32(&q, cmap); w16x2(&q, 0xFFFF, 0x8000); w16x2(&q, 0, 0);
  return send_q(c, &q);
}
/* CreateWindow 50x40 at 10,10 with optional colormap (0: attribute not given) and event mask */
static size_t create_window(int c, uint32_t wid, uint32_t cmap, uint32_t events) {
  rq q;
  beg(&q, 1, 16);
  w32(&q, wid); w32(&q, X11_ID_ROOT_WINDOW); w16x2(&q, 10, 10); w16x2(&q, 50, 40); w16x2(&q, 0, 1);
  w32(&q, 0);
  uint32_t mask = (1u << 1) | (events ? 1u << 11 : 0) | (cmap ? 1u << 13 : 0);
  w32(&q, mask); w32(&q, 0x1234);
  if (events) w32(&q, events);
  if (cmap) w32(&q, cmap);
  return send_q(c, &q);
}
static size_t set_window_cmap(int c, uint32_t wid, uint32_t cmap) {
  rq q;
  beg(&q, 2, 0); w32(&q, wid); w32(&q, 1u << 13); w32(&q, cmap);
  return send_q(c, &q);
}
static size_t select_events(int c, uint32_t wid, uint32_t mask) {
  rq q;
  beg(&q, 2, 0); w32(&q, wid); w32(&q, 1u << 11); w32(&q, mask);
  return send_q(c, &q);
}
static uint32_t window_cmap(int c, uint32_t wid) {
  rq q;
  beg(&q, 3, 0); w32(&q, wid);  /* GetWindowAttributes */
  size_t n = send_q(c, &q);
  CHECK(n >= 44 && out[0] == 1);
  return x_rd32(out + 28, 0);
}

static void test_create_and_use(void) {
  start(0);
  int c = open_client();
  uint32_t base = srv.clients[c].id_base, cm = base + 1, win = base + 2;
  int before = x11_resources_used(&srv);
  CHECK(create_cmap(c, 0, cm, X11_ID_ROOT_WINDOW, X11_ID_VISUAL_TRUECOLOR16) == 0);
  x11_resource *r = x_find(&srv, cm);
  CHECK(r && r->type == X11_RES_COLORMAP && r->owner == c && x11_resources_used(&srv) == before + 1);
  /* colour requests: the same answers as with the default colormap */
  CHECK(alloc_color(c, X11_ID_DEFAULT_COLORMAP) == 32 && out[0] == 1);
  uint32_t px_default = x_rd32(out + 16, 0);
  CHECK(alloc_color(c, cm) == 32 && out[0] == 1 && x_rd32(out + 16, 0) == px_default);
  rq q;
  beg(&q, 91, 0); w32(&q, cm); w32(&q, px_default);  /* QueryColors */
  CHECK(send_q(c, &q) >= 40 && out[0] == 1);
  beg(&q, 81, 0); w32(&q, cm);  /* InstallColormap */
  CHECK(send_q(c, &q) == 0);
  beg(&q, 82, 0); w32(&q, cm);  /* UninstallColormap */
  CHECK(send_q(c, &q) == 0);
  /* a window created with it (Qt's sequence) exists, maps, reports it */
  CHECK(create_window(c, win, cm, 0) == 0);
  CHECK(window_cmap(c, win) == cm);
  beg(&q, 8, 0); w32(&q, win);
  CHECK(send_q(c, &q) == 0);
  /* a second colormap from another window as the screen reference */
  CHECK(create_cmap(c, 0, base + 3, win, X11_ID_VISUAL_TRUECOLOR16) == 0);
  x11_server_free(&srv);
}

static void test_errors(void) {
  start(0);
  int c = open_client(), d = open_client();
  uint32_t base = srv.clients[c].id_base, win = base + 2;
  CHECK(create_window(c, win, 0, 0) == 0);
  size_t n = create_cmap(c, 0, base + 1, X11_ID_ROOT_WINDOW, 0x99);  /* unsupported visual */
  CHECK(err_of(n) == X11_BAD_MATCH && bad_of() == 0x99 && out[10] == 78);
  n = create_cmap(c, 1, base + 1, X11_ID_ROOT_WINDOW, X11_ID_VISUAL_TRUECOLOR16);  /* AllocAll: TrueColor is static */
  CHECK(err_of(n) == X11_BAD_MATCH);
  n = create_cmap(c, 2, base + 1, X11_ID_ROOT_WINDOW, X11_ID_VISUAL_TRUECOLOR16);  /* not None/All */
  CHECK(err_of(n) == X11_BAD_VALUE && bad_of() == 2);
  n = create_cmap(c, 0, srv.clients[d].id_base + 1, X11_ID_ROOT_WINDOW, X11_ID_VISUAL_TRUECOLOR16);  /* other's range */
  CHECK(err_of(n) == X11_BAD_IDCHOICE);
  n = create_cmap(c, 0, win, X11_ID_ROOT_WINDOW, X11_ID_VISUAL_TRUECOLOR16);  /* ID in use (the window) */
  CHECK(err_of(n) == X11_BAD_IDCHOICE && bad_of() == win);
  n = create_cmap(c, 0, base + 1, base + 77, X11_ID_VISUAL_TRUECOLOR16);  /* no such window */
  CHECK(err_of(n) == X11_BAD_WINDOW && bad_of() == base + 77);
  CHECK(x_find(&srv, base + 1) == NULL);  /* none of these created anything */
  /* FreeColormap of things that are not colormaps */
  CHECK(err_of(free_cmap(c, base + 1)) == X11_BAD_COLORMAP);
  CHECK(err_of(free_cmap(c, win)) == X11_BAD_COLORMAP && bad_of() == win);
  CHECK(err_of(alloc_color(c, win)) == X11_BAD_COLORMAP);
  CHECK(err_of(create_window(c, base + 5, win, 0)) == X11_BAD_COLORMAP);  /* a window as colormap */
  /* the default colormap: FreeColormap is a no-op, it stays usable */
  CHECK(free_cmap(c, X11_ID_DEFAULT_COLORMAP) == 0);
  CHECK(alloc_color(c, X11_ID_DEFAULT_COLORMAP) == 32 && out[0] == 1);
  x11_server_free(&srv);
  /* quota: the colormap counts as a resource */
  start(1);
  c = open_client();
  base = srv.clients[c].id_base;
  CHECK(create_cmap(c, 0, base + 1, X11_ID_ROOT_WINDOW, X11_ID_VISUAL_TRUECOLOR16) == 0);
  n = create_cmap(c, 0, base + 2, X11_ID_ROOT_WINDOW, X11_ID_VISUAL_TRUECOLOR16);
  CHECK(err_of(n) == X11_BAD_ALLOC && x_find(&srv, base + 2) == NULL);
  x11_server_free(&srv);
}

static void test_free_and_use_after_free(void) {
  start(0);
  int c = open_client(), watcher = open_client();
  uint32_t base = srv.clients[c].id_base, cm = base + 1, win = base + 2, other = base + 3;
  CHECK(create_cmap(c, 0, cm, X11_ID_ROOT_WINDOW, X11_ID_VISUAL_TRUECOLOR16) == 0);
  CHECK(create_window(c, win, cm, 0) == 0);
  CHECK(create_window(c, other, 0, 0) == 0);  /* default colormap: must not be touched */
  CHECK(select_events(watcher, win, X_EV_COLORMAP_CHANGE) == 0);
  int used = x11_resources_used(&srv);
  nev = 0;
  CHECK(free_cmap(c, cm) == 0);
  CHECK(x11_resources_used(&srv) == used - 1 && x_find(&srv, cm) == NULL);
  CHECK(window_cmap(c, win) == 0);                          /* None */
  CHECK(window_cmap(c, other) == X11_ID_DEFAULT_COLORMAP);
  CHECK(nev == 1 && ev_client[0] == watcher && evs[0][0] == 32);
  CHECK(x_rd32(evs[0] + 4, 0) == win && x_rd32(evs[0] + 8, 0) == 0 && evs[0][12] == 0 && evs[0][13] == 0);
  /* use after free */
  CHECK(err_of(alloc_color(c, cm)) == X11_BAD_COLORMAP && bad_of() == cm);
  CHECK(err_of(free_cmap(c, cm)) == X11_BAD_COLORMAP);
  CHECK(err_of(set_window_cmap(c, other, cm)) == X11_BAD_COLORMAP);
  CHECK(err_of(create_window(c, base + 4, cm, 0)) == X11_BAD_COLORMAP);
  rq q;
  beg(&q, 81, 0); w32(&q, cm);
  CHECK(err_of(send_q(c, &q)) == X11_BAD_COLORMAP);
  /* the ID can be used again by its owner */
  CHECK(create_cmap(c, 0, cm, X11_ID_ROOT_WINDOW, X11_ID_VISUAL_TRUECOLOR16) == 0);
  x11_server_free(&srv);
}

static void test_change_attribute_notifies(void) {
  start(0);
  int c = open_client();
  uint32_t base = srv.clients[c].id_base, cm = base + 1, win = base + 2;
  CHECK(create_cmap(c, 0, cm, X11_ID_ROOT_WINDOW, X11_ID_VISUAL_TRUECOLOR16) == 0);
  CHECK(create_window(c, win, 0, X_EV_COLORMAP_CHANGE) == 0);
  nev = 0;
  CHECK(set_window_cmap(c, win, cm) == 0);
  CHECK(nev == 1 && evs[0][0] == 32 && x_rd32(evs[0] + 8, 0) == cm && evs[0][12] == 1 && evs[0][13] == 0);
  CHECK(set_window_cmap(c, win, cm) == 0 && nev == 1);  /* unchanged: no event */
  CHECK(set_window_cmap(c, win, X11_ID_DEFAULT_COLORMAP) == 0);
  CHECK(nev == 2 && x_rd32(evs[1] + 8, 0) == X11_ID_DEFAULT_COLORMAP && evs[1][12] == 1 && evs[1][13] == 1);
  x11_server_free(&srv);
}

static void test_owner_close(void) {
  start(0);
  int a = open_client(), b = open_client();
  uint32_t cm = srv.clients[b].id_base + 1, win = srv.clients[a].id_base + 1;
  CHECK(create_cmap(b, 0, cm, X11_ID_ROOT_WINDOW, X11_ID_VISUAL_TRUECOLOR16) == 0);
  CHECK(create_window(a, win, cm, X_EV_COLORMAP_CHANGE) == 0);  /* another client's colormap */
  int used = x11_resources_used(&srv);
  nev = 0;
  x11_client_close(&srv, b);
  CHECK(x_find(&srv, cm) == NULL && x11_resources_used(&srv) == used - 1);
  CHECK(window_cmap(a, win) == 0);
  CHECK(nev == 1 && ev_client[0] == a && evs[0][0] == 32 && evs[0][12] == 0);
  /* a client that closes with a window using its own colormap: both go, in order */
  int c = open_client();
  uint32_t cm2 = srv.clients[c].id_base + 1, w2 = srv.clients[c].id_base + 2;
  CHECK(create_cmap(c, 0, cm2, X11_ID_ROOT_WINDOW, X11_ID_VISUAL_TRUECOLOR16) == 0);
  CHECK(create_window(c, w2, cm2, 0) == 0);
  used = x11_resources_used(&srv);
  x11_client_close(&srv, c);
  CHECK(x11_resources_used(&srv) == used - 2 && !x_find(&srv, cm2) && !x_find(&srv, w2));
  x11_server_free(&srv);
}

/* FreeColors after AllocColor/AllocNamedColor: only what the client allocated, once per
 * allocation (BadAccess otherwise), pixels outside the visual BadValue, the plane mask per
 * channel; records go with the client, the colormap, and move with CopyColormapAndFree. */
static size_t free_colors(int c, uint32_t cmap, uint32_t mask, uint32_t px) {
  rq q;
  beg(&q, 88, 0); w32(&q, cmap); w32(&q, mask); w32(&q, px);
  return send_q(c, &q);
}
static uint32_t alloc_px(int c, uint32_t cmap) {
  CHECK(alloc_color(c, cmap) == 32 && out[0] == 1);
  return x_rd32(out + 16, 0);
}
static void test_free_colors(void) {
  start(0);
  int a = open_client(), b = open_client();
  uint32_t D = X11_ID_DEFAULT_COLORMAP;
  uint32_t px = alloc_px(a, D);
  CHECK(px == 0xFC00);                                       /* red 0xFFFF, green 0x8000: 31, 32, 0 */
  CHECK(err_of(free_colors(b, D, 0, px)) == X11_BAD_ACCESS);  /* another client's */
  CHECK(err_of(free_colors(a, D, 0, px)) == 0);
  CHECK(err_of(free_colors(a, D, 0, px)) == X11_BAD_ACCESS && bad_of() == px);  /* only once */
  CHECK(err_of(free_colors(a, D, 0, 0x1234)) == X11_BAD_ACCESS);                /* never allocated */
  CHECK(err_of(free_colors(a, D, 0, 0x10000)) == X11_BAD_VALUE && bad_of() == 0x10000);
  CHECK(err_of(free_colors(a, D, 0x10000, 0)) == X11_BAD_VALUE);
  /* twice allocated: freed twice */
  alloc_px(a, D), alloc_px(a, D);
  CHECK(err_of(free_colors(a, D, 0, px)) == 0 && err_of(free_colors(a, D, 0, px)) == 0);
  /* AllocNamedColor allocates too */
  rq q;
  beg(&q, 85, 0); w32(&q, D); w16x2(&q, 3, 0); memcpy(q.b + q.n, "red", 3); q.n += 4;
  CHECK(send_q(a, &q) == 32 && out[0] == 1);
  uint32_t red = x_rd32(out + 8, 0);
  CHECK(err_of(free_colors(a, D, 0, red)) == 0);
  /* plane mask 1: blue entries 0 and 1; entry 1 was never allocated: BadAccess, entry 0 freed */
  alloc_px(a, D);
  CHECK(err_of(free_colors(a, D, 1, px)) == X11_BAD_ACCESS);
  CHECK(err_of(free_colors(a, D, 0, px)) == X11_BAD_ACCESS);  /* blue 0 went with the first call */
  /* CopyColormapAndFree: the allocation moves to the new colormap */
  uint32_t mid = srv.clients[a].id_base + 1;
  alloc_px(a, D);
  beg(&q, 80, 0); w32(&q, mid); w32(&q, D);
  CHECK(err_of(send_q(a, &q)) == 0);
  CHECK(err_of(free_colors(a, D, 0, px)) == X11_BAD_ACCESS);
  CHECK(err_of(free_colors(a, mid, 0, px)) == 0);
  beg(&q, 80, 0); w32(&q, mid); w32(&q, D);                   /* ID in use */
  CHECK(err_of(send_q(a, &q)) == X11_BAD_IDCHOICE);
  beg(&q, 80, 0); w32(&q, mid + 1); w32(&q, mid + 7);         /* no such colormap */
  CHECK(err_of(send_q(a, &q)) == X11_BAD_COLORMAP);
  /* records go with FreeColormap and with the client */
  alloc_px(a, mid);
  CHECK(srv.cmap_uses != 0);
  CHECK(err_of(free_cmap(a, mid)) == 0);
  alloc_px(b, D);
  x11_client_close(&srv, b);
  int left = 0;
  for (x11_cmap_use *u = srv.cmap_uses; u; u = u->next) left += u->cmap == mid || u->client == b;
  CHECK(left == 0);
  x11_server_free(&srv);
  CHECK(srv.cmap_uses == 0);
}

/* The TrueColor visual has no writable cells: AllocColorCells/Planes BadAlloc and
 * StoreColors/StoreNamedColor BadAccess, after the checks X.Org makes first. */
static void test_read_only_cells(void) {
  start(0);
  int a = open_client();
  uint32_t D = X11_ID_DEFAULT_COLORMAP;
  rq q;
  beg(&q, 86, 0); w32(&q, D); w16x2(&q, 1, 0);
  CHECK(err_of(send_q(a, &q)) == X11_BAD_ALLOC);
  beg(&q, 86, 0); w32(&q, D); w16x2(&q, 0, 0);
  CHECK(err_of(send_q(a, &q)) == X11_BAD_VALUE);
  beg(&q, 86, 2); w32(&q, D); w16x2(&q, 1, 0);
  CHECK(err_of(send_q(a, &q)) == X11_BAD_VALUE && bad_of() == 2);
  beg(&q, 86, 0); w32(&q, 0x99); w16x2(&q, 1, 0);
  CHECK(err_of(send_q(a, &q)) == X11_BAD_COLORMAP);
  beg(&q, 86, 0); w32(&q, D);
  CHECK(err_of(send_q(a, &q)) == X11_BAD_LENGTH);
  beg(&q, 87, 0); w32(&q, D); w16x2(&q, 1, 1); w16x2(&q, 1, 1);
  CHECK(err_of(send_q(a, &q)) == X11_BAD_ALLOC);
  beg(&q, 87, 0); w32(&q, D); w16x2(&q, 0, 1); w16x2(&q, 1, 1);
  CHECK(err_of(send_q(a, &q)) == X11_BAD_VALUE);
  beg(&q, 87, 0); w32(&q, D); w16x2(&q, 1, 1);
  CHECK(err_of(send_q(a, &q)) == X11_BAD_LENGTH);
  beg(&q, 89, 0); w32(&q, D); w32(&q, 0); w16x2(&q, 1, 2); w16x2(&q, 3, 7);  /* one item */
  CHECK(err_of(send_q(a, &q)) == X11_BAD_ACCESS);
  beg(&q, 89, 0); w32(&q, D);                                                   /* no items */
  CHECK(err_of(send_q(a, &q)) == X11_BAD_ACCESS);
  beg(&q, 89, 0); w32(&q, D); w32(&q, 0);                                       /* 4 bytes */
  CHECK(err_of(send_q(a, &q)) == X11_BAD_LENGTH);
  beg(&q, 89, 0); w32(&q, 0x99);
  CHECK(err_of(send_q(a, &q)) == X11_BAD_COLORMAP);
  beg(&q, 90, 7); w32(&q, D); w32(&q, 0); w16x2(&q, 3, 0); memcpy(q.b + q.n, "red", 3); q.n += 4;
  CHECK(err_of(send_q(a, &q)) == X11_BAD_ACCESS);
  beg(&q, 90, 7); w32(&q, D); w32(&q, 0); w16x2(&q, 5, 0); memcpy(q.b + q.n, "nosuc", 5); q.n += 8;
  CHECK(err_of(send_q(a, &q)) == X11_BAD_NAME);
  beg(&q, 90, 7); w32(&q, D); w32(&q, 0); w16x2(&q, 9, 0); memcpy(q.b + q.n, "red", 3); q.n += 4;
  CHECK(err_of(send_q(a, &q)) == X11_BAD_LENGTH);
  x11_server_free(&srv);
}

int main(void) {
  test_free_colors();
  test_read_only_cells();
  test_create_and_use();
  test_errors();
  test_free_and_use_after_free();
  test_change_attribute_notifies();
  test_owner_close();
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  puts("x11 colormap tests OK");
  return 0;
}
