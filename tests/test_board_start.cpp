// esp32/x11_esp32/crowpanel_board.h: the board start-up sequence against a recording fake.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "../esp32/x11_esp32/crowpanel_board.h"

using namespace crowpanel43_board;

struct Fake {
  int mcu_silent = 0, touch_silent = 0;  // how many probes of each address fail first
  std::vector<std::string> trace;
  bool answers(uint8_t addr) {
    int &left = addr == MCU_ADDR ? mcu_silent : touch_silent;
    bool ok = left <= 0;
    if (left > 0) --left;
    char b[32];
    snprintf(b, sizeof(b), "probe %02x %s", addr, ok ? "ack" : "nak");
    trace.push_back(b);
    return ok;
  }
  void send(uint8_t addr, uint8_t byte) { trace.push_back("send " + std::to_string(addr) + " " + std::to_string(byte)); }
  void pin_low(int gpio) { trace.push_back("low " + std::to_string(gpio)); }
  void pin_release(int gpio) { trace.push_back("release " + std::to_string(gpio)); }
  void wait_ms(int ms) { trace.push_back("wait " + std::to_string(ms)); }
  void log(const char *) { trace.push_back("log"); }
};

static const std::vector<std::string> RESTART = {"log", "send 48 250", "low 1", "wait 120", "release 1", "wait 100"};

static std::vector<std::string> expect(const std::vector<std::vector<std::string>> &parts) {
  std::vector<std::string> out;
  for (auto &p : parts) out.insert(out.end(), p.begin(), p.end());
  return out;
}

int main() {
  {  // both answer at once: settle, two probes, full backlight
    Fake f;
    assert(start(f));
    assert(f.trace == expect({{"wait 50", "probe 30 ack", "probe 5d ack", "send 48 0"}}));
  }
  {  // the microcontroller is silent twice: 0x5D is not probed then; two restarts
    Fake f;
    f.mcu_silent = 2;
    assert(start(f));
    assert(f.trace == expect({{"wait 50", "probe 30 nak"}, RESTART, {"probe 30 nak"}, RESTART,
                              {"probe 30 ack", "probe 5d ack", "send 48 0"}}));
  }
  {  // the touch controller is silent once
    Fake f;
    f.touch_silent = 1;
    assert(start(f));
    assert(f.trace == expect({{"wait 50", "probe 30 ack", "probe 5d nak"}, RESTART,
                              {"probe 30 ack", "probe 5d ack", "send 48 0"}}));
  }
  {  // nothing answers: 40 attempts, then full backlight anyway; false
    Fake f;
    f.mcu_silent = 1000;
    assert(!start(f));
    std::vector<std::vector<std::string>> parts = {{"wait 50"}};
    for (int i = 0; i < ATTEMPTS; ++i) {
      parts.push_back({"probe 30 nak"});
      parts.push_back(RESTART);
    }
    parts.push_back({"send 48 0"});
    assert(f.trace == expect(parts));
  }
  {  // the runtime backlight level: mapping (the Woodland Lab values), limits, write policy
    assert(backlight_command(100) == 0 && backlight_command(0) == 245);
    assert(backlight_command(20) == 196 && backlight_command(40) == 147 && backlight_command(10) == 221);
    for (int p = -5; p <= 300; ++p) assert(backlight_command(p) <= CMD_BACKLIGHT_OFF);
    for (int p = 1; p <= 100; ++p) assert(backlight_command(p) <= backlight_command(p - 1));
    Backlight b;
    assert(b.applied_pct == 100 && b.cmd == CMD_BACKLIGHT_FULL && !b.pending);  // the boot level
    for (long bad : {-1L, 0L, 9L, 101L, 255L, 1000L}) assert(!backlight_request(b, bad) && !b.pending && b.pct == 100);
    struct Bus {
      std::vector<int> sent;
      int fail = 0;  // writes not acknowledged first
      bool write(uint8_t addr, uint8_t byte) {
        assert(addr == MCU_ADDR && byte <= CMD_BACKLIGHT_OFF);
        sent.push_back(byte);
        return fail-- <= 0;
      }
    } bus;
    assert(!backlight_service(b, bus, 1000));                            // nothing requested
    assert(backlight_request(b, 40) && b.cmd == 147);
    assert(backlight_service(b, bus, 1000) && b.applied_pct == 40 && !b.pending && b.writes == 1);  // at once
    assert(backlight_request(b, 60) && backlight_request(b, 50));        // within 30 ms: coalesced
    assert(!backlight_service(b, bus, 1010) && !backlight_service(b, bus, 1029));
    assert(backlight_service(b, bus, 1030) && b.applied_pct == 50);
    assert((bus.sent == std::vector<int>{147, 123}));  // 40 %, then 50 % (60 % was superseded)
    bus.fail = 2;                                                         // not acknowledged twice
    assert(backlight_request(b, 100));
    assert(backlight_service(b, bus, 2000) && b.failed && b.pending && b.applied_pct == 50 && b.errors == 1);
    assert(!backlight_service(b, bus, 2999));                            // 1 s after an error
    assert(backlight_service(b, bus, 3000) && b.errors == 2);
    assert(backlight_service(b, bus, 4000) && !b.failed && !b.pending && b.applied_pct == 100 && b.writes == 3);
    assert(!backlight_service(b, bus, 5000));
  }
  {  // _WOODLAND_BACKLIGHT values: parsing
    auto parse = [](const char *v, uint32_t type = X11_ATOM_STRING, uint8_t format = 8, long *out = nullptr) {
      long pct = -1;
      uint8_t r = backlight_parse(type, format, (const uint8_t *)v, strlen(v), &pct);
      if (out) *out = pct;
      return r;
    };
    long pct = 0;
    assert(parse("10", X11_ATOM_STRING, 8, &pct) == BL_ACCEPTED && pct == 10);
    assert(parse("40", X11_ATOM_STRING, 8, &pct) == BL_ACCEPTED && pct == 40);
    assert(parse("100", X11_ATOM_STRING, 8, &pct) == BL_ACCEPTED && pct == 100);
    assert(parse(" 40 ", X11_ATOM_STRING, 8, &pct) == BL_ACCEPTED && pct == 40);
    assert(parse("040", X11_ATOM_STRING, 8, &pct) == BL_ACCEPTED && pct == 40);
    assert(parse("0") == BL_OUT_OF_RANGE && parse("9") == BL_OUT_OF_RANGE && parse("101") == BL_OUT_OF_RANGE);
    assert(parse("99999999999999") == BL_OUT_OF_RANGE);
    assert(parse("-40") == BL_NOT_NUMBER && parse("+40") == BL_NOT_NUMBER && parse("abc") == BL_NOT_NUMBER);
    assert(parse("40.5") == BL_NOT_NUMBER && parse("4 0") == BL_NOT_NUMBER && parse("40%") == BL_NOT_NUMBER);
    assert(parse("00000000000000040") == BL_NOT_NUMBER);  // longer than BACKLIGHT_PROPERTY_MAX
    assert(parse("") == BL_EMPTY && parse("   ") == BL_EMPTY);
    assert(parse("40", 4 /* ATOM */) == BL_BAD_TYPE && parse("40", X11_ATOM_STRING, 16) == BL_BAD_FORMAT);
    assert(parse("40", X11_ATOM_STRING, 32) == BL_BAD_FORMAT);
    const uint8_t nul[3] = {'4', '0', 0};  // a terminating NUL is tolerated
    assert(backlight_parse(X11_ATOM_STRING, 8, nul, 3, &pct) == BL_ACCEPTED && pct == 40);
  }
  {  // _WOODLAND_BACKLIGHT values: one decision per distinct value, no write for the same level
    Backlight b;
    BacklightProperty p;
    auto see = [&](const char *v) {
      return backlight_property(b, p, true, X11_ATOM_STRING, 8, (const uint8_t *)v, strlen(v));
    };
    assert(backlight_property(b, p, false, 0, 0, nullptr, 0) == BL_NONE);  // no property
    assert(see("100") == BL_UNCHANGED && !b.pending);                      // the boot level: nothing to write
    assert(see("100") == BL_NONE);                                          // the same value again: ignored
    assert(see("40") == BL_ACCEPTED && b.pending && b.cmd == 147 && p.last_pct == 40);
    assert(see("40") == BL_NONE && p.accepted == 1);
    assert(see(" 40") == BL_UNCHANGED && p.unchanged == 2);                 // new bytes, same level
    assert(see("101") == BL_OUT_OF_RANGE && b.pct == 40 && p.rejected == 1);  // rejected: level kept
    assert(see("x") == BL_NOT_NUMBER && p.rejected == 2 && p.last == BL_NOT_NUMBER);
    assert(backlight_property(b, p, true, 4, 8, (const uint8_t *)"60", 2) == BL_BAD_TYPE && b.pct == 40);
    assert(backlight_request(b, 70) && see("70") == BL_UNCHANGED);         // serial set 70 already
    assert(see("40") == BL_ACCEPTED && b.pct == 40);
    backlight_property(b, p, false, 0, 0, nullptr, 0);                      // deleted, then set again:
    assert(see("40") == BL_UNCHANGED);                                      // a new value, same level
  }
  {  // boot still switches the backlight fully on (unchanged default)
    Fake f;
    start(f);
    assert(f.trace.back() == "send 48 0");
  }
  puts("test_board_start: all OK");
  return 0;
}
