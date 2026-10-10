/**
 * @file hal/display.c
 * @brief ST7789V2 panel and PWM backlight on Zephyr.
 *
 * Zephyr's LVGL module creates the LVGL display from the "zephyr,display"
 * node at boot and sends the frames itself (on its flush thread, with two
 * draw buffers), so this file only handles the backlight, panel sleep and the
 * refresh rate.  The draw buffers are fixed at build time, so the 30 fps
 * setting needs no restart here.
 */
#include "hal/display.h"

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/led.h>
#include "esp_log.h"
#include "lvgl_private.h"

static const char *TAG = "display";

#define REFR_MS_60  16
#define REFR_MS_30  33

static const struct device *const s_panel = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
static const struct led_dt_spec s_bl = LED_DT_SPEC_GET(DT_ALIAS(backlight));

static lv_display_t *s_disp;
static uint8_t s_brightness = 100;
static bool s_fps30;

void display_set_backlight(uint8_t percent)
{
    if (percent > 100) percent = 100;
    s_brightness = percent;
    led_set_brightness_dt(&s_bl, percent);
}

uint8_t display_get_backlight(void)
{
    return s_brightness;
}

void display_set_sleep(bool sleep)
{
    if (sleep) display_blanking_on(s_panel);
    else       display_blanking_off(s_panel);
}

lv_display_t *display_get_lvgl_disp(void)
{
    return s_disp;
}

esp_err_t display_init(bool fps30)
{
    if (!device_is_ready(s_panel)) {
        ESP_LOGE(TAG, "Panel not ready");
        return ESP_ERR_INVALID_STATE;
    }
    if (!led_is_ready_dt(&s_bl)) ESP_LOGW(TAG, "Backlight not ready");
    s_disp = lv_display_get_default();
    if (!s_disp) {
        ESP_LOGE(TAG, "LVGL has no display");
        return ESP_FAIL;
    }
    s_fps30 = fps30;
    display_set_fps30(fps30);
    display_blanking_off(s_panel);
    return ESP_OK;
}

void display_set_fps30(bool fps30)
{
    s_fps30 = fps30;
    lv_timer_t *t = s_disp ? s_disp->refr_timer : NULL;
    if (t) lv_timer_set_period(t, fps30 ? REFR_MS_30 : REFR_MS_60);
}

bool display_buffers_fps30(void)
{
    return s_fps30;
}
