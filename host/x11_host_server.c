#define _POSIX_C_SOURCE 200809L  /* read/write/socket/select on Linux with -std=c99 */
#define _DARWIN_C_SOURCE  /* ru_maxrss (macOS) */
#define _DEFAULT_SOURCE   /* ru_maxrss (glibc) */
/* Host-side X11 server (macOS / Linux), listening on 127.0.0.1 only.
 *
 *   x11_host_server [-d DISPLAY] [--cookie HEX32] [--once] [-q] [-t] [--png PATH] [--input]
 *                   [--clients N] [--budget-mb MB] [--outq-kb KB] [--resources N] [--client-resources N]
 *                   [--request-budget-us US]
 *
 * Runs the portable core (core/x11_setup.c, core/x11_server.c: the code that will run on
 * the ESP32-S3) behind POSIX sockets, through the same transport as the ESP32 (net/x11_net.c):
 * several clients at once with select(), each with its own lazily grown buffers, byte order,
 * sequence numbers and resource-ID range, one memory budget for connections and the core,
 * backpressure, and Setup Failed for connections it cannot take. Limits:
 *   --clients N            authenticated clients at once (1..X11_SERVER_CLIENTS, default 8)
 *   --budget-mb MB         memory budget for connection buffers and core objects (default 64)
 *   --outq-kb KB           largest output queue per client (default 256)
 *   --resources N          resource table (all clients, default 4096)
 *   --client-resources N   per-client resource quota (default 0: none)
 *   --request-budget-us US request processing per loop pass (default 0: unlimited)
 *   --copy-reference       CopyArea through the per-pixel reference path (benchmark baseline)
 *   --reset                server reset when the last client leaves (protocol "Connection Close":
 *                          atoms, root properties, device settings, focus, root background)
 * Diagnostics: the root property _WOODLAND_DIAG (xprop -root _WOODLAND_DIAG), as on the
 * ESP32: host.*, core.* and net.* counters; never authorization data. Every request is logged
 * with its result (reply, no reply, or the error) unless -q. -t (trace) adds each request's
 * arguments (resource IDs, the names in InternAtom, OpenFont, ListFonts, colour requests) and
 * every event delivered, for protocol diagnosis; it never prints authorization data. --png writes the 800x480 RGB565
 * framebuffer as a PNG whenever it changed (at most 5 times a second) and at exit.
 *
 * --input: synthetic touchscreen on stdin, one command per line, for deterministic tests.
 * Samples go through the same touch filter as the ESP32's GT911 (input/x11_touch.c), with
 * the time given in the command, so debouncing and release timeouts are reproducible:
 *   touch MS POINTS X Y     a controller sample at time MS (POINTS 0: nothing touched)
 *   fail MS                 a failed controller read at time MS
 *   pointer X Y BUTTONS     direct pointer input (BUTTONS: bit 0 = Button1)
 *   mt MS N [ID X Y]...     a multi-touch controller report at MS: N points (hardware track ID,
 *                           x, y); through the chain the test firmware runs (mt/x11_touchchain:
 *                           tracker, two-finger gestures, gate) to the same pointer filter, and
 *                           gesture clicks (Button3/4/5); its counters join _WOODLAND_DIAG
 *   mtfail MS               a failed multi-touch read
 *   gesture TEXT            the root property _WOODLAND_GESTURE = TEXT, as the panel publishes
 *                           a pinch (x11_root_property_set; e.g. "pinch update 1 2 1.32 400 240")
 *   query                   only report
 * After each command one line on stdout: "ack X Y BUTTONS SPRITE GRAB FROZEN" (pointer
 * position, button state bits, the window under the pointer, the grabbing client or -1).
 *
 * Security: binds 127.0.0.1 only (no option to change that). Reach it from another machine
 * through an SSH tunnel (ssh -R / -L), never by exposing the port.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "../core/x11_internal.h"
#include "../input/x11_touch.h"
#include "../mt/x11_touchchain.h"
#include "../net/x11_net.h"
#include "png_write.h"
#include "viewer_link.h"
#include "x11_names.h"

static x11_server server;
static x11_net net;
static int quiet = 0;
static viewer_link viewer;
static int trace = 0;
static x11_touch touch;
static x11_tc tc;  /* the multi-touch chain, fed by "mt" lines (simulated GT9xx frames) */
static char input_line[256];
static size_t input_have = 0;
static fd_set rd_set, wr_set;
static uint64_t start_us;

static uint64_t mono_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
static uint32_t now_ms(void *ctx) {
  (void)ctx;
  return (uint32_t)(mono_us() / 1000u);
}
static void viewer_pointer(void *ctx, int x, int y, unsigned buttons) {
  x11_pointer_input((x11_server *)ctx, x, y, buttons);
}
static uint64_t now_us(void *ctx) {
  (void)ctx;
  return mono_us();
}

/* ---- socket I/O for x11_net -------------------------------------------------------------- */

static long io_recv(void *io, int fd, uint8_t *buf, size_t n) {
  (void)io;
  for (;;) {
    ssize_t got = read(fd, buf, n);
    if (got >= 0) return (long)got;
    if (errno == EINTR) continue;
    return errno == EAGAIN || errno == EWOULDBLOCK ? X11_IO_WOULD_BLOCK : X11_IO_ERROR;
  }
}
static long io_send(void *io, int fd, const uint8_t *buf, size_t n) {
  (void)io;
  for (;;) {
    ssize_t w = write(fd, buf, n);
    if (w >= 0) return (long)w;
    if (errno == EINTR) continue;
    return errno == EAGAIN || errno == EWOULDBLOCK ? X11_IO_WOULD_BLOCK : X11_IO_ERROR;
  }
}
static void io_close(void *io, int fd) {
  (void)io;
  close(fd);
}
static int io_ready(void *io, int fd, int write) {
  (void)io;
  return FD_ISSET(fd, write ? &wr_set : &rd_set);
}

static uint16_t rd16(const uint8_t *p, int msb) { return msb ? (uint16_t)(p[0] << 8 | p[1]) : (uint16_t)(p[1] << 8 | p[0]); }
static uint32_t rd32(const uint8_t *p, int msb) {
  return msb ? (uint32_t)rd16(p, 1) << 16 | rd16(p + 2, 1) : (uint32_t)rd16(p + 2, 0) << 16 | rd16(p, 0);
}

static const char *ERROR_NAMES[18] = {0, "BadRequest", "BadValue", "BadWindow", "BadPixmap", "BadAtom",
                                      "BadCursor", "BadFont", "BadMatch", "BadDrawable", "BadAccess", "BadAlloc",
                                      "BadColormap", "BadGC", "BadIDChoice", "BadName", "BadLength",
                                      "BadImplementation"};

/* Request arguments for the trace: the name for requests that carry one, else the first
 * three 32-bit words (usually resource IDs). */
static void trace_args(const uint8_t *q, size_t len, int msb, char *out, size_t cap) {
  size_t off = 0, n = 0;
  switch (q[0]) {
    case 16: if (len >= 8) off = 8, n = rd16(q + 4, msb); break;                         /* InternAtom */
    case 45: if (len >= 12) off = 12, n = rd16(q + 8, msb); break;                       /* OpenFont */
    case 49: case 50: if (len >= 8) off = 8, n = rd16(q + 6, msb); break;                /* ListFonts */
    case 85: case 92: if (len >= 12) off = 12, n = rd16(q + 8, msb); break;              /* named colours */
    case 98: return;                                                                     /* shown already */
    case 28: if (len >= 24) {
      snprintf(out, cap, " win 0x%x owner %u mask 0x%x pointer-mode %u button %u modifiers 0x%x",
               rd32(q + 4, msb), q[1], rd16(q + 8, msb), q[10], q[20], rd16(q + 22, msb));
      return;
    } break;
  }
  if (off && off + n <= len) {
    snprintf(out, cap, " \"%.*s\"", (int)n, (const char *)q + off);
    if (q[0] == 45) snprintf(out + strlen(out), cap - strlen(out), " fid 0x%x", rd32(q + 4, msb));
    return;
  }
  for (size_t o = 4; o + 4 <= len && o <= 12; o += 4)
    snprintf(out + strlen(out), cap - strlen(out), " 0x%x", rd32(q + o, msb));
}

static void on_event(void *io, int slot, const char *what, const char *detail) {
  (void)io;
  if (!strcmp(what, "closed")) {
    if (!quiet || strcmp(detail, "client left")) {
      const x11_conn *k = slot >= 0 ? &net.conns[slot] : 0;
      fprintf(stderr, "[%d] closed (%s) after %u requests, %u errors, output queue peak %zu\n", slot, detail,
              server.clients[slot].requests, server.clients[slot].errors, k ? k->q.peak : (size_t)0);
    }
  } else if (!strcmp(what, "refused")) {
    fprintf(stderr, "refusing a connection: %s\n", detail);
  } else if (!strcmp(what, "setup failed")) {
    fprintf(stderr, "[%d] setup FAILED: %s\n", slot, detail);
  } else if (!strcmp(what, "setup ok")) {
    const x11_client *c = &server.clients[slot];
    if (!quiet)
      fprintf(stderr, "[%d] setup OK: %s byte order, resource ids 0x%08x/0x%08x\n", slot, detail, c->id_base, c->id_mask);
  }
}

static void on_request(void *io, int slot, const uint8_t *req, size_t len, const uint8_t *out, size_t n) {
  (void)io;
  if (quiet && !(n && out[0] == 0)) return;  /* errors are always logged */
  const x11_client *c = &server.clients[slot];
  const char *res = !n ? "no reply" : out[0] == 1 ? "reply" : out[1] <= 17 ? ERROR_NAMES[out[1]] : "error";
  char extra[256] = "";
  if (req[0] == 98 && len >= 8) {  /* QueryExtension: show the name */
    size_t nl = c->msb ? (size_t)(req[4] << 8 | req[5]) : (size_t)(req[5] << 8 | req[4]);
    if (8 + nl <= len) snprintf(extra, sizeof(extra), " \"%.*s\"", (int)nl, (const char *)req + 8);
  }
  if (trace) trace_args(req, len, c->msb, extra + strlen(extra), sizeof(extra) - strlen(extra));
  if (n && out[0] == 0) {  /* error: show the bad value too */
    uint32_t bad = rd32(out + 4, c->msb);
    snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), " (bad value 0x%x)", bad);
  }
  fprintf(stderr, "[%d] #%u %s (%u)%s len %zu -> %s\n", slot, c->seq, x11_request_name(req[0]), req[0], extra, len, res);
}

static void on_deliver(void *io, int client, const uint8_t *data, size_t len) {
  (void)io;
  if (!trace || len != 32) return;
  int msb = server.clients[client].msb;
  uint32_t w1 = rd32(data + 4, msb), w2 = rd32(data + 8, msb);
  fprintf(stderr, "[%d]   <- %s%s (%u) 0x%x 0x%x\n", client, data[0] & 0x80 ? "sent " : "", x11_event_name(data[0]),
          data[0] & 0x7F, w1, w2);
}

static long maxrss_bytes(const struct rusage *ru) {
#ifdef __APPLE__
  return (long)ru->ru_maxrss;  /* bytes on macOS */
#else
  return (long)ru->ru_maxrss * 1024;  /* kilobytes on Linux */
#endif
}

/* _WOODLAND_DIAG: process, core and transport counters (cumulative). */
static size_t diag(void *ctx, char *out, size_t cap) {
  (void)ctx;
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  int n = snprintf(out, cap,
                   "host.uptime_ms=%llu\nhost.cpu_user_ms=%ld\nhost.cpu_sys_ms=%ld\nhost.maxrss_bytes=%ld\n"
                   "host.server_struct=%zu\nhost.net_struct=%zu\n",
                   (unsigned long long)((mono_us() - start_us) / 1000u),
                   (long)(ru.ru_utime.tv_sec * 1000 + ru.ru_utime.tv_usec / 1000),
                   (long)(ru.ru_stime.tv_sec * 1000 + ru.ru_stime.tv_usec / 1000), maxrss_bytes(&ru),
                   sizeof(x11_server), sizeof(x11_net));
  size_t used = n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap - 1;
  used += x11_server_stats(&server, out + used, cap - used);
  used += x11_net_stats(&net, out + used, cap - used);
  if (tc.frames && used + 1 < cap) used += x11_tc_text(&tc, out + used, cap - used);  /* mt.*, tc.*, g.*, gate.* */
  return used;
}

/* "mt MS N [ID X Y]...": one GT9xx report at MS (N as the controller's count; up to 8 points
 * given); "mtfail MS": a failed read. Through the chain (as the test firmware), the pointer
 * filter and the gesture clicks. */
static void mt_frame(long ms, int ok, const char *rest) {
  x11_mt_point p[8];
  for (int i = 0; i < 8; ++i) p[i].id = 0, p[i].x = -1, p[i].y = -1, p[i].size = 0;  /* not given: out of range */
  int n = 0, k = 0, used = 0;
  if (ok && sscanf(rest, "%d%n", &n, &used) == 1) {
    rest += used;
    for (k = 0; k < 8 && k < n; ++k) {
      int id, x, y;
      if (sscanf(rest, "%d %d %d%n", &id, &x, &y, &used) != 3) break;
      rest += used;
      p[k].id = (uint8_t)id, p[k].x = (int16_t)x, p[k].y = (int16_t)y, p[k].size = 20;
    }
    if (n > k) k = n;  /* the controller's count; the chain reads at most X11_MT_MAX of p (the rest:
                          out of range) and counts a count above X11_MT_MAX as malformed */
  }
  int x = 0, y = 0;
  uint64_t t0 = mono_us();
  int pts = x11_tc_frame(&tc, (uint32_t)ms, ok, p, k, &x, &y);
  uint32_t dt = (uint32_t)(mono_us() - t0);
  if (dt > tc.frame_us_max) tc.frame_us_max = dt;
  if (x11_touch_sample(&touch, (uint32_t)ms, 1, pts, x, y)) x11_pointer_input(&server, touch.x, touch.y, touch.down);
  x11_tc_apply(&tc, &server, touch.down);
}

static void input_command(char *line) {
  long ms;
  int points, x, y, buttons;
  if (sscanf(line, "touch %ld %d %d %d", &ms, &points, &x, &y) == 4) {
    if (x11_touch_sample(&touch, (uint32_t)ms, 1, points, x, y)) x11_pointer_input(&server, touch.x, touch.y, touch.down);
  } else if (sscanf(line, "fail %ld", &ms) == 1) {
    if (x11_touch_sample(&touch, (uint32_t)ms, 0, 0, 0, 0)) x11_pointer_input(&server, touch.x, touch.y, touch.down);
  } else if (!strncmp(line, "mt ", 3) && sscanf(line + 3, "%ld", &ms) == 1) {
    const char *rest = line + 3;
    while (*rest == ' ') ++rest;
    while (*rest && *rest != ' ') ++rest;
    mt_frame(ms, 1, rest);
  } else if (sscanf(line, "mtfail %ld", &ms) == 1) {
    mt_frame(ms, 0, "");
  } else if (sscanf(line, "pointer %d %d %d", &x, &y, &buttons) == 3) {
    x11_pointer_input(&server, x, y, (unsigned)buttons);
  } else if (!strncmp(line, "gesture ", 8)) {
    size_t n = strcspn(line + 8, "\r\n");
    if (x11_root_property_set(&server, "_WOODLAND_GESTURE", (const uint8_t *)line + 8, n) != 0)
      fprintf(stderr, "input: gesture property refused\n");
  } else if (strncmp(line, "query", 5)) {
    fprintf(stderr, "input: unknown command\n");
  }
  if (trace) fprintf(stderr, "input: %s -> pointer %d,%d buttons 0x%x\n", line, server.ptr_x, server.ptr_y, server.ptr_buttons);
  printf("ack %d %d %u 0x%x %d %d\n", server.ptr_x, server.ptr_y, server.ptr_buttons, server.sprite ? server.sprite->id : 0,
         server.pgrab.client, server.frozen);
  fflush(stdout);
}

/* Reads what stdin has; runs complete lines. Returns -1 at end of input. */
static int input_read(void) {
  ssize_t got = read(0, input_line + input_have, sizeof(input_line) - 1 - input_have);
  if (got <= 0) return got < 0 && errno == EINTR ? 0 : -1;
  input_have += (size_t)got;
  char *nl;
  while ((nl = memchr(input_line, '\n', input_have)) != NULL) {
    *nl = 0;
    input_command(input_line);
    size_t used = (size_t)(nl + 1 - input_line);
    memmove(input_line, nl + 1, input_have - used);
    input_have -= used;
  }
  if (input_have == sizeof(input_line) - 1) input_have = 0;  /* overlong line: discarded */
  return 0;
}

static int parse_hex(const char *s, uint8_t *out, size_t n) {
  if (strlen(s) != 2 * n) return -1;
  for (size_t i = 0; i < n; ++i) {
    unsigned v;
    if (sscanf(s + 2 * i, "%2x", &v) != 1) return -1;
    out[i] = (uint8_t)v;
  }
  return 0;
}

static long arg_num(const char *v, long lo, long hi, const char *name) {
  char *end;
  long x = strtol(v, &end, 10);
  if (*end || x < lo || x > hi) {
    fprintf(stderr, "%s must be %ld..%ld\n", name, lo, hi);
    exit(2);
  }
  return x;
}

int main(int argc, char **argv) {
  int display = 99, once = 0, input = 0;
  long clients = 8, budget_mb = 64, outq_kb = 256, resources = 4096, client_resources = 0, request_budget_us = 0;
  const char *png = NULL, *viewer_socket = NULL;
  static uint8_t cookie[X11_COOKIE_LEN];
  x11_server_config cfg;
  x11_default_config(&cfg);
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "-d") && i + 1 < argc) display = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--cookie") && i + 1 < argc) {
      if (parse_hex(argv[++i], cookie, sizeof(cookie))) { fprintf(stderr, "--cookie needs 32 hex digits\n"); return 2; }
      cfg.cookie = cookie;
    } else if (!strcmp(argv[i], "--once")) once = 1;
    else if (!strcmp(argv[i], "-q")) quiet = 1;
    else if (!strcmp(argv[i], "-t")) trace = 1;
    else if (!strcmp(argv[i], "--png") && i + 1 < argc) png = argv[++i];
    else if (!strcmp(argv[i], "--viewer-socket") && i + 1 < argc) viewer_socket = argv[++i];
    else if (!strcmp(argv[i], "--input")) input = 1;
    else if (!strcmp(argv[i], "--copy-reference")) xr_copy_fast_enabled = 0;  /* benchmarks: the per-pixel copy */
    else if (!strcmp(argv[i], "--reset")) cfg.reset_on_last_close = 1;  /* as X.Org without -noreset */
    else if (!strcmp(argv[i], "--clients") && i + 1 < argc) clients = arg_num(argv[++i], 1, X11_SERVER_CLIENTS, "--clients");
    else if (!strcmp(argv[i], "--budget-mb") && i + 1 < argc) budget_mb = arg_num(argv[++i], 1, 4096, "--budget-mb");
    else if (!strcmp(argv[i], "--outq-kb") && i + 1 < argc) outq_kb = arg_num(argv[++i], 72, 65536, "--outq-kb");
    else if (!strcmp(argv[i], "--resources") && i + 1 < argc)
      resources = arg_num(argv[++i], 64, X11_MAX_RESOURCES_LIMIT, "--resources");
    else if (!strcmp(argv[i], "--client-resources") && i + 1 < argc)
      client_resources = arg_num(argv[++i], 0, 65535, "--client-resources");
    else if (!strcmp(argv[i], "--request-budget-us") && i + 1 < argc)
      request_budget_us = arg_num(argv[++i], 0, 10000000, "--request-budget-us");
    else {
      fprintf(stderr, "usage: %s [-d DISPLAY] [--cookie HEX32] [--once] [-q] [-t] [--png PATH] [--viewer-socket PATH] [--input] [--copy-reference] [--reset] [--clients N] "
                      "[--budget-mb MB] [--outq-kb KB] [--resources N] [--client-resources N] [--request-budget-us US]\n",
              argv[0]);
      return 2;
    }
  }
  if (display < 1 || display > 999) { fprintf(stderr, "display must be 1..999\n"); return 2; }
  cfg.max_clients = (uint16_t)clients;
  cfg.max_resources = (uint16_t)resources;
  cfg.max_resources_per_client = (uint16_t)client_resources;
  signal(SIGPIPE, SIG_IGN);
  start_us = mono_us();
  x11_touch_config tcfg;
  x11_touch_default_config(&tcfg, cfg.width_px, cfg.height_px);
  x11_touch_init(&touch, &tcfg);
  x11_tc_config tccfg;
  x11_tc_default_config(&tccfg, cfg.width_px, cfg.height_px);
  x11_tc_init(&tc, &tccfg);
  x11_net_config ncfg;
  x11_net_default_config(&ncfg, (size_t)outq_kb * 1024, (size_t)budget_mb * 1024 * 1024);
  ncfg.request_budget_us = (uint32_t)request_budget_us;
  x11_net_ops ops = {0};
  ops.recv = io_recv, ops.send = io_send, ops.close = io_close, ops.now_ms = now_ms, ops.now_us = now_us;
  ops.on_event = on_event, ops.on_request = on_request, ops.on_deliver = on_deliver;
  x11_platform plat = {0};
  plat.now_ms = now_ms;
  plat.diag = diag;
  if (x11_net_init(&net, &server, &ncfg, &ops, &plat) != 0 || x11_server_init_platform(&server, &cfg, &plat) != 0) {
    fprintf(stderr, "out of memory\n");
    return 1;
  }
  viewer.listener = viewer.client = -1;
  if (viewer_socket && viewer_link_open(&viewer, viewer_socket, cfg.width_px, cfg.height_px) != 0) {
    perror("viewer socket");
    return 1;
  }
  uint32_t png_rev = (uint32_t)-1, png_at = 0;
  int srv = socket(AF_INET, SOCK_STREAM, 0);
  int on = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)(6000 + display));
  inet_pton(AF_INET, "127.0.0.1", &a.sin_addr); /* loopback only, by design */
  if (bind(srv, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(srv, 128) < 0) {
    perror("bind 127.0.0.1");
    return 1;
  }
  fcntl(srv, F_SETFL, fcntl(srv, F_GETFL) | O_NONBLOCK);
  fprintf(stderr, "X11 experiment listening on 127.0.0.1:%d (DISPLAY=127.0.0.1:%d), auth %s, up to %ld clients, "
                  "budget %ld MB, %zu bytes reserved per connection\n",
          6000 + display, display, cfg.cookie ? "MIT-MAGIC-COOKIE-1 required" : "none (loopback test mode)", clients,
          budget_mb, x11_net_conn_reserve(&net));
  int served = 0;
  for (;;) {
    if (viewer_socket) viewer_link_frame(&viewer, server.fb.pix, server.fb.w, server.fb.h, server.fb_rev, now_ms(NULL));
    FD_ZERO(&rd_set);
    FD_ZERO(&wr_set);
    FD_SET(srv, &rd_set);
    int maxfd = srv;
    if (input) FD_SET(0, &rd_set);
    if (viewer_socket) viewer_link_fds(&viewer, &rd_set, &wr_set, &maxfd);
    for (int i = 0; i < X11_SERVER_CLIENTS; ++i) {
      const x11_conn *k = &net.conns[i];
      if (k->fd < 0) continue;
      if (k->fd > maxfd) maxfd = k->fd;
      if (x11_net_want_read(&net, k)) FD_SET(k->fd, &rd_set);
      if (x11_net_want_write(&net, k)) FD_SET(k->fd, &wr_set);
    }
    for (int i = 0; i < X11_NET_REJECT_SLOTS; ++i)
      if (net.rejects[i].fd >= 0) {
        FD_SET(net.rejects[i].fd, &rd_set);
        if (net.rejects[i].fd > maxfd) maxfd = net.rejects[i].fd;
      }
    if (png && server.fb_rev != png_rev && now_ms(NULL) - png_at >= 200) {
      png_rev = server.fb_rev;
      png_at = now_ms(NULL);
      if (png_write_rgb565(png, server.fb.pix, server.fb.w, server.fb.h) != 0) perror("png");
    }
    if (once && served && !x11_net_open(&net)) break;
    int ready = x11_net_ready(&net);
    int rejecting = 0;
    for (int i = 0; i < X11_NET_REJECT_SLOTS; ++i) rejecting |= net.rejects[i].fd >= 0;
    struct timeval tv = {0, ready ? 0 : 200000};
    if (select(maxfd + 1, &rd_set, &wr_set, NULL, (png || ready || rejecting || !once) ? &tv : NULL) < 0) {
      if (errno == EINTR) continue;
      perror("select");
      return 1;
    }
    if (input && FD_ISSET(0, &rd_set) && input_read() < 0) input = 0;  /* stdin closed: no more input */
    if (viewer_socket) viewer_link_poll(&viewer, &rd_set, &wr_set, viewer_pointer, &server);
    if (FD_ISSET(srv, &rd_set)) {
      for (int burst = 0; burst < 64; ++burst) {  /* drain a connection burst */
        struct sockaddr_in peer;
        socklen_t pl = sizeof(peer);
        int fd = accept(srv, (struct sockaddr *)&peer, &pl);
        if (fd < 0) break;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
        x11_net_accept(&net, fd, ntohl(peer.sin_addr.s_addr), ntohs(peer.sin_port));
        ++served;
      }
    }
    x11_net_pass(&net, io_ready);
  }
  if (png) png_write_rgb565(png, server.fb.pix, server.fb.w, server.fb.h);
  fprintf(stderr, "server done: %llu events sent, screen revision %u\n", (unsigned long long)net.n.events, server.fb_rev);
  close(srv);
  if (viewer_socket) viewer_link_close(&viewer, viewer_pointer, &server);
  x11_net_close_all(&net, "server stopped");
  x11_server_free(&server);
  x11_net_free(&net);
  return 0;
}
