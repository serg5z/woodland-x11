/* Scroll deltas to X11 wheel clicks, see viewer_scroll.h. */
#include "viewer_scroll.h"

void viewer_scroll_reset(viewer_scroll *s) { s->y = s->x = 0; }

/* Adds d (in clicks) to *acc and returns the whole clicks taken out of it, bounded. */
static int take(double *acc, double d) {
  if ((d > 0 && *acc < 0) || (d < 0 && *acc > 0)) *acc = 0;  /* reversed: no stale remainder */
  *acc += d;
  int n = (int)*acc;  /* towards zero: the remainder keeps its sign */
  if (n > VIEWER_SCROLL_MAX_CLICKS || n < -VIEWER_SCROLL_MAX_CLICKS) {
    *acc = 0;  /* bounded: the excess is dropped, a fling does not keep clicking afterwards */
    return n > 0 ? VIEWER_SCROLL_MAX_CLICKS : -VIEWER_SCROLL_MAX_CLICKS;
  }
  *acc -= n;
  return n;
}

int viewer_scroll_event(viewer_scroll *s, double dy, double dx, int precise, int began, int *clicks_x) {
  if (began) viewer_scroll_reset(s);
  double unit = precise ? VIEWER_SCROLL_PX : 1.0;
  int x = take(&s->x, dx / unit);
  if (clicks_x) *clicks_x = x;
  return take(&s->y, dy / unit);
}
