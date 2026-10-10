/**
 * @file hal/power.c
 * @brief BAT_EN power latch (the PWR button itself is read by hal/button.c).
 */

#include "hal/power.h"
#include "pinout.h"

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "power";

#define BTN_POLL_MS         10

static inline bool btn_pressed(void)
{
    return gpio_get_level(PIN_BTN_PWR) == 0;   // active-low
}

void power_hold(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_BAT_EN,
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);
    gpio_set_level(PIN_BAT_EN, 1);
}

void power_off(void)
{
    ESP_LOGI(TAG, "Powering off");
    gpio_set_level(PIN_BAT_EN, 0);
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Still running → USB is supplying power.  Wait for a fresh PWR press
     * (after release) and reboot, which behaves like powering back on. */
    ESP_LOGI(TAG, "USB powered — press PWR to restart");
    while (btn_pressed()) vTaskDelay(pdMS_TO_TICKS(BTN_POLL_MS));
    while (!btn_pressed()) vTaskDelay(pdMS_TO_TICKS(50));
    esp_restart();
}
