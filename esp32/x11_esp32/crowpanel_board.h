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
#include <stddef.h>
#include <stdint.h>
#include <string.h>

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

// ---- The root property _WOODLAND_BACKLIGHT (IDF64 builds, -DX11_BACKLIGHT) ----------------
// A client sets the level over its ordinary X11 connection, for example
//   xprop -display PANEL:0 -root -f _WOODLAND_BACKLIGHT 8s -set _WOODLAND_BACKLIGHT 40
// The firmware looks at the property every BACKLIGHT_POLL_MS from loop() (the core has no
// property-change hook) and acts once per distinct value: type STRING, format 8, a decimal
// integer BACKLIGHT_MIN_PCT..BACKLIGHT_MAX_PCT, optionally surrounded by spaces. Anything
// else is rejected and changes nothing; a value equal to the requested level writes nothing.
// Deleting the property and setting it again makes the same value count as new.
constexpr uint32_t X11_ATOM_STRING = 31;  // predefined atom (protocol)
constexpr const char *BACKLIGHT_PROPERTY = "_WOODLAND_BACKLIGHT";
constexpr uint32_t BACKLIGHT_POLL_MS = 250;
constexpr size_t BACKLIGHT_PROPERTY_MAX = 16;

enum BacklightVerdict : uint8_t {
  BL_NONE, BL_ACCEPTED, BL_UNCHANGED, BL_BAD_TYPE, BL_BAD_FORMAT, BL_EMPTY, BL_NOT_NUMBER, BL_OUT_OF_RANGE
};

inline const char *backlight_verdict_name(uint8_t v) {
  static const char *const n[] = {"none", "accepted", "unchanged", "bad_type", "bad_format", "empty",
                                  "not_a_number", "out_of_range"};
  return v <= BL_OUT_OF_RANGE ? n[v] : "?";
}

// The property's value as a level: BL_ACCEPTED and *pct, or why it is not one.
inline uint8_t backlight_parse(uint32_t type, uint8_t format, const uint8_t *data, size_t n, long *pct) {
  if (type != X11_ATOM_STRING) return BL_BAD_TYPE;
  if (format != 8) return BL_BAD_FORMAT;
  if (n > BACKLIGHT_PROPERTY_MAX) return BL_NOT_NUMBER;
  size_t a = 0, b = n;
  while (a < b && data[a] == ' ') ++a;
  while (b > a && (data[b - 1] == ' ' || data[b - 1] == 0)) --b;
  if (a == b) return BL_EMPTY;
  long v = 0;
  for (size_t i = a; i < b; ++i) {
    if (data[i] < '0' || data[i] > '9') return BL_NOT_NUMBER;  // no sign, no fraction
    v = v * 10 + (data[i] - '0');
    if (v > 1000) v = 1000;  // still out of range, no overflow
  }
  if (v < BACKLIGHT_MIN_PCT || v > BACKLIGHT_MAX_PCT) return BL_OUT_OF_RANGE;
  *pct = v;
  return BL_ACCEPTED;
}

struct BacklightProperty {
  bool seen = false;          // the current value has been handled
  uint32_t hash = 0;          // of type, format and bytes of that value
  uint32_t accepted = 0, unchanged = 0, rejected = 0;
  uint8_t last_pct = 0;       // last accepted level
  uint8_t last = BL_NONE;     // verdict on the last new value
  uint32_t last_poll_ms = 0;
};

// One look at the property (present == false: it does not exist). Returns the verdict on a
// new value, BL_NONE when nothing is new. An accepted level goes to backlight_request().
inline uint8_t backlight_property(Backlight &b, BacklightProperty &p, bool present, uint32_t type,
                                  uint8_t format, const uint8_t *data, size_t n) {
  if (!present) {
    p.seen = false;
    return BL_NONE;
  }
  uint32_t h = 2166136261u;
  const uint8_t head[6] = {(uint8_t)type, (uint8_t)(type >> 8), (uint8_t)(type >> 16), (uint8_t)(type >> 24), format,
                           (uint8_t)(n > 255 ? 255 : n)};
  for (uint8_t c : head) h = (h ^ c) * 16777619u;
  for (size_t i = 0; i < n; ++i) h = (h ^ data[i]) * 16777619u;
  if (p.seen && h == p.hash) return BL_NONE;
  p.seen = true, p.hash = h;
  long pct = 0;
  uint8_t v = backlight_parse(type, format, data, n, &pct);
  if (v != BL_ACCEPTED) {
    ++p.rejected;
    return p.last = v;
  }
  if (pct == b.pct) {  // already the requested level: no write
    ++p.unchanged;
    return p.last = BL_UNCHANGED;
  }
  backlight_request(b, pct);
  ++p.accepted;
  p.last_pct = (uint8_t)pct;
  return p.last = BL_ACCEPTED;
}

// At most every BACKLIGHT_POLL_MS: finds _WOODLAND_BACKLIGHT among the root window's
// properties and hands it to backlight_property(). Server is x11_server (core/x11_server.h
// and core/x11_internal.h); one walk over a short list, no allocation, nothing else touched.
template <class Server>
uint8_t backlight_poll_root(Backlight &b, BacklightProperty &p, Server *s, uint32_t now_ms) {
  if (now_ms - p.last_poll_ms < BACKLIGHT_POLL_MS || !s || !s->root) return BL_NONE;
  p.last_poll_ms = now_ms;
  auto *q = s->root->props;
  for (; q; q = q->next) {
    const char *n = x11_atom_name(s, q->name);
    if (n && !strcmp(n, BACKLIGHT_PROPERTY)) break;
  }
  return q ? backlight_property(b, p, true, q->type, q->format, q->data, q->nbytes)
           : backlight_property(b, p, false, 0, 0, nullptr, 0);
}

}  // namespace crowpanel43_board
