/* Small ESP-IDF helpers: error names, GPIO, MAC, heap, app description. */
#include <string.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/sys/sys_heap.h>
#include <zephyr/version.h>
#include <zephyr/app_version.h>
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "driver/gpio.h"

const char *esp_err_to_name(esp_err_t code)
{
    switch (code) {
    case ESP_OK:                    return "ESP_OK";
    case ESP_FAIL:                  return "ESP_FAIL";
    case ESP_ERR_NO_MEM:            return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG:       return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE:     return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE:      return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND:         return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED:     return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_TIMEOUT:           return "ESP_ERR_TIMEOUT";
    case ESP_ERR_INVALID_RESPONSE:  return "ESP_ERR_INVALID_RESPONSE";
    case ESP_ERR_NVS_NOT_FOUND:     return "ESP_ERR_NVS_NOT_FOUND";
    case ESP_ERR_OTA_VALIDATE_FAILED: return "ESP_ERR_OTA_VALIDATE_FAILED";
    default:                        return "ESP_ERR";
    }
}

/* ---- GPIO (all pins on this chip are on gpio0) ---- */

static const struct device *const s_gpio = DEVICE_DT_GET(DT_NODELABEL(gpio0));

esp_err_t esp_gpio_config(const gpio_config_t *cfg)
{
    if (!device_is_ready(s_gpio)) return ESP_ERR_INVALID_STATE;
    for (int pin = 0; pin < 64; pin++) {
        if (!(cfg->pin_bit_mask & (1ULL << pin))) continue;
        gpio_flags_t f = 0;
        if (cfg->mode & GPIO_MODE_INPUT)  f |= GPIO_INPUT;
        if (cfg->mode & GPIO_MODE_OUTPUT) f |= GPIO_OUTPUT;
        if (cfg->mode == GPIO_MODE_OUTPUT_OD) f |= GPIO_OPEN_DRAIN;
        if (cfg->pull_up_en)   f |= GPIO_PULL_UP;
        if (cfg->pull_down_en) f |= GPIO_PULL_DOWN;
        if (gpio_pin_configure(s_gpio, pin, f) != 0) return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

int esp_gpio_get_level(gpio_num_t pin)
{
    return gpio_pin_get_raw(s_gpio, pin) > 0;
}

esp_err_t esp_gpio_set_level(gpio_num_t pin, uint32_t level)
{
    return gpio_pin_set_raw(s_gpio, pin, level ? 1 : 0) == 0 ? ESP_OK : ESP_FAIL;
}

/* ---- heap ---- */

extern int malloc_runtime_stats_get(struct sys_memory_stats *stats);

/* ---- app description ---- */

const esp_app_desc_t *esp_app_get_description(void)
{
    static esp_app_desc_t d;
    if (!d.version[0]) {
        strncpy(d.version, APP_VERSION_STRING, sizeof(d.version) - 1);
        strncpy(d.project_name, "smartwatch", sizeof(d.project_name) - 1);
        strncpy(d.date, __DATE__, sizeof(d.date) - 1);
        strncpy(d.time, __TIME__, sizeof(d.time) - 1);
        snprintf(d.idf_ver, sizeof(d.idf_ver), "Zephyr %s", KERNEL_VERSION_STRING);
    }
    return &d;
}

/* ---- settimeofday (picolibc has no system clock hook) ---- */

#include <sys/time.h>
#include <zephyr/sys/clock.h>

int settimeofday(const struct timeval *tv, const struct timezone *tz)
{
    if (!tv) return 0;
    struct timespec ts = { .tv_sec = tv->tv_sec, .tv_nsec = tv->tv_usec * 1000 };
    return sys_clock_settime(SYS_CLOCK_REALTIME, &ts);
}
