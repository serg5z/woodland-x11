#include <stdio.h>
#include <string.h>

#include "../spilink/event_drain.h"

typedef struct { int calls; int result; } fake_reader;

static int read_batch(void *ctx, void *buffer, size_t capacity) {
  fake_reader *r = ctx;
  ++r->calls;
  if (r->calls > 1) return -1;  /* a blocking GPIO read would wait here */
  if (buffer && capacity) memset(buffer, 0, capacity);
  return r->result;
}

int main(void) {
  unsigned char events[16];
  fake_reader queued = {0, 3};
  if (spl_read_edge_batch(read_batch, &queued, events, sizeof(events)) != 3 || queued.calls != 1) {
    fprintf(stderr, "queued event batch must be read once\n");
    return 1;
  }
  fake_reader empty = {0, 0};
  if (spl_read_edge_batch(read_batch, &empty, events, sizeof(events)) != 0 || empty.calls != 1) {
    fprintf(stderr, "empty event batch must return without another blocking read\n");
    return 1;
  }
  puts("event drain: one read per readiness notification");
  return 0;
}
