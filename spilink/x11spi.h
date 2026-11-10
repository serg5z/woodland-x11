/* One virtual X11 byte stream inside a Woodland SPI link message. Test transport only.
 * The link supplies ordering, CRC, retransmission and credit. The session keeps old
 * messages from a previous bridge run out of a new X11 connection. */
#ifndef WOODLAND_X11SPI_H
#define WOODLAND_X11SPI_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define XSPI_HDR 16u
#define XSPI_DATA_MAX (4074u - XSPI_HDR)
enum { XSPI_OPEN = 1, XSPI_OPENED = 2, XSPI_DATA = 3, XSPI_CLOSE = 4 };

static inline uint32_t xspi_u32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline void xspi_put32(uint8_t *p, uint32_t v) {
  for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i));
}
/* Return 0 for a valid packet. A caller may treat other messages as a different channel. */
static inline int xspi_parse(const uint8_t *p, size_t n, uint8_t *op, uint32_t *session,
                             const uint8_t **data, size_t *len) {
  if (n < XSPI_HDR || n > 4074 || p[0] != 'W' || p[1] != 'X' || p[2] != 1 || p[4] || p[5] ||
      p[3] < XSPI_OPEN || p[3] > XSPI_CLOSE || p[12] || p[13] || p[14] || p[15]) return -1;
  size_t count = (size_t)p[10] | (size_t)p[11] << 8;
  if (count != n - XSPI_HDR || (p[3] != XSPI_DATA && count)) return -1;
  *op = p[3];
  *session = xspi_u32(p + 6);
  *data = p + XSPI_HDR;
  *len = count;
  return 0;
}
static inline size_t xspi_make(uint8_t *out, uint8_t op, uint32_t session, const uint8_t *data, size_t n) {
  if (op < XSPI_OPEN || op > XSPI_CLOSE || n > XSPI_DATA_MAX || (op != XSPI_DATA && n)) return 0;
  out[0] = 'W', out[1] = 'X', out[2] = 1, out[3] = op, out[4] = out[5] = 0;
  xspi_put32(out + 6, session);
  out[10] = (uint8_t)n, out[11] = (uint8_t)(n >> 8);
  out[12] = out[13] = out[14] = out[15] = 0;
  if (n) memcpy(out + XSPI_HDR, data, n);
  return XSPI_HDR + n;
}
#endif
