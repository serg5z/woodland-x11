/* X11 connections over a stream socket API; see x11_net.h. */
#include "x11_net.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef X11_HOST_TEST_COPY_DELAY
extern void x11_host_test_before_request(uint8_t opcode);
#endif
#ifdef X11_HOST_TEST_SCHED_DELAY
extern void x11_host_test_before_polyfill(uint8_t opcode);
#endif

/* Close reasons (static strings: the counters compare pointers). */
static const char R_LEFT[] = "client left";
static const char R_READ[] = "read error";
static const char R_WRITE[] = "write error";
static const char R_PROTOCOL[] = "protocol";
static const char R_OVERFLOW[] = "output queue full (client does not read)";
static const char R_KILL[] = "KillClient";
static const char R_OOM[] = "out of memory";
static const char R_SETUP_FAILED[] = "setup failed";
static const char R_SETUP_TOO_LARGE[] = "setup request too large";
static const char R_SETUP_TIMEOUT[] = "setup timeout";
static const char R_NO_SLOT[] = "too many clients";
static const char R_NO_MEMORY[] = "server memory exhausted";

void x11_net_default_config(x11_net_config *cfg, size_t outq_max, size_t budget) {
  memset(cfg, 0, sizeof(*cfg));
  cfg->in_min = 256;
  cfg->in_max = 4 * 4096 + 64;  /* one maximum request (max_request_units 4096) */
  cfg->outq_min = 1024;
  cfg->outq_reserve = X11_REPLY_MAX + 64 * 32;  /* the largest reply and some events */
  cfg->outq_max = outq_max < cfg->outq_reserve + 4096 ? cfg->outq_reserve + 4096 : outq_max;
  cfg->budget = budget;
  cfg->setup_timeout_ms = 10000;
  cfg->reject_wait_ms = 1000;
}

/* ---- memory ------------------------------------------------------------------------------ */

typedef union {
  size_t n;
  double d;  /* alignment */
  void *p;
  long long l;
} hdr;

static void *raw_alloc(x11_net *net, size_t n) {
  return net->ops.alloc ? net->ops.alloc(net->ops.io, n) : malloc(n);
}

static void raw_free(x11_net *net, void *p) {
  if (!p) return;
  if (net->ops.release) net->ops.release(net->ops.io, p);
  else free(p);
}

static void note_total(x11_net *net) {
  size_t total = net->n.core_used + net->n.conn_bytes;
  if (total > net->n.total_peak) net->n.total_peak = total;
}

/* `reserved` guarantees each open connection its initial input/output buffers. Larger buffers
 * are charged as they grow. The guarantee overlaps the initial bytes in conn_bytes, so count
 * the larger of the two rather than adding the same allocation twice. */
static size_t budget_used(const x11_net *net) {
  size_t conn = net->n.reserved > net->n.conn_bytes ? net->n.reserved : net->n.conn_bytes;
  return conn + net->n.core_used;
}

static int budget_allows(const x11_net *net, size_t n) {
  if (!net->cfg.budget) return 1;
  size_t used = budget_used(net);
  return used <= net->cfg.budget && n <= net->cfg.budget - used;
}

/* The core's allocations: charged to the budget (reservations of open connections count). */
static void *core_alloc(void *ctx, size_t n) {
  x11_net *net = (x11_net *)ctx;
  if (!budget_allows(net, n)) {
    ++net->n.core_alloc_failed;
    return 0;
  }
  hdr *h = (hdr *)raw_alloc(net, sizeof(hdr) + n);
  if (!h) {
    ++net->n.core_alloc_failed;
    return 0;
  }
  h->n = n;
  net->n.core_used += n;
  if (net->n.core_used > net->n.core_peak) net->n.core_peak = net->n.core_used;
  note_total(net);
  return h + 1;
}

static void core_release(void *ctx, void *p) {
  x11_net *net = (x11_net *)ctx;
  if (!p) return;
  hdr *h = (hdr *)p - 1;
  net->n.core_used -= h->n;
  raw_free(net, h);
}

size_t x11_net_conn_reserve(const x11_net *net) { return net->cfg.in_min + net->cfg.outq_min; }

static uint8_t *conn_buf(x11_net *net, size_t n) {
  if (!budget_allows(net, n)) {
    ++net->n.conn_alloc_failed;
    return 0;
  }
  uint8_t *p = (uint8_t *)raw_alloc(net, n);
  if (!p) {
    ++net->n.conn_alloc_failed;
    return 0;
  }
  net->n.conn_bytes += n;
  if (net->n.conn_bytes > net->n.conn_bytes_peak) net->n.conn_bytes_peak = net->n.conn_bytes;
  note_total(net);
  return p;
}

static void conn_buf_free(x11_net *net, uint8_t *p, size_t n) {
  if (!p) return;
  net->n.conn_bytes -= n;
  raw_free(net, p);
}

static size_t pow2_at_least(size_t want, size_t min, size_t max) {
  size_t c = min;
  while (c < want && c < max) c <<= 1;
  return c > max ? max : c;
}

/* Input buffer of at least `want` bytes (<= in_max). Returns 0, or -1 without memory. */
static int in_reserve(x11_net *net, x11_conn *k, size_t want) {
  if (want <= k->in_cap) return 0;
  if (want > net->cfg.in_max) return -1;
  size_t cap = pow2_at_least(want, k->in_cap ? k->in_cap : net->cfg.in_min, net->cfg.in_max);
  uint8_t *p = conn_buf(net, cap);
  if (!p) return -1;
  if (k->have) memcpy(p, k->in, k->have);
  conn_buf_free(net, k->in, k->in_cap);
  k->in = p, k->in_cap = cap;
  return 0;
}

/* Room for n more bytes in the output queue (<= outq_max in all). Returns 0 or -1. */
static int q_room(x11_net *net, x11_conn *k, size_t n) {
  if (x11_outq_free(&k->q) >= n) return 0;
  size_t want = k->q.len + n;
  if (want > net->cfg.outq_max) return -1;
  size_t cap = pow2_at_least(want, k->q.cap ? k->q.cap : net->cfg.outq_min, net->cfg.outq_max);
  uint8_t *p = conn_buf(net, cap);
  if (!p) return -1;
  size_t old_cap = k->q.cap;
  conn_buf_free(net, x11_outq_move(&k->q, p, cap), old_cap);
  return 0;
}

static int q_push(x11_net *net, x11_conn *k, const void *data, size_t n) {
  if (q_room(net, k, n) != 0) return -1;
#ifdef X11_LATENCY_TRACE
  if (!k->q.len && net->ops.now_us) {
    k->trace_output_since_us = net->ops.now_us(net->ops.io);
    k->trace_output_first_us = 0;
  }
#endif
#ifdef X11_LATENCY_TRACE
  int rc = x11_outq_push(&k->q, data, n);
  if (!rc) x11_latency_trace_depth(k->have, k->q.len);
  return rc;
#else
  return x11_outq_push(&k->q, data, n);
#endif
}

/* Idle connections give memory back: buffers return to their initial size (only after
 * X11_NET_IDLE_MS without traffic, so busy clients do not reallocate every pass). */
static void shrink(x11_net *net, x11_conn *k) {
  if (net->ops.now_ms && net->ops.now_ms(net->ops.io) - k->active_ms < X11_NET_IDLE_MS) return;
  if (!k->have && k->in_cap > net->cfg.in_min) {
    uint8_t *p = conn_buf(net, net->cfg.in_min);
    if (p) {
      conn_buf_free(net, k->in, k->in_cap);
      k->in = p, k->in_cap = net->cfg.in_min;
    }
  }
  if (!k->q.len && k->q.cap > net->cfg.outq_min) {
    uint8_t *p = conn_buf(net, net->cfg.outq_min);
    if (p) {
      size_t old_cap = k->q.cap;
      conn_buf_free(net, x11_outq_move(&k->q, p, net->cfg.outq_min), old_cap);
    }
  }
}

/* ---- records ----------------------------------------------------------------------------- */

static uint32_t now_ms(x11_net *net) { return net->ops.now_ms ? net->ops.now_ms(net->ops.io) : 0; }

#ifdef X11_LATENCY_TRACE
static void trace_flush_done(x11_net *net, uint64_t started, int had_queue) {
  if (had_queue && net->ops.now_us)
    x11_latency_trace_add(X11_TRACE_OUTPUT_FLUSH, net->ops.now_us(net->ops.io) - started);
}
#endif

static void record(x11_net *net, x11_net_record *ring, unsigned *next, uint32_t ip, uint16_t port, int slot,
                   const char *reason, uint32_t requests, uint32_t errors, uint32_t since) {
  x11_net_record *r = &ring[*next % X11_NET_HISTORY];
  ++*next;
  r->t_ms = now_ms(net);
  r->ip = ip, r->port = port, r->slot = (int16_t)slot, r->reason = reason;
  r->requests = requests, r->errors = errors;
  r->seconds = (r->t_ms - since) / 1000;
}

static void hook(x11_net *net, int slot, const char *what, const char *detail) {
  if (net->ops.on_event) net->ops.on_event(net->ops.io, slot, what, detail);
}

/* ---- setup and teardown -------------------------------------------------------------------- */

static void deliver(void *ctx, int client, const uint8_t *data, size_t len) {
  x11_net *net = (x11_net *)ctx;
  x11_conn *k = &net->conns[client];
  if (k->fd < 0 || k->dead) return;
  if (net->stream_owner == client) {
    if (len != 32 || net->deferred_count == X11_NET_DEFERRED_EVENTS) {
      k->dead = R_OVERFLOW;
      return;
    }
    unsigned tail = (net->deferred_head + net->deferred_count) % X11_NET_DEFERRED_EVENTS;
    memcpy(net->deferred_events[tail], data, 32);
    ++net->deferred_count;
    ++net->n.events;
    if (net->ops.on_deliver) net->ops.on_deliver(net->ops.io, client, data, len);
    return;
  }
  if (q_push(net, k, data, len) != 0) {
    ++k->q.overflows;
    k->dead = k->q.len + len > net->cfg.outq_max ? R_OVERFLOW : R_OOM;
    return;
  }
  ++net->n.events;
  if (net->ops.on_deliver) net->ops.on_deliver(net->ops.io, client, data, len);
}

int x11_net_init(x11_net *net, x11_server *s, const x11_net_config *cfg, const x11_net_ops *ops, x11_platform *plat) {
  memset(net, 0, sizeof(*net));
#ifdef X11_LATENCY_TRACE
  x11_latency_trace_reset();
#endif
  net->s = s;
  net->cfg = *cfg;
  net->ops = *ops;
  net->stream_owner = -1;
  for (int i = 0; i < X11_SERVER_CLIENTS; ++i) net->conns[i].fd = -1;
  for (int i = 0; i < X11_NET_REJECT_SLOTS; ++i) net->rejects[i].fd = -1;
  net->reply = (uint8_t *)raw_alloc(net, X11_REPLY_MAX);
  if (!net->reply) return -1;
  plat->alloc = core_alloc;
  plat->release = core_release;
  plat->deliver = deliver;
  plat->ctx = net;
  return 0;
}

static void count_close(x11_net *net, const char *why) {
  if (why == R_LEFT) ++net->n.closed_left;
  else if (why == R_READ) ++net->n.closed_read_error;
  else if (why == R_WRITE) ++net->n.closed_write_error;
  else if (why == R_OVERFLOW) ++net->n.closed_overflow;
  else if (why == R_KILL) ++net->n.closed_kill;
  else if (why == R_OOM) ++net->n.closed_oom;
  else if (why == R_SETUP_TIMEOUT) ++net->n.setup_timeouts;
  else if (why == R_PROTOCOL || why == R_SETUP_TOO_LARGE) ++net->n.closed_protocol;
}

static void conn_close(x11_net *net, x11_conn *k, const char *why) {
#ifdef X11_LATENCY_TRACE
  x11_latency_trace_client_reset((unsigned)k->slot);
#endif
  if (net->stream_owner == k->slot) {
    net->stream_owner = -1;
    net->deferred_head = net->deferred_count = 0;
    memset(&net->stream, 0, sizeof(net->stream));
  }
  const x11_client *c = &net->s->clients[k->slot];
  record(net, net->closes, &net->close_next, k->ip, k->port, k->slot, why, c->requests, c->errors, k->since_ms);
  count_close(net, why);
  hook(net, k->slot, "closed", why);
  x11_client_close(net->s, k->slot);
  if (net->ops.close) net->ops.close(net->ops.io, k->fd);
  conn_buf_free(net, k->in, k->in_cap);
  conn_buf_free(net, k->q.buf, k->q.cap);
  net->n.reserved -= x11_net_conn_reserve(net);
  memset(k, 0, sizeof(*k));
  k->fd = -1;
}

void x11_net_close_all(x11_net *net, const char *reason) {
  for (int i = 0; i < X11_SERVER_CLIENTS; ++i)
    if (net->conns[i].fd >= 0) conn_close(net, &net->conns[i], reason);
  for (int i = 0; i < X11_NET_REJECT_SLOTS; ++i)
    if (net->rejects[i].fd >= 0) {
      if (net->ops.close) net->ops.close(net->ops.io, net->rejects[i].fd);
      net->rejects[i].fd = -1;
    }
}

#ifdef X11_SPI_X11
void x11_net_close_fd(x11_net *net, int fd, const char *reason) {
  for (int i = 0; i < X11_SERVER_CLIENTS; ++i)
    if (net->conns[i].fd == fd) conn_close(net, &net->conns[i], reason);
  for (int i = 0; i < X11_NET_REJECT_SLOTS; ++i)
    if (net->rejects[i].fd == fd) {
      if (net->ops.close) net->ops.close(net->ops.io, fd);
      net->rejects[i].fd = -1;
    }
}
#endif

void x11_net_free(x11_net *net) {
  x11_net_close_all(net, "server stopped");
  raw_free(net, net->reply);
  net->reply = 0;
}

static void refuse(x11_net *net, int fd, uint32_t ip, uint16_t port, const char *reason) {
  record(net, net->refusals, &net->refusal_next, ip, port, -1, reason, 0, 0, now_ms(net));
  hook(net, -1, "refused", reason);
  for (int i = 0; i < X11_NET_REJECT_SLOTS; ++i)
    if (net->rejects[i].fd < 0) {
      x11_reject *r = &net->rejects[i];
      r->fd = fd, r->ip = ip, r->port = port, r->since_ms = now_ms(net), r->reason = reason;
      return;
    }
  ++net->n.refused_dropped;  /* no room to answer: closed without a reply */
  if (net->ops.close) net->ops.close(net->ops.io, fd);
}

void x11_net_refuse(x11_net *net, int fd, uint32_t ip, uint16_t port, const char *reason) {
  ++net->n.refused_memory;
  refuse(net, fd, ip, port, reason);
}

void x11_net_accept(x11_net *net, int fd, uint32_t ip, uint16_t port) {
  int slot = x11_client_open(net->s);
  if (slot < 0) {
    ++net->n.refused_slots;
    refuse(net, fd, ip, port, R_NO_SLOT);
    return;
  }
  size_t reserve = x11_net_conn_reserve(net);
  x11_conn *k = &net->conns[slot];
  if (!budget_allows(net, reserve)) {
    x11_client_close(net->s, slot);
    ++net->n.refused_memory;
    refuse(net, fd, ip, port, R_NO_MEMORY);
    return;
  }
  memset(k, 0, sizeof(*k));
  k->in = conn_buf(net, net->cfg.in_min);
  uint8_t *qm = k->in ? conn_buf(net, net->cfg.outq_min) : 0;
  if (!qm) {
    conn_buf_free(net, k->in, net->cfg.in_min);
    k->in = 0;
    k->fd = -1;
    x11_client_close(net->s, slot);
    ++net->n.refused_memory;
    refuse(net, fd, ip, port, R_NO_MEMORY);
    return;
  }
  k->in_cap = net->cfg.in_min;
  x11_outq_init(&k->q, qm, net->cfg.outq_min);
  k->fd = fd, k->slot = slot, k->ip = ip, k->port = port, k->since_ms = k->active_ms = now_ms(net);
  net->n.reserved += reserve;
  ++net->n.accepted;
  hook(net, slot, "accepted", 0);
}

/* ---- requests ------------------------------------------------------------------------------ */

static int room_for_request(const x11_net *net, const x11_conn *k) {
  return k->q.len + net->cfg.outq_reserve <= net->cfg.outq_max;
}

static int over_time(x11_net *net) {
  return net->cfg.request_budget_us && net->ops.now_us && net->ops.now_us(net->ops.io) > net->deadline_us;
}

static uint64_t sat_inc_u64(uint64_t n) { return n == UINT64_MAX ? n : n + 1; }

static uint64_t sat_add_u64(uint64_t a, uint64_t b) { return b > UINT64_MAX - a ? UINT64_MAX : a + b; }

static uint32_t request_u32(const uint8_t *p, int msb) {
  return msb ? ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]
             : ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

static int is_diag_get_property(const x11_net *net, const x11_conn *k) {
  if (k->have < 24 || k->in[0] != 20 ||
      request_u32(k->in + 4, net->s->clients[k->slot].msb) != X11_ID_ROOT_WINDOW)
    return 0;
  const char *name = x11_atom_name(net->s, request_u32(k->in + 8, net->s->clients[k->slot].msb));
  return name && strcmp(name, X11_DIAG_ATOM) == 0;
}

/* While a stream reads directly from a drawable, defer requests that can mutate pixels or
 * window visibility, free a possible pixmap source, or close its owner. Other client queries,
 * properties, selections and input requests continue. */
static int can_run_during_stream(const x11_net *net, const x11_conn *k) {
  if (net->stream_owner < 0) return 1;
  if (net->stream_owner == k->slot || k->have < 4) return 0;
  uint8_t op = k->in[0];
  if (op >= 1 && op <= 14) return 0;       /* window tree/visibility/geometry */
  if (op == 54 || (op >= 61 && op <= 73)) return 0; /* frees, drawing, competing image streams */
  if (op == 113) return 0;                 /* could close the drawable owner */
  return 1;
}

/* Runs buffered requests while there is room for their replies and the pass's time lasts
 * (at least one per call, so every client progresses). Returns 0 to keep, -1 to close. */
static int process(x11_net *net, x11_conn *k) {
  x11_server *s = net->s;
  for (int first = 1;; first = 0) {
    if (k->dead) return -1;
    /* Preserve the image while allowing unrelated clients to continue making progress. */
    if (!can_run_during_stream(net, k)) return 0;
#ifdef X11_LATENCY_TRACE
    if (k->set_up && k->have >= 4 && !k->trace_request_ready_us) {
      size_t pending = x11_request_size(s, k->slot, k->in);
      size_t required = pending ? pending : 4;
      if (k->have >= required && net->ops.now_us)
        k->trace_request_ready_us = net->ops.now_us(net->ops.io);
    }
#endif
    if (!first && over_time(net)) {
      ++net->n.passes_over_budget;
      return 0;
    }
    if (!room_for_request(net, k)) {
      ++net->n.backpressure_waits;
      return 0;
    }
    if (!k->set_up) {
      x11_setup_result r;
      size_t n = x11_client_setup(s, k->slot, k->in, k->have, net->reply, X11_REPLY_MAX, &r);
      if (r.status == X11_SETUP_NEED_MORE) {
        if (r.needed > net->cfg.in_max) {
          k->dead = R_SETUP_TOO_LARGE;
          return -1;
        }
        if (in_reserve(net, k, r.needed) != 0) k->dead = R_OOM;
        return k->dead ? -1 : 0;
      }
      if (n && q_push(net, k, net->reply, n) != 0) {
        k->dead = R_OOM;
        return -1;
      }
      memmove(k->in, k->in + r.consumed, k->have - r.consumed);
      k->have -= r.consumed;
      if (!k->have) k->input_head_since_us = 0;
      if (r.status == X11_SETUP_FAILED) {
        ++net->n.setup_failed;
        record(net, net->refusals, &net->refusal_next, k->ip, k->port, k->slot, r.reason, 0, 0, k->since_ms);
        hook(net, k->slot, "setup failed", r.reason);
        k->dead = R_SETUP_FAILED;
        return -1;
      }
      k->set_up = 1;
      ++net->n.setup_ok;
      hook(net, k->slot, "setup ok", r.msb_first ? "MSB" : "LSB");
      continue;
    }
    if (k->skip) {  /* the rest of a request above the maximum length: discard it as it comes */
      size_t d = k->skip < k->have ? k->skip : k->have;
      memmove(k->in, k->in + d, k->have - d);
      k->have -= d;
      k->skip -= d;
      if (!k->have) k->input_head_since_us = 0;
      if (k->skip) return 0;
      continue;
    }
    if (k->have < 4 || !x11_client_may_run(s, k->slot)) return 0;  /* GrabServer by another */
    size_t need = x11_request_size(s, k->slot, k->in);
    if (!need) {  /* too long: BadLength, and the client stays (as X.Org does) */
      size_t skip;
      size_t n = x11_client_request_too_long(s, k->slot, k->in, net->reply, X11_REPLY_MAX, &skip);
      if (n) {
        ++net->n.requests;
        ++net->n.errors;
        if (net->ops.on_request) net->ops.on_request(net->ops.io, k->slot, k->in, 4, net->reply, n);
        if (q_push(net, k, net->reply, n) != 0) {
          k->dead = R_OOM;
          return -1;
        }
        k->skip = skip;
        continue;
      }
    }
    size_t len = need ? need : 4;  /* length 0 (no BIG-REQUESTS): the 4-byte header gets BadLength */
    if (k->have < len) {
      if (in_reserve(net, k, len) != 0) k->dead = R_OOM;
      return k->dead ? -1 : 0;
    }
    int close_after = 0;
    uint64_t t0 = net->ops.now_us ? net->ops.now_us(net->ops.io) : 0;
    uint8_t opcode = k->in[0];
    int diag_request = is_diag_get_property(net, k);
    if (k->input_head_since_us && net->ops.now_us) {
      uint64_t wait = net->ops.now_us(net->ops.io) - k->input_head_since_us;
      if (wait > net->n.request_buffer_wait_us_max) net->n.request_buffer_wait_us_max = (uint32_t)wait;
      if (diag_request && wait > net->n.diag_buffer_wait_us_max)
        net->n.diag_buffer_wait_us_max = (uint32_t)wait;
#ifdef X11_LATENCY_TRACE
      x11_latency_trace_add(X11_TRACE_INPUT_AGE, wait);
#endif
    }
#ifdef X11_LATENCY_TRACE
    if (net->ops.now_us) {
      uint64_t now = net->ops.now_us(net->ops.io);
      if (k->trace_ready_since_us) {
        x11_latency_trace_add(X11_TRACE_SELECT_READY_DISPATCH, now - k->trace_ready_since_us);
        k->trace_ready_since_us = 0;
      }
      if (k->trace_request_ready_us) {
        x11_latency_trace_add(X11_TRACE_REQUEST_WAIT, now - k->trace_request_ready_us);
        k->trace_request_ready_us = 0;
      }
    }
#endif
#ifdef X11_HOST_TEST_COPY_DELAY
    x11_host_test_before_request(opcode);
#endif
#ifdef X11_HOST_TEST_SCHED_DELAY
    x11_host_test_before_polyfill(opcode);
#endif
    x11_reply_stream local_stream = {0};
    x11_reply_stream *stream = net->stream_owner < 0 ? &net->stream : &local_stream;
    size_t n = x11_client_request_stream(s, k->slot, k->in, len, net->reply, X11_REPLY_MAX, &close_after, stream);
    if (opcode >= 62 && opcode <= 77) net->n.graphics_requests = sat_inc_u64(net->n.graphics_requests);
    if (net->ops.now_us) {
      uint64_t dt = net->ops.now_us(net->ops.io) - t0;
#ifdef X11_LATENCY_TRACE
      x11_latency_trace_add(X11_TRACE_DISPATCH, dt);
      if (diag_request) x11_latency_trace_add(X11_TRACE_DIAG_DISPATCH, dt);
      if (opcode == 70) x11_latency_trace_polyfill_time(dt);
#endif
      if (dt > net->n.request_us_max) net->n.request_us_max = (uint32_t)dt, net->n.request_us_max_op = opcode;
      if (diag_request && dt > net->n.diag_service_us_max) net->n.diag_service_us_max = (uint32_t)dt;
      if (opcode >= 62 && opcode <= 77) {
        net->n.graphics_us_total = sat_add_u64(net->n.graphics_us_total, dt);
        if (dt > net->n.graphics_us_max) net->n.graphics_us_max = (uint32_t)dt;
      }
    }
    ++net->n.requests;
    if (k->pass_requests != UINT16_MAX) ++k->pass_requests;
    if (k->pass_requests > k->pass_requests_max) k->pass_requests_max = k->pass_requests;
    if (n && net->reply[0] == 0) ++net->n.errors;
    if (net->ops.on_request) net->ops.on_request(net->ops.io, k->slot, k->in, len, net->reply, n);
    if (n && q_push(net, k, net->reply, n) != 0) {
      k->dead = R_OOM;
      return -1;
    }
    if (stream->active) net->stream_owner = k->slot;
    memmove(k->in, k->in + len, k->have - len);
    k->have -= len;
    if (!k->have) k->input_head_since_us = 0;
    if (close_after) {
      k->dead = R_PROTOCOL;
      return -1;
    }
  }
}

static int request_ready(const x11_net *net, const x11_conn *k) {
  if (k->fd >= 0 && !k->dead && k->set_up && k->skip && k->have) return 1;  /* bytes to discard */
  if (k->fd < 0 || k->dead || !k->set_up || k->have < 4 || !room_for_request(net, k) ||
      !can_run_during_stream(net, k) ||
      !x11_client_may_run(net->s, k->slot))
    return 0;
  size_t need = x11_request_size(net->s, k->slot, k->in);
  return k->have >= (need ? need : 4);
}

int x11_net_ready(const x11_net *net) {
  if (net->stream_owner >= 0) {
    const x11_conn *k = &net->conns[net->stream_owner];
    size_t free_bytes = k->q.cap - k->q.len;
    if (k->fd >= 0 && ((net->stream.active && free_bytes) || (net->deferred_count && free_bytes >= 32))) return 1;
  }
  for (int i = 0; i < X11_SERVER_CLIENTS; ++i)
    if (request_ready(net, &net->conns[i])) return 1;
  return 0;
}

int x11_net_want_read(const x11_net *net, const x11_conn *k) {
  return k->fd >= 0 && !k->dead && k->have < net->cfg.in_max && room_for_request(net, k);
}

int x11_net_want_write(const x11_net *net, const x11_conn *k) {
  (void)net;
  return k->fd >= 0 && k->q.len > 0;
}

/* Returns 1 when the read filled the buffer (more may be waiting in the socket). */
static int read_some(x11_net *net, x11_conn *k) {
  if (k->have == k->in_cap) {  /* full: grow (up to in_max), else the bytes wait in the socket */
    size_t want = k->in_cap * 2 > net->cfg.in_max ? net->cfg.in_max : k->in_cap * 2;
    if (want <= k->in_cap || in_reserve(net, k, want) != 0) return 0;
  }
  size_t room = k->in_cap - k->have;
  long got = net->ops.recv(net->ops.io, k->fd, k->in + k->have, room);
  if (got > 0) {
    if (!k->have && net->ops.now_us) k->input_head_since_us = net->ops.now_us(net->ops.io);
    k->have += (size_t)got;
    k->active_ms = now_ms(net);
    k->bytes_in += (uint64_t)got;
    net->n.bytes_in += (uint64_t)got;
#ifdef X11_LATENCY_TRACE
    x11_latency_trace_depth(k->have, k->q.len);
#endif
    return (size_t)got == room;
  }
  if (got == 0) k->dead = R_LEFT;
  else if (got == X11_IO_ERROR) k->dead = R_READ;
  return 0;
}

static void flush(x11_net *net, x11_conn *k) {
  const uint8_t *p;
  size_t n;
#ifdef X11_LATENCY_TRACE
  uint64_t flush_t0 = net->ops.now_us ? net->ops.now_us(net->ops.io) : 0;
  int had_queue = k->q.len != 0;
#endif
  while ((n = x11_outq_peek(&k->q, &p)) > 0) {
#ifdef X11_LATENCY_TRACE
    uint64_t send_t0 = net->ops.now_us ? net->ops.now_us(net->ops.io) : 0;
#endif
    long w = net->ops.send(net->ops.io, k->fd, p, n);
#ifdef X11_LATENCY_TRACE
    if (net->ops.now_us) x11_latency_trace_add(X11_TRACE_OUTPUT_SEND_CALL,
                                                 net->ops.now_us(net->ops.io) - send_t0);
#endif
    if (w == X11_IO_WOULD_BLOCK) {
      net->n.write_would_block = sat_inc_u64(net->n.write_would_block);
      if (k->q.len > net->n.write_stall_queue_max) net->n.write_stall_queue_max = (uint32_t)k->q.len;
#ifdef X11_LATENCY_TRACE
      trace_flush_done(net, flush_t0, had_queue);
#endif
      return;
    }
    if (w < 0) {
      if (!k->dead) k->dead = R_WRITE;
#ifdef X11_LATENCY_TRACE
      trace_flush_done(net, flush_t0, had_queue);
#endif
      return;
    }
    if ((size_t)w < n) net->n.write_partial = sat_inc_u64(net->n.write_partial);
    x11_outq_pop(&k->q, (size_t)w);
#ifdef X11_LATENCY_TRACE
    if (k->trace_output_since_us && !k->trace_output_first_us && net->ops.now_us) {
      k->trace_output_first_us = net->ops.now_us(net->ops.io);
      x11_latency_trace_add(X11_TRACE_OUTPUT_FIRST, k->trace_output_first_us - k->trace_output_since_us);
    }
#endif
    k->active_ms = now_ms(net);
    k->bytes_out += (uint64_t)w;
    net->n.bytes_out += (uint64_t)w;
  }
#ifdef X11_LATENCY_TRACE
  if (!k->q.len && k->trace_output_since_us && net->ops.now_us) {
    x11_latency_trace_add(X11_TRACE_OUTPUT_DRAIN, net->ops.now_us(net->ops.io) - k->trace_output_since_us);
    k->trace_output_since_us = k->trace_output_first_us = 0;
  }
  trace_flush_done(net, flush_t0, had_queue);
#endif
}

/* Generate one bounded payload block per pass. The X request loop remains available on
 * subsequent passes; platform touch polling and LCD work run between passes. */
static void stream_pass(x11_net *net, x11_conn *k) {
  if (net->stream_owner != k->slot || k->dead) return;
  if (net->stream.active) {
    size_t want = net->stream.total - net->stream.offset;
    if (want > sizeof(net->stream_buf)) want = sizeof(net->stream_buf);
    size_t available = k->q.cap - k->q.len;
    if (want > available) want = available;
    if (want) {
      size_t n = x11_reply_stream_read(&net->stream, net->stream_buf, want);
      if (n && q_push(net, k, net->stream_buf, n) != 0) {
        k->dead = R_OOM;
        return;
      }
    }
  }
  if (!net->stream.active) {
    while (net->deferred_count) {
      uint8_t *event = net->deferred_events[net->deferred_head];
      if (q_push(net, k, event, 32) != 0) break; /* retry after socket drains */
      net->deferred_head = (uint8_t)((net->deferred_head + 1) % X11_NET_DEFERRED_EVENTS);
      --net->deferred_count;
    }
    if (!net->deferred_count) {
      net->stream_owner = -1;
      net->deferred_head = 0;
      memset(&net->stream, 0, sizeof(net->stream));
    }
  }
}

static void serve_rejects(x11_net *net, int (*ready)(void *io, int fd, int write)) {
  for (int i = 0; i < X11_NET_REJECT_SLOTS; ++i) {
    x11_reject *r = &net->rejects[i];
    if (r->fd < 0) continue;
    int done = now_ms(net) - r->since_ms > net->cfg.reject_wait_ms;
    if (!done && ready(net->ops.io, r->fd, 0)) {
      uint8_t in[12], out[64];
      long got = net->ops.recv(net->ops.io, r->fd, in, sizeof(in));
      if (got > 0) {
        size_t n = x11_setup_refuse(in, (size_t)got, r->reason, out, sizeof(out));
        if (n) net->ops.send(net->ops.io, r->fd, out, n);  /* best effort: small, fits the socket */
      }
      done = got != X11_IO_WOULD_BLOCK;
    }
    if (done) {
      if (net->ops.close) net->ops.close(net->ops.io, r->fd);
      r->fd = -1;
    }
  }
}

void x11_net_pass(x11_net *net, int (*ready)(void *io, int fd, int write)) {
  static unsigned rotate = 0;  /* fair start: a different first client each pass */
  unsigned start = rotate++ % X11_SERVER_CLIENTS;
  uint64_t pass_t0 = net->ops.now_us ? net->ops.now_us(net->ops.io) : 0;
  uint64_t requests_before = net->n.requests;
  for (int i = 0; i < X11_SERVER_CLIENTS; ++i) net->conns[i].pass_requests = 0;
  if (net->cfg.request_budget_us && net->ops.now_us)
    net->deadline_us = net->ops.now_us(net->ops.io) + net->cfg.request_budget_us;
  uint32_t now = now_ms(net);
  for (unsigned n = 0; n < X11_SERVER_CLIENTS; ++n) {
    x11_conn *k = &net->conns[(start + n) % X11_SERVER_CLIENTS];
    if (k->fd < 0) continue;
#ifdef X11_LATENCY_TRACE
    uint64_t visit_t0 = net->ops.now_us ? net->ops.now_us(net->ops.io) : 0;
    int socket_ready = !k->dead && ready(net->ops.io, k->fd, 0);
    if (socket_ready && x11_net_want_read(net, k)) {
#else
    if (!k->dead && ready(net->ops.io, k->fd, 0) && x11_net_want_read(net, k)) {
#endif
#ifdef X11_LATENCY_TRACE
      if (!k->trace_ready_since_us && net->ops.now_us)
        k->trace_ready_since_us = net->ops.read_ready_since_us
            ? net->ops.read_ready_since_us(net->ops.io, k->fd) : net->ops.now_us(net->ops.io);
#endif
      /* read and run until the socket is empty (small buffers must not split a client's
       * burst across passes), the client must wait, or the pass's time is up */
      for (int i = 0; i < 256; ++i) {
#ifdef X11_LATENCY_TRACE
        if (!k->trace_ready_since_us && net->ops.now_us)
          k->trace_ready_since_us = net->ops.read_ready_since_us
              ? net->ops.read_ready_since_us(net->ops.io, k->fd) : net->ops.now_us(net->ops.io);
#endif
        int more = read_some(net, k);
        if (!k->dead) process(net, k);
        if (!more || k->dead || !x11_net_want_read(net, k) || over_time(net)) break;
      }
    } else if (!k->dead) {
      process(net, k);
    }
    if (!k->dead && !k->set_up && net->cfg.setup_timeout_ms && now - k->since_ms > net->cfg.setup_timeout_ms)
      k->dead = R_SETUP_TIMEOUT;
#ifdef X11_LATENCY_TRACE
    if (!k->dead && net->ops.now_us)
      x11_latency_trace_client_visit((unsigned)k->slot, visit_t0,
                                     k->pass_requests, k->have, k->q.len);
#endif
  }
  for (int i; (i = x11_take_kill(net->s)) >= 0;)  /* KillClient */
    if (net->conns[i].fd >= 0 && !net->conns[i].dead) net->conns[i].dead = R_KILL;
  /* send queued output (also events this pass queued for other clients), then close */
  for (int i = 0; i < X11_SERVER_CLIENTS; ++i) {
    x11_conn *k = &net->conns[i];
    if (k->fd < 0) continue;
    flush(net, k);
    if (!k->dead && net->stream_owner == k->slot) {
      stream_pass(net, k);
      flush(net, k);
      if (k->dead) conn_close(net, k, k->dead);
      continue;
    }
    if (!k->dead && k->have && room_for_request(net, k) && !over_time(net)) {  /* resume after backpressure */
      process(net, k);
      flush(net, k);
    }
    if (k->dead && net->stream_owner >= 0 && net->stream_owner != k->slot) {
      /* Defer teardown: an in-flight pixmap stream may reference this client's resource. */
    } else if (k->dead) conn_close(net, k, k->dead);
    else shrink(net, k);
  }
  serve_rejects(net, ready);
  uint64_t pass_requests = net->n.requests - requests_before;
  if (pass_requests > net->n.requests_pass_max) net->n.requests_pass_max = (uint32_t)pass_requests;
  for (int i = 0; i < X11_SERVER_CLIENTS; ++i)
    if (net->conns[i].pass_requests > net->n.client_requests_pass_max)
      net->n.client_requests_pass_max = net->conns[i].pass_requests;
  if (net->ops.now_us) {
    uint64_t dt = net->ops.now_us(net->ops.io) - pass_t0;
#ifdef X11_LATENCY_TRACE
    x11_latency_trace_add(X11_TRACE_NET_PASS, dt);
#endif
    if (dt > net->n.pass_us_max) net->n.pass_us_max = (uint32_t)dt;
    if (net->cfg.request_budget_us && dt > net->cfg.request_budget_us) {
      net->n.passes_exceeded_request_budget = sat_inc_u64(net->n.passes_exceeded_request_budget);
      uint64_t over = dt - net->cfg.request_budget_us;
      if (over > net->n.request_budget_over_us_max) net->n.request_budget_over_us_max = (uint32_t)over;
    }
  }
}

int x11_net_open(const x11_net *net) {
  int n = 0;
  for (int i = 0; i < X11_SERVER_CLIENTS; ++i) n += net->conns[i].fd >= 0;
  return n;
}

/* ---- diagnostics --------------------------------------------------------------------------- */

typedef struct {
  char *p;
  size_t n, cap;
} sbuf;

static void put(sbuf *b, const char *fmt, ...) {
  if (b->n + 1 >= b->cap) return;
  va_list ap;
  va_start(ap, fmt);
  int w = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
  va_end(ap);
  if (w < 0) return;
  b->n += (size_t)w < b->cap - b->n ? (size_t)w : b->cap - b->n - 1;
}

static void put_ip(sbuf *b, uint32_t ip, uint16_t port) {
  put(b, "%u.%u.%u.%u:%u", ip >> 24 & 255, ip >> 16 & 255, ip >> 8 & 255, ip & 255, port);
}

static void put_records(sbuf *b, const char *name, const x11_net_record *ring, unsigned next) {
  unsigned n = next < X11_NET_HISTORY ? next : X11_NET_HISTORY;
  for (unsigned i = 0; i < n; ++i) {
    const x11_net_record *r = &ring[(next - 1 - i) % X11_NET_HISTORY];  /* newest first */
    put(b, "net.%s.%u=t_ms=%lu peer=", name, i, (unsigned long)r->t_ms);
    put_ip(b, r->ip, r->port);
    put(b, " slot=%d reason=\"%s\" requests=%lu errors=%lu seconds=%lu\n", r->slot, r->reason ? r->reason : "",
        (unsigned long)r->requests, (unsigned long)r->errors, (unsigned long)r->seconds);
  }
}

size_t x11_net_stats(const x11_net *net, char *out, size_t cap) {
  sbuf b = {out, 0, cap};
  if (cap) out[0] = 0;
  const x11_net_counters *c = &net->n;
  size_t qbytes = 0, qpeak = 0, inbytes = 0;
  int open = 0, set_up = 0;
  for (int i = 0; i < X11_SERVER_CLIENTS; ++i) {
    const x11_conn *k = &net->conns[i];
    if (k->fd < 0) continue;
    ++open;
    set_up += k->set_up;
    qbytes += k->q.len;
    inbytes += k->have;
    if (k->q.peak > qpeak) qpeak = k->q.peak;
  }
  put(&b, "net.open=%d\nnet.set_up=%d\nnet.accepted=%lu\nnet.refused_slots=%lu\nnet.refused_memory=%lu\n"
          "net.refused_dropped=%lu\nnet.setup_ok=%lu\nnet.setup_failed=%lu\nnet.setup_timeouts=%lu\n",
      open, set_up, (unsigned long)c->accepted, (unsigned long)c->refused_slots, (unsigned long)c->refused_memory,
      (unsigned long)c->refused_dropped, (unsigned long)c->setup_ok, (unsigned long)c->setup_failed,
      (unsigned long)c->setup_timeouts);
  put(&b, "net.closed_left=%lu\nnet.closed_read_error=%lu\nnet.closed_write_error=%lu\nnet.closed_protocol=%lu\n"
          "net.closed_overflow=%lu\nnet.closed_kill=%lu\nnet.closed_oom=%lu\n",
      (unsigned long)c->closed_left, (unsigned long)c->closed_read_error, (unsigned long)c->closed_write_error,
      (unsigned long)c->closed_protocol, (unsigned long)c->closed_overflow, (unsigned long)c->closed_kill,
      (unsigned long)c->closed_oom);
  put(&b, "net.bytes_in=%llu\nnet.bytes_out=%llu\nnet.requests=%llu\nnet.errors=%llu\nnet.events=%llu\n"
          "net.backpressure_waits=%lu\nnet.passes_over_budget=%lu\n"
          "net.request_us_max=%lu\nnet.request_us_max_op=%u\nnet.pass_us_max=%lu\n",
      (unsigned long long)c->bytes_in, (unsigned long long)c->bytes_out, (unsigned long long)c->requests,
      (unsigned long long)c->errors, (unsigned long long)c->events, (unsigned long)c->backpressure_waits,
      (unsigned long)c->passes_over_budget, (unsigned long)c->request_us_max, (unsigned)c->request_us_max_op,
      (unsigned long)c->pass_us_max);
  put(&b, "net.requests_pass_max=%lu\nnet.client_requests_pass_max=%lu\n"
          "net.passes_exceeded_request_budget=%llu\nnet.request_budget_over_us_max=%lu\n"
          "net.graphics_requests=%llu\nnet.graphics_us_total=%llu\nnet.graphics_us_max=%lu\n"
          "net.write_would_block=%llu\nnet.write_partial=%llu\nnet.write_stall_queue_max=%lu\n"
          "net.request_buffer_wait_us_max=%lu\nnet.diag_buffer_wait_us_max=%lu\nnet.diag_service_us_max=%lu\n",
      (unsigned long)c->requests_pass_max, (unsigned long)c->client_requests_pass_max,
      (unsigned long long)c->passes_exceeded_request_budget, (unsigned long)c->request_budget_over_us_max,
      (unsigned long long)c->graphics_requests, (unsigned long long)c->graphics_us_total,
      (unsigned long)c->graphics_us_max, (unsigned long long)c->write_would_block,
      (unsigned long long)c->write_partial, (unsigned long)c->write_stall_queue_max,
      (unsigned long)c->request_buffer_wait_us_max, (unsigned long)c->diag_buffer_wait_us_max,
      (unsigned long)c->diag_service_us_max);
  put(&b, "net.budget=%lu\nnet.reserve_per_conn=%lu\nnet.reserved=%lu\nnet.core_used=%lu\nnet.core_peak=%lu\n"
          "net.conn_bytes=%lu\nnet.conn_bytes_peak=%lu\nnet.total_peak=%lu\nnet.queue_bytes=%lu\n"
          "net.queue_peak_max=%lu\nnet.input_bytes=%lu\nnet.core_alloc_failed=%lu\nnet.conn_alloc_failed=%lu\n",
      (unsigned long)net->cfg.budget, (unsigned long)x11_net_conn_reserve(net), (unsigned long)c->reserved,
      (unsigned long)c->core_used, (unsigned long)c->core_peak, (unsigned long)c->conn_bytes,
      (unsigned long)c->conn_bytes_peak, (unsigned long)c->total_peak, (unsigned long)qbytes, (unsigned long)qpeak,
      (unsigned long)inbytes, (unsigned long)c->core_alloc_failed, (unsigned long)c->conn_alloc_failed);
  int shown = 0;
  for (int i = 0; i < X11_SERVER_CLIENTS; ++i) {
    const x11_conn *k = &net->conns[i];
    if (k->fd < 0) continue;
    if (shown == 32) {
      put(&b, "net.conn_more=%d\n", open - shown);
      break;
    }
    ++shown;
    const x11_client *cl = &net->s->clients[k->slot];
    put(&b, "net.conn.%d=peer=", k->slot);
    put_ip(&b, k->ip, k->port);
    put(&b, " setup=%u age_s=%lu requests=%lu errors=%lu resources=%u in=%lu/%lu queue=%lu/%lu peak=%lu pass_reqs=%u pass_reqs_max=%u"
        "\n", k->set_up,
        (unsigned long)((now_ms((x11_net *)net) - k->since_ms) / 1000), (unsigned long)cl->requests,
        (unsigned long)cl->errors, (unsigned)cl->resources, (unsigned long)k->have, (unsigned long)k->in_cap,
        (unsigned long)k->q.len, (unsigned long)k->q.cap, (unsigned long)k->q.peak,
        (unsigned)k->pass_requests, (unsigned)k->pass_requests_max
        );
  }
  put_records(&b, "close", net->closes, net->close_next);
  put_records(&b, "refusal", net->refusals, net->refusal_next);
#ifdef X11_LATENCY_TRACE
  if (b.n < b.cap)
    b.n += x11_latency_trace_stats(b.p + b.n, b.cap - b.n);
#endif
  return b.n;
}
