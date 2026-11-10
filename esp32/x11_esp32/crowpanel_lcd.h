// LovyanGFX device for the Elecrow CrowPanel Advance 4.3" (ESP32-S3-N16R8, 800x480 RGB565
// IPS panel, GT911 touch). Board facts only: the RGB data and sync GPIOs, sync timings, the
// default 16 MHz pixel clock, a PSRAM framebuffer, and the touch controller's I2C bus.
// The pixel clock can be changed at run time through _bus_instance (pclk/, -DX11_PCLK).
#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <driver/i2c.h>

namespace crowpanel43 {
constexpr int WIDTH = 800, HEIGHT = 480;
constexpr uint32_t PCLK_HZ = 16000000;
// RGB565 data lines, least significant bit first: blue B0..B4, green G0..G5, red R0..R4.
constexpr int8_t DATA_GPIO[16] = {21, 47, 48, 45, 38, 9, 10, 11, 12, 13, 14, 7, 17, 18, 3, 46};
constexpr int8_t DE_GPIO = 42, VSYNC_GPIO = 41, HSYNC_GPIO = 40, PCLK_GPIO = 39;
// Sync timing (pixels / lines): front porch, pulse width, back porch; both polarities 0.
constexpr uint16_t H_FRONT = 8, H_PULSE = 4, H_BACK = 8;
constexpr uint16_t V_FRONT = 8, V_PULSE = 4, V_BACK = 8;
// GT911 on I2C port 0, 400 kHz, address 0x5D; no interrupt or reset line used.
constexpr int8_t TOUCH_SDA = 15, TOUCH_SCL = 16;
constexpr uint8_t TOUCH_ADDR = 0x5D;
constexpr uint32_t TOUCH_I2C_HZ = 400000;
}  // namespace crowpanel43

class LGFX : public lgfx::LGFX_Device {
 public:
  lgfx::Bus_RGB _bus_instance;
  lgfx::Panel_RGB _panel_instance;
  lgfx::Touch_GT911 _touch_instance;

  LGFX(void) {
    using namespace crowpanel43;
    {
      auto p = _panel_instance.config();
      p.memory_width = WIDTH;
      p.memory_height = HEIGHT;
      p.panel_width = WIDTH;
      p.panel_height = HEIGHT;
      p.offset_x = 0;
      p.offset_y = 0;
      _panel_instance.config(p);
    }
    {
      auto d = _panel_instance.config_detail();
      d.use_psram = 1;
      _panel_instance.config_detail(d);
    }
    {
      auto b = _bus_instance.config();
      b.panel = &_panel_instance;
      b.pin_d0 = (gpio_num_t)DATA_GPIO[0];
      b.pin_d1 = (gpio_num_t)DATA_GPIO[1];
      b.pin_d2 = (gpio_num_t)DATA_GPIO[2];
      b.pin_d3 = (gpio_num_t)DATA_GPIO[3];
      b.pin_d4 = (gpio_num_t)DATA_GPIO[4];
      b.pin_d5 = (gpio_num_t)DATA_GPIO[5];
      b.pin_d6 = (gpio_num_t)DATA_GPIO[6];
      b.pin_d7 = (gpio_num_t)DATA_GPIO[7];
      b.pin_d8 = (gpio_num_t)DATA_GPIO[8];
      b.pin_d9 = (gpio_num_t)DATA_GPIO[9];
      b.pin_d10 = (gpio_num_t)DATA_GPIO[10];
      b.pin_d11 = (gpio_num_t)DATA_GPIO[11];
      b.pin_d12 = (gpio_num_t)DATA_GPIO[12];
      b.pin_d13 = (gpio_num_t)DATA_GPIO[13];
      b.pin_d14 = (gpio_num_t)DATA_GPIO[14];
      b.pin_d15 = (gpio_num_t)DATA_GPIO[15];
      b.pin_henable = (gpio_num_t)DE_GPIO;
      b.pin_vsync = (gpio_num_t)VSYNC_GPIO;
      b.pin_hsync = (gpio_num_t)HSYNC_GPIO;
      b.pin_pclk = (gpio_num_t)PCLK_GPIO;
      b.freq_write = PCLK_HZ;
      b.hsync_polarity = 0;
      b.hsync_front_porch = H_FRONT;
      b.hsync_pulse_width = H_PULSE;
      b.hsync_back_porch = H_BACK;
      b.vsync_polarity = 0;
      b.vsync_front_porch = V_FRONT;
      b.vsync_pulse_width = V_PULSE;
      b.vsync_back_porch = V_BACK;
      b.pclk_idle_high = 1;
      _bus_instance.config(b);
    }
    _panel_instance.setBus(&_bus_instance);
    {
      auto t = _touch_instance.config();
      t.x_min = 0;
      t.x_max = WIDTH;
      t.y_min = 0;
      t.y_max = HEIGHT;
      t.pin_int = -1;
      t.pin_rst = -1;
      t.bus_shared = false;
      t.offset_rotation = 0;
      t.i2c_port = I2C_NUM_0;
      t.pin_sda = TOUCH_SDA;
      t.pin_scl = TOUCH_SCL;
      t.freq = TOUCH_I2C_HZ;
      t.i2c_addr = TOUCH_ADDR;
      _touch_instance.config(t);
      _panel_instance.setTouch(&_touch_instance);
    }
    setPanel(&_panel_instance);
  }
};
