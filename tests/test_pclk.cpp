// pclk/x11_pclk.cpp over pclk/pclk_trial.cpp, with a fake NVS store.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <map>
#include <string>

#include "../pclk/x11_pclk.h"

struct FakeStore : pclk::Store {
  std::map<std::string, uint8_t> kv;
  int writes = 0, fail_after = -1;
  bool get(const char *k, uint8_t &v) override {
    auto it = kv.find(k);
    if (it == kv.end()) return false;
    v = it->second;
    return true;
  }
  bool set(const char *k, uint8_t v) override {
    if (fail_after >= 0 && writes >= fail_after) return false;
    ++writes;
    kv[k] = v;
    return true;
  }
};

static void boot_ok(FakeStore &st, uint8_t apply, bool trial, bool restricted) {
  x11pclk::Boot b = x11pclk::decide(st);
  if (b.apply_mhz != apply || b.trial != trial || b.restricted != restricted) {
    fprintf(stderr, "boot: apply %u trial %d restricted %d (want %u %d %d) note %s\n", b.apply_mhz, b.trial,
            b.restricted, apply, trial, restricted, pclk::boot_note_name(b.note));
    assert(0);
  }
}

int main() {
  /* empty NVS: 16 MHz, no trial */
  { FakeStore st; boot_ok(st, 16, false, false); }

  /* request 14 -> trial boot at 14 -> Keep -> 14 from then on */
  {
    FakeStore st;
    assert(pclk::request(st, 14));
    x11pclk::Boot b = x11pclk::decide(st);
    assert(b.apply_mhz == 14 && b.trial && b.confirmed_mhz == 16 && b.note == pclk::BootNote::TrialStarted);
    assert(pclk::keep(st, 14));
    boot_ok(st, 14, false, false);
    boot_ok(st, 14, false, false);
  }

  /* request 12 -> trial -> no Keep (timeout, reset, power loss): the next boot reverts to 16, never retried */
  {
    FakeStore st;
    assert(pclk::request(st, 12));
    boot_ok(st, 12, true, false);
    x11pclk::Boot b = x11pclk::decide(st);  /* the trial boot ended without Keep */
    assert(b.apply_mhz == 16 && !b.trial && b.note == pclk::BootNote::TrialAbandoned);
    boot_ok(st, 16, false, false);
  }

  /* explicit Revert */
  {
    FakeStore st;
    assert(pclk::request(st, 12));
    boot_ok(st, 12, true, false);
    assert(pclk::revert(st));
    boot_ok(st, 16, false, false);
  }

  /* a Woodland leftover: confirmed 20 (not allowed here) -> 16, restricted, nothing written */
  {
    FakeStore st;
    st.kv[pclk::KEY_CONFIRMED] = 20;
    int w = st.writes;
    x11pclk::Boot b = x11pclk::decide(st);
    assert(b.apply_mhz == 16 && !b.trial && b.restricted && b.stored_confirmed == 20 && st.writes == w);
  }
  /* a Woodland leftover trial of 24: never run, cleared; confirmed 14 (allowed) is used */
  {
    FakeStore st;
    st.kv[pclk::KEY_CONFIRMED] = 14;
    st.kv[pclk::KEY_PENDING] = 24;
    st.kv[pclk::KEY_TRIAL] = pclk::TRIAL_REQUESTED;
    x11pclk::Boot b = x11pclk::decide(st);
    assert(b.apply_mhz == 14 && !b.trial && b.restricted);
    boot_ok(st, 14, false, false);  /* cleared: the next boot is a normal one */
  }
  /* a trial of an allowed value while the confirmed one is a leftover (18): the trial runs; revert -> 16 */
  {
    FakeStore st;
    st.kv[pclk::KEY_CONFIRMED] = 18;
    assert(pclk::request(st, 12));
    x11pclk::Boot b = x11pclk::decide(st);
    assert(b.apply_mhz == 12 && b.trial && b.confirmed_mhz == 16 && b.restricted);
    pclk::revert(st);
    boot_ok(st, 16, false, true);
  }

  /* a failed NVS write while starting the trial: the trial does not run */
  {
    FakeStore st;
    assert(pclk::request(st, 12));
    st.fail_after = st.writes;  /* the ATTEMPTED write fails */
    x11pclk::Boot b = x11pclk::decide(st);
    assert(b.apply_mhz == 16 && !b.trial);
  }

  /* request parsing */
  using x11pclk::Cmd;
  auto P = [](const char *s) { return x11pclk::parse_request(s, strlen(s)); };
  assert(P("request 14 77").cmd == Cmd::Request && P("request 14 77").mhz == 14 && P("request 14 77").nonce == 77);
  assert(P("request 12 1").mhz == 12 && P("request 16 4294967295").nonce == 4294967295u);
  assert(P("reset 9").cmd == Cmd::Reset && P("reset 9").nonce == 9);
  assert(P("request 18 5").cmd == Cmd::Invalid);   /* not offered */
  assert(P("request 14").cmd == Cmd::Invalid);     /* no nonce */
  assert(P("request 14 5 x").cmd == Cmd::Invalid); /* trailing junk */
  assert(P("reboot 5").cmd == Cmd::Invalid);
  assert(P("").cmd == Cmd::None);
  {
    char big[100];
    memset(big, 'a', sizeof(big));
    assert(x11pclk::parse_request(big, sizeof(big)).cmd == Cmd::Invalid);
  }
  /* nominal refresh: 16 MHz 39.0 fps, 14 MHz 34.1, 12 MHz 29.3 */
  assert(fabsf(x11pclk::nominal_fps(16000000) - 39.02f) < 0.01f);
  assert(fabsf(x11pclk::nominal_fps(14000000) - 34.15f) < 0.01f);
  assert(fabsf(x11pclk::nominal_fps(12000000) - 29.27f) < 0.01f);
  assert(x11pclk::allowed(12) && x11pclk::allowed(14) && x11pclk::allowed(16) && !x11pclk::allowed(18) && !x11pclk::allowed(8));
  printf("test_pclk: all OK\n");
  return 0;
}
