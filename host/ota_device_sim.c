#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
/* Simulated CrowPanel for OTA tests: a 16 MB flash image in a file, the partition table read
 * from it, a model of the ESP-IDF v5.5 bootloader's OTA slot selection with
 * CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, the app-side otadata updates of esp_ota_* and the
 * WLOTA1 engine (ota/x11_ota.c, the code the ESP32 runs) on 127.0.0.1.
 *
 * THIS IS A SIMULATION. The bootloader model is written from ESP-IDF's
 * bootloader_utility.c / bootloader_common.c / esp_ota_ops.c behaviour (otadata entries
 * {seq, label, state, crc32}, two copies one sector apart; the active entry is the valid one
 * with the highest seq; NEW -> PENDING_VERIFY at boot; a PENDING_VERIFY entry found at boot
 * becomes ABORTED; slot = (seq - 1) % app count; an image that fails to load is skipped and
 * the slot actually booted is written back as VALID). Real hardware must confirm it.
 *
 *   ota_device_sim --flash FILE --key-file FILE [--port N] [--erase-ms N] [--program-ms N]
 *                  [--rx-chunk N] [--rx-delay-ms N] [--fail-write-at OFFSET]
 * --erase-ms/--program-ms: simulated time per 4 KiB sector erase/program (the loop stalls, as on
 * the ESP32); --rx-chunk/--rx-delay-ms: read at most N bytes per loop pass and wait between
 * passes (slow TCP delivery); --fail-write-at: the sector write at that slot offset fails.
 * The transport never reads the socket while the engine commits a block (as on the ESP32).
 * stdin, one command per line (each answered with one "state ..." line on stdout):
 *   boot            power on / reset: run the bootloader model, start the app
 *   confirm         the app passed its health check: esp_ota_mark_app_valid_cancel_rollback
 *   fail-health     the app failed its health check: esp_ota_mark_app_invalid_rollback_and_reboot
 *   crash           reset without confirming (watchdog, panic, power loss)
 *   corrupt-otadata N   flip a bit in otadata copy N        erase-otadata   both copies 0xFF
 *   corrupt-slot N  flip a bit in the image in app slot N (a damaged image the bootloader rejects)
 *   state           report       quit            write the flash file and exit
 *   stats           one "stats ota.up.k=v ..." line (upload timing counters), then the state line
 * The flash file is rewritten after every change, so tests can inspect it. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../ota/x11_ota.h"

#define FLASH_SIZE (16u * 1024 * 1024)
#define SECTOR 4096u
enum { NEW = 0, PENDING = 1, VALID = 2, INVALID = 3, ABORTED = 4 };
#define UNDEF 0xFFFFFFFFu  /* ESP_OTA_IMG_UNDEFINED */

typedef struct {
  uint32_t seq;
  uint8_t label[20];
  uint32_t state;
  uint32_t crc;
} otadata_t;

static uint8_t *flash;
static const char *flash_path;
static uint32_t ota_off, ota_size_part, nvs_off, nvs_size;
static uint32_t slot_off[2], slot_size[2];
static int nslots;
static int running = -1;           /* booted slot, -1: none (no bootable app) */
static int written_slot = -1;      /* begin() target */
static uint32_t write_size, erased_upto;
static int erase_ms = 0, program_ms = 0, rx_chunk = 8192, rx_delay_ms = 0;
static long fail_write_at = -1;
static uint32_t sector_erases;     /* counted: every sector must be erased exactly once */
static char idf_running[33] = "";
static int want_reboot = 0;

/* ---- flash file -------------------------------------------------------------------------- */

static void save(void) {
  FILE *f = fopen(flash_path, "wb");
  if (!f || fwrite(flash, 1, FLASH_SIZE, f) != FLASH_SIZE) perror("save flash");
  if (f) fclose(f);
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static uint32_t crc32_le(const uint8_t *p, size_t n) {  /* esp_rom_crc32_le(UINT32_MAX, p, n) */
  uint32_t c = 0;  /* ~UINT32_MAX */
  for (size_t i = 0; i < n; ++i) {
    c ^= p[i];
    for (int k = 0; k < 8; ++k) c = c & 1 ? (c >> 1) ^ 0xEDB88320u : c >> 1;
  }
  return ~c;
}

static int parse_table(void) {
  const uint8_t *t = flash + 0x8000;
  nslots = 0, ota_off = 0, nvs_off = 0;
  for (int i = 0; i < 0xC00 / 32; ++i) {
    const uint8_t *e = t + 32 * i;
    if (e[0] != 0xAA || e[1] != 0x50) break;
    uint8_t type = e[2], sub = e[3];
    uint32_t off = rd32(e + 4), size = rd32(e + 8);
    if (type == 0 && sub >= 0x10 && sub < 0x12) {
      slot_off[sub - 0x10] = off, slot_size[sub - 0x10] = size;
      if (sub - 0x10 + 1 > nslots) nslots = sub - 0x10 + 1;
    } else if (type == 1 && sub == 0x00) {
      ota_off = off, ota_size_part = size;
    } else if (type == 1 && sub == 0x02) {
      nvs_off = off, nvs_size = size;
    }
  }
  return nslots > 0 && ota_off ? 0 : -1;
}

/* ---- otadata (bootloader_common_* model) ------------------------------------------------- */

static void read_otadata(otadata_t o[2]) {
  for (int i = 0; i < 2; ++i) memcpy(&o[i], flash + ota_off + SECTOR * i, sizeof(otadata_t));
}

static void write_otadata(const otadata_t *o, int copy) {
  memset(flash + ota_off + SECTOR * copy, 0xFF, SECTOR);  /* erase the sector, then write */
  memcpy(flash + ota_off + SECTOR * copy, o, sizeof(*o));
}

static uint32_t entry_crc(const otadata_t *o) { return crc32_le((const uint8_t *)&o->seq, 4); }

static int entry_valid(const otadata_t *o) {
  return o->seq != 0xFFFFFFFFu && o->crc == entry_crc(o) && o->state != INVALID && o->state != ABORTED;
}

static int active_otadata(const otadata_t o[2]) {
  int v0 = entry_valid(&o[0]), v1 = entry_valid(&o[1]);
  if (v0 && v1) return o[0].seq >= o[1].seq ? 0 : 1;
  return v0 ? 0 : v1 ? 1 : -1;
}

static otadata_t make_entry(uint32_t seq, uint32_t state) {
  otadata_t e;
  memset(&e, 0xFF, sizeof(e));
  e.seq = seq, e.state = state;
  e.crc = entry_crc(&e);
  return e;
}

/* The image in slot i, if it is a valid application (bootloader's image check). */
static int slot_image_valid(int i, char idf_out[33]) {
  const uint8_t *p = flash + slot_off[i];
  if (p[0] != 0xE9) return 0;
  /* find the image's end from its segments, then check it exactly */
  uint32_t off = 24, n = p[1];
  for (uint32_t k = 0; k < n && off + 8 <= slot_size[i]; ++k) off += 8 + rd32(p + off + 4);
  uint32_t end = ((off + 1 + 15) & ~15u) + (p[23] ? 32 : 0);
  if (end > slot_size[i] || x11_ota_image_check(p, end) != 0) return 0;
  if (idf_out) {
    memcpy(idf_out, p + 32 + 112, 32);
    idf_out[32] = 0;
  }
  return 1;
}

/* ---- the bootloader model -------------------------------------------------------------------- */

static const char *boot_log = "";
static x11_ota_endpoint *endpoint;  /* for the "stats" command */

static void bootloader(void) {
  otadata_t o[2];
  read_otadata(o);
  for (int i = 0; i < 2; ++i)  /* an app that never confirmed: its trial is over */
    if (entry_valid(&o[i]) && o[i].state == PENDING) {
      o[i].state = ABORTED;
      write_otadata(&o[i], i);
    }
  int act = active_otadata(o), start = -1;
  if (act >= 0) {
    start = (int)((o[act].seq - 1) % (uint32_t)nslots);
    if (o[act].state == NEW) {
      o[act].state = PENDING;
      write_otadata(&o[act], act);
    }
  }
  running = -1;
  boot_log = "no bootable app";
  if (start >= 0 && slot_image_valid(start, idf_running)) {
    running = start;
    boot_log = "selected slot";
  } else {  /* backwards from the selected slot, then forwards (factory: none) */
    for (int i = start - 1; i >= 0 && running < 0; --i)
      if (slot_image_valid(i, idf_running)) running = i;
    for (int i = start + 1; i < nslots && running < 0; ++i)
      if (slot_image_valid(i, idf_running)) running = i;
    if (running >= 0) {  /* set_actual_ota_seq: the slot that booted becomes the active entry */
      otadata_t e = make_entry((uint32_t)running + 1, VALID);
      write_otadata(&e, 0);
      boot_log = "fallback slot";
    }
  }
  save();
}

static int running_entry(otadata_t o[2]) {
  read_otadata(o);
  int act = active_otadata(o);
  if (act < 0 || running < 0 || (int)((o[act].seq - 1) % (uint32_t)nslots) != running) return -1;
  return act;
}

static void report(void) {
  otadata_t o[2];
  read_otadata(o);
  int re = running_entry(o);
  const char *st = re < 0 ? "none" : o[re].state == PENDING ? "pending" : o[re].state == VALID ? "valid"
                                   : o[re].state == UNDEF   ? "undefined" : o[re].state == NEW ? "new" : "other";
  printf("state running=%d entry=%s boot=%s", running, st, boot_log);
  for (int i = 0; i < 2; ++i)
    printf(" ota%d=%s:%u:%u", i, entry_valid(&o[i]) ? "valid" : "invalid", o[i].seq == 0xFFFFFFFFu ? 0 : o[i].seq,
           o[i].state == UNDEF ? 255 : o[i].state);
  printf(" erases=%u", sector_erases);
  printf(" slot0=%s slot1=%s\n", slot_image_valid(0, NULL) ? "image" : "empty", nslots > 1 && slot_image_valid(1, NULL) ? "image" : "empty");
  fflush(stdout);
}

/* ---- esp_ota_* model, as the OTA receiver uses it ---------------------------------------------- */

static void ops_slots(void *ctx, x11_ota_slots *out) {
  (void)ctx;
  out->has_inactive = nslots >= 2 && running >= 0;
  out->inactive_size = out->has_inactive ? slot_size[1 - running] : 0;
  out->running = (uint8_t)(running < 0 ? 0 : running);
  otadata_t o[2];
  int re = running_entry(o);
  snprintf(out->text, sizeof(out->text), "running=app%d %s other=%s", running,
           re >= 0 && o[re].state == PENDING ? "pending" : "valid",
           nslots > 1 && slot_image_valid(1 - running, NULL) ? "image" : "empty");
}

static int ops_begin(void *ctx, uint32_t size) {
  (void)ctx;
  if (nslots < 2 || running < 0) return -1;
  written_slot = 1 - running;  /* never the running slot */
  if (size > slot_size[written_slot]) return -1;
  write_size = size, erased_upto = 0;  /* nothing erased here (esp_ota_begin, sequential mode) */
  return 0;
}

static uint64_t ops_now_us(void *ctx);

/* esp_partition_erase_range + esp_partition_write of one sector, timed (simulated delays). */
static int ops_write_sector(void *ctx, uint32_t off, const uint8_t *d, size_t n, uint32_t *erase_us, uint32_t *program_us) {
  if (written_slot < 0 || off % SECTOR || n > SECTOR || off + n > write_size || off != erased_upto) return -1;
  if (fail_write_at >= 0 && (long)off == fail_write_at) return -1;
  uint8_t *base = flash + slot_off[written_slot];
  uint64_t t0 = ops_now_us(ctx);
  memset(base + off, 0xFF, SECTOR);
  ++sector_erases;
  erased_upto = off + SECTOR;
  if (erase_ms) usleep((useconds_t)erase_ms * 1000);
  uint64_t t1 = ops_now_us(ctx);
  memcpy(base + off, d, n);
  if (program_ms) usleep((useconds_t)program_ms * 1000);
  uint64_t t2 = ops_now_us(ctx);
  *erase_us = (uint32_t)(t1 - t0), *program_us = (uint32_t)(t2 - t1);
  return 0;
}

static int ops_finish(void *ctx, uint32_t size) {
  (void)ctx;
  int ok = size == write_size && x11_ota_image_check(flash + slot_off[written_slot], size) == 0;
  save();
  return ok ? 0 : -1;
}

static void ops_abort(void *ctx) {
  (void)ctx;
  written_slot = -1;
  save();  /* a partly written inactive slot stays as it is: it is never selected */
}

/* esp_ota_set_boot_partition: the next seq that maps to `slot`, written to the other copy. */
static int set_boot(int slot) {
  if (!slot_image_valid(slot, NULL)) return -1;
  otadata_t o[2];
  read_otadata(o);
  int act = active_otadata(o);
  uint32_t seq;
  int copy;
  if (act >= 0) {
    seq = o[act].seq + 1;
    while ((seq - 1) % (uint32_t)nslots != (uint32_t)slot) ++seq;
    copy = (act + 1) % 2;
  } else {
    seq = (uint32_t)slot + 1;
    copy = 0;
  }
  otadata_t e = make_entry(seq, NEW);
  write_otadata(&e, copy);
  save();
  return 0;
}

static int ops_activate(void *ctx) {
  (void)ctx;
  return written_slot < 0 ? -1 : set_boot(written_slot);
}

static int ops_boot_other(void *ctx) {
  (void)ctx;
  return nslots < 2 || running < 0 ? -1 : set_boot(1 - running);
}

static void ops_reboot(void *ctx) {
  (void)ctx;
  want_reboot = 1;
}

static void ops_random(void *ctx, uint8_t *out, size_t n) {
  (void)ctx;
  arc4random_buf(out, n);
}

static uint32_t ops_now(void *ctx) {
  (void)ctx;
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static uint64_t ops_now_us(void *ctx) {
  (void)ctx;
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static const char *ops_idf(void *ctx) {
  (void)ctx;
  return idf_running;
}

/* the app's health decision (Arduino: verifyRollbackLater() true, then our own check) */
static void app_confirm(void) {
  otadata_t o[2];
  int re = running_entry(o);
  if (re >= 0 && o[re].state == PENDING) {
    o[re].state = VALID;
    write_otadata(&o[re], re);
    save();
  }
}

static void app_fail_health(void) {
  otadata_t o[2];
  int re = running_entry(o);
  if (re < 0 || nslots < 2 || !slot_image_valid(1 - running, NULL)) return;  /* rollback not possible */
  o[re].state = INVALID;
  write_otadata(&o[re], re);
  save();
  bootloader();
}

/* ---- main ------------------------------------------------------------------------------------- */

static void command(char *line, x11_ota_session *sess) {
  int n;
  if (!strcmp(line, "boot") || !strcmp(line, "crash")) {
    if (sess->state != X11_OTA_S_DONE) x11_ota_closed(sess);
    bootloader();
  } else if (!strcmp(line, "confirm")) {
    app_confirm();
  } else if (!strcmp(line, "fail-health")) {
    app_fail_health();
  } else if (sscanf(line, "corrupt-otadata %d", &n) == 1 && n >= 0 && n < 2) {
    flash[ota_off + SECTOR * (uint32_t)n] ^= 0x01;
    save();
  } else if (sscanf(line, "corrupt-slot %d", &n) == 1 && n >= 0 && n < nslots) {
    flash[slot_off[n] + 600] ^= 0x01;  /* inside the first segment: checksum and hash fail */
    save();
  } else if (!strcmp(line, "erase-otadata")) {
    memset(flash + ota_off, 0xFF, 2 * SECTOR);
    save();
  } else if (!strcmp(line, "stats")) {
    char text[1024];
    x11_ota_stats_text(endpoint, text, sizeof(text));
    for (char *c = text; *c; ++c)
      if (*c == '\n') *c = ' ';
    printf("stats %s\n", text);
  } else if (!strcmp(line, "quit")) {
    save();
    exit(0);
  }
  report();
}

static uint8_t in[65536];

static long sim_recv(void *ctx, uint8_t *buf, size_t n) {
  int fd = *(int *)ctx;
  if (rx_delay_ms) usleep((useconds_t)rx_delay_ms * 1000);
  ssize_t r = recv(fd, buf, n, MSG_DONTWAIT);
  if (r > 0) return (long)r;
  if (r == 0) return 0;
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? -1 : -2;
}

static long sim_send(void *ctx, const uint8_t *buf, size_t n) {
  ssize_t w = write(*(int *)ctx, buf, n);
  return w < 0 ? -2 : (long)w;
}

int main(int argc, char **argv) {
  int port = 5232;
  const char *key_file = NULL;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--flash") && i + 1 < argc) flash_path = argv[++i];
    else if (!strcmp(argv[i], "--key-file") && i + 1 < argc) key_file = argv[++i];
    else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--erase-ms") && i + 1 < argc) erase_ms = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--program-ms") && i + 1 < argc) program_ms = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--rx-chunk") && i + 1 < argc) rx_chunk = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--rx-delay-ms") && i + 1 < argc) rx_delay_ms = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--fail-write-at") && i + 1 < argc) fail_write_at = strtol(argv[++i], NULL, 0);
    else { fprintf(stderr, "usage: %s --flash FILE --key-file FILE [--port N] [--erase-ms N] [--program-ms N] "
                           "[--rx-chunk N] [--rx-delay-ms N] [--fail-write-at OFFSET]\n", argv[0]); return 2; }
  }
  if (!flash_path || !key_file) { fprintf(stderr, "--flash and --key-file are required\n"); return 2; }
  flash = malloc(FLASH_SIZE);
  FILE *f = fopen(flash_path, "rb");
  if (!flash || !f || fread(flash, 1, FLASH_SIZE, f) != FLASH_SIZE) { fprintf(stderr, "cannot read a 16 MB flash image\n"); return 1; }
  fclose(f);
  uint8_t key[X11_OTA_KEY_LEN];
  char hex[80] = "";
  f = fopen(key_file, "r");
  if (!f || !fgets(hex, sizeof(hex), f) || strlen(hex) < 64) { fprintf(stderr, "key file: 64 hex digits\n"); return 1; }
  fclose(f);
  for (int i = 0; i < 32; ++i) {
    unsigned v;
    if (sscanf(hex + 2 * i, "%2x", &v) != 1) { fprintf(stderr, "key file: 64 hex digits\n"); return 1; }
    key[i] = (uint8_t)v;
  }
  if (parse_table() != 0) { fprintf(stderr, "partition table: no OTA app slot or no otadata\n"); return 1; }
  x11_ota_endpoint ep;
  x11_ota_endpoint_init(&ep, key);
  endpoint = &ep;
  memset(key, 0, sizeof(key));
  if (rx_chunk < 1 || rx_chunk > 65536) rx_chunk = 8192;
  static uint8_t block_buf[X11_OTA_MAX_BLOCK];
  x11_ota_ops ops = {ops_slots, ops_begin, ops_write_sector, ops_finish, ops_abort, ops_activate, ops_boot_other,
                     ops_reboot, ops_random, ops_now, ops_now_us, ops_idf, block_buf, NULL};
  int srv = socket(AF_INET, SOCK_STREAM, 0), on = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)port);
  inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);  /* loopback only */
  if (bind(srv, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(srv, 4) < 0) { perror("bind"); return 1; }
  fprintf(stderr, "OTA simulator listening on 127.0.0.1:%d, %d app slots, otadata 0x%x, nvs 0x%x+0x%x\n", port, nslots,
          ota_off, nvs_off, nvs_size);
  bootloader();
  report();
  x11_ota_session sess;
  memset(&sess, 0, sizeof(sess));
  sess.state = X11_OTA_S_DONE;
  int cfd = -1;
  x11_ota_io io = {sim_recv, sim_send, &cfd, in, (size_t)rx_chunk, 4};
  char line[128];
  size_t ln = 0;
  uint8_t out[2 * X11_OTA_REPLY_LEN + X11_OTA_HELLO_LEN];
  for (;;) {
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(0, &rd);
    FD_SET(srv, &rd);
    int maxfd = srv;
    /* the socket is not read while the engine commits a block (its bytes stay in the socket) */
    int reading = cfd >= 0 && !x11_ota_busy(&sess);
    if (reading) FD_SET(cfd, &rd), maxfd = cfd > maxfd ? cfd : maxfd;
    struct timeval tv = {0, cfd >= 0 && x11_ota_busy(&sess) ? 0 : 100000};
    if (select(maxfd + 1, &rd, NULL, NULL, &tv) < 0 && errno != EINTR) return 1;
    if (FD_ISSET(srv, &rd)) {
      int fd = accept(srv, NULL, NULL);
      if (fd >= 0 && (cfd >= 0 || running < 0)) {  /* one session at a time; nothing while not booted */
        uint8_t busy[X11_OTA_REPLY_LEN] = "WLOTA2RS";
        busy[8] = X11_OTA_BUSY;
        if (write(fd, busy, sizeof(busy)) < 0) {}
        close(fd);
      } else if (fd >= 0) {
        cfd = fd;
        size_t n = x11_ota_open(&sess, &ep, &ops, out, sizeof(out));
        if (write(cfd, out, n) < 0) {}
      }
    }
    if (cfd >= 0) {
      /* the shared transport loop (also the ESP32's): reads never happen during a commit */
      if (!x11_ota_pump(&sess, &io)) {
        shutdown(cfd, SHUT_WR);
        usleep(20000);
        close(cfd), cfd = -1;
        x11_ota_closed(&sess);
      }
    }
    if (want_reboot) {
      want_reboot = 0;
      bootloader();
      report();
    }
    if (FD_ISSET(0, &rd)) {
      ssize_t got = read(0, line + ln, sizeof(line) - 1 - ln);
      if (got <= 0) { save(); return 0; }
      ln += (size_t)got;
      char *nl;
      while ((nl = memchr(line, '\n', ln)) != NULL) {
        *nl = 0;
        command(line, &sess);
        size_t used = (size_t)(nl + 1 - line);
        memmove(line, nl + 1, ln - used);
        ln -= used;
      }
    }
  }
}
