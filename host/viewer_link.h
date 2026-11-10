/* Local, host-only RGB565 frame and pointer transport for the desktop viewer. */
#ifndef WOODLAND_VIEWER_LINK_H
#define WOODLAND_VIEWER_LINK_H
#include <stdint.h>
#include <stddef.h>
#include <sys/select.h>

typedef struct {
  int listener, client;
  char path[104];
  uint8_t *frame;
  size_t frame_size, sent;
  uint32_t queued_rev, last_rev, last_frame_ms;
  char input[256];
  size_t input_size;
  int x, y;
  unsigned buttons;
} viewer_link;

typedef void (*viewer_pointer_fn)(void *ctx, int x, int y, unsigned buttons);
int viewer_link_open(viewer_link *v, const char *path, int width, int height);
void viewer_link_fds(viewer_link *v, fd_set *read_set, fd_set *write_set, int *maxfd);
void viewer_link_poll(viewer_link *v, const fd_set *read_set, const fd_set *write_set,
                      viewer_pointer_fn pointer, void *ctx);
void viewer_link_frame(viewer_link *v, const uint16_t *pixels, int width, int height,
                       uint32_t revision, uint32_t now_ms);
void viewer_link_close(viewer_link *v, viewer_pointer_fn pointer, void *ctx);
#endif
