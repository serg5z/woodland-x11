/* Font descriptors over the generated glyph tables (see x11_font.h). */
#include "x11_font.h"

static const char *const FIXED_ALIASES[] = {"6x12", X11_FONT_XLFD};

const x11_font x11_font_fixed = {
    "fixed", FIXED_ALIASES, 2, X11_FONT_FIRST, X11_FONT_LAST, ' ', X11_FONT_ASCENT, X11_FONT_DESCENT,
    {0, X11_FONT_W, X11_FONT_W, X11_FONT_ASCENT, X11_FONT_DESCENT},
    {0, X11_FONT_W, X11_FONT_W, X11_FONT_ASCENT, X11_FONT_DESCENT}, 0, 0, 0, 1, X11_FONT_XLFD, 0, 0, 0, 0, 0, 0, 0};

const x11_font *const x11_fonts[2] = {&x11_font_fixed, &x11_font_cursor};

int x11_font_slots(const x11_font *f) { return (f->max_byte1 - f->min_byte1 + 1) * (f->last - f->first + 1); }

int x11_font_slot(const x11_font *f, int c) {
  int b1 = c >> 8 & 0xFF, b2 = c & 0xFF;
  if (b1 < f->min_byte1 || b1 > f->max_byte1 || b2 < f->first || b2 > f->last) return -1;
  return (b1 - f->min_byte1) * (f->last - f->first + 1) + (b2 - f->first);
}

const x11_glyph_metrics *x11_font_slot_metrics(const x11_font *f, int slot) {
  return f->metrics ? &f->metrics[slot] : &f->max_bounds;
}

const x11_glyph_metrics *x11_font_glyph(const x11_font *f, int c) {
  int i = x11_font_slot(f, c);
  if (i < 0) return 0;
  const x11_glyph_metrics *m = x11_font_slot_metrics(f, i);
  if (!m->lsb && !m->rsb && !m->width && !m->ascent && !m->descent) return 0;  /* does not exist */
  return m;
}

int x11_font_pixel(const x11_font *f, int i, int x, int y) {
  if (f->bits) {
    const x11_glyph_metrics *m = x11_font_slot_metrics(f, i);
    int per_row = (m->rsb - m->lsb + 7) / 8;
    return f->bits[f->bits_offset[i] + (uint32_t)(y * per_row + x / 8)] >> (7 - (x & 7)) & 1;
  }
  if (!f->index)  /* the 6x12 table: bit 7 = leftmost */
    return x11_font_fixed_rows[i][y] >> (7 - x) & 1;
  return f->rows[f->index[i] + y] >> x & 1;
}
