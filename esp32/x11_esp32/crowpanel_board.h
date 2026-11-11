// Start-up of the Elecrow CrowPanel Advance 4.3" board over I2C, before the display driver:
// the board microcontroller (backlight and touch power) and the GT911 must both answer.
// Hardware facts:
//   - I2C on GPIO15 (SDA) / GPIO16 (SCL), shared by both devices;
//   - board microcontroller at 0x30, one-byte commands: 0..245 backlight (0 brightest,
//     245 off), 250 enables the touch controller;
//   - GT911 at 0x5D; GPIO1 held low ~120 ms, then released, restarts it.
// Portable C++ over a small I/O interface, so the sequence is tested on the host
// (tests/test_board_start.cpp); x11_lcd.cpp supplies the Arduino implementation.
#pragma once
#include <stdint.h>

namespace crowpanel43_board {

constexpr int SDA_GPIO = 15, SCL_GPIO = 16;
constexpr uint8_t MCU_ADDR = 0x30;
constexpr uint8_t TOUCH_ADDR = 0x5D;
constexpr uint8_t CMD_TOUCH_ENABLE = 250;
constexpr uint8_t CMD_BACKLIGHT_FULL = 0;
constexpr int TOUCH_RESTART_GPIO = 1;
constexpr int BUS_SETTLE_MS = 50;      // after the I2C master starts
constexpr int RESTART_LOW_MS = 120;    // GPIO1 low
constexpr int RESTART_SETTLE_MS = 100; // after GPIO1 is released
constexpr int ATTEMPTS = 40;           // about 10 s if nothing answers

// Io must provide:
//   bool answers(uint8_t addr)            address probe (an empty write is acknowledged)
//   void send(uint8_t addr, uint8_t byte) one-byte write
//   void pin_low(int gpio)                drive the pin low (output)
//   void pin_release(int gpio)            make the pin an input again
//   void wait_ms(int ms)
//   void log(const char *text)
// The I2C master is already started on SDA_GPIO / SCL_GPIO. Returns true when both devices
// answered; the backlight is switched fully on either way.
template <class Io>
bool start(Io &io) {
  io.wait_ms(BUS_SETTLE_MS);
  bool ready = false;
  for (int attempt = 1; attempt <= ATTEMPTS && !ready; ++attempt) {
    ready = io.answers(MCU_ADDR) && io.answers(TOUCH_ADDR);
    if (ready) break;
    io.log("board: microcontroller 0x30 or touch 0x5D silent; enabling touch and restarting it");
    io.send(MCU_ADDR, CMD_TOUCH_ENABLE);
    io.pin_low(TOUCH_RESTART_GPIO);
    io.wait_ms(RESTART_LOW_MS);
    io.pin_release(TOUCH_RESTART_GPIO);
    io.wait_ms(RESTART_SETTLE_MS);
  }
  io.send(MCU_ADDR, CMD_BACKLIGHT_FULL);
  return ready;
}

// ---- Runtime backlight level (IDF64 builds, -DX11_BACKLIGHT) -----------------------------
// The board microcontroller dims the backlight itself; the level is one command byte. The
// mapping and the write policy are those of the Woodland Lab firmware's display settings:
// percent to command linearly (100 % -> 0, 0 % -> 245), never a command above 245 (246+ are
// other board commands), at most one write per 30 ms, a 1 s pause after a failed write.
// The level is not stored: every boot starts at 100 % (start() sends CMD_BACKLIGHT_FULL).
constexpr uint8_t CMD_BACKLIGHT_OFF = 245;
constexpr int BACKLIGHT_MIN_PCT = 10;   // the command cannot switch the backlight off
constexpr int BACKLIGHT_MAX_PCT = 100;
constexpr uint32_t BACKLIGHT_MIN_INTERVAL_MS = 30;
constexpr uint32_t BACKLIGHT_RETRY_MS = 1000;

constexpr uint8_t backlight_command(int pct) {
  return (uint8_t)(((100 - (pct < 0 ? 0 : pct > 100 ? 100 : pct)) * CMD_BACKLIGHT_OFF + 50) / 100);
}

struct Backlight {
  uint8_t pct = 100;                   // requested level
  uint8_t cmd = CMD_BACKLIGHT_FULL;    // its command
  uint8_t applied_pct = 100;           // last level the microcontroller acknowledged (boot: 100)
  bool pending = false;                // a write is due
  bool failed = false;                 // the last write was not acknowledged
  uint32_t last_ms = 0, writes = 0, errors = 0;
};

// A new level, BACKLIGHT_MIN_PCT..BACKLIGHT_MAX_PCT; false (and nothing changes) otherwise.
inline bool backlight_request(Backlight &b, long pct) {
  if (pct < BACKLIGHT_MIN_PCT || pct > BACKLIGHT_MAX_PCT) return false;
  b.pct = (uint8_t)pct;
  b.cmd = backlight_command((int)pct);
  b.pending = true;
  return true;
}

// From loop(): sends a due level. Io::write(addr, byte) returns whether it was acknowledged.
// Returns true when a write was attempted.
template <class Io>
bool backlight_service(Backlight &b, Io &io, uint32_t now_ms) {
  if (!b.pending) return false;
  if (b.writes + b.errors && now_ms - b.last_ms < (b.failed ? BACKLIGHT_RETRY_MS : BACKLIGHT_MIN_INTERVAL_MS))
    return false;
  b.last_ms = now_ms;
  if (b.cmd > CMD_BACKLIGHT_OFF) {  // cannot happen through backlight_request
    b.pending = false;
    return false;
  }
  if (io.write(MCU_ADDR, b.cmd)) {
    ++b.writes;
    b.failed = b.pending = false;
    b.applied_pct = b.pct;
  } else {
    ++b.errors;
    b.failed = true;
  }
  return true;
}

}  // namespace crowpanel43_board
