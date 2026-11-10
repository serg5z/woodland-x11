/* Prints frames built by spilink/ (hex, one per line) for the Python codec's cross-check:
 * an empty first frame, then data frames carrying known messages, then a NAK frame. */
#include <stdio.h>
#include <string.h>

#include "../../spilink/spilink.h"

static void hex(const uint8_t *f, size_t n) {
  for (size_t i = 0; i < n; ++i) printf("%02x", f[i]);
  printf("\n");
}

int main(void) {
  static uint8_t atx[8192], arx[8192], afl[SPL_WINDOW * 512], btx[8192], brx[8192], bfl[SPL_WINDOW * 512];
  spl_link a, b;
  spl_config ca = {512, 4, 0x5A}, cb = {512, 4, 0xA5};
  uint8_t fa[512], fb[512];
  if (spl_init(&a, &ca, atx, sizeof(atx), arx, sizeof(arx), afl) || spl_init(&b, &cb, btx, sizeof(btx), brx, sizeof(brx), bfl))
    return 1;
  spl_build(&a, fa);
  hex(fa, sizeof(fa));                 /* 1: empty, epoch 0x5A */
  spl_build(&b, fb);
  spl_accept(&a, fb);
  spl_accept(&b, fa);
  const char *msgs[] = {"hello", "Woodland", "x"};
  for (int i = 0; i < 3; ++i) spl_send(&a, (const uint8_t *)msgs[i], strlen(msgs[i]));
  spl_build(&a, fa);
  hex(fa, sizeof(fa));                 /* 2: data, seq 0, three messages */
  memset(fb, 0, sizeof(fb));
  spl_accept(&a, fb);                  /* a bad frame received */
  spl_build(&a, fa);
  hex(fa, sizeof(fa));                 /* 3: NAK set */
  return 0;
}
