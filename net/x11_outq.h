/* Bounded per-client output queue (portable C99, no OS calls): replies and events wait
 * here until the socket accepts them, so a slow client never blocks the server loop, the
 * display or other clients. Used through net/x11_net.c by the host server and the ESP32.
 *
 * The queue does not allocate: its owner gives it memory and may move it to a larger or
 * smaller block with x11_outq_move (x11_net grows it on demand up to a per-client maximum
 * and shrinks it when it is empty).
 *
 * Policy (implemented by x11_net):
 *   - a client whose queue could not take the largest reply plus some events (within its
 *     maximum size) gets no further requests processed until it reads (backpressure: only
 *     that client waits);
 *   - an event for a client whose queue is at its maximum closes that client (events cannot
 *     be dropped without breaking the protocol stream, and cannot be held back). */
#ifndef X11_OUTQ_H
#define X11_OUTQ_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint8_t *buf;
  size_t cap, head, len;
  uint32_t overflows;     /* pushes refused (diagnostics) */
  size_t peak;            /* highest fill level */
} x11_outq;

void x11_outq_init(x11_outq *q, uint8_t *mem, size_t cap);
/* Appends all n bytes, or nothing (returns -1) when they do not fit. */
int x11_outq_push(x11_outq *q, const void *data, size_t n);
/* Contiguous bytes at the head (to pass to send()); 0 when empty. */
size_t x11_outq_peek(const x11_outq *q, const uint8_t **p);
void x11_outq_pop(x11_outq *q, size_t n);
/* Moves the queued bytes, in order, to `mem` (cap bytes, cap >= len) and uses it from now on.
 * Returns the old block for the caller to free. Counters are kept. */
uint8_t *x11_outq_move(x11_outq *q, uint8_t *mem, size_t cap);
static inline size_t x11_outq_free(const x11_outq *q) { return q->cap - q->len; }
static inline void x11_outq_clear(x11_outq *q) { q->head = q->len = 0; }

#ifdef __cplusplus
}
#endif
#endif
