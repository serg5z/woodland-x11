// LCD pixel-clock setting, X11 policy: see x11_pclk.h.
#include "x11_pclk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace x11pclk {

bool allowed(int mhz) {
  for (uint8_t o : OPTIONS)
    if (mhz == o) return true;
  return false;
}

Boot decide(pclk::Store &st) {
  Boot b = {};
  uint8_t v = 0;
  b.stored_confirmed = st.get(pclk::KEY_CONFIRMED, v) ? v : 0;
  pclk::BootDecision d = pclk::decide_boot(st);
  b.note = d.note;
  b.confirmed_mhz = allowed(d.confirmed_mhz) ? d.confirmed_mhz : BASELINE_MHZ;
  if (d.confirmed_mhz != b.confirmed_mhz) b.restricted = true;
  if (allowed(d.apply_mhz)) {
    b.apply_mhz = d.apply_mhz;
    b.trial = d.trial;
    if (!d.trial) b.apply_mhz = b.confirmed_mhz;  // a non-trial boot uses the (allowed) confirmed value
  } else {
    b.restricted = true;
    if (d.trial) pclk::revert(st);  // a trial of a disallowed value: never run it
    b.apply_mhz = b.confirmed_mhz;
    b.trial = false;
  }
  return b;
}

Parsed parse_request(const char *s, size_t n) {
  Parsed p = {Cmd::None, 0, 0};
  char buf[64];
  if (!n) return p;
  if (n >= sizeof(buf)) return p.cmd = Cmd::Invalid, p;
  memcpy(buf, s, n);
  buf[n] = 0;
  char word[16];
  unsigned a = 0, b = 0;
  char extra;
  if (sscanf(buf, "%15s %u %u %c", word, &a, &b, &extra) == 3 && !strcmp(word, "request")) {
    if (!allowed((int)a)) return p.cmd = Cmd::Invalid, p;
    p.cmd = Cmd::Request, p.mhz = (uint8_t)a, p.nonce = b;
    return p;
  }
  if (sscanf(buf, "%15s %u %c", word, &a, &extra) == 2 && !strcmp(word, "reset")) {
    p.cmd = Cmd::Reset, p.nonce = a;
    return p;
  }
  p.cmd = Cmd::Invalid;
  return p;
}

float nominal_fps(uint32_t pclk_hz) { return (float)pclk_hz / (820.0f * 500.0f); }

}  // namespace x11pclk
