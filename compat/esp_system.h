#pragma once
#include "esp_err.h"
#include <zephyr/sys/reboot.h>

static inline void esp_restart(void)
{
    sys_reboot(SYS_REBOOT_COLD);
}
