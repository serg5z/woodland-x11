#include "sdkconfig.h"
#include "lwip/opt.h"
#include "lwip/sockets.h"
#include "esp_vfs.h"

_Static_assert(CONFIG_LWIP_MAX_SOCKETS >= 72, "socket/netconn count below target");
_Static_assert(CONFIG_LWIP_MAX_ACTIVE_TCP >= 72, "active TCP PCB count below target");
_Static_assert(CONFIG_LWIP_MAX_LISTENING_TCP >= 2, "not enough X11 and OTA listeners");
_Static_assert(MEMP_NUM_NETCONN == CONFIG_LWIP_MAX_SOCKETS, "netconn count differs");
_Static_assert(MEMP_NUM_TCP_PCB == CONFIG_LWIP_MAX_ACTIVE_TCP, "TCP PCB count differs");
_Static_assert(MEMP_NUM_TCP_PCB_LISTEN == CONFIG_LWIP_MAX_LISTENING_TCP,
               "listening PCB count differs");
_Static_assert(FD_SETSIZE == 80, "FD_SETSIZE must be 80");
_Static_assert(LWIP_SOCKET_OFFSET >= 6, "not enough non-socket descriptors");
_Static_assert(MAX_FDS >= CONFIG_LWIP_MAX_SOCKETS, "VFS fd table too small");
