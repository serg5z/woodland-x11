/* X11 connections over any stream socket API (portable C99): the part of the transport that
 * the host server and the ESP32 share. The platform owns the listening socket and select();
 * this module owns everything per connection:
 *
 *   - admission: a client slot (x11_client_open) and its minimum input/output buffers from one
 *     global memory budget; larger connection buffers and core objects are charged as allocated.
 *     Otherwise Setup Failed ("too many clients", "server memory exhausted") is sent and the
 *     connection is closed, never just dropped;
 *   - lazy, bounded buffers: the input buffer and the output queue start small and grow
 *     only as far as the client needs (up to in_max / outq_max), and shrink when idle;
 *   - the core's allocations (windows, pixmaps, properties...) come from the same budget,
 *     so exhausting it gives BadAlloc to the client that asked, not a crash;
 *   - request processing with backpressure, an optional time budget per pass, events,
 *     KillClient, close reasons, and counters for diagnostics (caller address and reason of
 *     the last closes and refusals; never authorization data).
 *
 * Memory model: each admitted connection reserves conn_reserve() bytes for its initial buffers.
 * Larger buffers and core allocations are charged as they happen. The connection reservation
 * overlaps its initial buffers, so only the larger of the reservation total or actual connection
 * buffer bytes is charged. Allocation failure is reported to the requesting connection. */
#ifndef X11_NET_H
#define X11_NET_H

#include <stddef.h>
#include <stdint.h>

#include "../core/x11_server.h"
#include "x11_outq.h"
#ifdef X11_LATENCY_TRACE
#include "x11_latency_trace.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define X11_NET_HISTORY 8        /* last closes and last refusals kept for diagnostics */
#define X11_NET_DEFERRED_EVENTS 64 /* events held behind an in-flight reply stream */
#ifndef X11_NET_REJECT_SLOTS
#define X11_NET_REJECT_SLOTS 4   /* connections being refused at once (each holds a socket) */
#endif
#define X11_NET_IDLE_MS 2000     /* a connection idle this long gets its buffers back to the minimum */

/* I/O results */
enum { X11_IO_WOULD_BLOCK = -1, X11_IO_ERROR = -2 };

typedef struct {
  /* > 0: bytes; 0: end of stream (recv); X11_IO_WOULD_BLOCK; X11_IO_ERROR */
  long (*recv)(void *io, int fd, uint8_t *buf, size_t n);
  long (*send)(void *io, int fd, const uint8_t *buf, size_t n);
  void (*close)(void *io, int fd);
  uint32_t (*now_ms)(void *io);
  uint64_t (*now_us)(void *io);       /* for the request time budget; NULL: no time budget */
#ifdef X11_LATENCY_TRACE
  uint64_t (*read_ready_since_us)(void *io, int fd); /* timestamp of select result; optional */
#endif
  void *(*alloc)(void *io, size_t n); /* NULL: malloc */
  void (*release)(void *io, void *p); /* NULL: free */
  /* Optional log hooks (NULL: silent). reason/what are static strings. */
  void (*on_event)(void *io, int slot, const char *what, const char *detail);
  void (*on_request)(void *io, int slot, const uint8_t *req, size_t len, const uint8_t *out, size_t n);
  void (*on_deliver)(void *io, int slot, const uint8_t *ev, size_t len);  /* an event was queued */
  void *io;
} x11_net_ops;

typedef struct {
  size_t in_min, in_max;         /* input buffer: initial, largest (one maximum request) */
  size_t outq_min, outq_max;     /* output queue: initial (kept while idle), largest */
  size_t outq_reserve;           /* room needed before running a request (largest reply + events) */
  size_t budget;                 /* bytes for connection buffers and core allocations; 0: unlimited */
  uint32_t request_budget_us;    /* request processing per pass (needs now_us); 0: unlimited */
  uint32_t setup_timeout_ms;     /* a connection must finish setup within this; 0: no limit */
  uint32_t reject_wait_ms;       /* how long a refused connection may take to send its first byte */
} x11_net_config;

/* Defaults for a platform: in 256..(4*max_request_units+64), outq 1 KiB..outq_max. */
void x11_net_default_config(x11_net_config *cfg, size_t outq_max, size_t budget);

typedef struct {
  uint32_t t_ms;
  uint32_t ip;                   /* IPv4, host order; 0 unknown */
  uint16_t port;
  int16_t slot;                  /* -1: never had one */
  const char *reason;
  uint32_t requests, errors;
  uint32_t seconds;              /* connected for */
} x11_net_record;

typedef struct {
  int fd;                        /* -1: free */
  int slot;
  uint8_t set_up;
  const char *dead;              /* set: close at the end of this pass, with this reason */
  uint8_t *in;
  size_t in_cap, have;
  size_t skip;                   /* bytes still to discard of a request above the maximum length */
  x11_outq q;
  uint32_t ip;
  uint16_t port;
  uint32_t since_ms;
  uint32_t active_ms;            /* last read or write: buffers shrink after X11_NET_IDLE_MS */
  uint64_t bytes_in, bytes_out;
  uint64_t input_head_since_us;   /* first byte currently buffered in the input queue */
  uint16_t pass_requests, pass_requests_max;
#ifdef X11_LATENCY_TRACE
  uint64_t trace_ready_since_us, trace_request_ready_us;
  uint64_t trace_output_since_us, trace_output_first_us;
#endif
} x11_conn;

typedef struct {
  int fd;                        /* -1: free */
  uint32_t ip;
  uint16_t port;
  uint32_t since_ms;
  const char *reason;
} x11_reject;

typedef struct {
  /* admission */
  uint32_t accepted, refused_slots, refused_memory, refused_dropped;
  uint32_t setup_ok, setup_failed, setup_timeouts;
  /* closes */
  uint32_t closed_left, closed_read_error, closed_write_error, closed_protocol, closed_overflow, closed_kill,
      closed_oom;
  /* traffic, cumulative (open and closed connections) */
  uint64_t bytes_in, bytes_out, requests, errors, events;
  uint32_t backpressure_waits;   /* passes a client waited for its queue to drain */
  uint32_t passes_over_budget;   /* passes that stopped on the request time budget */
  /* time (needs ops.now_us; all-time maxima): the slowest single request and its major
   * opcode, and the slowest x11_net_pass (requests, reads, writes) */
  uint32_t request_us_max, pass_us_max;
  uint8_t request_us_max_op;
  /* Bounded load diagnostics. These waits measure from first byte received into the input
   * buffer to dispatch; they do not include Wi-Fi transit or unread lwIP socket data. */
  uint32_t requests_pass_max, client_requests_pass_max;
  uint64_t passes_exceeded_request_budget;
  uint32_t request_budget_over_us_max;
  uint64_t graphics_requests, graphics_us_total;
  uint32_t graphics_us_max;
  uint64_t write_would_block, write_partial;
  uint32_t write_stall_queue_max, request_buffer_wait_us_max, diag_buffer_wait_us_max, diag_service_us_max;
  /* memory */
  size_t reserved, core_used, core_peak, conn_bytes, conn_bytes_peak, total_peak;
  uint32_t core_alloc_failed, conn_alloc_failed;
} x11_net_counters;

typedef struct {
  x11_server *s;
  x11_net_config cfg;
  x11_net_ops ops;
  x11_conn conns[X11_SERVER_CLIENTS];
  x11_reject rejects[X11_NET_REJECT_SLOTS];
  uint8_t *reply;                /* X11_REPLY_MAX, shared by all connections */
  uint8_t stream_buf[X11_REPLY_STREAM_CHUNK]; /* bounded scratch shared by the single active stream */
  x11_reply_stream stream;
  int stream_owner;              /* slot whose reply owns the stream, -1 when idle */
  uint8_t deferred_head, deferred_count;
  uint8_t deferred_events[X11_NET_DEFERRED_EVENTS][32];
  x11_net_counters n;
  x11_net_record closes[X11_NET_HISTORY], refusals[X11_NET_HISTORY];
  unsigned close_next, refusal_next;
  uint64_t deadline_us;
} x11_net;

/* Sets up the transport and fills in plat.alloc/release/deliver/ctx so the core uses the
 * budget and delivers events through the connections; then call x11_server_init_platform
 * with that plat. Returns -1 when the shared reply buffer cannot be allocated. */
int x11_net_init(x11_net *net, x11_server *s, const x11_net_config *cfg, const x11_net_ops *ops, x11_platform *plat);
void x11_net_free(x11_net *net);

/* A connection the platform accepted (nonblocking socket). Takes ownership of fd: the
 * connection is admitted, or refused with a Setup Failed reply, or closed. */
void x11_net_accept(x11_net *net, int fd, uint32_t ip, uint16_t port);

/* Refuses a connection for a platform reason (e.g. internal RAM low): Setup Failed with
 * `reason` (a static string), counted as refused_memory. Takes ownership of fd. */
void x11_net_refuse(x11_net *net, int fd, uint32_t ip, uint16_t port, const char *reason);

/* What the platform's select() should wait for. */
int x11_net_want_read(const x11_net *net, const x11_conn *k);
int x11_net_want_write(const x11_net *net, const x11_conn *k);
/* A connection has a complete request that can run now: do not wait in select(). */
int x11_net_ready(const x11_net *net);

/* One pass after select(): `ready(io, fd, write)` tells whether fd is readable (write 0) or
 * writable (write 1). Reads, runs requests (backpressure, time budget), sends, closes. */
void x11_net_pass(x11_net *net, int (*ready)(void *io, int fd, int write));

/* Closes every connection (reason), e.g. when the network goes away. */
void x11_net_close_all(x11_net *net, const char *reason);
#ifdef X11_SPI_X11
/* Close one transport endpoint, including a pending Setup Failed refusal. */
void x11_net_close_fd(x11_net *net, int fd, const char *reason);
#endif

/* Open connections, and the memory they hold now. */
int x11_net_open(const x11_net *net);
size_t x11_net_conn_reserve(const x11_net *net);  /* initial input + output buffers per connection */

/* "net.*" key=value lines (counters, memory, per-connection lines, last closes and refusals)
 * into out (NUL-terminated); returns the length. No authorization data is ever included. */
size_t x11_net_stats(const x11_net *net, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
