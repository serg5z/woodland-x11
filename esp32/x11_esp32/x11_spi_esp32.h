// SPI link to a host computer (Phase 3 echo test): the panel is the SPI2 slave of the
// Woodland SPI link (spilink/, unchanged), with a READY line. Built only with -DX11_SPI_ECHO
// (a test image); a normal build compiles none of it.
//
// Pins (CrowPanel V1.3, DIP K1 in position 1: S1=0, S0=1):
//   SCLK GPIO5 -> J9-2 (U11 A1X)   MOSI GPIO6 -> J9-4 (U11 B1X)   MISO GPIO4 -> J9-3 (U11 A1Y)
//   CS   GPIO8 -> J11-6            READY GPIO2 -> J11-5           GND -> J9-6
//
// Electrical safety: the slave (and so MISO and READY as outputs) is active only while the host
// is alive. CS has a pull-down here; a powered host holds chip select HIGH between transfers. CS HIGH
// for SPI_ALIVE_MS turns the slave on; CS LOW for SPI_DEAD_MS (no frame is that long) turns it
// off again and returns MISO and READY to inputs (high impedance), so an unpowered host is never
// driven by the panel.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef X11_SPI_ECHO
static constexpr int SPI_PIN_SCLK = 5, SPI_PIN_MOSI = 6, SPI_PIN_MISO = 4, SPI_PIN_CS = 8, SPI_PIN_READY = 2;
static constexpr size_t SPI_FRAME = 4096;
static constexpr uint32_t SPI_ALIVE_MS = 100;  // CS HIGH this long: the host is up -> start the slave
static constexpr uint32_t SPI_DEAD_MS = 50;    // CS LOW this long (a 4 KiB frame at 3.9 MHz: 8.4 ms) -> stop

void spi_echo_init();
// Once per loop pass: liveness, transaction hand-off, echo of received messages. Non-blocking.
void spi_echo_poll();
// Diagnostics lines ("spi.*") for _WOODLAND_DIAG.
size_t spi_echo_diag(char *out, size_t cap);
#endif
#ifdef X11_SPI_X11
#ifndef X11_SPI_ECHO
#error "X11_SPI_X11 needs X11_SPI_ECHO"
#endif
// A single virtual X11 stream. The caller owns X11 protocol parsing and authentication.
// The existing SPI echo and screenshot messages continue to use their own formats.
void spi_x11_init(int (*message)(uint8_t op, uint32_t session, const uint8_t *data, size_t n),
                  void (*reset)());
int spi_x11_send(uint8_t op, uint32_t session, const uint8_t *data, size_t n);
#endif

#ifdef X11_SPI_SHOT  // screenshots over the link; test builds only
#ifndef X11_SPI_ECHO
#error "X11_SPI_SHOT needs X11_SPI_ECHO"
#endif
// The framebuffer to capture (w x h RGB565 LE) and its change counter. A request
// (WLSHOTRQ) copies it into a PSRAM snapshot inside one loop pass, then the info, the
// chunks and the CRC-32 go out as the send queue allows.
void spi_shot_source(const uint16_t *fb, const uint32_t *rev, int w, int h);
#define SPI_SHOT_SOURCE(fb, rev, w, h) spi_shot_source(fb, rev, w, h)
#elif defined(X11_SPI_ECHO)
#define SPI_SHOT_SOURCE(fb, rev, w, h) ((void)0)
#endif
