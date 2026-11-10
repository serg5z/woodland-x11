#ifndef X11_LATENCY_TRACE_H
#define X11_LATENCY_TRACE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  X11_TRACE_SELECT_READY_DISPATCH,
  X11_TRACE_REQUEST_WAIT,
  X11_TRACE_INPUT_AGE,
  X11_TRACE_DISPATCH,
  X11_TRACE_DIAG_DISPATCH,
  X11_TRACE_OUTPUT_FIRST,
  X11_TRACE_OUTPUT_DRAIN,
  X11_TRACE_OUTPUT_SEND_CALL,
  X11_TRACE_OUTPUT_FLUSH,
  X11_TRACE_CLIENT_VISIT,
  X11_TRACE_NET_PASS,
  X11_TRACE_SELECT_WAIT,
  X11_TRACE_LOOP_GAP,
  X11_TRACE_POLYFILL,
  X11_TRACE_METRICS
};

void x11_latency_trace_reset(void);
void x11_latency_trace_client_reset(unsigned slot);
void x11_latency_trace_add(unsigned metric, uint64_t elapsed_us);
void x11_latency_trace_client_visit(unsigned slot, uint64_t now_us, unsigned requests,
                                    size_t input_bytes, size_t output_bytes);
void x11_latency_trace_depth(size_t input_bytes, size_t output_bytes);
void x11_latency_trace_polyfill_begin(unsigned rectangles);
void x11_latency_trace_polyfill_pixels(uint64_t pixels);
void x11_latency_trace_polyfill_end(void);
void x11_latency_trace_polyfill_time(uint64_t elapsed_us);
size_t x11_latency_trace_stats(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
