// LCD pixel-clock setting for the X11 firmware, test builds only
// (-DX11_PCLK). Pure C++, host-tested: the X11 policy on top of the pixel-clock trial logic
// (pclk/pclk_trial.{h,cpp}).
//
//   - only 12, 14 and 16 MHz are ever applied; any other stored value (a Woodland leftover)
//     is not applied: the boot uses 16 MHz (or the confirmed value if it is allowed), a
//     leftover trial of a disallowed value is cleared, and the boot is marked "restricted";
//   - the on-panel confirmation counts down CONFIRM_SECONDS;
//   - requests arrive as the root property _WOODLAND_PCLK_REQUEST (STRING):
//       "request <12|14|16> <nonce>"   "reset <nonce>"   (nonce: a client-chosen number,
//     each handled once).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "pclk_trial.h"

namespace x11pclk {

constexpr uint8_t OPTIONS[] = {12, 14, 16};
constexpr uint8_t BASELINE_MHZ = 16;
constexpr uint32_t CONFIRM_SECONDS = 10;
constexpr const char *REQUEST_ATOM = "_WOODLAND_PCLK_REQUEST";

bool allowed(int mhz);

struct Boot {
  uint8_t apply_mhz;        // for the RGB initialisation
  uint8_t confirmed_mhz;    // the confirmed value as used (16 if the stored one is not allowed)
  uint8_t stored_confirmed; // what NVS held before this boot (0: none)
  bool trial;               // apply_mhz is unconfirmed: show the Keep/Revert dialog
  bool restricted;          // a stored value outside 12/14/16 was not applied
  pclk::BootNote note;
};
Boot decide(pclk::Store &st);

enum class Cmd : uint8_t { None, Request, Reset, Invalid };
struct Parsed {
  Cmd cmd;
  uint8_t mhz;
  uint32_t nonce;
};
// The property's bytes (not NUL-terminated). Request with a value outside 12/14/16: Invalid.
Parsed parse_request(const char *s, size_t n);

// Nominal refresh rate of the 800x480 panel with its porches (820 x 500 clocks per frame).
float nominal_fps(uint32_t pclk_hz);

}  // namespace x11pclk
