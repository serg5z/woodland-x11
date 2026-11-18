#pragma once

#include <Arduino.h>
#include <LovyanGFX.hpp>

// Pins and native dimensions follow the installed Arduino-ESP32
// 3.3.12 variant jczn_2432s028r. The two-USB board's LCD controller is
// selected at runtime in LCD_Bars.ino; the variant does not specify it.
template <class Panel>
class LGFX_CYD : public lgfx::LGFX_Device {
  lgfx::Bus_SPI bus_;
  Panel panel_;
  lgfx::Touch_XPT2046 touch_;

 public:
  LGFX_CYD() {
    auto bus = bus_.config();
    bus.spi_host = HSPI_HOST;
    bus.spi_mode = 0;
    bus.freq_write = 20000000;  // conservative first bring-up clock
    bus.freq_read = 5000000;  // verified pixel-perfect GRAM read clock
    bus.spi_3wire = false;
    bus.use_lock = true;
    bus.dma_channel = SPI_DMA_CH_AUTO;
    bus.pin_sclk = CYD_TFT_SCK;
    bus.pin_mosi = CYD_TFT_MOSI;
    bus.pin_miso = CYD_TFT_MISO;
    bus.pin_dc = CYD_TFT_DC;
    bus_.config(bus);
    panel_.setBus(&bus_);

    auto panel = panel_.config();
    panel.pin_cs = CYD_TFT_CS;
    panel.pin_rst = -1;  // panel reset is tied to board reset on this design
    panel.pin_busy = -1;
    panel.panel_width = 240;
    panel.panel_height = 320;
    panel.offset_x = 0;
    panel.offset_y = 0;
    panel.readable = true;
    panel.bus_shared = false;
    panel.dummy_read_pixel = 8;
    panel_.config(panel);

    auto touch = touch_.config();
    touch.spi_host = VSPI_HOST;  // LovyanGFX expects the ESP-IDF host id, not Arduino's VSPI bus number (3)
    touch.pin_sclk = CYD_TP_CLK;
    touch.pin_mosi = CYD_TP_MOSI;
    touch.pin_miso = CYD_TP_MISO;
    touch.pin_cs = CYD_TP_CS;
    touch.pin_int = CYD_TP_IRQ;
    touch.freq = 1000000;
    touch.x_min = 300;
    touch.x_max = 3900;
    touch.y_min = 400;
    touch.y_max = 3900;
    touch.offset_rotation = 0;
    touch.bus_shared = false;  // CYD touch uses VSPI; the LCD uses HSPI
    touch_.config(touch);
    panel_.setTouch(&touch_);
    setPanel(&panel_);
  }
};
