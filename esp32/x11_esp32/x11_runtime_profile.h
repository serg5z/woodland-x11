#ifndef X11_RUNTIME_PROFILE_H
#define X11_RUNTIME_PROFILE_H

#ifdef X11_RUNTIME_PROFILE
#include <stddef.h>
#include <stdint.h>

void x11_runtime_profile_poll(void);
void x11_runtime_profile_select_wait(uint32_t wait_us);
size_t x11_runtime_profile_diag(char *out, size_t cap);
#endif

#endif
