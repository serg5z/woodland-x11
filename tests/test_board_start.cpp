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
  puts("test_board_start: all OK");
  return 0;
}
