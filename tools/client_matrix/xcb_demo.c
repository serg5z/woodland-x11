/* Minimal XCB draw/readback/input probe for the isolated host server. */
#include <xcb/xcb.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double seconds(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1000000000.0;
}

int main(void) {
  int screen = 0;
  xcb_connection_t *c = xcb_connect(NULL, &screen);
  if (xcb_connection_has_error(c)) { fputs("connect failed\n", stderr); return 2; }
  const xcb_setup_t *setup = xcb_get_setup(c);
  xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
  while (screen--) xcb_screen_next(&it);
  xcb_screen_t *s = it.data;
  xcb_window_t w = xcb_generate_id(c);
  uint32_t attrs[] = {s->black_pixel, XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE};
  xcb_create_window(c, s->root_depth, w, s->root, 30, 30, 160, 100, 0,
                    XCB_WINDOW_CLASS_INPUT_OUTPUT, s->root_visual,
                    XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, attrs);
  xcb_gcontext_t gc = xcb_generate_id(c);
  uint32_t red = 0xf800;
  xcb_create_gc(c, gc, w, XCB_GC_FOREGROUND, &red);
  xcb_map_window(c, w);
  xcb_flush(c);
  int exposed = 0, pressed = 0, released = 0, pixel_ok = 0;
  double until = seconds() + 4.0;
  while (seconds() < until && !released) {
    xcb_generic_event_t *e;
    while ((e = xcb_poll_for_event(c))) {
      switch (e->response_type & 0x7f) {
        case XCB_EXPOSE: {
          ++exposed;
          xcb_rectangle_t r = {10, 10, 40, 30};
          xcb_poly_fill_rectangle(c, w, gc, 1, &r);
          xcb_get_image_cookie_t cookie = xcb_get_image(c, XCB_IMAGE_FORMAT_Z_PIXMAP, w, 15, 15, 1, 1, 0xffff);
          xcb_get_image_reply_t *reply = xcb_get_image_reply(c, cookie, NULL);
          if (reply && xcb_get_image_data_length(reply) >= 2) {
            unsigned char *p = xcb_get_image_data(reply);
            pixel_ok = (p[0] | (p[1] << 8)) == 0xf800;
          }
          free(reply);
          break;
        }
        case XCB_BUTTON_PRESS: ++pressed; break;
        case XCB_BUTTON_RELEASE: ++released; break;
        case 0: fprintf(stderr, "X error\n"); free(e); goto done;
      }
      free(e);
    }
    struct timespec pause = {0, 10000000};
    nanosleep(&pause, NULL);
  }
done:
  printf("xcb expose=%d pixel=%d press=%d release=%d\n", exposed, pixel_ok, pressed, released);
  xcb_free_gc(c, gc);
  xcb_destroy_window(c, w);
  xcb_disconnect(c);
  return exposed && pixel_ok && pressed && released ? 0 : 1;
}
