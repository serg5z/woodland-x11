/* Replays logged contacts through the gesture recogniser (host tool; tools/gesture_replay.py
 * feeds it). stdin: one read per line, "t_ms n [id x y]...", contacts down at that time.
 * stdout: one line per gesture event. */
#include <stdio.h>
#include <stdlib.h>

#include "../mt/x11_gesture.h"

int main(void) {
  x11_gesture g;
  x11_gesture_init(&g, NULL);
  unsigned long t;
  int n;
  while (scanf("%lu %d", &t, &n) == 2) {
    x11_gesture_contact c[8];
    if (n < 0 || n > 8) return 2;
    for (int i = 0; i < n; ++i) {
      int id, x, y;
      if (scanf("%d %d %d", &id, &x, &y) != 3) return 2;
      c[i].id = (uint8_t)id, c[i].x = (int16_t)x, c[i].y = (int16_t)y;
    }
    x11_gesture_feed(&g, (uint32_t)t, c, n);
    x11_gesture_event e;
    while (x11_gesture_pop(&g, &e))
      printf("%lu %s %s c=%d,%d d=%d,%d total=%ld,%ld scale=%.3f steps=%d\n", (unsigned long)e.t_ms,
             x11_gesture_event_name(e.type), x11_gesture_kind_name(e.kind), e.cx, e.cy, e.dx, e.dy,
             (long)e.total_dx, (long)e.total_dy, e.scale, e.steps);
  }
  printf("summary pairs=%lu scroll_v=%lu scroll_h=%lu pan=%lu pinch=%lu taps=%lu ends=%lu cancels=%lu dropped=%lu\n",
         (unsigned long)g.pairs, (unsigned long)g.starts[X11_G_SCROLL_V], (unsigned long)g.starts[X11_G_SCROLL_H],
         (unsigned long)g.starts[X11_G_PAN], (unsigned long)g.starts[X11_G_PINCH], (unsigned long)g.taps,
         (unsigned long)g.ends, (unsigned long)g.cancels, (unsigned long)g.dropped);
  return 0;
}
