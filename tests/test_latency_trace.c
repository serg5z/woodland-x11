#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../net/x11_latency_trace.h"

static int failures;
#define CHECK(v) do { if (!(v)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #v); ++failures; } } while (0)

static const char *value(const char *text, const char *key) {
  static char result[64];
  char needle[96];
  snprintf(needle, sizeof(needle), "%s=", key);
  const char *p = strstr(text, needle);
  if (!p) return NULL;
  p += strlen(needle);
  size_t n = strcspn(p, "\n");
  if (n >= sizeof(result)) n = sizeof(result) - 1;
  memcpy(result, p, n); result[n] = 0;
  return result;
}

int main(void) {
  char out[8192];
  x11_latency_trace_reset();
  for (uint32_t i = 1; i <= 100; ++i) x11_latency_trace_add(X11_TRACE_DISPATCH, i);
  CHECK(x11_latency_trace_stats(out, sizeof(out)) < sizeof(out));
  CHECK(!strcmp(value(out, "net.trace.dispatch.n"), "100"));
  CHECK(!strcmp(value(out, "net.trace.dispatch.p50_us"), "63"));
  CHECK(!strcmp(value(out, "net.trace.dispatch.p95_us"), "100"));
  CHECK(!strcmp(value(out, "net.trace.dispatch.p99_us"), "100"));
  CHECK(!strcmp(value(out, "net.trace.dispatch.max_us"), "100"));

  x11_latency_trace_reset();
  x11_latency_trace_add(X11_TRACE_DISPATCH, UINT32_MAX);
  x11_latency_trace_stats(out, sizeof(out));
  CHECK(!strcmp(value(out, "net.trace.dispatch.p50_us"), "4294967295"));
  CHECK(!strcmp(value(out, "net.trace.dispatch.p99_us"), "4294967295"));
  CHECK(!strcmp(value(out, "net.trace.static_bytes"), "2730"));

  x11_latency_trace_client_visit(3, 1000, 0, 32, 0);
  x11_latency_trace_client_visit(3, 1500, 4, 64, 48);
  x11_latency_trace_stats(out, sizeof(out));
  CHECK(!strcmp(value(out, "net.trace.client_visit.n"), "1"));
  CHECK(!strcmp(value(out, "net.trace.requests_per_client_pass.0"), "1"));
  CHECK(!strcmp(value(out, "net.trace.requests_per_client_pass.4"), "1"));
  CHECK(!strcmp(value(out, "net.trace.input_depth_max"), "64"));
  CHECK(!strcmp(value(out, "net.trace.output_depth_max"), "48"));

  x11_latency_trace_polyfill_begin(2);
  x11_latency_trace_polyfill_pixels(12);
  x11_latency_trace_polyfill_pixels(9);
  x11_latency_trace_polyfill_end();
  x11_latency_trace_polyfill_time(77);
  x11_latency_trace_stats(out, sizeof(out));
  CHECK(!strcmp(value(out, "net.trace.polyfill_requests"), "1"));
  CHECK(!strcmp(value(out, "net.trace.polyfill_rectangles"), "2"));
  CHECK(!strcmp(value(out, "net.trace.polyfill_clipped_pixels"), "21"));
  CHECK(!strcmp(value(out, "net.trace.polyfill.p99_us"), "77"));

  x11_latency_trace_reset();
  x11_latency_trace_stats(out, sizeof(out));
  CHECK(!strcmp(value(out, "net.trace.dispatch.n"), "0"));
  CHECK(!strcmp(value(out, "net.trace.polyfill_clipped_pixels"), "0"));
  CHECK(strtoul(value(out, "net.trace.static_bytes"), NULL, 10) > 0);
  if (failures) return 1;
  puts("latency trace: ok");
  return 0;
}
