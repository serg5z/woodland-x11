/* Minimal PNG writer: 8-bit RGB, deflate "stored" blocks (no compression), CRC-32 and
 * Adler-32 computed here. Writes to path.tmp and renames, so readers never see a partial
 * file. Host tool only; not part of the portable core. */
#define _POSIX_C_SOURCE 200809L
#include "png_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc_table[256];

static uint32_t crc(uint32_t c, const uint8_t *p, size_t n) {
  if (!crc_table[1])
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t v = i;
      for (int k = 0; k < 8; ++k) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
      crc_table[i] = v;
    }
  c = ~c;
  while (n--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
  return ~c;
}

static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v; }

static int chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n) {
  uint8_t h[8];
  be32(h, n);
  memcpy(h + 4, type, 4);
  uint32_t c = crc(crc(0, h + 4, 4), data, n);
  uint8_t t[4];
  be32(t, c);
  return fwrite(h, 1, 8, f) == 8 && (n == 0 || fwrite(data, 1, n, f) == n) && fwrite(t, 1, 4, f) == 4;
}

int png_write_rgb565(const char *path, const uint16_t *pix, int w, int h) {
  size_t raw_n = (size_t)h * (1 + 3 * (size_t)w);
  size_t blocks = (raw_n + 65534) / 65535;
  size_t z_n = 2 + raw_n + 5 * blocks + 4;
  uint8_t *raw = malloc(raw_n), *z = malloc(z_n);
  if (!raw || !z) { free(raw); free(z); return -1; }
  uint8_t *r = raw;
  for (int y = 0; y < h; ++y) {
    *r++ = 0;  /* filter: none */
    for (int x = 0; x < w; ++x) {
      uint16_t v = pix[(size_t)y * w + x];
      uint8_t r5 = v >> 11, g6 = (v >> 5) & 63, b5 = v & 31;
      *r++ = (uint8_t)(r5 << 3 | r5 >> 2), *r++ = (uint8_t)(g6 << 2 | g6 >> 4), *r++ = (uint8_t)(b5 << 3 | b5 >> 2);
    }
  }
  uint8_t *o = z;
  *o++ = 0x78, *o++ = 0x01;
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < raw_n; ++i) a = (a + raw[i]) % 65521, b = (b + a) % 65521;
  for (size_t off = 0; off < raw_n; off += 65535) {
    size_t n = raw_n - off < 65535 ? raw_n - off : 65535;
    *o++ = off + n >= raw_n ? 1 : 0;
    *o++ = (uint8_t)n, *o++ = (uint8_t)(n >> 8), *o++ = (uint8_t)~n, *o++ = (uint8_t)(~n >> 8);
    memcpy(o, raw + off, n);
    o += n;
  }
  be32(o, b << 16 | a);
  o += 4;
  char tmp[1024];
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  FILE *f = fopen(tmp, "wb");
  int ok = f != NULL;
  if (ok) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)w), be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8, ihdr[9] = 2, ihdr[10] = 0, ihdr[11] = 0, ihdr[12] = 0;
    ok = fwrite(sig, 1, 8, f) == 8 && chunk(f, "IHDR", ihdr, 13) && chunk(f, "IDAT", z, (uint32_t)(o - z)) &&
         chunk(f, "IEND", NULL, 0);
    ok = fclose(f) == 0 && ok;
  }
  free(raw);
  free(z);
  if (!ok) return -1;
  return rename(tmp, path);
}
