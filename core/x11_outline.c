/* Rectangle outlines for PolyRectangle (see x11_render.h).
 *
 * The protocol draws each rectangle as a closed five-point PolyLine with the GC's line width,
 * line style, dashes, cap and join styles, and draws no pixel more than once per rectangle.
 * Which pixels belong to wide lines, caps and joins follows the X.Org sample server (fb for thin
 * lines; mi/miwideline.c and mi/mipolyrect.c for wide ones), so that clients see what they see on
 * Xorg:
 *   - thin lines (width 0): the outline's pixels clockwise from (x, y); a dash step per pixel;
 *     the closing point is drawn again unless CapNotLast, except for solid lines with a solid
 *     fill (fb's fast path: a 0x0 rectangle draws nothing);
 *   - solid wide lines with JoinMiter: up to four bands (mi's fast path);
 *   - other wide lines: a band of `width` along each side, split into dashes, with caps where a
 *     dash ends inside a side and joins at corners where the line goes on, by mi's rules.
 * Pixel rule for wide shapes: a pixel is in when its centre is inside, centres on a left or top
 * edge are in, on a right or bottom edge out. Coordinates below are in half pixels.
 *
 * Where pieces overlap (corners, the closing point, sides folded back on each other) a pixel
 * takes the class of the piece Xorg draws last along the path (mi's span groups give the same
 * for functions such as GXxor, where it merges pieces before drawing).
 *
 * Deliberate difference from Xorg: Xorg draws some pixels twice within one rectangle (corners,
 * folded 0-wide or 0-high rectangles), which the protocol rules out; here each pixel is drawn
 * once. Only functions where drawing twice shows (GXxor, GXand, GXor, ...) differ.
 * Round caps and joins of lines wider than ARC_MAX use a plain circle test instead of mi's arc
 * code (no difference was found up to width 120). Where mi's own geometry is odd (the closing
 * projecting cap of a 0-wide rectangle covers its whole side), the pixels are still Xorg's.
 * Dash lists: up to XR_MAX_DASHES lengths (longer ones: BadImplementation, x11_server.c).
 *
 * Parts are ported from X.Org's xserver (mi/miwideline.c: the wide-line, dash, cap, join and
 * arc code; mi/mipolyrect.c: the solid JoinMiter rectangle path). Their notices follow and are
 * also in licenses/X.Org-xserver-mi-COPYING.txt (see NOTICE.md).
 *
 * From mi/miwideline.c:
 *
 * Copyright 1988, 1998  The Open Group
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation.
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE OPEN GROUP BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Except as contained in this notice, the name of The Open Group shall
 * not be used in advertising or otherwise to promote the sale, use or
 * other dealings in this Software without prior written authorization
 * from The Open Group.
 *
 * Copyright 1989 by Digital Equipment Corporation, Maynard, Massachusetts.
 *
 *                         All Rights Reserved
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose and without fee is hereby granted,
 * provided that the above copyright notice appear in all copies and that
 * both that copyright notice and this permission notice appear in
 * supporting documentation, and that the name of Digital not be
 * used in advertising or publicity pertaining to distribution of the
 * software without specific, written prior permission.
 *
 * DIGITAL DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE, INCLUDING
 * ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO EVENT SHALL
 * DIGITAL BE LIABLE FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR
 * ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
 * ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 *
 * From mi/mipolyrect.c:
 *
 * Copyright 1987, 1998  The Open Group
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation.
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * OPEN GROUP BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
 * AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Except as contained in this notice, the name of The Open Group shall not be
 * used in advertising or otherwise to promote the sale, use or other dealings
 * in this Software without prior written authorization from The Open Group.
 *
 * Copyright 1987 by Digital Equipment Corporation, Maynard, Massachusetts.
 *
 *                         All Rights Reserved
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose and without fee is hereby granted,
 * provided that the above copyright notice appear in all copies and that
 * both that copyright notice and this permission notice appear in
 * supporting documentation, and that the name of Digital not be
 * used in advertising or publicity pertaining to distribution of the
 * software without specific, written prior permission.
 *
 * DIGITAL DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE, INCLUDING
 * ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO EVENT SHALL
 * DIGITAL BE LIABLE FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR
 * ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
 * ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 * SOFTWARE.
 */
#include "x11_render.h"

#include <math.h>
#include <string.h>

enum { NONE = 0, ODD = 1, EVEN = 2 };
enum { SOLID = 0, ONOFF = 1, DOUBLE = 2 };
enum { CAP_NOTLAST = 0, CAP_BUTT = 1, CAP_ROUND = 2, CAP_PROJ = 3 };
enum { JOIN_MITER = 0, JOIN_ROUND = 1, JOIN_BEVEL = 2 };

#define CHUNK 256    /* columns per row pass */
#define ARC_MAX 64   /* round caps and joins: mi's arc spans up to this line width */

typedef struct {
  const xr_surface *s;
  const xr_region *clip;
  const xr_paint *even, *odd;
  int lw, style, gc_style, cap, join;
  int solid;                       /* draw as a solid line (no dash pattern) */
  int merge;                       /* mi merges spans before drawing (miSpansCarefulRop) */
  int fast_thin;                   /* fb's fast thin solid lines: no closing point */
  int32_t dl[2 * XR_MAX_DASHES];   /* dash lengths, odd-length lists doubled */
  int nd;
  int32_t period, off;
  int ex1, ey1, ex2, ey2;          /* clip extents (half-open) */
} ol;

static uint8_t row[CHUNK], ord[CHUNK];  /* class per column; the order of the piece it came from */

static int ceil2(int32_t v) { return v >= 0 ? (v + 1) / 2 : -((-v) / 2); }  /* ceil(v / 2) */
static int64_t ceil_div(int64_t n, int64_t d) { return n >= 0 ? (n + d - 1) / d : -((-n) / d); }  /* d > 0 */

/* The dash covering path position s >= 0: its index, and its extent [*a, *b) on the path. */
static int dash_at(const ol *o, int32_t s, int32_t *a, int32_t *b) {
  int32_t r = (int32_t)(((int64_t)o->off + s) % o->period), acc = 0;
  int i = 0;
  while (r >= acc + o->dl[i]) acc += o->dl[i++];
  *a = s - (r - acc), *b = *a + o->dl[i];
  return i;
}

static int dash_cls(const ol *o, int32_t s) {
  if (o->solid) return EVEN;
  int32_t a, b;
  return (dash_at(o, s, &a, &b) & 1) ? ODD : EVEN;
}

static int order;  /* the piece being marked (its place along the path) */

static void mark(int x1, int x2, int cls, int cx) {
  if (x1 < cx) x1 = cx;
  if (x2 > cx + CHUNK) x2 = cx + CHUNK;
  for (int x = x1; x < x2; ++x) {
    uint8_t *r = &row[x - cx], *o = &ord[x - cx];
    if (!*r || order > *o || (order == *o && cls > *r)) *r = (uint8_t)cls, *o = (uint8_t)order;
  }
}

static void flush(const ol *o, int py, int cx, int n) {
  for (int i = 0; i < n;) {
    int c = row[i], j = i + 1;
    while (j < n && row[j] == c) ++j;
    if (c) xr_fill_rect(o->s, o->clip, c == EVEN ? o->even : o->odd, cx + i, py, j - i, 1);
    i = j;
  }
}

/* ---- thin lines (fb's zero-width lines) ---------------------------------------------------- */

/* The class a pixel of the outline ends with: its last visit that draws. Visits are numbered
 * along the path (the dash position); sides of length 0 are not visited. */
static int thin_cls(const ol *o, int px, int py, int x, int y, int w, int h) {
  int32_t k[5];
  int n = 0, cls = NONE;
  if (py == y && px - x >= 0 && px - x < w) k[n++] = px - x;
  if (px == x + w && py - y >= 0 && py - y < h) k[n++] = w + (py - y);
  if (py == y + h && x + w - px >= 0 && x + w - px < w) k[n++] = w + h + (x + w - px);
  if (px == x && y + h - py >= 0 && y + h - py < h) k[n++] = 2 * w + h + (y + h - py);
  if (px == x && py == y && o->cap != CAP_NOTLAST && !o->fast_thin) k[n++] = 2 * w + 2 * h;  /* closing point */
  for (int i = 0; i < n; ++i) {
    int c = dash_cls(o, k[i]);
    if (c == EVEN || (c == ODD && o->style == DOUBLE)) cls = c;
  }
  return cls;
}

static void thin_row(const ol *o, int py, int x, int y, int w, int h) {
  if (py < o->ey1 || py >= o->ey2) return;
  int a = x > o->ex1 ? x : o->ex1, b = x + w + 1 < o->ex2 ? x + w + 1 : o->ex2;
  for (int cx = a; cx < b; cx += CHUNK) {
    int n = b - cx < CHUNK ? b - cx : CHUNK;
    for (int i = 0; i < n; ++i) row[i] = (uint8_t)thin_cls(o, cx + i, py, x, y, w, h);
    flush(o, py, cx, n);
  }
}

static void thin_col(const ol *o, int px, int x, int y, int w, int h) {
  if (px < o->ex1 || px >= o->ex2) return;
  int a = y + 1 > o->ey1 ? y + 1 : o->ey1, b = y + h < o->ey2 ? y + h : o->ey2;
  for (int py = a; py < b; ++py) {
    int c = thin_cls(o, px, py, x, y, w, h);
    if (c) xr_fill_rect(o->s, o->clip, c == EVEN ? o->even : o->odd, px, py, 1, 1);
  }
}

static void thin(const ol *o, int x, int y, int w, int h) {
  thin_row(o, y, x, y, w, h);
  if (h) thin_row(o, y + h, x, y, w, h);
  thin_col(o, x, x, y, w, h);
  if (w) thin_col(o, x + w, x, y, w, h);
}

/* ---- round caps and joins (mi's miLineArcI and miLineArcD at whole-pixel centres) ---------- */

typedef struct {
  int lw;
  int16_t xl[2 * ARC_MAX + 4], wd[2 * ARC_MAX + 4];  /* by row offset + ARC_MAX + 2; wd 0: none */
} arc_tab;
static arc_tab arc_i, arc_d;

static void arc_put(arc_tab *t, int dy, int xl, int w) {
  int i = dy + ARC_MAX + 2;
  if (i >= 0 && i < 2 * ARC_MAX + 4) t->xl[i] = (int16_t)xl, t->wd[i] = (int16_t)w;
}

static void arc_i_make(int lw) {
  if (arc_i.lw == lw) return;
  memset(&arc_i, 0, sizeof(arc_i));
  arc_i.lw = lw;
  if (lw == 1) {
    arc_put(&arc_i, 0, 0, 1);
    return;
  }
  int y = (lw >> 1) + 1, e = (lw & 1) ? -((4 * y) + 3) : -(8 * y), ex = -4, x = 0;
  while (y) {
    e += (8 * y) - 4;
    while (e >= 0) {
      x++;
      e += (ex = -((8 * x) + 4));
    }
    y--;
    int slw = (2 * x) + 1;
    if (e == ex && slw > 1) slw--;
    arc_put(&arc_i, -y, -x, slw);
    if (y != 0 && (slw > 1 || e != ex)) arc_put(&arc_i, y, -x, slw);
  }
}

static void arc_d_make(int lw) {
  if (arc_d.lw == lw) return;
  memset(&arc_d, 0, sizeof(arc_d));
  arc_d.lw = lw;
  const double xlk = 1.0, xrk = -1.0, yk = -1.0, radius = lw / 2.0;
  int y = (int)floor(radius + 1.0), ybase = -y, xl = 0, xr = 0;
  double el = radius * radius - (double)y * y - xlk, er = radius * radius - (double)y * y + xrk;
  while (y > 0) {
    double k = (2 * y) + yk;
    er += k;
    while (er > 0.0) {
      xr++;
      er += xrk - (2 * xr);
    }
    el += k;
    while (el >= 0.0) {
      xl--;
      el += (2 * xl) - xlk;
    }
    y--, ybase++;
    if (xr >= xl) arc_put(&arc_d, ybase, xl, xr - xl + 1);
  }
  er = xrk - (2 * xr) - er;
  el = (2 * xl) - xlk - el;
  int boty = (int)floor(-radius + 1.0);
  while (y > boty) {
    double k = (2 * y) + yk;
    er -= k;
    while (er >= 0.0 && xr >= 0) {
      xr--;
      er += xrk - (2 * xr);
    }
    el -= k;
    while (el > 0.0 && xl <= 0) {
      xl++;
      el += (2 * xl) - xlk;
    }
    y--, ybase++;
    if (xr >= xl) arc_put(&arc_d, ybase, xl, xr - xl + 1);
  }
}

/* mi's clipped arcs: a round join or cap limited by the faces of the line (miRoundJoinClip,
 * miRoundJoinFace, miRoundCapClip and miPolyBuildEdge), drawn by miLineArcD with its clip edges.
 * Sides are parallel to the axes, so each face gives a column edge or a row edge. */
typedef struct {
  int8_t kind, left;     /* kind 0: none, 1: column edge (x from row y down), 2: row edge (row y) */
  int32_t x, y;
} cedge;
typedef struct {
  cedge e[2];            /* mi's edge1 (left face) and edge2 (right face) */
} arc_clip;

typedef struct {
  int dx, dy;            /* direction (a left face: the side's; a right face: back along it) */
  int x, y;              /* the point the face is measured from */
  double xa, ya, k;      /* mi's face offsets and line constant */
} mface;

static mface face_left(int x, int y, int ux, int uy, double l) {  /* a side leaving (x, y) along (ux, uy) */
  mface f = {ux, uy, x, y, l * uy, -l * ux, 0};
  return f;
}
static mface face_right(int x, int y, int ux, int uy, double l) {  /* a side arriving at (x, y) along (ux, uy) */
  mface f = {-ux, -uy, x, y, -l * uy, l * ux, 0};
  return f;
}

/* join: miRoundJoinFace; else miRoundCapClip (isint: the line goes through the face point). */
static void face_edge(cedge *ce, const mface *f, int join, int isint) {
  int dx = -f->dy, dy = f->dx, left = 1;
  double ya = f->ya, k = join || isint ? 0.0 : f->k;
  if (join && ya > 0) ya = 0;
  if (dy < 0 || (dy == 0 && dx > 0)) {
    dx = -dx, dy = -dy, left = !left;
    if (!join) ya = -ya;
  }
  ce->left = (int8_t)!left;
  if (dy == 0) {
    ce->kind = 2, ce->y = (int32_t)ceil(f->ya) + f->y;
    return;
  }
  int64_t y = (int64_t)ceil(ya), xady = (int64_t)ceil(k) + y * dx;  /* miPolyBuildEdge (dx is 0) */
  int64_t x = xady <= 0 ? -(-xady / dy) - 1 : (xady - 1) / dy;
  ce->kind = 1, ce->x = (int32_t)(x + ce->left + f->x), ce->y = (int32_t)(y + f->y);
}

static void clip_make(arc_clip *c, const mface *f1, const mface *f2, int join, int isint) {
  memset(c, 0, sizeof(*c));
  if (f1) face_edge(&c->e[0], f1, join, isint);
  if (f2) face_edge(&c->e[1], f2, join, isint);
}

/* miLineArcD at a whole-pixel centre with clip edges: the span of row py, or 0. */
static int arc_d_clipped(int lw, int cx, int cy, const arc_clip *c, int py, int *x1, int *x2) {
  const double xlk = 1.0, xrk = -1.0, yk = -1.0, radius = lw / 2.0;
  int y = (int)floor(radius + 1.0), ybase = cy - y, ymin = ybase, ymax = 65536, xl = 0, xr = 0;
  int32_t ey[2], ex[2];
  int min_[2] = {0, 0}, ymin_[2];
  for (int i = 0; i < 2; ++i) {
    const cedge *e = &c->e[i];
    ey[i] = 65536, ex[i] = e->x, ymin_[i] = 65536;
    if (!e->kind) continue;
    ey[i] = e->y, ymin_[i] = e->y;
    if (e->kind == 2) {
      if (e->left) min_[i] = 1;
      else ymax = e->y;
      ey[i] = 65536;
    } else if (!e->left) {  /* (signdx < 0) == edgeleft, signdx 1 */
      min_[i] = 1;
    }
  }
  if (min_[0]) ymin = min_[1] && ymin_[0] > ymin_[1] ? ymin_[1] : ymin_[0];
  else if (min_[1]) ymin = ymin_[1];
  double el = radius * radius - (double)y * y - xlk, er = radius * radius - (double)y * y + xrk;
  int boty = 0, found = 0;
  if (ybase + y - boty > ymax) boty = ymax - ybase - y;
  for (int pass = 0; pass < 2; ++pass) {
    if (pass) {
      er = xrk - (2 * xr) - er;
      el = (2 * xl) - xlk - el;
      boty = (int)floor(-radius + 1.0);
      if (ybase + y - boty > ymax) boty = ymax - ybase - y;
    }
    while (y > boty) {
      double k = (2 * y) + yk;
      if (!pass) {
        er += k;
        while (er > 0.0) xr++, er += xrk - (2 * xr);
        el += k;
        while (el >= 0.0) xl--, el += (2 * xl) - xlk;
      } else {
        er -= k;
        while (er >= 0.0 && xr >= 0) xr--, er += xrk - (2 * xr);
        el -= k;
        while (el > 0.0 && xl <= 0) xl++, el += (2 * xl) - xlk;
      }
      y--, ybase++;
      if (ybase < ymin) continue;
      int xcl = xl + cx, xcr = xr + cx;
      for (int i = 0; i < 2; ++i)  /* CLIPSTEPEDGE */
        if (ybase == ey[i]) {
          if (c->e[i].left && ex[i] > xcl) xcl = ex[i];
          if (!c->e[i].left && ex[i] < xcr) xcr = ex[i];
          ey[i]++;
        }
      if (ybase == py) {
        *x1 = xcl, *x2 = xcr + 1;
        found = xcr >= xcl;
      }
    }
  }
  return found;
}

/* One row of a disc of diameter lw centred on (cx, cy): clipped as mi does (c), or limited to the
 * side (kx, ky) points to with the boundary included (caps inside a side). */
static void disc_row(const ol *o, int cx, int cy, int d, int kx, int ky, const arc_clip *c, int cls, int py,
                     int px0) {
  int dy = py - cy, xl, w;
  if ((ky > 0 && dy < 0) || (ky < 0 && dy > 0)) return;
  if (c) {
    int x1, x2;
    if (arc_d_clipped(o->lw, cx, cy, c, py, &x1, &x2)) mark(x1, x2, cls, px0);
    return;
  }
  if (o->lw <= ARC_MAX) {
    const arc_tab *t = d ? &arc_d : &arc_i;
    int i = dy + ARC_MAX + 2;
    if (i < 0 || i >= 2 * ARC_MAX + 4 || !t->wd[i]) return;
    xl = t->xl[i], w = t->wd[i];
  } else {  /* wide: pixel centres strictly inside the circle */
    int64_t r2 = (int64_t)o->lw * o->lw, h = 0;
    if (4 * (int64_t)dy * dy >= r2) return;
    while (4 * ((h + 1) * (h + 1) + (int64_t)dy * dy) < r2) ++h;
    xl = (int)-h, w = (int)(2 * h + 1);
  }
  int x1 = cx + xl, x2 = x1 + w;
  if (kx > 0 && x1 < cx) x1 = cx;
  if (kx < 0 && x2 > cx + 1) x2 = cx + 1;
  if (x1 < x2) mark(x1, x2, cls, px0);
}

/* ---- wide lines (mi's miWideLine and miWideDash on the rectangle's path) ------------------- */

typedef struct {
  int x, y, dx, dy;   /* start and unit direction */
  int32_t len, s0;    /* length; path position of the start */
} side;

enum { SH_RECT, SH_TRI, SH_DISC };
typedef struct {
  uint8_t kind, cls, arcd, order, clipped;
  int32_t v[6];       /* RECT: x1 x2 y1 y2 (half pixels, half-open); TRI: 3 points; DISC: centre */
  arc_clip c;         /* DISC, when clipped */
} shape;

typedef struct {
  side e[4];
  int n;              /* sides of non-zero length, in path order */
  int project_right;  /* the last dash of the last side gets a projecting cap */
  shape sh[8];
  int nsh;
  int order;          /* for the shapes added next: after the sides drawn so far */
} path;

static int clip_caps(const ol *o) {  /* mi clips round caps to their side of the end */
  return (o->gc_style != SOLID || o->lw > 2) && o->cap == CAP_ROUND && o->join != JOIN_ROUND;
}
static int clip_joins(const ol *o) {
  return (o->gc_style != SOLID || o->lw > 2) && o->join == JOIN_ROUND && o->cap == CAP_BUTT;
}

/* A round join or cap centred on (x, y); with faces (f1 left, f2 right) it is clipped as mi does
 * and drawn with miLineArcD, without them it is whole (arcd: miLineArcD, else miLineArcI). */
static void add_disc(const ol *o, path *p, int x, int y, int arcd, const mface *f1, const mface *f2, int join,
                     int cls) {
  shape *s = &p->sh[p->nsh++];
  memset(s, 0, sizeof(*s));
  s->kind = SH_DISC, s->cls = (uint8_t)cls, s->arcd = (uint8_t)(arcd || f1 || f2), s->order = (uint8_t)p->order;
  s->v[0] = x, s->v[1] = y;
  if (f1 || f2) s->clipped = 1, clip_make(&s->c, f1, f2, join, 1);
  (void)o;
}

/* A round join where a side arriving along a meets a side leaving along b: mi clips it to the
 * outside of both faces when clip_joins(). */
static void add_round_join(const ol *o, path *p, const side *a, const side *b, int x, int y, int cls) {
  if (!clip_joins(o)) {
    add_disc(o, p, x, y, 0, 0, 0, 1, cls);
    return;
  }
  double l = o->lw / 2.0;
  mface fl = face_left(x, y, b->dx, b->dy, l), fr = face_right(x, y, a->dx, a->dy, l);
  if (-fl.dx * fr.dy + fr.dx * fl.dy >= 0) fl.xa = -fl.xa, fl.ya = -fl.ya;  /* miRoundJoinClip */
  else fr.xa = -fr.xa, fr.ya = -fr.ya;
  add_disc(o, p, x, y, 1, &fl, &fr, 1, cls);
}

static void add_rect(path *p, int32_t x1, int32_t x2, int32_t y1, int32_t y2, int cls) {
  shape *s = &p->sh[p->nsh++];
  s->kind = SH_RECT, s->cls = (uint8_t)cls, s->order = (uint8_t)p->order;
  s->v[0] = x1 < x2 ? x1 : x2, s->v[1] = x1 < x2 ? x2 : x1, s->v[2] = y1 < y2 ? y1 : y2, s->v[3] = y1 < y2 ? y2 : y1;
}

/* The join where side a (arriving) meets side b (leaving) at (x, y). */
static void add_join(const ol *o, path *p, const side *a, const side *b, int x, int y, int cls) {
  int lw = o->lw;
  if (lw == 1 && !o->merge) {  /* miLineJoin's shortcut: a side may draw the joining pixel */
    if (b->dx > 0 || (b->dx == 0 && b->dy > 0)) return;
    if (a->dx < 0 || (a->dx == 0 && a->dy < 0)) return;
    if (o->join != JOIN_ROUND && a->dx == -b->dx && a->dy == -b->dy) return;
    if (o->join != JOIN_MITER) {
      add_rect(p, 2 * x - 1, 2 * x + 1, 2 * y - 1, 2 * y + 1, cls);
      return;
    }
  }
  if (a->dx == -b->dx && a->dy == -b->dy) {  /* the path turns back: only a round join draws */
    if (o->join == JOIN_ROUND) add_round_join(o, p, a, b, x, y, cls);
    return;
  }
  int kx = a->dx - b->dx, ky = a->dy - b->dy;  /* the outer quadrant */
  if (o->join == JOIN_MITER) {
    add_rect(p, 2 * x, 2 * x + lw * kx, 2 * y, 2 * y + lw * ky, cls);
  } else if (o->join == JOIN_BEVEL) {
    shape *s = &p->sh[p->nsh++];
    s->kind = SH_TRI, s->cls = (uint8_t)cls, s->order = (uint8_t)p->order;
    s->v[0] = 2 * x, s->v[1] = 2 * y;
    s->v[2] = 2 * x - lw * b->dx, s->v[3] = 2 * y - lw * b->dy;
    s->v[4] = 2 * x + lw * a->dx, s->v[5] = 2 * y + lw * a->dy;
  } else {
    add_round_join(o, p, a, b, x, y, cls);
  }
}

/* A round cap at a corner: at the start of side e (start) or at the end of it. */
static void add_round_cap(const ol *o, path *p, const side *e, int start, int cls) {
  int x = start ? e->x : e->x + e->dx * e->len, y = start ? e->y : e->y + e->dy * e->len;
  if (!clip_caps(o)) {
    add_disc(o, p, x, y, 0, 0, 0, 0, cls);
    return;
  }
  double l = o->lw / 2.0;
  mface f = start ? face_left(x, y, e->dx, e->dy, l) : face_right(x, y, e->dx, e->dy, l);
  add_disc(o, p, x, y, 1, start ? &f : 0, start ? 0 : &f, 0, cls);
}

static void build(const ol *o, path *p, int x, int y, int w, int h) {
  static const int dir[4][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};
  int px[4] = {x, x + w, x + w, x}, py[4] = {y, y, y + h, y + h};
  int32_t len[4] = {w, h, w, h}, s = 0;
  memset(p, 0, sizeof(*p));
  for (int i = 0; i < 4; ++i) {
    if (len[i]) {
      side *e = &p->e[p->n++];
      e->x = px[i], e->y = py[i], e->dx = dir[i][0], e->dy = dir[i][1], e->len = len[i], e->s0 = s;
    }
    s += len[i];
  }
  int lw = o->lw;
  if (!p->n) {  /* all four points coincide */
    int cls = o->solid ? EVEN : dash_cls(o, 0);
    if (!o->solid && o->style == ONOFF && cls == ODD) return;
    if (o->cap == CAP_PROJ) {
      add_rect(p, 2 * (x - (lw >> 1)), 2 * (x - (lw >> 1) + lw), 2 * (y - (lw >> 1)), 2 * (y - (lw >> 1) + lw), cls);
    } else if (o->cap == CAP_ROUND) {
      add_disc(o, p, x, y, o->solid ? clip_caps(o) : 1, 0, 0, 0, cls);  /* miWideLine: isInt; miWideDash: not */
    }
    return;
  }
  const side *first = &p->e[0], *last = &p->e[p->n - 1];
  if (o->solid) {
    for (int i = 1; i < p->n; ++i) {
      p->order = 2 * i + 2;
      add_join(o, p, &p->e[i - 1], &p->e[i], p->e[i].x, p->e[i].y, EVEN);
    }
    p->order = 2 * p->n + 1;
    add_join(o, p, last, first, first->x, first->y, EVEN);
    return;
  }
  int first_fg = dash_cls(o, first->s0) == EVEN, prev_fg = 0;
  p->project_right = o->cap == CAP_PROJ && !first_fg && h > 0;  /* mi: only on the path's last segment */
  for (int i = 0; i < p->n; ++i) {
    const side *e = &p->e[i];
    int start_fg = dash_cls(o, e->s0) == EVEN;
    p->order = 2 * i + 2;
    if (i && (o->style == DOUBLE || start_fg)) {
      int cls = start_fg ? EVEN : ODD;
      if (o->style == ONOFF && !prev_fg) {
        if (o->cap == CAP_ROUND) add_round_cap(o, p, e, 1, cls);
      } else {
        add_join(o, p, &p->e[i - 1], e, e->x, e->y, cls);
      }
    }
    prev_fg = dash_cls(o, e->s0 + e->len - 1) == EVEN;
  }
  int end_fg = prev_fg;
  p->order = 2 * p->n + 1;
  if (o->style == DOUBLE || end_fg) {
    int cls = end_fg ? EVEN : ODD;
    if (o->style == DOUBLE || first_fg)
      add_join(o, p, last, first, first->x, first->y, cls);
    else if (o->cap == CAP_ROUND)
      add_round_cap(o, p, last, 0, cls);
  } else if (first_fg) {  /* OnOffDash ending off: a cap on the start of the first dash */
    if (o->cap == CAP_PROJ) {  /* miLineProjectingCap */
      int fx = 2 * first->x, fy = 2 * first->y, h1 = lw >> 1;
      if (first->dx)
        add_rect(p, fx, fx - lw * first->dx, fy - lw, fy + lw, EVEN);
      else  /* a 0-wide rectangle starts down its right side: mi's cap covers that whole side */
        add_rect(p, 2 * (first->x - h1), 2 * (first->x - h1 + lw), 2 * (first->y - h1), 2 * (first->y + first->len), EVEN);
    } else if (o->cap == CAP_ROUND) {
      add_round_cap(o, p, first, 1, EVEN);
    }
  }
}

/* The clips of the round caps of the dash [t0, t1] inside side e (mi's lcapFace and rcapFace in
 * miWideDashSegment: measured from the side's start, or from its end for the last dash). */
static void dash_cap_clips(const ol *o, const side *e, int32_t t0, int32_t t1, arc_clip *c0, arc_clip *c1) {
  double l = o->lw / 2.0, L = e->len, rdx = l * e->dx, rdy = l * e->dy;
  int neg = e->dx < 0, x2 = e->x + e->dx * e->len, y2 = e->y + e->dy * e->len;
  mface f;
  if (t1 < e->len) {  /* a dash ending inside the side */
    f.dx = e->dx * e->len, f.dy = e->dy * e->len, f.x = e->x, f.y = e->y;
    if (neg) f.xa = -(-rdy + t0 * e->dx), f.ya = -(rdx + t0 * e->dy), f.k = -t0 * L;
    else f.xa = rdy + t0 * e->dx, f.ya = -rdx + t0 * e->dy, f.k = t0 * L;
    clip_make(c0, &f, 0, 0, 0);
    f.dx = -f.dx, f.dy = -f.dy;
    if (neg) f.xa = -rdy + t1 * e->dx, f.ya = rdx + t1 * e->dy, f.k = t1 * L;
    else f.xa = -(rdy + t1 * e->dx), f.ya = -(-rdx + t1 * e->dy), f.k = -t1 * L;
    clip_make(c1, 0, &f, 0, 0);
  } else {  /* the side's last dash: only its start has a cap */
    f.dx = e->dx * e->len, f.dy = e->dy * e->len, f.x = x2, f.y = y2;
    if (neg) f.xa = -(-rdy + (t0 - L) * e->dx), f.ya = -(rdx + (t0 - L) * e->dy), f.k = (L - t0) * L;
    else f.xa = rdy + (t0 - L) * e->dx, f.ya = -rdx + (t0 - L) * e->dy, f.k = -(L - t0) * L;
    clip_make(c0, &f, 0, 0, 0);
  }
}

/* The dashes of one side that cover positions [ta, tb], marked for row py. */
static void side_row(const ol *o, const path *p, const side *e, int last, int32_t ta, int32_t tb, int py, int px0) {
  int lw = o->lw;
  int32_t t = ta < 0 ? 0 : ta, hi = tb < e->len - 1 ? tb : e->len - 1;
  int horiz = e->dy == 0;
  int band = horiz ? (2 * py >= 2 * e->y - lw && 2 * py < 2 * e->y + lw) : 0;
  while (t <= hi) {
    int32_t a, b, t0, t1;
    int idx = o->solid ? 0 : dash_at(o, e->s0 + t, &a, &b);
    if (o->solid) a = e->s0, b = e->s0 + e->len;
    t0 = a - e->s0 < 0 ? 0 : a - e->s0;
    t1 = b - e->s0 > e->len ? e->len : b - e->s0;
    t = t1;
    int cls = (idx & 1) ? ODD : EVEN;
    if (o->style == ONOFF && cls == ODD) continue;
    int ext0 = 0, ext1 = 0, cap0 = 0, cap1 = 0;
    if (o->style == ONOFF) {
      if (t0 > 0) ext0 = o->cap == CAP_PROJ ? lw : 0, cap0 = o->cap == CAP_ROUND;
      if (t1 < e->len) ext1 = o->cap == CAP_PROJ ? lw : 0, cap1 = o->cap == CAP_ROUND;
    }
    if (t1 == e->len && last && p->project_right) ext1 = lw;
    /* the band, in half pixels along the side, then across */
    int32_t u0 = 2 * t0 - ext0, u1 = 2 * t1 + ext1;
    if (horiz) {
      if (band) {
        int32_t xa = 2 * e->x + e->dx * u0, xb = 2 * e->x + e->dx * u1;
        mark(ceil2(xa < xb ? xa : xb), ceil2(xa < xb ? xb : xa), cls, px0);
      }
    } else {
      int32_t ya = 2 * e->y + e->dy * u0, yb = 2 * e->y + e->dy * u1;
      if (2 * py >= (ya < yb ? ya : yb) && 2 * py < (ya < yb ? yb : ya))
        mark(ceil2(2 * e->x - lw), ceil2(2 * e->x + lw), cls, px0);
    }
    if (cap0 || cap1) {
      arc_clip c0, c1;
      int c = clip_caps(o);
      if (c) dash_cap_clips(o, e, t0, t1, &c0, &c1);
      if (cap0) disc_row(o, e->x + e->dx * t0, e->y + e->dy * t0, 1, 0, 0, c ? &c0 : 0, cls, py, px0);
      if (cap1) disc_row(o, e->x + e->dx * t1, e->y + e->dy * t1, 1, 0, 0, c ? &c1 : 0, cls, py, px0);
    }
  }
}

static void shape_row(const ol *o, const shape *s, int py, int px0) {
  int32_t Y = 2 * py;
  order = s->order;
  if (s->kind == SH_RECT) {
    if (Y >= s->v[2] && Y < s->v[3]) mark(ceil2(s->v[0]), ceil2(s->v[1]), s->cls, px0);
  } else if (s->kind == SH_DISC) {
    disc_row(o, s->v[0], s->v[1], s->arcd, 0, 0, s->clipped ? &s->c : 0, s->cls, py, px0);
  } else {  /* a triangle: rows top-inclusive; left edges in, right edges out */
    int64_t xn[2], xd[2];
    int k = 0;
    for (int i = 0; i < 3 && k < 2; ++i) {
      int32_t x1 = s->v[2 * i], y1 = s->v[2 * i + 1], x2 = s->v[(2 * i + 2) % 6], y2 = s->v[(2 * i + 3) % 6];
      if (y1 == y2) continue;
      int32_t lo = y1 < y2 ? y1 : y2, hi = y1 < y2 ? y2 : y1;
      if (Y < lo || Y >= hi) continue;
      int64_t d = y2 - y1, n = (int64_t)x1 * d + (int64_t)(Y - y1) * (x2 - x1);
      if (d < 0) d = -d, n = -n;
      xn[k] = n, xd[k] = d, ++k;
    }
    if (k < 2) return;
    int l = xn[0] * xd[1] <= xn[1] * xd[0] ? 0 : 1;
    int64_t a = ceil_div(xn[l], 2 * xd[l]), b = ceil_div(xn[!l], 2 * xd[!l]);
    if (a < b) mark((int)a, (int)b, s->cls, px0);
  }
}

static void wide_row(const ol *o, const path *p, int py, int x1, int x2) {
  int R = o->lw / 2 + 2;
  for (int cx = x1; cx < x2; cx += CHUNK) {
    int n = x2 - cx < CHUNK ? x2 - cx : CHUNK;
    memset(row, 0, (size_t)n);
    memset(ord, 0, (size_t)n);
    for (int i = 0; i < p->n; ++i) {
      const side *e = &p->e[i];
      int last = i == p->n - 1;
      order = 2 * i + 1;
      if (e->dy == 0) {
        if (py < e->y - R || py > e->y + R) continue;
        int32_t a = e->dx > 0 ? cx - R - e->x : e->x - (cx + n + R), b = e->dx > 0 ? cx + n + R - e->x : e->x - (cx - R);
        side_row(o, p, e, last, a, b, py, cx);
      } else {
        if (cx + n <= e->x - R || cx > e->x + R) continue;
        int32_t t = (py - e->y) * e->dy;
        side_row(o, p, e, last, t - R, t + R, py, cx);
      }
    }
    for (int i = 0; i < p->nsh; ++i) shape_row(o, &p->sh[i], py, cx);
    flush(o, py, cx, n);
  }
}

static void wide(const ol *o, int x, int y, int w, int h) {
  static path p;  /* off the loop task's stack, like row[] (one request at a time) */
  build(o, &p, x, y, w, h);
  int R = o->lw / 2 + 2;
  int y1 = y - R > o->ey1 ? y - R : o->ey1, y2 = y + h + R + 1 < o->ey2 ? y + h + R + 1 : o->ey2;
  int xa = x - R > o->ex1 ? x - R : o->ex1, xb = x + w + R + 1 < o->ex2 ? x + w + R + 1 : o->ex2;
  for (int py = y1; py < y2; ++py) {
    if (py <= y + R || py >= y + h - R) {  /* near the top or bottom side: the whole width */
      wide_row(o, &p, py, xa, xb);
      continue;
    }
    int l1 = x - R > xa ? x - R : xa, l2 = x + R + 1 < xb ? x + R + 1 : xb;  /* else only the sides */
    int r1 = x + w - R > xa ? x + w - R : xa, r2 = x + w + R + 1 < xb ? x + w + R + 1 : xb;
    if (r1 < l2) r1 = l2;
    if (l1 < l2) wide_row(o, &p, py, l1, l2);
    if (r1 < r2) wide_row(o, &p, py, r1, r2);
  }
}

/* mi's fast path for solid wide lines with JoinMiter: bands, with mi's bounds and its handling
 * of rectangles thinner than the line. */
static int16_t bound(int v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }
static uint16_t ubound(int v) { return (uint16_t)(v > 65535 ? 65535 : v); }

static void miter_rect(const ol *o, int ox, int oy, int x, int y, int w, int h) {
  int lw = o->lw, o1 = lw >> 1, o3 = lw - o1;
  int16_t rx[4], ry[4];
  uint16_t rw[4], rh[4];
  int n = 0;
  if (h < lw || w < o1) {
    if (h == 0) rx[0] = (int16_t)x, rw[0] = (uint16_t)w;
    else rx[0] = bound(x - o1), rw[0] = ubound(w + lw);
    if (w == 0) ry[0] = (int16_t)y, rh[0] = (uint16_t)h;
    else ry[0] = bound(y - o1), rh[0] = ubound(h + lw);
    n = 1;
  } else {
    rx[0] = bound(x - o1), ry[0] = bound(y - o1), rw[0] = ubound(w + lw), rh[0] = (uint16_t)lw;
    rx[1] = bound(x - o1), ry[1] = bound(y + o3), rw[1] = (uint16_t)lw, rh[1] = (uint16_t)(h - lw);
    rx[2] = bound(x + w - o1), ry[2] = bound(y + o3), rw[2] = (uint16_t)lw, rh[2] = (uint16_t)(h - lw);
    rx[3] = bound(x - o1), ry[3] = bound(y + h - o1), rw[3] = ubound(w + lw), rh[3] = (uint16_t)lw;
    n = 4;
  }
  for (int i = 0; i < n; ++i) xr_fill_rect(o->s, o->clip, o->even, ox + rx[i], oy + ry[i], rw[i], rh[i]);
}

void xr_rect_outline(const xr_surface *s, const xr_region *clip, const xr_paint *even, const xr_paint *odd,
                     const xr_line_attr *la, int ox, int oy, int x, int y, int w, int h) {
  ol o;
  memset(&o, 0, sizeof(o));
  o.s = s, o.clip = clip, o.even = even, o.odd = odd;
  o.merge = (even->function & 0xc) == 0x8 || (even->function & 0x3) == 0x2;
  o.lw = la->width, o.gc_style = la->style, o.cap = la->cap, o.join = la->join;
  o.style = la->style;
  if (la->style == DOUBLE && (la->fill_style == 1 || la->fill_style == 3)) o.style = SOLID;  /* odd look like even */
  o.fast_thin = la->style == SOLID && la->fill_style == 0;
  o.solid = o.style == SOLID;
  if (!o.solid) {
    int n = la->ndash;
    for (int r = 0; r < ((n & 1) ? 2 : 1); ++r)
      for (int i = 0; i < n; ++i) o.dl[o.nd++] = la->dash[i];
    for (int i = 0; i < o.nd; ++i) o.period += o.dl[i];
    o.off = la->dash_offset % o.period;
  }
  if (!clip->n) return;
  o.ex1 = clip->r[0].x1, o.ey1 = clip->r[0].y1, o.ex2 = clip->r[0].x2, o.ey2 = clip->r[0].y2;
  for (int i = 1; i < clip->n; ++i) {
    if (clip->r[i].x1 < o.ex1) o.ex1 = clip->r[i].x1;
    if (clip->r[i].y1 < o.ey1) o.ey1 = clip->r[i].y1;
    if (clip->r[i].x2 > o.ex2) o.ex2 = clip->r[i].x2;
    if (clip->r[i].y2 > o.ey2) o.ey2 = clip->r[i].y2;
  }
  /* mi bounds the corners to 16 bits (drawable coordinates) */
  if (x + w > 32767) w = 32767 - x;
  if (y + h > 32767) h = 32767 - y;
  if (o.lw == 0) {
    if (o.solid && w && h) {  /* the common case: four bands, each pixel once (as the model draws it) */
      int ax = ox + x, ay = oy + y;
      xr_fill_rect(s, clip, even, ax, ay, w + 1, 1);
      xr_fill_rect(s, clip, even, ax, ay + h, w + 1, 1);
      xr_fill_rect(s, clip, even, ax, ay + 1, 1, h - 1);
      xr_fill_rect(s, clip, even, ax + w, ay + 1, 1, h - 1);
      return;
    }
    thin(&o, ox + x, oy + y, w, h);
    return;
  }
  if (la->style == SOLID && o.join == JOIN_MITER && (w || h)) {
    miter_rect(&o, ox, oy, x, y, w, h);
    return;
  }
  if (o.lw <= ARC_MAX && (o.cap == CAP_ROUND || o.join == JOIN_ROUND)) arc_i_make(o.lw), arc_d_make(o.lw);
  wide(&o, ox + x, oy + y, w, h);
}
