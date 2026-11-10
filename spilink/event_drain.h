/* Read one queued GPIO event batch after poll() reports the request fd ready.
 * libgpiod's edge-event read blocks when the queue is empty, so callers must
 * not loop and make a second read without another readiness notification. */
#ifndef WOODLAND_EVENT_DRAIN_H
#define WOODLAND_EVENT_DRAIN_H

#include <stddef.h>
#include <stdint.h>

typedef int (*spl_edge_batch_reader)(void *ctx, void *buffer, size_t capacity);

static inline uint32_t spl_read_edge_batch(spl_edge_batch_reader read_batch,
                                           void *ctx, void *buffer,
                                           size_t capacity) {
  int n = read_batch(ctx, buffer, capacity);
  return n > 0 ? (uint32_t)n : 0;
}

#endif
