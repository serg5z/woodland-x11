/* Xt/Athena Command widget event and paint probe. */
#include <X11/Intrinsic.h>
#include <X11/StringDefs.h>
#include <X11/Xaw/Command.h>
#include <stdio.h>
#include <time.h>

static int clicked;
static void action(Widget widget, XtPointer client, XtPointer call) {
  (void)widget; (void)client; (void)call;
  ++clicked;
}
static double seconds(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1000000000.0;
}
int main(int argc, char **argv) {
  XtAppContext app;
  Widget top = XtVaAppInitialize(&app, "WoodlandProbe", NULL, 0, &argc, argv, NULL,
                                 XtNwidth, 160, XtNheight, 100, XtNx, 30, XtNy, 30, NULL);
  Widget button = XtVaCreateManagedWidget("Tap", commandWidgetClass, top,
                                          XtNwidth, 160, XtNheight, 100, NULL);
  XtAddCallback(button, XtNcallback, action, NULL);
  XtRealizeWidget(top);
  XSync(XtDisplay(top), False);
  double until = seconds() + 4.0;
  while (seconds() < until && !clicked) {
    while (XtAppPending(app)) XtAppProcessEvent(app, XtIMAll);
    struct timespec pause = {0, 10000000};
    nanosleep(&pause, NULL);
  }
  printf("xaw realized=%d clicked=%d\n", XtIsRealized(top), clicked);
  Display *d = XtDisplay(top);
  XtDestroyWidget(top);
  XtCloseDisplay(d);
  return clicked ? 0 : 1;
}
