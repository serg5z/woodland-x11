// RGB pixel-clock trial with automatic rollback. Pure logic over a small key/value store,
// so every transition (including power loss between two writes) is covered by host tests.
//
// NVS (namespace "wl_pclk"), three independent u8 keys:
//   confirmed_pclk  MHz the panel boots with normally (missing/invalid -> 16, the baseline)
//   pending_pclk    MHz requested for the next boot's trial (0 = none)
//   trial_state     NONE / REQUESTED / ATTEMPTED
//
// Flow:
//   request(m)     pending = m, then trial = REQUESTED; caller restarts the ESP32.
//   decide_boot()  BEFORE the RGB driver is initialised:
//                    REQUESTED + valid pending -> write trial = ATTEMPTED, boot with pending.
//                      If that write fails, the trial is not run (confirmed is used).
//                    ATTEMPTED (a trial boot ended without Keep: reset, watchdog, power loss)
//                      -> clear pending and trial, boot with confirmed.
//                    anything else -> confirmed.
//   keep()         confirmed = pending, then pending = 0, trial = NONE.
//   revert()       pending = 0, trial = NONE; caller restarts.
// Write order makes every interruption safe: a pending value only becomes confirmed by
// keep(), and an interrupted trial is never retried.
#pragma once
#include <stdint.h>

namespace pclk {

constexpr uint8_t BASELINE_MHZ = 16;  // verified Elecrow configuration
constexpr uint8_t OPTIONS[] = {8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30, 32};
constexpr int OPTION_COUNT = sizeof(OPTIONS) / sizeof(OPTIONS[0]);
constexpr uint32_t SOURCE_HZ = 240000000;  // LovyanGFX Bus_RGB divides the 240 MHz PLL clock

bool valid_mhz(int mhz);
inline bool integer_divider(int mhz) { return mhz > 0 && SOURCE_HZ % ((uint32_t)mhz * 1000000u) == 0; }

enum Trial : uint8_t { TRIAL_NONE = 0, TRIAL_REQUESTED = 1, TRIAL_ATTEMPTED = 2 };

constexpr const char *KEY_CONFIRMED = "confirmed_pclk";
constexpr const char *KEY_PENDING = "pending_pclk";
constexpr const char *KEY_TRIAL = "trial_state";

// Minimal persistent store. get() returns false when the key is missing or unreadable;
// set() returns false when the value could not be committed.
struct Store {
  virtual bool get(const char *key, uint8_t &value) = 0;
  virtual bool set(const char *key, uint8_t value) = 0;
  virtual ~Store() {}
};

enum class BootNote : uint8_t {
  Normal,          // confirmed frequency, no trial involved
  TrialStarted,    // this boot runs the pending frequency; confirmation required
  TrialAbandoned,  // previous trial boot ended without Keep; reverted to confirmed
  TrialSkipped,    // trial requested but could not be marked ATTEMPTED (or invalid): not run
  Repaired,        // confirmed value was invalid; baseline used
};

struct BootDecision {
  uint8_t apply_mhz;      // use this for RGB initialisation
  uint8_t confirmed_mhz;
  bool trial;             // apply_mhz is unconfirmed; show the Keep/Revert dialog
  BootNote note;
};

BootDecision decide_boot(Store &st);
bool request(Store &st, uint8_t mhz);      // false: invalid value or write failed
bool keep(Store &st, uint8_t trial_mhz);   // false: a write failed (next boot reverts)
bool revert(Store &st);                    // best effort; a restart reverts anyway
bool reset_to_baseline(Store &st);         // confirmed = 16, no trial (serial recovery)

const char *boot_note_name(BootNote n);

// Timing of a trial boot.
constexpr uint32_t CONFIRM_SECONDS = 15;          // dialog countdown
constexpr uint32_t HARD_DEADLINE_MS = 40000;      // from setup(): restart unless confirmed

}  // namespace pclk
