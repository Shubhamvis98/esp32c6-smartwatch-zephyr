/**
 * @file main.c
 * @brief Smartwatch firmware for the Waveshare ESP32-C6-Touch-LCD-1.69.
 *
 * Boot order matters:
 *   1. power_hold()      — latch battery power before the user lets go of PWR
 *   2. NVS + settings
 *   3. I2C bus, then RTC (sets the system clock), display/LVGL, touch
 *   4. IMU, audio, battery ADC  (optional — the watch still runs without them)
 *   5. UI, then background services (activity, buttons, Wi-Fi manager)
 */

#include <stdlib.h>
#include <time.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "hal/power.h"
#include "hal/button.h"
#include "pinout.h"
#include "hal/i2c_bus.h"
#include "hal/rtc.h"
#include "hal/display.h"
#include "hal/touch.h"
#include "hal/imu.h"
#include "hal/audio.h"
#include "hal/battery.h"
#include "hal/storage.h"
#include "app/recorder.h"
#include "app/settings.h"
#include "app/activity.h"
#include "app/wifi_mgr.h"
#include "app/ble_mgr.h"
#include "app/contacts.h"
#include "app/weather.h"
#include "app/calendar.h"
#include "app/ota_update.h"
#include "ui/ui_manager.h"
#include "lvgl_port.h"

static const char *TAG = "main";

void on_button(button_id_t id, button_event_t ev)
{
    const watch_settings_t *cfg = settings_get();
    bool pwr = id == BUTTON_PWR;
    switch (ev) {
    case BUTTON_CLICK:  ui_handle_button_action(pwr ? cfg->pwr_click  : cfg->boot_click, pwr);  break;
    case BUTTON_DOUBLE: ui_handle_button_action(pwr ? cfg->pwr_double : cfg->boot_double, pwr); break;
    case BUTTON_HOLD:   ui_handle_button_action(pwr ? cfg->pwr_long   : cfg->boot_long, pwr);   break;
    case BUTTON_LONG_HOLD:
        /* Whatever PWR is set to, a 5 s hold always reaches Restart / Power off */
        if (pwr && cfg->pwr_long != BTN_ACT_POWER_MENU) ui_handle_power_long_press();
        break;
    }
}

static void on_wrist_raise(void)
{
    ui_screen_wake();
}

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    power_hold();

    init_nvs();
    watch_settings_load();
    watch_settings_t *cfg = settings_get();

    settings_apply_timezone();

    ESP_ERROR_CHECK(i2c_bus_init());
    if (rtc_init() != ESP_OK) ESP_LOGE(TAG, "RTC unavailable");

    ESP_ERROR_CHECK(display_init(settings_get()->fps30));
    ESP_ERROR_CHECK(lvgl_port_init());
    if (touch_init() != ESP_OK) ESP_LOGE(TAG, "Touch unavailable");

    bool imu_ok = imu_init() == ESP_OK;
    if (audio_init() == ESP_OK) {
        audio_set_volume(cfg->volume);
    } else {
        ESP_LOGE(TAG, "Audio unavailable");
    }
    if (battery_init() != ESP_OK) ESP_LOGE(TAG, "Battery ADC unavailable");
    if (storage_init() == ESP_OK) {
        recorder_init();
    } else {
        ESP_LOGE(TAG, "Storage unavailable — no voice recordings");
    }

    contacts_init();                     // Phone app: favourites + recent calls
    weather_init();                      // last weather and calendar from the phone
    calendar_init();

    ESP_ERROR_CHECK(ui_init());

    if (imu_ok) {
        activity_set_raise_cb(on_wrist_raise);
        activity_start();
    } else {
        ESP_LOGE(TAG, "IMU unavailable — no steps or raise-to-wake");
    }
    button_set_double(BUTTON_PWR, settings_get()->pwr_double != BTN_ACT_NONE);
    button_set_double(BUTTON_BOOT, settings_get()->boot_double != BTN_ACT_NONE);
    ESP_ERROR_CHECK(button_start(BUTTON_PWR, PIN_BTN_PWR, on_button));
    if (button_start(BUTTON_BOOT, PIN_BTN_BOOT, on_button) != ESP_OK) ESP_LOGE(TAG, "BOOT button unavailable");
    if (wifi_mgr_start() != ESP_OK) ESP_LOGE(TAG, "Wi-Fi unavailable");
    if (ble_mgr_start() != ESP_OK) ESP_LOGE(TAG, "Bluetooth unavailable");

    extern size_t watch_heap_free(void);
    ESP_LOGI(TAG, "Smartwatch running, heap free %u", (unsigned)watch_heap_free());
    ota_confirm_running();               // a fresh update made it this far: keep it
}

/* Zephyr entry point: run the same start-up as ESP-IDF's app_main. */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Start-up runs on a heap stack that is given back afterwards, so the
 * Zephyr main thread itself only needs a small static stack. */
static void app_main_task(void *arg)
{
    app_main();
    vTaskDelete(NULL);
}

int main(void)
{
    if (xTaskCreate(app_main_task, "app_main", 8192, NULL, 14, NULL) != pdPASS) app_main();
    return 0;
}
