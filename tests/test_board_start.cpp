// esp32/x11_esp32/crowpanel_board.h: the board start-up sequence against a recording fake.
#include <assert.h>
#include <stdio.h>
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
  {  // boot still switches the backlight fully on (unchanged default)
    Fake f;
    start(f);
    assert(f.trace.back() == "send 48 0");
  }
  puts("test_board_start: all OK");
  return 0;
}
