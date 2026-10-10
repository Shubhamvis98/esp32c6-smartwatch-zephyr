/**
 * @file app/settings.c
 * @brief User settings persisted as a single NVS blob.
 */

#include "app/settings.h"
#include "ui/theme.h"

#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "settings";

#define SETTINGS_NS   "settings"
#define SETTINGS_KEY  "v1"

static watch_settings_t s_settings = {
    .brightness    = 70,
    .timeout_s     = 15,
    .raise_to_wake = true,
    .volume        = 70,
    .tz            = CONFIG_WATCH_TIMEZONE,
    .use_24h       = true,
    .watchface     = WATCHFACE_DIGITAL,
    .wifi_enabled  = true,
    .ble_name      = "C6 Watch",
    .auto_tz       = true,
    .step_goal     = CONFIG_WATCH_STEP_GOAL,
    .boot_click    = 1,       // BTN_ACT_BACK
    .boot_double   = 7,       // BTN_ACT_PLAY_PAUSE
    .boot_long     = 6,       // BTN_ACT_TORCH
    .pwr_set       = true,
    .pwr_click     = 16,      // BTN_ACT_HOME_OR_APPS
    .pwr_double    = 0,       // BTN_ACT_NONE: press acts straight away
    .pwr_long      = 17,      // BTN_ACT_POWER_MENU
};

void watch_settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open(SETTINGS_NS, NVS_READONLY, &h) == ESP_OK) {
        /* A blob from older firmware is shorter: load it over the defaults so
         * fields added since keep their default values. */
        size_t len = 0;
        if (nvs_get_blob(h, SETTINGS_KEY, NULL, &len) == ESP_OK && len <= sizeof(s_settings)) {
            nvs_get_blob(h, SETTINGS_KEY, &s_settings, &len);
            /* The old blob's trailing padding byte overlaps auto_tz and was
             * loaded as false; blobs without tz_name predate auto_tz. */
            if (len < offsetof(watch_settings_t, tz_name) + sizeof(s_settings.tz_name)) {
                s_settings.auto_tz = true;
            }
        }
        nvs_close(h);
    }

    s_settings.tz[sizeof(s_settings.tz) - 1] = 0;
    s_settings.ble_name[sizeof(s_settings.ble_name) - 1] = 0;
    if (!s_settings.ble_name[0]) strcpy(s_settings.ble_name, "C6 Watch");
    s_settings.face_id[sizeof(s_settings.face_id) - 1] = 0;
    s_settings.tz_name[sizeof(s_settings.tz_name) - 1] = 0;
    if (s_settings.theme >= THEME_COUNT) s_settings.theme = THEME_MONO;
    if (s_settings.theme_bg >= THEME_BG_COUNT) s_settings.theme_bg = THEME_BG_BLACK;
    /* 18 = BTN_ACT_COUNT */
    if (s_settings.boot_click  >= 18) s_settings.boot_click  = 1;
    if (s_settings.boot_double >= 18) s_settings.boot_double = 7;
    if (s_settings.boot_long   >= 18) s_settings.boot_long   = 6;
    if (!s_settings.pwr_set || s_settings.pwr_click >= 18 || s_settings.pwr_double >= 18 ||
        s_settings.pwr_long >= 18) {
        s_settings.pwr_set = true;
        s_settings.pwr_click = 16;
        s_settings.pwr_double = 0;
        s_settings.pwr_long = 17;
    }
    if (s_settings.theme_accent & ~(THEME_ACCENT_SET | 0xFFFFFFu)) s_settings.theme_accent = 0;
    /* 0 = saved before the goal was a setting */
    if (s_settings.step_goal < STEP_GOAL_MIN || s_settings.step_goal > STEP_GOAL_MAX) {
        s_settings.step_goal = CONFIG_WATCH_STEP_GOAL;
    }
    if (!s_settings.face_id[0]) {
        /* Nothing saved, or saved before face_id existed: map the old style index */
        static const char *const legacy[WATCHFACE_COUNT] = { "digital", "analog", "minimal" };
        strcpy(s_settings.face_id, legacy[s_settings.watchface < WATCHFACE_COUNT ? s_settings.watchface : 0]);
    }
}

watch_settings_t *settings_get(void)
{
    return &s_settings;
}

void watch_settings_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(SETTINGS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, SETTINGS_KEY, &s_settings, sizeof(s_settings));
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) ESP_LOGW(TAG, "Save failed: %s", esp_err_to_name(err));
}

void settings_apply_timezone(void)
{
    setenv("TZ", s_settings.tz[0] ? s_settings.tz : "UTC0", 1);
    tzset();
}

void settings_set_timezone(const char *tz)
{
    strlcpy(s_settings.tz, tz, sizeof(s_settings.tz));
    settings_apply_timezone();
    watch_settings_save();
}
