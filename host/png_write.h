/* Minimal PNG writer for RGB565 framebuffers (host tool only, no zlib: stored deflate). */
#ifndef PNG_WRITE_H
#define PNG_WRITE_H
#include <stdint.h>
int png_write_rgb565(const char *path, const uint16_t *pix, int w, int h);
#endif
