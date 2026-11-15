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

const x11_glyph_metrics *x11_font_glyph_at(const x11_font *f, int c, int *slot) {
  int i = x11_font_slot(f, c);
  if (i < 0) return 0;
  const x11_glyph_metrics *m = x11_font_slot_metrics(f, i);
  if (!m->lsb && !m->rsb && !m->width && !m->ascent && !m->descent) return 0;  /* does not exist */
  *slot = i;
  return m;
}

const x11_glyph_metrics *x11_font_glyph(const x11_font *f, int c) {
  int i;
  return x11_font_glyph_at(f, c, &i);
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

static uint32_t rev8(uint32_t b) {  /* bit 7 leftmost -> bit 0 leftmost */
  b = (b & 0xF0u) >> 4 | (b & 0x0Fu) << 4;
  b = (b & 0xCCu) >> 2 | (b & 0x33u) << 2;
  return (b & 0xAAu) >> 1 | (b & 0x55u) << 1;
}

void x11_font_rows(const x11_font *f, int i, int y0, int n, uint32_t *out) {
  if (f->bits) {  /* x11_font_rows_ok: rsb - lsb <= 32, so per_row <= 4 */
    const x11_glyph_metrics *m = x11_font_slot_metrics(f, i);
    int per_row = (m->rsb - m->lsb + 7) / 8;
    const uint8_t *r = f->bits + f->bits_offset[i] + (uint32_t)(y0 * per_row);
    for (int y = 0; y < n; ++y, r += per_row) {
      uint32_t v = 0;
      for (int k = 0; k < per_row; ++k) v |= rev8(r[k]) << (8 * k);
      out[y] = v;
    }
  } else if (!f->index) {
    for (int y = 0; y < n; ++y) out[y] = rev8(x11_font_fixed_rows[i][y0 + y]);
  } else {
    const uint16_t *r = f->rows + f->index[i] + y0;
    for (int y = 0; y < n; ++y) out[y] = r[y];
  }
}
