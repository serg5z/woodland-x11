#include "x11_outq.h"

#include <string.h>

void x11_outq_init(x11_outq *q, uint8_t *mem, size_t cap) {
  q->buf = mem;
  q->cap = cap;
  q->head = q->len = 0;
  q->overflows = 0;
  q->peak = 0;
}

int x11_outq_push(x11_outq *q, const void *data, size_t n) {
  if (n > q->cap - q->len) {
    ++q->overflows;
    return -1;
  }
  size_t tail = (q->head + q->len) % q->cap;
  size_t first = q->cap - tail < n ? q->cap - tail : n;
  memcpy(q->buf + tail, data, first);
  memcpy(q->buf, (const uint8_t *)data + first, n - first);
  q->len += n;
  if (q->len > q->peak) q->peak = q->len;
  return 0;
}

size_t x11_outq_peek(const x11_outq *q, const uint8_t **p) {
  *p = q->buf + q->head;
  size_t to_end = q->cap - q->head;
  return q->len < to_end ? q->len : to_end;
}

uint8_t *x11_outq_move(x11_outq *q, uint8_t *mem, size_t cap) {
  size_t first = q->len < q->cap - q->head ? q->len : q->cap - q->head;
  if (q->len) {
    memcpy(mem, q->buf + q->head, first);
    memcpy(mem + first, q->buf, q->len - first);
  }
  uint8_t *old = q->buf;
  q->buf = mem;
  q->cap = cap;
  q->head = 0;
  return old;
}

void x11_outq_pop(x11_outq *q, size_t n) {
  if (n > q->len) n = q->len;
  q->head = (q->head + n) % q->cap;
  q->len -= n;
  if (!q->len) q->head = 0;
}
