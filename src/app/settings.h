#pragma once
/**
 * @file app/settings.h
 * @brief User settings persisted in NVS.
 */

#include <stdbool.h>
#include <stdint.h>

/* Watchface index used by settings saved before faces became modules */
typedef enum {
    WATCHFACE_DIGITAL,
    WATCHFACE_ANALOG,
    WATCHFACE_MINIMAL,
    WATCHFACE_COUNT,
} watchface_style_t;

/* New fields must be appended: older saved blobs are loaded as a prefix. */
typedef struct {
    uint8_t  brightness;      // backlight %, 5–100
    uint16_t timeout_s;       // screen-off timeout
    bool     raise_to_wake;
    uint8_t  volume;          // 0–100
    char     tz[48];          // POSIX TZ string
    bool     use_24h;
    uint8_t  watchface;       // legacy watchface_style_t, see face_id
    bool     ble_enabled;
    uint8_t  ble_mode;        // ble_mode_t (app/ble_mgr.h)
    bool     wifi_enabled;
    char     ble_name[20];    // Bluetooth name (BLE_NAME_MAX_LEN chars max)
    char     face_id[24];     // selected watchface (ui/faces/watchface.h)
    bool     auto_tz;         // set the timezone from the IP address at each time sync
    char     tz_name[32];     // IANA name of tz ("Asia/Kolkata"), "" if unknown
    bool     aod;             // always-on display: dim the watchface instead of turning it off
    bool     raise_cal;       // raise_up/raise_down hold a calibration
    int16_t  raise_up[3];     // accel while looking at the watch, milli-g
    int16_t  raise_down[3];   // accel with the arm hanging down, milli-g
    /* Idle screen pages other than the watchface (ui_tile_t values, swipe
     * order), 0 ends the list; all zero = the default order.  Pages missing
     * from the list are added at the end. */
    uint8_t  idle_order[6];
    uint8_t  idle_hidden;     // bit (1 << tile) set: page left out of the idle screen
    uint8_t  idle_face_pos;   // where the watchface sits among them (0 = first)
    bool     fps30;           // 30 fps and smaller draw buffers (saves ~15 KB RAM after a restart)
    uint16_t step_goal;       // daily step goal, STEP_GOAL_MIN–STEP_GOAL_MAX
    uint8_t  theme;           // theme_mode_t (ui/theme.h): Monochrome / Colour
    uint8_t  theme_bg;        // theme_bg_t
    bool     app_colors;      // app menu icons in their own colours
    uint32_t theme_accent;    // Colour accent: 0 = default green, else THEME_ACCENT_SET | 0xRRGGBB
    /* BOOT button actions (btn_action_t, ui/ui_manager.h) */
    uint8_t  boot_click;
    uint8_t  boot_double;
    uint8_t  boot_long;
    /* PWR button actions; pwr_set is false in settings saved before they
     * existed, which then get the defaults */
    bool     pwr_set;
    uint8_t  pwr_click;
    uint8_t  pwr_double;
    uint8_t  pwr_long;
} watch_settings_t;

#define STEP_GOAL_MIN   1000
#define STEP_GOAL_MAX   50000
#define STEP_GOAL_STEP  500

/** @brief Load settings from NVS (defaults if absent).  Needs nvs_flash_init(). */
void watch_settings_load(void);

/** @brief Current settings.  Modify fields, then call watch_settings_save(). */
watch_settings_t *settings_get(void);

void watch_settings_save(void);

/** @brief Set the timezone (POSIX TZ string), apply it and save. */
void settings_set_timezone(const char *tz);

/** @brief Apply the stored timezone to the C library (call once at boot). */
void settings_apply_timezone(void);
