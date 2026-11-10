/* Scroll deltas to X11 wheel clicks for the macOS viewer (portable C99, no I/O).
 *
 * AppKit reports a scroll as deltas: precise ones in points (trackpads, Magic Mouse: many small
 * events per swipe, then momentum) or line ones (an ordinary wheel: about 1 per notch, more with
 * acceleration). X11 has no deltas, only wheel clicks (Button4 up, Button5 down; 6/7 left/right).
 * The deltas are accumulated and turned into whole clicks:
 *   - precise: one click per VIEWER_SCROLL_PX points;
 *   - lines: one click per line;
 *   - at most VIEWER_SCROLL_MAX_CLICKS clicks per event (momentum or a fling cannot flood);
 *   - the remainder is kept, but dropped when the direction reverses, when a new gesture begins
 *     and on viewer_scroll_reset (connect, disconnect).
 * Directions follow AppKit's scrollingDelta (already adjusted for "natural" scrolling):
 * positive y = the content moves down = X11 "scroll up" (Button4). */
#ifndef VIEWER_SCROLL_H
#define VIEWER_SCROLL_H

#define VIEWER_SCROLL_PX 24.0
#define VIEWER_SCROLL_MAX_CLICKS 4

typedef struct {
  double y, x;  /* accumulated, not yet clicked */
} viewer_scroll;

void viewer_scroll_reset(viewer_scroll *s);

/* One AppKit scroll event: dy, dx as scrollingDeltaY/X; precise as hasPreciseScrollingDeltas;
 * began: a new gesture starts (phase Began, or a wheel event after a pause). Returns the vertical
 * clicks (> 0 Button4, < 0 Button5) and sets *clicks_x (> 0 Button6, < 0 Button7). */
int viewer_scroll_event(viewer_scroll *s, double dy, double dx, int precise, int began, int *clicks_x);

#endif
