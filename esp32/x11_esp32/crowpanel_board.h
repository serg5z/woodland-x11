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

}  // namespace crowpanel43_board
