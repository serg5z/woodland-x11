#include <Arduino.h>
#include "esp_system.h"
#include "lwip/opt.h"
#include "lwip/sockets.h"

#if CONFIG_LWIP_MAX_SOCKETS < 72 || CONFIG_LWIP_MAX_ACTIVE_TCP < 72
#error "lwIP pools are below the Woodland 64-client planning target"
#endif
#if FD_SETSIZE != 80 || LWIP_SOCKET_OFFSET < 6
#error "FD_SETSIZE must leave at least six VFS descriptors"
#endif

extern "C" void app_main(void) {
  initArduino();
  Serial.begin(115200);
  Serial.printf("IDF=%s Arduino=%s sockets=%d netconn=%d tcp=%d listen=%d fdset=%d offset=%d\n",
      esp_get_idf_version(), ESP_ARDUINO_VERSION_STR, CONFIG_LWIP_MAX_SOCKETS,
      MEMP_NUM_NETCONN, CONFIG_LWIP_MAX_ACTIVE_TCP, CONFIG_LWIP_MAX_LISTENING_TCP,
      FD_SETSIZE, LWIP_SOCKET_OFFSET);
}
