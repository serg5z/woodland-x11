/* Portable raster backend for the X11 experiment (Milestone 2A). C99, no allocation, no
 * I/O: draws into caller-owned surfaces (the 800x480 RGB565 framebuffer, depth-16 and
 * depth-1 pixmaps), clipped to a rectangle-list region. Runs unchanged on the ESP32-S3.
 *
 * Exactness: rectangles, thin lines (Bresenham), points, images and copies are pixel exact
 * to the X11 rules used here; polygons follow the X fill rules (pixel centres, even-odd or
 * winding). Wide lines and arcs are polygon approximations (butt caps, round joins as
 * discs; arcs as 0.5-degree polylines), visually right but not the X11 spec's exact
 * pixelisation.
 */
#ifndef X11_RENDER_H
#define X11_RENDER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_MAX_RECTS 96
#ifndef XR_MAX_POLY
#define XR_MAX_POLY 1024  /* polygon vertices per request (ESP32 build: 512) */
#endif

typedef struct {
  int16_t x1, y1, x2, y2;  /* half-open: [x1, x2) x [y1, y2) */
} xr_rect;

typedef struct {
  int n;
  int overflow;            /* a subtraction did not fit: region is conservative (smaller) */
  xr_rect r[XR_MAX_RECTS];
} xr_region;

/* ---- region scratch pool ------------------------------------------------------------------
 * A region is 776 bytes; temporaries on the loop task's stack cost about 5 KB of its 8 KB on
 * the ESP32 (CopyArea, as GTK 3 uses it). They come from a bounded LIFO
 * pool instead, whose storage its owner provides (the server keeps it in its state: PSRAM on
 * the ESP32). xr_pool_get returns NULL when the pool is exhausted or none is in use: callers
 * then work on less (a smaller clip region never paints too much) and the failure is
 * counted; the server answers the request with BadAlloc. */
#ifndef XR_POOL_REGIONS
#define XR_POOL_REGIONS 16
#endif
typedef struct {
  xr_region slot[XR_POOL_REGIONS];
  int top, peak;           /* slots in use now; most ever in use */
  uint32_t gets, failures; /* allocations; allocations refused (exhausted) */
} xr_pool;

void xr_pool_use(xr_pool *p);    /* the pool temporaries come from (NULL: none) */
xr_pool *xr_pool_current(void);
xr_region *xr_pool_get(void);    /* an empty region, or NULL (exhausted, counted) */
int xr_pool_mark(void);          /* release everything taken after a mark (LIFO) */
void xr_pool_release(int mark);

/* Work counters (tests, diagnostics): pixels copied by the span fast path / per pixel. */
typedef struct {
  unsigned long copy_fast_px, copy_slow_px;
} xr_counters;
extern xr_counters xr_count;
extern int xr_copy_fast_enabled;  /* 1; tests set 0 to run the per-pixel reference path */

void xr_region_set(xr_region *g, int x1, int y1, int x2, int y2);
void xr_region_clear(xr_region *g);
void xr_region_intersect(xr_region *g, const xr_rect *r);
void xr_region_intersect_region(xr_region *g, const xr_region *other);
void xr_region_subtract(xr_region *g, const xr_rect *r);
void xr_region_translate(xr_region *g, int dx, int dy);
int xr_region_empty(const xr_region *g);
long xr_region_area(const xr_region *g);

/* Bounding box of pixels written since the last reset (damage tracking). */
typedef struct {
  int x1, y1, x2, y2;      /* half-open; empty when x1 >= x2 */
} xr_damage;

typedef struct {
  void *pix;               /* depth 16: uint16_t per pixel; depth 1: uint8_t per pixel (0/1) */
  int w, h, stride;        /* stride in pixels */
  int depth;               /* 1 or 16 */
  xr_damage *dmg;          /* NULL: no tracking (pixmaps); the screen tracks what it changes */
} xr_surface;

void xr_damage_reset(xr_damage *d);

typedef struct {
  uint8_t function;        /* GXclear 0 .. GXset 15 */
  uint32_t plane_mask;
  uint32_t fg, bg;
} xr_paint;

/* GC fill style for the drawing that follows (one request at a time, like the pool): the source
 * of every pixel drawn by points, lines, fills, polygons, arcs and text becomes the tile
 * (Tiled), the foreground where the stipple has a 1 (Stippled: nothing elsewhere), or the
 * foreground/background by the stipple (OpaqueStippled). The pattern repeats from (x, y) in
 * surface coordinates. NULL (the default): solid. Copies and images never use it. */
typedef struct {
  uint8_t style;            /* 1 Tiled, 2 Stippled, 3 OpaqueStippled */
  const xr_surface *pattern;/* tile (the target's depth) or stipple (depth 1); NULL: all fg / all 1 */
  int x, y;
} xr_fill;
void xr_fill_use(const xr_fill *f);
typedef struct {
  int32_t x, y;
} xr_point;

/* All coordinates are in surface pixels; the clip region is in the same coordinates. */
void xr_fill_rect(const xr_surface *s, const xr_region *clip, const xr_paint *p, int x, int y, int w, int h);
void xr_point_set(const xr_surface *s, const xr_region *clip, const xr_paint *p, int x, int y);
void xr_line(const xr_surface *s, const xr_region *clip, const xr_paint *p, int x0, int y0, int x1, int y1,
             int width, int skip_last);
/* rule 0 EvenOdd, 1 Winding */
void xr_fill_polygon(const xr_surface *s, const xr_region *clip, const xr_paint *p, const xr_point *pts, int n,
                     int rule);
/* angles in 1/64 degree, X11 convention (0 = 3 o'clock, counter-clockwise positive).
 * Outline uses `width`; filled arcs use arc_mode 0 Chord, 1 PieSlice. */
void xr_arc(const xr_surface *s, const xr_region *clip, const xr_paint *p, int x, int y, int w, int h, int a1,
            int a2, int width);
void xr_fill_arc(const xr_surface *s, const xr_region *clip, const xr_paint *p, int x, int y, int w, int h, int a1,
                 int a2, int arc_mode);
/* Copy a rectangle between surfaces of the same depth (src may equal dst, overlap safe).
 * Destination pixels outside clip are untouched; the function applies with src as source. */
void xr_copy(const xr_surface *dst, const xr_region *clip, const xr_paint *p, const xr_surface *src, int sx, int sy,
             int w, int h, int dx, int dy);
/* Depth-1 source plane drawn as fg (1) / bg (0) into dst (CopyPlane, XYBitmap images).
 * opaque 0: only 1-bits are drawn (stipple-like). */
void xr_copy_plane(const xr_surface *dst, const xr_region *clip, const xr_paint *p, const xr_surface *src, int sx,
                   int sy, int w, int h, int dx, int dy, int opaque);

/* Fill a rectangle with a tile (same depth as dst), the tile's origin at (ox, oy). */
void xr_fill_tile(const xr_surface *dst, const xr_region *clip, const xr_paint *p, const xr_surface *tile, int ox,
                  int oy, int x, int y, int w, int h);

uint32_t xr_get(const xr_surface *s, int x, int y);

#ifdef __cplusplus
}
#endif
#endif
