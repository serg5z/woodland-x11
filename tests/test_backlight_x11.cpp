// _WOODLAND_BACKLIGHT end to end on the host: the real X11 core receives the requests xprop
// sends (InternAtom, ChangeProperty, DeleteProperty on the root window); the firmware's own
// code (crowpanel_board.h: backlight_poll_root, backlight_property, backlight_service) reads
// the property and writes to a fake I2C bus. Run with ASan/UBSan.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <vector>

extern "C" {
#include "../core/x11_internal.h"
}
#include "../core/x11_server.h"
#include "../core/x11_setup.h"
#include "../esp32/x11_esp32/crowpanel_board.h"

using namespace crowpanel43_board;

static x11_server srv;
static int client;
static uint8_t reply[X11_REPLY_MAX];

static void deliver(void *, int, const uint8_t *, size_t) {}

static size_t request(std::vector<uint8_t> q) {
  while (q.size() % 4) q.push_back(0);
  q[2] = (uint8_t)(q.size() / 4), q[3] = (uint8_t)(q.size() / 4 >> 8);
  int close_after = 0;
  size_t n = x11_client_request(&srv, client, q.data(), q.size(), reply, sizeof(reply), &close_after);
  assert(!close_after);
  return n;
}
static void put32(std::vector<uint8_t> &q, uint32_t v) {
  for (int i = 0; i < 4; ++i) q.push_back((uint8_t)(v >> (8 * i)));
}
static uint32_t intern(const char *name) {
  std::vector<uint8_t> q = {16, 0, 0, 0, (uint8_t)strlen(name), 0, 0, 0};
  q.insert(q.end(), name, name + strlen(name));
  assert(request(q) == 32 && reply[0] == 1);
  return reply[8] | reply[9] << 8 | reply[10] << 16 | (uint32_t)reply[11] << 24;
}
// xprop -root -f NAME 8s -set NAME VALUE: ChangeProperty, mode Replace, type STRING, format 8
static void set_property(uint32_t atom, const char *value, uint32_t type = X11_ATOM_STRING, uint8_t format = 8) {
  size_t n = strlen(value), units = format == 8 ? n : n / (format / 8);
  std::vector<uint8_t> q = {18, 0, 0, 0};
  put32(q, X11_ID_ROOT_WINDOW), put32(q, atom), put32(q, type);
  q.push_back(format), q.push_back(0), q.push_back(0), q.push_back(0);
  put32(q, (uint32_t)units);
  q.insert(q.end(), value, value + n);
  assert(request(q) == 0);  // no error
}
static void delete_property(uint32_t atom) {
  std::vector<uint8_t> q = {19, 0, 0, 0};
  put32(q, X11_ID_ROOT_WINDOW), put32(q, atom);
  assert(request(q) == 0);
}

struct Bus {
  std::vector<int> sent;
  int fail = 0;
  bool write(uint8_t addr, uint8_t byte) {
    assert(addr == MCU_ADDR && byte <= CMD_BACKLIGHT_OFF);
    sent.push_back(byte);
    return fail-- <= 0;
  }
};

int main() {
  x11_server_config cfg;
  x11_default_config(&cfg);
  x11_platform plat = {};
  plat.deliver = deliver;
  assert(x11_server_init_platform(&srv, &cfg, &plat) == 0);
  client = x11_client_open(&srv);
  uint8_t setup[12] = {'l', 0, 11, 0};
  x11_setup_result r;
  static uint8_t setup_reply[4096];
  x11_client_setup(&srv, client, setup, sizeof(setup), setup_reply, sizeof(setup_reply), &r);

  Backlight b;  // the boot state: 100 %, nothing pending (start() already sent 0)
  BacklightProperty p;
  Bus bus;
  uint32_t now = 1000;
  auto tick = [&](uint32_t dt) {  // one pass of loop(): poll, then service
    now += dt;
    uint8_t v = backlight_poll_root(b, p, &srv, now);
    backlight_service(b, bus, now);
    return v;
  };

  assert(tick(300) == BL_NONE && bus.sent.empty() && b.applied_pct == 100);  // no property: nothing
  uint32_t atom = intern(BACKLIGHT_PROPERTY);

  set_property(atom, "40");
  assert(tick(300) == BL_ACCEPTED && b.applied_pct == 40 && (bus.sent == std::vector<int>{147}));
  set_property(atom, "40");  // xprop again with the same value: no write
  assert(tick(300) == BL_NONE && bus.sent.size() == 1);

  for (const char *bad : {"0", "101", "-5", "", "abc", "40.5"}) {  // rejected, level kept
    set_property(atom, bad);
    uint8_t v = tick(300);
    assert(v != BL_NONE && v != BL_ACCEPTED && v != BL_UNCHANGED);
    assert(b.applied_pct == 40 && bus.sent.size() == 1);
  }
  set_property(atom, "40", 4 /* ATOM */, 8);
  assert(tick(300) == BL_BAD_TYPE && bus.sent.size() == 1);
  set_property(atom, "4000", X11_ATOM_STRING, 16);
  assert(tick(300) == BL_BAD_FORMAT && bus.sent.size() == 1);
  assert(p.rejected == 8);

  set_property(atom, "10");
  assert(tick(300) == BL_ACCEPTED && b.applied_pct == 10 && bus.sent.back() == 221);
  set_property(atom, "100");
  assert(tick(300) == BL_ACCEPTED && b.applied_pct == 100 && bus.sent.back() == 0);

  // rapid updates between two polls: only the latest value is seen and written
  size_t before = bus.sent.size();
  set_property(atom, "20"), set_property(atom, "30"), set_property(atom, "60");
  assert(tick(10) == BL_NONE);  // not yet time to poll
  assert(tick(300) == BL_ACCEPTED && b.applied_pct == 60 && bus.sent.size() == before + 1);

  // an unacknowledged write is retried after 1 s; the X11 side does not change meanwhile
  bus.fail = 1;
  set_property(atom, "50");
  assert(tick(300) == BL_ACCEPTED && b.failed && b.applied_pct == 60 && b.errors == 1);
  assert(tick(300) == BL_NONE && b.failed && bus.sent.back() == 123 && bus.sent.size() == before + 2);
  assert(tick(800) == BL_NONE && !b.failed && b.applied_pct == 50 && bus.sent.size() == before + 3);

  // deleted and set again to the same value: seen as new, but no write (same level)
  delete_property(atom);
  assert(tick(300) == BL_NONE);
  set_property(atom, "50");
  assert(tick(300) == BL_UNCHANGED && bus.sent.size() == before + 3);

  // the property is ordinary data to clients: GetProperty returns what was set
  {
    std::vector<uint8_t> q = {20, 0, 0, 0};
    put32(q, X11_ID_ROOT_WINDOW), put32(q, atom), put32(q, 0), put32(q, 0), put32(q, 16);
    assert(request(q) >= 34 && reply[0] == 1 && reply[1] == 8 && !memcmp(reply + 32, "50", 2));
  }
  x11_server_free(&srv);
  printf("test_backlight_x11: all OK (%zu I2C writes)\n", bus.sent.size());
  return 0;
}
