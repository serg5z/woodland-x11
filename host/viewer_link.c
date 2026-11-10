#define _POSIX_C_SOURCE 200809L
#include "viewer_link.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static void le16(uint8_t *p, unsigned n) { p[0] = (uint8_t)n; p[1] = (uint8_t)(n >> 8); }
static void le32(uint8_t *p, uint32_t n) {
  p[0] = (uint8_t)n; p[1] = (uint8_t)(n >> 8); p[2] = (uint8_t)(n >> 16); p[3] = (uint8_t)(n >> 24);
}
static void nonblock(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }

int viewer_link_open(viewer_link *v, const char *path, int width, int height) {
  memset(v, 0, sizeof(*v));
  v->listener = v->client = -1;
  if (!path || strlen(path) >= sizeof(v->path) || width <= 0 || height <= 0) return -1;
  size_t bytes = (size_t)width * (size_t)height * 2;
  if (bytes > 16 * 1024 * 1024) return -1;
  v->frame = malloc(16 + bytes);
  if (!v->frame) return -1;
  v->frame_size = 16 + bytes;
  v->listener = socket(AF_UNIX, SOCK_STREAM, 0);
  if (v->listener < 0) goto fail;
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strcpy(addr.sun_path, path);
  mode_t old = umask(0077);
  int ok = bind(v->listener, (struct sockaddr *)&addr, sizeof(addr));
  umask(old);
  if (ok < 0) goto fail; /* Never remove a preexisting socket. */
  strcpy(v->path, path);
  if (listen(v->listener, 1) < 0) goto fail;
  nonblock(v->listener);
  return 0;
fail:
  if (v->listener >= 0) close(v->listener);
  if (v->path[0]) unlink(v->path);
  free(v->frame);
  v->listener = -1; v->frame = NULL;
  return -1;
}

void viewer_link_fds(viewer_link *v, fd_set *r, fd_set *w, int *maxfd) {
  if (v->listener < 0) return;
  FD_SET(v->listener, r);
  if (v->listener > *maxfd) *maxfd = v->listener;
  if (v->client >= 0) {
    FD_SET(v->client, r);
    if (v->sent < v->frame_size && v->queued_rev != (uint32_t)-1) FD_SET(v->client, w);
    if (v->client > *maxfd) *maxfd = v->client;
  }
}

static void drop(viewer_link *v, viewer_pointer_fn pointer, void *ctx) {
  if (v->client >= 0) close(v->client);
  v->client = -1;
  if (v->buttons) pointer(ctx, v->x, v->y, 0);
  v->buttons = 0;
  v->input_size = 0;
  v->queued_rev = (uint32_t)-1;
  v->last_rev = (uint32_t)-1;
}

static void command(viewer_link *v, char *line, viewer_pointer_fn pointer, void *ctx) {
  int x, y;
  unsigned buttons;
  char extra;
  if (sscanf(line, "pointer %d %d %u %c", &x, &y, &buttons, &extra) != 3 || buttons > 31) return;
  v->x = x; v->y = y; v->buttons = buttons;
  pointer(ctx, x, y, buttons);
}

void viewer_link_poll(viewer_link *v, const fd_set *r, const fd_set *w,
                      viewer_pointer_fn pointer, void *ctx) {
  if (v->listener < 0) return;
  if (FD_ISSET(v->listener, r)) {
    int fd = accept(v->listener, NULL, NULL);
    if (fd >= 0) {
      if (v->client >= 0) close(fd);
      else { v->client = fd; nonblock(fd); v->last_rev = (uint32_t)-1; v->queued_rev = (uint32_t)-1; }
    }
  }
  if (v->client < 0) return;
  if (FD_ISSET(v->client, r)) {
    char buf[256];
    ssize_t n = read(v->client, buf, sizeof(buf));
    if (n <= 0) { if (!n || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) drop(v, pointer, ctx); return; }
    for (ssize_t i = 0; i < n; ++i) {
      if (buf[i] == '\n') { v->input[v->input_size] = 0; command(v, v->input, pointer, ctx); v->input_size = 0; }
      else if (v->input_size < sizeof(v->input) - 1) v->input[v->input_size++] = buf[i];
      else v->input_size = 0;
    }
  }
  if (v->client >= 0 && FD_ISSET(v->client, w) && v->queued_rev != (uint32_t)-1) {
    ssize_t n = write(v->client, v->frame + v->sent, v->frame_size - v->sent);
    if (n > 0) v->sent += (size_t)n;
    else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) { drop(v, pointer, ctx); return; }
    if (v->sent == v->frame_size) { v->last_rev = v->queued_rev; v->queued_rev = (uint32_t)-1; }
  }
}

void viewer_link_frame(viewer_link *v, const uint16_t *pixels, int width, int height,
                       uint32_t revision, uint32_t now_ms) {
  if (v->client < 0 || v->queued_rev != (uint32_t)-1 || revision == v->last_rev ||
      (v->last_rev != (uint32_t)-1 && now_ms - v->last_frame_ms < 33)) return;
  memcpy(v->frame, "WLV1", 4);
  le16(v->frame + 4, (unsigned)width); le16(v->frame + 6, (unsigned)height);
  le32(v->frame + 8, revision); le32(v->frame + 12, (uint32_t)(v->frame_size - 16));
  for (size_t i = 0; i < (size_t)width * (size_t)height; ++i) le16(v->frame + 16 + i * 2, pixels[i]);
  v->sent = 0;
  v->queued_rev = revision;
  v->last_frame_ms = now_ms;
}

void viewer_link_close(viewer_link *v, viewer_pointer_fn pointer, void *ctx) {
  drop(v, pointer, ctx);
  if (v->listener >= 0) close(v->listener);
  if (v->path[0]) unlink(v->path);
  free(v->frame);
  v->listener = -1; v->frame = NULL;
}
