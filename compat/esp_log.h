/* ESP_LOGx on Zephyr's printk. */
#pragma once
#include <zephyr/sys/printk.h>
#include <zephyr/kernel.h>

#ifndef WATCH_LOG_LEVEL
#define WATCH_LOG_LEVEL 3     /* 1 = E, 2 = W, 3 = I, 4 = D */
#endif

#define ESP_LOG_LINE_(lvl, ch, tag, fmt, ...) do {                       \
        if (WATCH_LOG_LEVEL >= (lvl))                                    \
            printk(ch " (%u) %s: " fmt "\n", k_uptime_get_32(), tag, ##__VA_ARGS__); \
    } while (0)

#define ESP_LOGE(tag, fmt, ...) ESP_LOG_LINE_(1, "E", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) ESP_LOG_LINE_(2, "W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) ESP_LOG_LINE_(3, "I", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) ESP_LOG_LINE_(4, "D", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) do { } while (0)
#define ESP_EARLY_LOGE ESP_LOGE
#define ESP_EARLY_LOGW ESP_LOGW
#define ESP_EARLY_LOGI ESP_LOGI
