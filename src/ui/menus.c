/**
 * @file ui/menus.c
 * @brief PWR-button menus, both shown as full-screen pages (ui_page.c):
 *
 *   App menu   (single press on the watchface) — scrollable launcher
 *   Power menu (long press)                     — Restart / Power off
 *
 * A further PWR press, ← or swipe right closes either one.
 */

#include "ui/ui_internal.h"

/* ── App menu ────────────────────────────────────────────────────────────── */
/* Apps only; Wi-Fi, Bluetooth and the rest of the configuration live in
 * Settings (the last entry). */
typedef enum {
    APP_ACTIVITY,
    APP_MUSIC,
    APP_PHONE,
    APP_NAV,
    APP_WEATHER,
    APP_CALENDAR,
    APP_TIMER,
    APP_STOPWATCH,
    APP_ALARMS,
    APP_RULES,
    APP_RECORDER,
    APP_SOUNDMETER,
    APP_SENSORS,
    APP_CAMERA,
    APP_CALC,
    APP_TORCH,
    APP_SETTINGS,
    APP_COUNT,
} app_t;

static const struct {
    const char *icon;                            // symbol, or NULL to use image()
    const lv_image_dsc_t *(*image)(void);
    const char *name;
    uint32_t    color;
} s_apps[APP_COUNT] = {
    [APP_ACTIVITY]   = { LV_SYMBOL_SHUFFLE,    NULL,           "Activity",    0x3DDC84 },
    [APP_MUSIC]      = { LV_SYMBOL_PLAY,       NULL,           "Music",       0xFF375F },
    [APP_PHONE]      = { NULL,                 ui_icon_phone,  "Phone",       0x30D158 },
    [APP_NAV]        = { NULL,                 ui_icon_navigate, "Navigation", 0x0A84FF },
    [APP_WEATHER]    = { NULL,                 ui_icon_weather_menu, "Weather", 0x64D2FF },
    [APP_CALENDAR]   = { NULL,                 ui_icon_calendar, "Calendar",  0x5E5CE6 },
    [APP_CAMERA]     = { NULL,                 ui_icon_camera, "Camera",      0xD1D1D6 },
    [APP_CALC]       = { NULL,                 ui_icon_calculator, "Calculator", 0x63E6BE },
    [APP_TIMER]      = { NULL,                 ui_icon_hourglass, "Timer",    0xFF9F0A },
    [APP_STOPWATCH]  = { NULL,                 ui_icon_timer,  "Stopwatch",   0x0A84FF },
    [APP_ALARMS]     = { LV_SYMBOL_BELL,       NULL,           "Alarms",      0xFF453A },
    [APP_RULES]      = { LV_SYMBOL_ENVELOPE,   NULL,           "Notify alarms", 0xFF6B35 },
    [APP_RECORDER]   = { LV_SYMBOL_AUDIO,      NULL,           "Recorder",    0xBF5AF2 },
    [APP_SOUNDMETER] = { LV_SYMBOL_VOLUME_MAX, NULL,           "Sound meter", 0x64D2FF },
    [APP_SENSORS]    = { LV_SYMBOL_GPS,        NULL,           "Sensors",     0x30D158 },
    [APP_TORCH]      = { NULL,                 ui_icon_torch,  "Torch",       0xFFD60A },
    [APP_SETTINGS]   = { LV_SYMBOL_SETTINGS,   NULL,           "Settings",    0x8E8E93 },
};

static void app_cb(lv_event_t *e)
{
    app_t app = (app_t)(intptr_t)lv_event_get_user_data(e);
    ui_page_close();

    switch (app) {
    case APP_ACTIVITY: ui_open_tile(TILE_ACTIVITY); break;
    case APP_MUSIC:    ui_open_tile(TILE_MUSIC);    break;
    case APP_PHONE:    screen_phone_open();         break;
    case APP_NAV:      screen_nav_open();           break;
    case APP_WEATHER:  screen_weather_open();       break;
    case APP_CALENDAR: screen_calendar_open();      break;
    case APP_CAMERA:   screen_camera_open();        break;
    case APP_CALC:     screen_calc_open();          break;
    case APP_ALARMS:   screen_alarms_open();        break;
    case APP_RULES:    screen_rules_open();         break;
    case APP_TIMER:    ui_open_tile(TILE_TIMER);    break;
    case APP_STOPWATCH: screen_stopwatch_open();    break;
    case APP_RECORDER: screen_recorder_open();      break;
    case APP_SOUNDMETER: screen_soundmeter_open();  break;
    case APP_SENSORS:  screen_sensors_open();       break;
    case APP_SETTINGS: ui_open_tile(TILE_SETTINGS); break;
    case APP_TORCH:    quick_panel_torch_start();   break;
    default: break;
    }
}

void menu_apps_open(void)
{
    lv_obj_t *c = ui_page_open("Menu", NULL);

    for (int i = 0; i < APP_COUNT; i++) {
        lv_obj_t *row = lv_button_create(c);
        lv_obj_set_size(row, LV_PCT(100), 52);
        ui_fill(row, UI_COLOR_SURFACE);
        lv_obj_set_style_radius(row, 14, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_pad_hor(row, 8, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 12, 0);
        lv_obj_add_event_cb(row, app_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        /* Coloured round icon */
        lv_obj_t *badge = lv_obj_create(row);
        lv_obj_remove_style_all(badge);
        lv_obj_set_size(badge, 36, 36);
        lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
        lv_color_t badge_color = g_ui_theme.app_colors ? lv_color_hex(s_apps[i].color)
                                                       : UI_COLOR_ACCENT;
        lv_color_t icon_color = g_ui_theme.app_colors ? UI_COLOR_BG : ui_contrast(badge_color);
        lv_obj_set_style_bg_color(badge, badge_color, 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
        lv_obj_set_clickable(badge, false);
        lv_obj_t *ic;
        if (s_apps[i].icon) {
            ic = lv_label_create(badge);
            lv_label_set_text(ic, s_apps[i].icon);
            lv_obj_set_style_text_color(ic, icon_color, 0);
        } else {
            ic = ui_icon_create(badge, s_apps[i].image(), icon_color);
        }
        lv_obj_center(ic);

        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text(name, s_apps[i].name);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_20, 0);
    }
}

/* ── Power menu ──────────────────────────────────────────────────────────── */
static void restart_cb(lv_event_t *e)
{
    ui_request_restart();
}

static void power_off_cb(lv_event_t *e)
{
    ui_request_power_off();
}

static void cancel_cb(lv_event_t *e)
{
    ui_page_close();
}

static lv_obj_t *power_button(lv_obj_t *parent, const char *icon, const char *text,
                              lv_color_t color, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, LV_PCT(100), 60);
    ui_fill(btn, color);
    lv_obj_set_style_radius(btn, 30, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text_fmt(lbl, "%s  %s", icon, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(lbl);
    return btn;
}

void menu_power_open(void)
{
    lv_obj_t *c = ui_page_open("Power", NULL);
    lv_obj_set_style_pad_top(c, 16, 0);
    lv_obj_set_style_pad_row(c, 14, 0);

    power_button(c, LV_SYMBOL_REFRESH, "Restart", UI_COLOR_RAISED, restart_cb);
    power_button(c, LV_SYMBOL_POWER, "Power off", UI_COLOR_DANGER, power_off_cb);

    lv_obj_t *cancel = lv_button_create(c);
    lv_obj_set_style_bg_opa(cancel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(cancel, 0, 0);
    lv_obj_add_event_cb(cancel, cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(cancel);
    lv_label_set_text(lbl, "Cancel");
    lv_obj_set_style_text_color(lbl, UI_COLOR_DIM, 0);
}
