/* Unit tests for net/x11_outq.c: all-or-nothing pushes, wrap-around, order, accounting. */
#include <stdio.h>
#include <string.h>

#include "../net/x11_outq.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

int main(void) {
  uint8_t mem[100];
  x11_outq q;
  x11_outq_init(&q, mem, sizeof(mem));
  uint8_t a[60], b[60], got[200];
  for (int i = 0; i < 60; ++i) a[i] = (uint8_t)i, b[i] = (uint8_t)(100 + i);
  CHECK(x11_outq_push(&q, a, 60) == 0);
  CHECK(x11_outq_push(&q, b, 60) == -1 && q.len == 60 && q.overflows == 1);  /* nothing partial */
  const uint8_t *p;
  size_t n = x11_outq_peek(&q, &p);
  CHECK(n == 60 && p[0] == 0);
  x11_outq_pop(&q, 50);
  CHECK(x11_outq_push(&q, b, 60) == 0 && q.len == 70);  /* wraps around the end */
  size_t k = 0;
  while ((n = x11_outq_peek(&q, &p)) > 0) {  /* drained in two contiguous pieces, in order */
    memcpy(got + k, p, n);
    k += n;
    x11_outq_pop(&q, n);
  }
  CHECK(k == 70);
  for (int i = 0; i < 10; ++i) CHECK(got[i] == 50 + i);
  for (int i = 0; i < 60; ++i) CHECK(got[10 + i] == 100 + i);
  CHECK(q.peak == 70 && x11_outq_free(&q) == 100);
  /* random pushes and pops keep the byte stream exact */
  unsigned seed = 7, wv = 0, rv = 0;
  for (int i = 0; i < 100000; ++i) {
    seed = seed * 1103515245u + 12345u;
    uint8_t chunk[32];
    size_t m = (seed >> 16) % 32 + 1;
    if ((seed >> 8) & 1) {
      for (size_t j = 0; j < m; ++j) chunk[j] = (uint8_t)(wv + j);
      if (x11_outq_push(&q, chunk, m) == 0) wv += (unsigned)m;
    } else {
      n = x11_outq_peek(&q, &p);
      if (n > m) n = m;
      for (size_t j = 0; j < n; ++j) CHECK(p[j] == (uint8_t)(rv + j));
      rv += (unsigned)n;
      x11_outq_pop(&q, n);
    }
  }
  if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
  puts("x11 output queue tests OK");
  return 0;
}
