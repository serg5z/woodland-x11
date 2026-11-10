/* The X11 colour database (rgb.txt) for AllocNamedColor and LookupColor. */
#ifndef X11_COLORS_H
#define X11_COLORS_H
#include <stddef.h>
#include <stdint.h>

/* Looks up a colour name as X servers do (case-insensitive, spaces ignored, "grey" ==
 * "gray"). Writes 8-bit RGB and returns 1, or returns 0 for an unknown name. */
int x11_color_lookup(const char *name, size_t n, uint8_t rgb[3]);
#endif
