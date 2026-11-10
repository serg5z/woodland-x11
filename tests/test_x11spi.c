#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "../spilink/x11spi.h"

int main(void) {
  uint8_t packet[4074], bytes[XSPI_DATA_MAX], op;
  const uint8_t *data;
  uint32_t session;
  size_t len;
  for (size_t i = 0; i < sizeof(bytes); ++i) bytes[i] = (uint8_t)i;
  size_t n = xspi_make(packet, XSPI_DATA, 0x12345678u, bytes, sizeof(bytes));
  assert(n == 4074);
  assert(xspi_parse(packet, n, &op, &session, &data, &len) == 0);
  assert(op == XSPI_DATA && session == 0x12345678u && len == sizeof(bytes));
  assert(memcmp(data, bytes, len) == 0);
  assert(xspi_make(packet, XSPI_DATA, 1, bytes, sizeof(bytes) + 1) == 0);
  assert(xspi_parse(packet, n - 1, &op, &session, &data, &len) != 0);
  packet[2] = 2;
  assert(xspi_parse(packet, n, &op, &session, &data, &len) != 0);
  n = xspi_make(packet, XSPI_OPEN, 9, NULL, 0);
  assert(n == XSPI_HDR);
  assert(xspi_parse(packet, n, &op, &session, &data, &len) == 0 && op == XSPI_OPEN && session == 9);
  assert(xspi_make(packet, XSPI_OPEN, 9, bytes, 1) == 0);
  return 0;
}
