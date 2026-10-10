#pragma once
#include <stdint.h>
#include <zephyr/kernel.h>

static inline int64_t esp_timer_get_time(void)
{
    return (int64_t)k_ticks_to_us_floor64(k_uptime_ticks());
}
