/* Minimal Xlib draw/readback/input probe for the isolated host server. */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double seconds(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1000000000.0;
}

int main(void) {
  Display *d = XOpenDisplay(NULL);
  if (!d) { fputs("connect failed\n", stderr); return 2; }
  int s = DefaultScreen(d);
  Window w = XCreateSimpleWindow(d, RootWindow(d, s), 30, 30, 160, 100, 0, 0, 0);
  XSelectInput(d, w, ExposureMask | ButtonPressMask | ButtonReleaseMask);
  XMapWindow(d, w);
  GC gc = XCreateGC(d, w, 0, NULL);
  int exposed = 0, pressed = 0, released = 0, pixel_ok = 0;
  double until = seconds() + 4.0;
  while (seconds() < until && !released) {
    while (XPending(d)) {
      XEvent e;
      XNextEvent(d, &e);
      if (e.type == Expose) {
        ++exposed;
        XSetForeground(d, gc, 0xf800);
        XFillRectangle(d, w, gc, 10, 10, 40, 30);
        XSetForeground(d, gc, 0xffff);
        XDrawString(d, w, gc, 10, 65, "Xlib", 4);
        XSync(d, False);
        XImage *im = XGetImage(d, w, 15, 15, 1, 1, AllPlanes, ZPixmap);
        if (im) { pixel_ok = XGetPixel(im, 0, 0) == 0xf800; XDestroyImage(im); }
      } else if (e.type == ButtonPress) ++pressed;
      else if (e.type == ButtonRelease) ++released;
    }
    struct timespec pause = {0, 10000000};
    nanosleep(&pause, NULL);
  }
  printf("xlib expose=%d pixel=%d press=%d release=%d\n", exposed, pixel_ok, pressed, released);
  XFreeGC(d, gc);
  XDestroyWindow(d, w);
  XCloseDisplay(d);
  return exposed && pixel_ok && pressed && released ? 0 : 1;
}
