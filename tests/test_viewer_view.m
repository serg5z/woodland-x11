/* The macOS viewer's view (host/macos_viewer.m), driven with synthetic AppKit events: every
 * "pointer X Y BUTTONS" line it writes is read back from a socket pair and checked. No screen
 * input is used and nothing is shown. macOS only (make test runs it on Darwin). */
#define main viewer_main
#include "../host/macos_viewer.m"
#undef main
#include <sys/socket.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

typedef struct { int x, y; unsigned b; } line_t;
static int sv[2];

/* The lines written since the last call. */
static int lines(line_t *out, int max) {
  static char pending[65536];
  static size_t have;
  ssize_t n;
  while ((n = recv(sv[1], pending + have, sizeof(pending) - 1 - have, MSG_DONTWAIT)) > 0) have += (size_t)n;
  int k = 0;
  char *p = pending, *nl;
  while ((nl = memchr(p, '\n', (size_t)(pending + have - p)))) {
    if (k < max && sscanf(p, "pointer %d %d %u", &out[k].x, &out[k].y, &out[k].b) == 3) ++k;
    p = nl + 1;
  }
  memmove(pending, p, (size_t)(pending + have - p));
  have = (size_t)(pending + have - p);
  return k;
}

static NSWindow *win;
static NSEvent *mouse(NSEventType t, double x, double y, NSInteger number) {
  NSEvent *e = [NSEvent mouseEventWithType:t location:NSMakePoint(x, y) modifierFlags:0 timestamp:0
                              windowNumber:win.windowNumber context:nil eventNumber:0 clickCount:1 pressure:1];
  if (number < 0) return e;
  CGEventRef c = CGEventCreateCopy(e.CGEvent);  /* set the button number (otherMouse events) */
  CGEventSetIntegerValueField(c, kCGMouseEventButtonNumber, number);
  NSEvent *r = [NSEvent eventWithCGEvent:c];
  CFRelease(c);
  return r;
}
static NSEvent *wheel(int32_t dy, int32_t dx, int precise) {
  CGEventRef c = CGEventCreateScrollWheelEvent(NULL, precise ? kCGScrollEventUnitPixel : kCGScrollEventUnitLine, 2, dy, dx);
  CGEventSetLocation(c, CGPointMake(0, 0));
  NSEvent *e = [NSEvent eventWithCGEvent:c];
  CFRelease(c);
  return e;
}

static ViewerView *view(void) {
  ViewerView *v = [[ViewerView alloc] initWithFrame:NSMakeRect(0, 0, 800, 480)];
  v.connection = sv[0];
  v.width = 800, v.height = 480;
  win.contentView = v;
  return v;
}

/* No line after the last may carry a wheel bit; every wheel press is released by the next line. */
static void check_wheel_pairs(const line_t *l, int n) {
  for (int i = 0; i < n; ++i)
    if (l[i].b & 24) CHECK(i + 1 < n && !(l[i + 1].b & 24));
  CHECK(n == 0 || !(l[n - 1].b & 24));
}

static void test_scroll_releases(void) {
  ViewerView *v = view();
  line_t l[256];
  lines(l, 256);
  [v scrollWheel:wheel(1, 0, 0)];  /* one mouse-wheel notch up */
  int n = lines(l, 256);
  CHECK(n >= 2);
  check_wheel_pairs(l, n);
  CHECK((v.buttons & 24) == 0);  /* the held-button state never keeps a wheel button */
  /* rapid alternating notches: one click each, alternating */
  for (int i = 0; i < 20; ++i) [v scrollWheel:wheel(i & 1 ? -1 : 1, 0, 0)];
  n = lines(l, 256);
  check_wheel_pairs(l, n);
  CHECK((v.buttons & 24) == 0);
  /* a click after scrolling carries no wheel bit */
  [v mouseDown:mouse(NSEventTypeLeftMouseDown, 600, 240, -1)];
  [v mouseUp:mouse(NSEventTypeLeftMouseUp, 600, 240, -1)];
  n = lines(l, 256);
  CHECK(n == 2 && l[0].b == 1 && l[1].b == 0);
  /* scrolling while the left button is held keeps it held around the wheel click */
  [v mouseDown:mouse(NSEventTypeLeftMouseDown, 600, 240, -1)];
  [v scrollWheel:wheel(-1, 0, 0)];
  [v mouseUp:mouse(NSEventTypeLeftMouseUp, 600, 240, -1)];
  n = lines(l, 256);
  CHECK(n == 4 && l[0].b == 1 && l[1].b == 17 && l[2].b == 1 && l[3].b == 0);
}

/* Trackpad deltas accumulate (host/viewer_scroll.h): a 120-point swipe in 40 events is 5 clicks,
 * not 40; a new connection drops the remainder. */
static void test_trackpad_scroll(void) {
  ViewerView *v = view();
  line_t l[512];
  lines(l, 512);
  for (int i = 0; i < 40; ++i) [v scrollWheel:wheel(3, 0, 1)];
  int n = lines(l, 512), presses = 0;
  for (int i = 0; i < n; ++i) presses += (l[i].b & 8) != 0;
  CHECK(presses == 5 && n == 10);
  check_wheel_pairs(l, n);
  for (int i = 0; i < 6; ++i) [v scrollWheel:wheel(-3, 0, 1)];  /* reversal: 18 points down, no click yet */
  CHECK(lines(l, 512) == 0);
  [v resetInput];                                               /* a reconnect */
  for (int i = 0; i < 3; ++i) [v scrollWheel:wheel(-3, 0, 1)];  /* 9 more: still under 24 */
  CHECK(lines(l, 512) == 0);
  [v scrollWheel:wheel(0, 50, 1)];                              /* horizontal: nothing sent */
  CHECK(lines(l, 512) == 0);
  [v scrollWheel:wheel(-30, 0, 1)];                             /* 39 points down: one Button5 click */
  n = lines(l, 512);
  CHECK(n == 2 && l[0].b == 16 && l[1].b == 0);
}

/* The click that activates the window (another app in front) reaches X11: AppKit routes it to the
 * view only if acceptsFirstMouse: says YES. Real routing: -[NSWindow sendEvent:] on a window that
 * is not key, in an app that is not active. */
static void test_first_click(void) {
  NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(-3000, -3000, 200, 120) styleMask:NSWindowStyleMaskTitled
                                              backing:NSBackingStoreBuffered defer:NO];
  ViewerView *v = [[ViewerView alloc] initWithFrame:NSMakeRect(0, 0, 200, 120)];
  v.connection = sv[0];
  v.width = 800, v.height = 480;
  w.contentView = v;
  [w orderBack:nil];
  line_t l[16];
  lines(l, 16);
  CHECK(!w.isKeyWindow && !NSApp.isActive);
  for (NSEventType t = NSEventTypeLeftMouseDown; t <= NSEventTypeLeftMouseUp; ++t)
    [w sendEvent:[NSEvent mouseEventWithType:t location:NSMakePoint(100, 60) modifierFlags:0 timestamp:0
                                windowNumber:w.windowNumber context:nil eventNumber:0 clickCount:1 pressure:1]];
  int n = lines(l, 16);
  CHECK(n == 2 && l[0].b == 1 && l[1].b == 0 && l[0].x == 400 && l[0].y == 240);
  [w orderOut:nil];
}

/* Lifecycle: extra buttons, a held button when the window resigns key, resized coordinates,
 * drags outside the view, the tracking area. */
static void test_lifecycle(void) {
  ViewerView *v = view();
  line_t l[64];
  lines(l, 64);
  [v otherMouseDown:mouse(NSEventTypeOtherMouseDown, 10, 10, 3)];  /* back button */
  [v otherMouseUp:mouse(NSEventTypeOtherMouseUp, 10, 10, 3)];
  [v otherMouseDown:mouse(NSEventTypeOtherMouseDown, 10, 10, 4)];  /* forward button */
  [v otherMouseUp:mouse(NSEventTypeOtherMouseUp, 10, 10, 4)];
  CHECK(lines(l, 64) == 0);
  [v otherMouseDown:mouse(NSEventTypeOtherMouseDown, 10, 10, 2)];  /* middle: Button2 */
  [v otherMouseUp:mouse(NSEventTypeOtherMouseUp, 10, 10, 2)];
  int n = lines(l, 64);
  CHECK(n == 2 && l[0].b == 2 && l[1].b == 0);
  /* a press whose mouseUp never comes: resigning key releases it once, at the last position */
  [v rightMouseDown:mouse(NSEventTypeRightMouseDown, 200, 100, -1)];
  [v releaseHeld];
  [v releaseHeld];
  n = lines(l, 64);
  CHECK(n == 2 && l[0].b == 4 && l[1].b == 0 && l[1].x == l[0].x && l[1].y == l[0].y && v.buttons == 0);
  /* nothing held: nothing sent */
  [v releaseHeld];
  CHECK(lines(l, 64) == 0);
  /* resized to half: the corners still map to the whole 800x480 screen; a drag outside is clamped */
  [v setFrameSize:NSMakeSize(400, 240)];
  [v mouseDown:mouse(NSEventTypeLeftMouseDown, 0, 240, -1)];
  [v mouseDragged:mouse(NSEventTypeLeftMouseDragged, 399.9, 0.1, -1)];
  [v mouseDragged:mouse(NSEventTypeLeftMouseDragged, 900, -50, -1)];
  [v mouseUp:mouse(NSEventTypeLeftMouseUp, 900, -50, -1)];
  n = lines(l, 64);
  CHECK(n == 4 && l[0].x == 0 && l[0].y == 0 && l[1].x == 799 && l[1].y == 479 && l[2].x == 799 && l[2].y == 479);
  CHECK(l[0].b == 1 && l[1].b == 1 && l[2].b == 1 && l[3].b == 0);
  /* the tracking area (hover while another app is in front) */
  [v updateTrackingAreas];
  CHECK(v.trackingAreas.count == 1 && (v.trackingAreas[0].options & NSTrackingActiveAlways));
}

int main(void) {
  @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];  /* no Dock icon, never active */
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 800, 480) styleMask:NSWindowStyleMaskTitled
                                        backing:NSBackingStoreBuffered defer:YES];
    test_scroll_releases();
    test_trackpad_scroll();
    test_first_click();
    test_lifecycle();
  }
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  puts("macOS viewer view tests OK");
  return 0;
}
