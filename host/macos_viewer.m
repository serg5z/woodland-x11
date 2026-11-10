/* Native host viewer. Wire protocol: WLV1 + u16 width/height + u32 revision/bytes + RGB565 LE. */
#import <AppKit/AppKit.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <signal.h>
#include "viewer_scroll.h"

static unsigned rd16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static unsigned rd32(const unsigned char *p) { return rd16(p) | (rd16(p + 2) << 16); }
static int screenCoord(double value, double extent, unsigned pixels, BOOL invert) {
  if (extent <= 0 || pixels == 0) return 0;
  double scaled = (invert ? extent - value : value) * pixels / extent;
  return MAX(0, MIN((int)pixels - 1, (int)floor(scaled)));
}
static void rgb565ToRGBA(const unsigned char *rgb565, unsigned char *rgba, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    unsigned p = rd16(rgb565 + i * 2);
    unsigned r = p >> 11, g = (p >> 5) & 63, b = p & 31;
    rgba[i * 4] = (unsigned char)((r << 3) | (r >> 2));
    rgba[i * 4 + 1] = (unsigned char)((g << 2) | (g >> 4));
    rgba[i * 4 + 2] = (unsigned char)((b << 3) | (b >> 2));
    rgba[i * 4 + 3] = 255;
  }
}
static BOOL readFull(int fd, void *out, size_t size) {
  unsigned char *p = out;
  while (size) { ssize_t n = read(fd, p, size); if (n <= 0) return NO; p += n; size -= (size_t)n; }
  return YES;
}

@interface ViewerView : NSView
@property (nonatomic, strong) NSImage *image;
@property (nonatomic) int connection;
@property (nonatomic) unsigned width, height, buttons;
@property (nonatomic) int pointerX, pointerY;
- (void)resetInput;
- (void)releaseHeld;
@end

@implementation ViewerView {
  viewer_scroll scroll;  /* deltas not yet worth a wheel click (host/viewer_scroll.h) */
}
- (BOOL)isFlipped { return NO; }
/* A new connection or none: nothing is held, no scroll remainder carries over. */
- (void)resetInput { self.buttons = 0; viewer_scroll_reset(&scroll); }
- (BOOL)acceptsFirstResponder { return YES; }
/* The click that brings the window forward is a click for X11 too (the default, NO, lets AppKit
 * use it only to activate the window: the first click after switching apps was lost). Only this
 * view's own clicks; nothing outside the window is captured. */
- (BOOL)acceptsFirstMouse:(NSEvent *)e { (void)e; return YES; }
- (void)drawRect:(NSRect)dirty {
  (void)dirty;
  [[NSColor blackColor] setFill]; NSRectFill(self.bounds);
  if (self.image) [self.image drawInRect:self.bounds fromRect:NSZeroRect
                              operation:NSCompositingOperationCopy fraction:1.0];
}
- (void)sendPointer:(NSEvent *)event buttons:(unsigned)buttons {
  NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
  if (self.bounds.size.width <= 0 || self.bounds.size.height <= 0 || !self.width || !self.height) return;
  self.pointerX = screenCoord(p.x, self.bounds.size.width, self.width, NO);
  self.pointerY = screenCoord(p.y, self.bounds.size.height, self.height, YES);
  self.buttons = buttons & 7u;  /* only real buttons are held; wheel clicks (8, 16) never are */
  if (self.connection < 0) return;
  char line[80];
  int len = snprintf(line, sizeof(line), "pointer %d %d %u\n", self.pointerX, self.pointerY, buttons);
  if (write(self.connection, line, (size_t)len) < 0) self.connection = -1;
}
- (void)mouseMoved:(NSEvent *)e { [self sendPointer:e buttons:self.buttons]; }
- (void)mouseDragged:(NSEvent *)e { [self sendPointer:e buttons:self.buttons]; }
- (void)rightMouseDragged:(NSEvent *)e { [self sendPointer:e buttons:self.buttons]; }
- (void)otherMouseDragged:(NSEvent *)e { [self sendPointer:e buttons:self.buttons]; }
- (void)mouseDown:(NSEvent *)e { [self.window makeFirstResponder:self]; [self sendPointer:e buttons:self.buttons | 1]; }
- (void)mouseUp:(NSEvent *)e { [self sendPointer:e buttons:self.buttons & ~1u]; }
- (void)rightMouseDown:(NSEvent *)e { [self sendPointer:e buttons:self.buttons | 4]; }
- (void)rightMouseUp:(NSEvent *)e { [self sendPointer:e buttons:self.buttons & ~4u]; }
/* Only the middle button (number 2) is X11 Button2; other buttons (back, forward: 3, 4, ...) have
 * no X11 meaning here and used to become middle clicks (a paste in xterm). */
- (void)otherMouseDown:(NSEvent *)e { if (e.buttonNumber == 2) [self sendPointer:e buttons:self.buttons | 2]; }
- (void)otherMouseUp:(NSEvent *)e { if (e.buttonNumber == 2) [self sendPointer:e buttons:self.buttons & ~2u]; }
/* A button still held when the window stops being key (an app switch or a system dialog during a
 * press) may never get its mouseUp: release it at the last position, as the server does when the
 * viewer disconnects, so no button and no automatic grab stays behind. */
- (void)releaseHeld {
  if (!self.buttons || self.connection < 0) { self.buttons = 0; return; }
  char line[80];
  int len = snprintf(line, sizeof(line), "pointer %d %d 0\n", self.pointerX, self.pointerY);
  if (write(self.connection, line, (size_t)len) < 0) self.connection = -1;
  self.buttons = 0;
}
/* Motion over the view while another app is in front (hover); the view's own rectangle only.
 * Leaving the view sends nothing: the X pointer stays at its last position on the screen. */
- (void)updateTrackingAreas {
  [super updateTrackingAreas];
  for (NSTrackingArea *a in self.trackingAreas) [self removeTrackingArea:a];
  [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:NSZeroRect
                                                     options:NSTrackingMouseMoved | NSTrackingActiveAlways | NSTrackingInVisibleRect
                                                       owner:self userInfo:nil]];
}
/* Deltas accumulate into whole wheel clicks (host/viewer_scroll.h: 24 points per click for
 * trackpads, one per line for wheels, at most 4 per event). Each click is a press and a release
 * of Button4 (up) or Button5 (down) around the held buttons. Horizontal deltas are accumulated
 * but not sent: the server has Buttons 1-5 only (6/7 would be left/right). */
- (void)scrollWheel:(NSEvent *)e {
  int began = e.phase == NSEventPhaseBegan, clicks_x = 0;
  int clicks = viewer_scroll_event(&scroll, e.scrollingDeltaY, e.scrollingDeltaX, e.hasPreciseScrollingDeltas, began,
                                   &clicks_x);
  unsigned bit = clicks > 0 ? 8 : 16;            /* X11 Buttons 4 and 5 */
  unsigned held = self.buttons & 7u;             /* before sendPointer: changes self.buttons */
  for (int i = 0; i < (clicks > 0 ? clicks : -clicks); ++i) {
    [self sendPointer:e buttons:held | bit];     /* the wheel click: press ... */
    [self sendPointer:e buttons:held];           /* ... and release */
  }
}
@end

@interface ViewerApp : NSObject <NSApplicationDelegate>
@property (nonatomic, strong) NSWindow *window;
@property (nonatomic, strong) ViewerView *view;
@property (nonatomic, strong) NSString *path;
@property (nonatomic) BOOL stopping;
@end

@implementation ViewerApp
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
  (void)notification;
  self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(100, 100, 800, 480)
                         styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                   NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                           backing:NSBackingStoreBuffered defer:NO];
  self.view = [[ViewerView alloc] initWithFrame:NSMakeRect(0, 0, 800, 480)];
  self.view.connection = -1; self.view.width = 800; self.view.height = 480;
  self.window.contentView = self.view;
  self.window.title = @"Woodland X11 (connecting; keyboard unavailable)";
  self.window.acceptsMouseMovedEvents = YES;
  [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowDidResignKeyNotification object:self.window queue:nil
                                                usingBlock:^(NSNotification *n) { (void)n; [self.view releaseHeld]; }];
  [self.window makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];
  __weak ViewerApp *weakSelf = self;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    ViewerApp *app = weakSelf;
    while (app && !app.stopping) {
      int fd = socket(AF_UNIX, SOCK_STREAM, 0);
      if (fd < 0) { usleep(250000); continue; }
      int yes = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
      struct sockaddr_un addr = {0}; addr.sun_family = AF_UNIX;
      const char *path = app.path.fileSystemRepresentation;
      if (strlen(path) >= sizeof(addr.sun_path)) { close(fd); break; }
      strcpy(addr.sun_path, path);
      if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(fd); usleep(250000); continue; }
      dispatch_async(dispatch_get_main_queue(), ^{
        app.view.connection = fd; [app.view resetInput];
        app.window.title = @"Woodland X11 (keyboard unavailable: server has no key input)";
      });
      unsigned char header[16];
      while (!app.stopping && readFull(fd, header, sizeof(header))) {
        @autoreleasepool {
        unsigned w = rd16(header + 4), h = rd16(header + 6), bytes = rd32(header + 12);
        if (memcmp(header, "WLV1", 4) || !w || !h || w > 4096 || h > 4096 || bytes != w * h * 2) break;
        unsigned char *rgb565 = malloc(bytes), *rgba = malloc((size_t)w * h * 4);
        if (!rgb565 || !rgba || !readFull(fd, rgb565, bytes)) { free(rgb565); free(rgba); break; }
        rgb565ToRGBA(rgb565, rgba, (size_t)w * h);
        free(rgb565);
        NSData *data = [NSData dataWithBytesNoCopy:rgba length:(size_t)w * h * 4 freeWhenDone:YES];
        CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)data);
        CGColorSpaceRef color = CGColorSpaceCreateDeviceRGB();
        CGImageRef cg = CGImageCreate(w, h, 8, 32, w * 4, color,
                                    kCGBitmapByteOrder32Big | kCGImageAlphaNoneSkipLast,
                                    provider, NULL, false, kCGRenderingIntentDefault);
        CGColorSpaceRelease(color); CGDataProviderRelease(provider);
        if (!cg) break;
        NSImage *image = [[NSImage alloc] initWithCGImage:cg size:NSMakeSize(w, h)];
        CGImageRelease(cg);
        dispatch_async(dispatch_get_main_queue(), ^{
          if (app.view.connection == fd) {
            app.view.width = w; app.view.height = h; app.view.image = image;
            [app.view setNeedsDisplay:YES];
          }
        });
        }
      }
      close(fd);
      dispatch_async(dispatch_get_main_queue(), ^{
        if (app.view.connection == fd) {
          app.view.connection = -1; [app.view resetInput];
          app.window.title = @"Woodland X11 (disconnected; reconnecting)";
        }
      });
      usleep(250000);
      app = weakSelf;
    }
  });
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender { (void)sender; return YES; }
- (void)applicationWillTerminate:(NSNotification *)notification {
  (void)notification; self.stopping = YES;
  if (self.view.connection >= 0) shutdown(self.view.connection, SHUT_RDWR);
}
@end

int main(int argc, const char **argv) {
  @autoreleasepool {
    if (argc == 2 && !strcmp(argv[1], "--self-test")) {
      unsigned char pixels[8] = {0x00, 0xf8, 0xe0, 0x07, 0x1f, 0x00, 0xff, 0xff};
      unsigned char rgba[16];
      const unsigned char want[16] = {255, 0, 0, 255, 0, 255, 0, 255,
                                      0, 0, 255, 255, 255, 255, 255, 255};
      rgb565ToRGBA(pixels, rgba, 4);
      BOOL ok = !memcmp(rgba, want, sizeof(want)) &&
                screenCoord(0, 400, 800, NO) == 0 && screenCoord(200, 400, 800, NO) == 400 &&
                screenCoord(400, 400, 800, NO) == 799 && screenCoord(0, 240, 480, YES) == 479 &&
                screenCoord(120, 240, 480, YES) == 240 && screenCoord(240, 240, 480, YES) == 0;
      puts(ok ? "RGB565 and resized pointer mapping: ok" : "self-test failed");
      return ok ? 0 : 1;
    }
    if (argc != 2) { fprintf(stderr, "usage: macos_viewer SOCKET_PATH\n"); return 2; }
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    ViewerApp *app = [ViewerApp new];
    app.path = [NSString stringWithUTF8String:argv[1]];
    NSApp.delegate = app;
    [NSApp run];
  }
  return 0;
}
