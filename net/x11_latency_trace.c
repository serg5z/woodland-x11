/* Bounded timing and queue diagnostics for the test-only ESP32 runtime profile. */
#ifdef X11_LATENCY_TRACE
#include "x11_latency_trace.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifndef X11_SERVER_CLIENTS
#define X11_SERVER_CLIENTS 64
#endif

#define TRACE_BUCKETS 33
#define TRACE_REQUEST_BUCKETS 10

typedef struct {
  uint32_t bucket[TRACE_BUCKETS];
  uint32_t max_us;
  uint64_t count, total_us;
} trace_hist;

static const char *const metric_names[X11_TRACE_METRICS] = {
    "select_ready_dispatch", "request_wait", "input_age", "dispatch", "diag_dispatch",
    "output_first", "output_drain", "output_send_call", "output_flush", "client_visit",
    "net_pass", "select_wait", "loop_gap", "polyfill"};
static trace_hist hist[X11_TRACE_METRICS];
static uint64_t last_visit_us[X11_SERVER_CLIENTS];
static uint32_t request_pass[TRACE_REQUEST_BUCKETS];
static uint32_t input_depth_max, output_depth_max, polyfill_rectangles_max;
static uint64_t polyfill_requests, polyfill_rectangles, polyfill_pixels;
static uint64_t active_polyfill_pixels;
static unsigned active_polyfill_rectangles;
static uint8_t polyfill_active, polyfill_time_pending;

static uint32_t sat_inc(uint32_t n) { return n == UINT32_MAX ? n : n + 1; }
static uint64_t sat_add(uint64_t a, uint64_t b) { return b > UINT64_MAX - a ? UINT64_MAX : a + b; }

static unsigned bucket_for(uint64_t us) {
  unsigned b = 0;
  while (b + 1 < TRACE_BUCKETS && us >= ((uint64_t)1 << b)) ++b;
  return b;
}

static void hist_add(trace_hist *h, uint64_t us) {
  unsigned b = bucket_for(us);
  h->bucket[b] = sat_inc(h->bucket[b]);
  h->count = sat_add(h->count, 1);
  h->total_us = sat_add(h->total_us, us);
  if (us > h->max_us) h->max_us = us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
}

static uint32_t percentile_us(const trace_hist *h, unsigned pct) {
  if (!h->count) return 0;
  uint64_t rank = (h->count / 100) * pct + ((h->count % 100) * pct + 99) / 100;
  uint64_t seen = 0;
  for (unsigned b = 0; b < TRACE_BUCKETS; ++b) {
    seen += h->bucket[b];
    if (seen >= rank) {
      if (b == 0) return 0;
      if (b >= TRACE_BUCKETS - 1) return h->max_us;
      uint64_t upper = ((uint64_t)1 << b) - 1;
      return upper > h->max_us ? h->max_us : (uint32_t)upper;
    }
  }
  return h->max_us;
}

void x11_latency_trace_reset(void) {
  memset(hist, 0, sizeof(hist));
  memset(last_visit_us, 0, sizeof(last_visit_us));
  memset(request_pass, 0, sizeof(request_pass));
  input_depth_max = output_depth_max = polyfill_rectangles_max = 0;
  polyfill_requests = polyfill_rectangles = polyfill_pixels = active_polyfill_pixels = 0;
  active_polyfill_rectangles = 0;
  polyfill_active = polyfill_time_pending = 0;
}

void x11_latency_trace_client_reset(unsigned slot) {
  if (slot < X11_SERVER_CLIENTS) last_visit_us[slot] = 0;
}

void x11_latency_trace_add(unsigned metric, uint64_t elapsed_us) {
  if (metric < X11_TRACE_METRICS) hist_add(&hist[metric], elapsed_us);
}

void x11_latency_trace_client_visit(unsigned slot, uint64_t now_us, unsigned requests,
                                    size_t input_bytes, size_t output_bytes) {
  if (slot < X11_SERVER_CLIENTS) {
    if (last_visit_us[slot] && now_us >= last_visit_us[slot])
      hist_add(&hist[X11_TRACE_CLIENT_VISIT], now_us - last_visit_us[slot]);
    last_visit_us[slot] = now_us;
  }
  unsigned b = requests >= TRACE_REQUEST_BUCKETS - 1 ? TRACE_REQUEST_BUCKETS - 1 : requests;
  request_pass[b] = sat_inc(request_pass[b]);
  x11_latency_trace_depth(input_bytes, output_bytes);
}

void x11_latency_trace_depth(size_t input_bytes, size_t output_bytes) {
  if (input_bytes > input_depth_max)
    input_depth_max = input_bytes > UINT32_MAX ? UINT32_MAX : (uint32_t)input_bytes;
  if (output_bytes > output_depth_max)
    output_depth_max = output_bytes > UINT32_MAX ? UINT32_MAX : (uint32_t)output_bytes;
}

void x11_latency_trace_polyfill_begin(unsigned rectangles) {
  active_polyfill_rectangles = rectangles;
  active_polyfill_pixels = 0;
  polyfill_time_pending = 0;
  polyfill_active = 1;
}

void x11_latency_trace_polyfill_pixels(uint64_t pixels) {
  if (polyfill_active) active_polyfill_pixels = sat_add(active_polyfill_pixels, pixels);
}

void x11_latency_trace_polyfill_end(void) {
  if (!polyfill_active) return;
  polyfill_active = 0;
  polyfill_requests = sat_add(polyfill_requests, 1);
  polyfill_rectangles = sat_add(polyfill_rectangles, active_polyfill_rectangles);
  polyfill_pixels = sat_add(polyfill_pixels, active_polyfill_pixels);
  if (active_polyfill_rectangles > polyfill_rectangles_max)
    polyfill_rectangles_max = active_polyfill_rectangles;
  polyfill_time_pending = 1;
}

void x11_latency_trace_polyfill_time(uint64_t elapsed_us) {
  if (polyfill_time_pending) {
    hist_add(&hist[X11_TRACE_POLYFILL], elapsed_us);
    polyfill_time_pending = 0;
  }
}

static size_t append(char *out, size_t cap, size_t used, const char *fmt, ...) {
  if (used >= cap) return used;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(out + used, cap - used, fmt, ap);
  va_end(ap);
  if (n < 0) return used;
  size_t avail = cap - used;
  return used + ((size_t)n < avail ? (size_t)n : avail - 1);
}

size_t x11_latency_trace_stats(char *out, size_t cap) {
  size_t used = 0;
  if (cap) out[0] = 0;
  for (unsigned i = 0; i < X11_TRACE_METRICS; ++i) {
    const trace_hist *h = &hist[i];
    used = append(out, cap, used,
        "net.trace.%s.n=%llu\nnet.trace.%s.p50_us=%lu\nnet.trace.%s.p95_us=%lu\n"
        "net.trace.%s.p99_us=%lu\nnet.trace.%s.max_us=%lu\nnet.trace.%s.mean_us=%llu\n",
        metric_names[i], (unsigned long long)h->count,
        metric_names[i], (unsigned long)percentile_us(h, 50),
        metric_names[i], (unsigned long)percentile_us(h, 95),
        metric_names[i], (unsigned long)percentile_us(h, 99),
        metric_names[i], (unsigned long)h->max_us,
        metric_names[i], (unsigned long long)(h->count ? h->total_us / h->count : 0));
  }
  used = append(out, cap, used,
      "net.trace.input_depth_max=%lu\nnet.trace.output_depth_max=%lu\n"
      "net.trace.polyfill_requests=%llu\nnet.trace.polyfill_rectangles=%llu\n"
      "net.trace.polyfill_rectangles_max=%lu\nnet.trace.polyfill_clipped_pixels=%llu\n",
      (unsigned long)input_depth_max, (unsigned long)output_depth_max,
      (unsigned long long)polyfill_requests, (unsigned long long)polyfill_rectangles,
      (unsigned long)polyfill_rectangles_max, (unsigned long long)polyfill_pixels);
  used = append(out, cap, used, "net.trace.static_bytes=%lu\n",
                (unsigned long)(sizeof(hist) + sizeof(last_visit_us) + sizeof(request_pass) +
                                sizeof(input_depth_max) + sizeof(output_depth_max) +
                                sizeof(polyfill_rectangles_max) + sizeof(polyfill_requests) +
                                sizeof(polyfill_rectangles) + sizeof(polyfill_pixels) +
                                sizeof(active_polyfill_pixels) + sizeof(active_polyfill_rectangles) +
                                sizeof(polyfill_active) + sizeof(polyfill_time_pending)));
  for (unsigned i = 0; i < TRACE_REQUEST_BUCKETS; ++i)
    used = append(out, cap, used, "net.trace.requests_per_client_pass.%u=%lu\n", i,
                  (unsigned long)request_pass[i]);
  return used;
}
#endif  /* X11_LATENCY_TRACE */
