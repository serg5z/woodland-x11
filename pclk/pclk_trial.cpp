#include "pclk_trial.h"

namespace pclk {

bool valid_mhz(int mhz) {
  for (int i = 0; i < OPTION_COUNT; ++i)
    if (OPTIONS[i] == mhz) return true;
  return false;
}

static void clear_trial(Store &st) {
  // trial first: once it is NONE the pending value is inert even if the second write fails.
  st.set(KEY_TRIAL, TRIAL_NONE);
  st.set(KEY_PENDING, 0);
}

BootDecision decide_boot(Store &st) {
  BootDecision d{BASELINE_MHZ, BASELINE_MHZ, false, BootNote::Normal};

  uint8_t confirmed = 0;
  bool have_confirmed = st.get(KEY_CONFIRMED, confirmed);
  if (!have_confirmed || !valid_mhz(confirmed)) {
    if (have_confirmed) d.note = BootNote::Repaired;
    confirmed = BASELINE_MHZ;
    if (have_confirmed) st.set(KEY_CONFIRMED, BASELINE_MHZ);
  }
  d.confirmed_mhz = d.apply_mhz = confirmed;

  uint8_t trial = TRIAL_NONE, pending = 0;
  bool have_trial = st.get(KEY_TRIAL, trial);
  bool have_pending = st.get(KEY_PENDING, pending);

  if (!have_trial || trial == TRIAL_NONE) {
    if (have_pending && pending != 0) st.set(KEY_PENDING, 0);  // stray value from an interrupted request()
    return d;
  }

  if (trial == TRIAL_REQUESTED && have_pending && valid_mhz(pending) && pending != confirmed) {
    // Mark the attempt BEFORE the RGB driver starts. If this boot never reaches keep(),
    // the next boot sees ATTEMPTED and goes back to the confirmed value.
    if (st.set(KEY_TRIAL, TRIAL_ATTEMPTED)) {
      uint8_t check = 0;
      if (st.get(KEY_TRIAL, check) && check == TRIAL_ATTEMPTED) {
        d.apply_mhz = pending;
        d.trial = true;
        d.note = BootNote::TrialStarted;
        return d;
      }
    }
    clear_trial(st);
    d.note = BootNote::TrialSkipped;
    return d;
  }

  // ATTEMPTED (interrupted trial), REQUESTED with a bad/equal pending value, or an
  // unknown state byte: all resolve to the confirmed frequency.
  d.note = trial == TRIAL_ATTEMPTED ? BootNote::TrialAbandoned : BootNote::TrialSkipped;
  // keep() writes confirmed first; if power failed right after, the trial was in fact kept.
  if (trial == TRIAL_ATTEMPTED && have_pending && pending == confirmed) d.note = BootNote::Normal;
  clear_trial(st);
  return d;
}

bool request(Store &st, uint8_t mhz) {
  if (!valid_mhz(mhz)) return false;
  // pending first: if power fails before trial is written, decide_boot() ignores it.
  if (!st.set(KEY_PENDING, mhz)) return false;
  return st.set(KEY_TRIAL, TRIAL_REQUESTED);
}

bool keep(Store &st, uint8_t trial_mhz) {
  if (!valid_mhz(trial_mhz)) return false;
  // confirmed first: if power fails before the trial is cleared, the next boot sees
  // ATTEMPTED, clears it and boots with the (already confirmed) new value.
  if (!st.set(KEY_CONFIRMED, trial_mhz)) return false;
  bool ok = st.set(KEY_TRIAL, TRIAL_NONE);
  ok = st.set(KEY_PENDING, 0) && ok;
  return ok;
}

bool revert(Store &st) {
  bool ok = st.set(KEY_TRIAL, TRIAL_NONE);
  ok = st.set(KEY_PENDING, 0) && ok;
  return ok;
}

bool reset_to_baseline(Store &st) {
  bool ok = st.set(KEY_TRIAL, TRIAL_NONE);
  ok = st.set(KEY_PENDING, 0) && ok;
  ok = st.set(KEY_CONFIRMED, BASELINE_MHZ) && ok;
  return ok;
}

const char *boot_note_name(BootNote n) {
  switch (n) {
    case BootNote::TrialStarted: return "trial started";
    case BootNote::TrialAbandoned: return "previous trial ended without Keep: reverted";
    case BootNote::TrialSkipped: return "trial request not run";
    case BootNote::Repaired: return "invalid confirmed value: baseline restored";
    default: return "normal";
  }
}

}  // namespace pclk
