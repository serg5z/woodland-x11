/* The server's built-in bitmap fonts.
 *   "fixed" (aliases "6x12" and its XLFD): 6x12 cells, ascent 10, descent 2, characters
 *     32..126, from Terminus Font (licenses/OFL-Terminus.txt). The default font.
 *   "cursor": the standard X cursor font (glyphs 0..153, tools/gen_cursor_font.py), for
 *     CreateGlyphCursor and XCreateFontCursor. Cursors are stored, not drawn (no pointer).
 * Nothing else is advertised: OpenFont and ListFonts know exactly these names. */
#ifndef X11_FONT_H_INCLUDED
#define X11_FONT_H_INCLUDED
#include <stdint.h>

#define X11_FONT_W 6
#define X11_FONT_H 12
#define X11_FONT_ASCENT 10
#define X11_FONT_DESCENT 2
#define X11_FONT_FIRST 32
#define X11_FONT_LAST 126
#define X11_FONT_XLFD "-misc-fixed-medium-r-normal--12-120-75-75-c-60-iso8859-1"

extern const uint8_t x11_font_fixed_rows[95][12];  /* bit 7 = leftmost pixel */

/* A character's metrics (X11 CHARINFO without attributes). All zero: it does not exist. */
typedef struct {
  int16_t lsb, rsb, width, ascent, descent;
} x11_glyph_metrics;

/* A font property: an atom value (str) or an integer. */
typedef struct {
  const char *name, *str;
  int32_t value;
} x11_font_prop;

/* Characters are byte1 << 8 | byte2: byte1 in min_byte1..max_byte1 (0..0 for a single-byte
 * font), byte2 in first..last. Slot i = (byte1 - min_byte1) * (last - first + 1) +
 * (byte2 - first) indexes metrics, index and bits_offset. */
typedef struct {
  const char *name;                   /* canonical name */
  const char *const *aliases;         /* more names for the same font */
  int naliases;
  uint16_t first, last;               /* byte2 range (min/max-char-or-byte2) */
  uint16_t default_char;              /* byte1 << 8 | byte2 */
  int16_t ascent, descent;            /* font-ascent, font-descent */
  x11_glyph_metrics min_bounds, max_bounds;
  const x11_glyph_metrics *metrics;   /* per slot; NULL: all max_bounds */
  const uint16_t *index;              /* per slot: first row in rows; NULL: fixed rows or bits */
  const uint16_t *rows;               /* bit x = pixel x right of lsb */
  int monospace;
  const char *font_property;          /* value of the FONT property (the full name), or NULL */
  /* two-byte fonts and byte-row glyph bitmaps (the generated test fonts) */
  uint8_t min_byte1, max_byte1;
  const uint32_t *bits_offset;        /* per slot: first byte in bits */
  const uint8_t *bits;                /* rows of (rsb - lsb + 7) / 8 bytes, bit 7 leftmost */
  const x11_font_prop *props;         /* all properties (font_property is then unused) */
  int nprops;
  uint8_t direction;                  /* draw-direction: 0 LeftToRight, 1 RightToLeft */
} x11_font;

extern const x11_font x11_font_fixed, x11_font_cursor;
extern const x11_font *const x11_fonts[2];
#ifdef X11_TEST_FONTS  /* host test builds: fonts compiled by tools/gen_pcf_fonts.py */
extern const x11_font *const x11_test_fonts[];
extern const int x11_test_font_count;
#endif

/* Number of slots (characters in the encoding ranges, existing or not). */
int x11_font_slots(const x11_font *f);
/* Slot of character c (byte1 << 8 | byte2), or -1 outside the ranges. */
int x11_font_slot(const x11_font *f, int c);
/* Metrics of a slot (all zero for a character that does not exist). */
const x11_glyph_metrics *x11_font_slot_metrics(const x11_font *f, int slot);
/* Metrics of character c, or NULL if the font does not define it. */
const x11_glyph_metrics *x11_font_glyph(const x11_font *f, int c);
/* Pixel (x, y) of the character in `slot`, which must exist: x from 0 at lsb, y from 0 at
 * the top (ascent + descent rows). */
int x11_font_pixel(const x11_font *f, int slot, int x, int y);
#endif
