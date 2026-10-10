/**
 * @file ui/screen_settings.c
 * @brief Settings tile: a menu whose rows open full-screen pages.
 *
 *   Wi-Fi             MyNet ›     → Wi-Fi tile (hidden from swiping)
 *   Bluetooth           Watch ›     → Bluetooth tile (hidden from swiping)
 *   Watchface        Digital ›     → pick the watchface style
 *   Time              London ›     → 24 h switch, timezone (auto or picked), NTP sync
 *   Display              70% ›     → brightness, screen timeout, raise to wake
 *   Sound                70% ›     → volume, test beep
 *   Buttons                  ›     → what the PWR and BOOT buttons do
 *   About                85% ›     → battery, firmware, memory
 *   [ Factory reset ]                → confirm page, hold to erase everything
 *   [ Hold to power off ]
 */

#include "ui/ui_internal.h"
#include "ui/ui_manager.h"
#include "hal/storage.h"
#include "ui/faces/watchface.h"
#include "app/face_server.h"
#include "app/ota_update.h"
#include "app/timezones.h"
#include "hal/display.h"
#include "hal/audio.h"
#include "hal/battery.h"
#include "hal/button.h"
#include "hal/rtc.h"
#include "app/settings.h"
#include "app/activity.h"
#include "app/wifi_mgr.h"
#include "app/ble_mgr.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"

/* ── Data ────────────────────────────────────────────────────────────────── */
static const uint16_t s_timeouts[] = { 5, 10, 15, 30, 60 };
#define TIMEOUT_OPTIONS "5 s\n10 s\n15 s\n30 s\n60 s"

/* ── Menu tile ───────────────────────────────────────────────────────────── */
typedef enum {
    ROW_WIFI, ROW_BLUETOOTH, ROW_FACE, ROW_IDLE, ROW_ACTIVITY, ROW_TIME, ROW_DISPLAY, ROW_THEME, ROW_SOUND, ROW_BUTTONS, ROW_UPDATE, ROW_ABOUT, ROW_COUNT
} row_t;

static lv_obj_t *s_row_hint[ROW_COUNT];

static void page_face(void);
static void page_idle(void);
static void apply_raise_calibration(void);
static void page_time(void);
static void page_activity(void);
static void page_display(void);
static void page_sound(void);
static void page_buttons(void);
static void page_update(void);
static void page_theme(void);
static void page_about(void);

static void row_cb(lv_event_t *e)
{
    switch ((row_t)(intptr_t)lv_event_get_user_data(e)) {
    case ROW_WIFI:    ui_open_tile(TILE_WIFI);  break;
    case ROW_BLUETOOTH: ui_open_tile(TILE_BLUETOOTH); break;
    case ROW_FACE:    page_face();    break;
    case ROW_IDLE:    page_idle();    break;
    case ROW_ACTIVITY: page_activity(); break;
    case ROW_TIME:    page_time();    break;
    case ROW_DISPLAY: page_display(); break;
    case ROW_THEME:   page_theme();   break;
    case ROW_SOUND:   page_sound();   break;
    case ROW_BUTTONS: page_buttons(); break;
    case ROW_UPDATE:  page_update();  break;
    case ROW_ABOUT:   page_about();   break;
    default: break;
    }
}

/* @p icon is a symbol string, or NULL to use @p image instead */
static void menu_row_ex(lv_obj_t *parent, row_t id, const char *icon,
                        const lv_image_dsc_t *image, const char *text)
{
    lv_obj_t *row = lv_button_create(parent);
    lv_obj_set_size(row, LV_PCT(100), 42);
    ui_fill(row, UI_COLOR_SURFACE);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_pad_hor(row, 10, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_add_event_cb(row, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)id);

    lv_obj_t *ic;
    if (icon) {
        ic = lv_label_create(row);
        lv_label_set_text(ic, icon);
        lv_obj_set_style_text_color(ic, UI_COLOR_ACCENT, 0);
    } else {
        ic = ui_icon_create(row, image, UI_COLOR_ACCENT);
        lv_image_set_inner_align(ic, LV_IMAGE_ALIGN_LEFT_MID);
    }
    lv_obj_set_width(ic, 18);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, text);                  // full width; the hint gives way

    /* Hint fills the rest of the row on one line, "…" if it doesn't fit */
    lv_obj_t *hint = lv_label_create(row);
    s_row_hint[id] = hint;
    lv_label_set_text(hint, "");
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(hint, 1);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, UI_COLOR_DIM, 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_height(hint, lv_font_get_line_height(&lv_font_montserrat_14));

    lv_obj_t *arrow = lv_label_create(row);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(arrow, UI_COLOR_DIM, 0);
}

static void menu_row(lv_obj_t *parent, row_t id, const char *icon, const char *text)
{
    menu_row_ex(parent, id, icon, NULL, text);
}

static void power_off_cb(lv_event_t *e)
{
    ui_request_power_off();
}

/* ── Factory reset confirmation ──────────────────────────────────────────── */
static void reset_confirm_cb(lv_event_t *e)
{
    ui_request_factory_reset();
}

static void reset_cancel_cb(lv_event_t *e)
{
    ui_page_close();
}

static void factory_reset_cb(lv_event_t *e)
{
    lv_obj_t *c = ui_page_open("Factory reset", NULL);

    lv_obj_t *msg = lv_label_create(c);
    lv_obj_set_width(msg, LV_PCT(100));
    lv_label_set_long_mode(msg, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(msg, "Erases all settings, alarms, steps, saved Wi-Fi networks, "
                           "Bluetooth pairings and voice recordings, then restarts.");

    lv_obj_t *btn = lv_button_create(c);
    lv_obj_set_size(btn, LV_PCT(100), 50);
    ui_fill(btn, UI_COLOR_DANGER);
    lv_obj_set_style_radius(btn, 25, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, reset_confirm_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, LV_SYMBOL_TRASH " Hold to reset");
    lv_obj_center(lbl);

    lv_obj_t *cancel = lv_button_create(c);
    lv_obj_set_width(cancel, LV_PCT(100));
    lv_obj_set_style_bg_opa(cancel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(cancel, 0, 0);
    lv_obj_add_event_cb(cancel, reset_cancel_cb, LV_EVENT_CLICKED, NULL);
    lbl = lv_label_create(cancel);
    lv_label_set_text(lbl, "Cancel");
    lv_obj_set_style_text_color(lbl, UI_COLOR_DIM, 0);
    lv_obj_center(lbl);
}

void screen_settings_create(lv_obj_t *parent)
{
    activity_set_raise_enabled(settings_get()->raise_to_wake);
    apply_raise_calibration();

    lv_obj_t *col = ui_tile_layout(parent, "Settings");

    menu_row(col, ROW_WIFI,    LV_SYMBOL_WIFI,       "Wi-Fi");
    menu_row(col, ROW_BLUETOOTH, LV_SYMBOL_BLUETOOTH, "Bluetooth");
    menu_row_ex(col, ROW_FACE, NULL, ui_icon_watch(), "Watchface");
    menu_row(col, ROW_IDLE,    LV_SYMBOL_HOME,       "Idle screen");
    menu_row(col, ROW_ACTIVITY, LV_SYMBOL_SHUFFLE,    "Activity");
    menu_row_ex(col, ROW_TIME, NULL, ui_icon_clock(), "Time");
    menu_row(col, ROW_DISPLAY, LV_SYMBOL_EYE_OPEN,   "Display");
    menu_row_ex(col, ROW_THEME, NULL, ui_icon_palette(), "Theme");
    menu_row(col, ROW_SOUND,   LV_SYMBOL_VOLUME_MAX, "Sound");
    menu_row(col, ROW_BUTTONS, LV_SYMBOL_KEYBOARD,   "Buttons");
    menu_row(col, ROW_UPDATE,  LV_SYMBOL_DOWNLOAD,   "Firmware update");
    menu_row(col, ROW_ABOUT,   LV_SYMBOL_LIST,       "About");

    lv_obj_t *reset = lv_button_create(col);
    lv_obj_set_size(reset, LV_PCT(100), 42);
    ui_fill(reset, UI_COLOR_SURFACE);
    lv_obj_set_style_radius(reset, 12, 0);
    lv_obj_set_style_shadow_width(reset, 0, 0);
    lv_obj_add_event_cb(reset, factory_reset_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = lv_label_create(reset);
    lv_label_set_text(rl, LV_SYMBOL_TRASH " Factory reset");
    lv_obj_set_style_text_color(rl, UI_COLOR_DANGER, 0);
    lv_obj_center(rl);

    lv_obj_t *off = lv_button_create(col);
    lv_obj_set_size(off, LV_PCT(100), 42);
    ui_fill(off, UI_COLOR_DANGER);
    lv_obj_set_style_radius(off, 12, 0);
    lv_obj_add_event_cb(off, power_off_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_t *lbl = lv_label_create(off);
    lv_label_set_text(lbl, LV_SYMBOL_POWER " Hold to power off");
    lv_obj_center(lbl);

    screen_settings_update();
}

void screen_settings_update(void)
{
    watch_settings_t *cfg = settings_get();

    char ssid[33];
    wifi_mgr_get_ssid(ssid, sizeof(ssid));
    switch (wifi_mgr_get_state()) {
    case WIFI_CONN_CONNECTED:  lv_label_set_text(s_row_hint[ROW_WIFI], ssid);  break;
    case WIFI_CONN_CONNECTING: lv_label_set_text(s_row_hint[ROW_WIFI], "...");  break;
    default:
        lv_label_set_text(s_row_hint[ROW_WIFI], wifi_mgr_is_enabled() ? "" : "Off");
        break;
    }

    if (!ble_mgr_is_enabled()) {
        lv_label_set_text(s_row_hint[ROW_BLUETOOTH], "Off");
    } else if (ble_mgr_get_state() == BLE_STATE_CONNECTED) {
        lv_label_set_text(s_row_hint[ROW_BLUETOOTH], "Connected");
    } else {
        lv_label_set_text(s_row_hint[ROW_BLUETOOTH],
                          ble_mgr_get_mode() == BLE_MODE_WATCH ? "Watch" : "Remote");
    }


    const watchface_t *face = screen_watchface_active();
    lv_label_set_text(s_row_hint[ROW_FACE], face ? face->name : "");
    {
        /* Number of idle pages, watchface included */
        ui_tile_t pages[TILE_SWIPE_COUNT];
        int n = ui_idle_pages(pages), on = 0;
        for (int i = 0; i < n; i++) {
            on += pages[i] == TILE_WATCHFACE || !(cfg->idle_hidden & (1u << pages[i]));
        }
        lv_label_set_text_fmt(s_row_hint[ROW_IDLE], "%d page%s", on, on == 1 ? "" : "s");
    }
    {
        char goal[16];
        ui_format_thousands(goal, sizeof(goal), cfg->step_goal);
        lv_label_set_text(s_row_hint[ROW_ACTIVITY], goal);
    }
    lv_label_set_text(s_row_hint[ROW_THEME], ui_theme_mode_name(cfg->theme));
    int zi = tz_current_index();
    if (zi >= 0) {
        lv_label_set_text(s_row_hint[ROW_TIME], tz_get(zi)->city);
    } else if (cfg->tz_name[0]) {
        const char *city = strrchr(cfg->tz_name, '/');
        lv_label_set_text(s_row_hint[ROW_TIME], city ? city + 1 : cfg->tz_name);
    } else {
        lv_label_set_text(s_row_hint[ROW_TIME], "Custom");
    }
    lv_label_set_text_fmt(s_row_hint[ROW_DISPLAY], "%u%%", cfg->brightness);
    if (cfg->volume) {
        lv_label_set_text_fmt(s_row_hint[ROW_SOUND], "%u%%", cfg->volume);
    } else {
        lv_label_set_text(s_row_hint[ROW_SOUND], "Off");
    }

    int mv = ui_battery_mv();
    if (battery_mv_is_charging(mv)) {
        lv_label_set_text(s_row_hint[ROW_ABOUT], LV_SYMBOL_CHARGE);
    } else {
        lv_label_set_text_fmt(s_row_hint[ROW_ABOUT], "%u%%", battery_percent_from_mv(mv));
    }
}

/* ── Watchface page ──────────────────────────────────────────────────────── */
#define FACE_ROWS_MAX  24

static lv_obj_t *s_face_checks[FACE_ROWS_MAX];
static int       s_face_rows;

static void face_refresh_checks(void)
{
    for (int i = 0; i < s_face_rows; i++) {
        const watchface_t *f = watchface_get(i);
        bool sel = f && strcmp(f->id, settings_get()->face_id) == 0;
        lv_label_set_text(s_face_checks[i], sel ? LV_SYMBOL_OK : "");
    }
}

static void face_cb(lv_event_t *e)
{
    const watchface_t *f = watchface_get((int)(intptr_t)lv_event_get_user_data(e));
    if (!f) return;
    strlcpy(settings_get()->face_id, f->id, sizeof(settings_get()->face_id));
    watch_settings_save();
    screen_watchface_apply_style();
    face_refresh_checks();
    screen_settings_update();
}

/* Upload page: runs the web server while it is open */
static lv_obj_t *s_upload_url;
static lv_obj_t *s_upload_hint;

static void upload_page_update(void)
{
    char ip[16];
    wifi_mgr_get_ip(ip, sizeof(ip));
    if (ip[0] && !face_server_is_running()) face_server_start();

    if (face_server_is_running()) {
        lv_label_set_text_fmt(s_upload_url, "http://%s", ip);
        lv_label_set_text(s_upload_hint, "Open this address in a browser on the same Wi-Fi "
                                         "to add, edit or delete watchfaces.");
    } else {
        lv_label_set_text(s_upload_url, LV_SYMBOL_WIFI "  Not connected");
        lv_label_set_text(s_upload_hint, "Connect to Wi-Fi (Settings > Wi-Fi) to upload watchfaces.");
    }
}

static void upload_page_deleted_cb(lv_event_t *e)
{
    face_server_stop();
}

static void face_upload_cb(lv_event_t *e)
{
    lv_obj_t *c = ui_page_open("Upload faces", upload_page_update);
    lv_obj_add_event_cb(c, upload_page_deleted_cb, LV_EVENT_DELETE, NULL);

    s_upload_url = lv_label_create(c);
    lv_obj_set_style_text_font(s_upload_url, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_upload_url, UI_COLOR_ACCENT, 0);

    s_upload_hint = lv_label_create(c);
    lv_obj_set_width(s_upload_hint, LV_PCT(100));
    lv_label_set_long_mode(s_upload_hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(s_upload_hint, UI_COLOR_DIM, 0);

    lv_obj_t *note = lv_label_create(c);
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(note, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(note, UI_COLOR_DIM, 0);
    lv_label_set_text(note, "The page stays reachable while this screen is open.");

    upload_page_update();
}

static lv_obj_t *face_row(lv_obj_t *parent, const char *name, const char *desc,
                          lv_event_cb_t cb, intptr_t arg)
{
    /* Name on one line, description below (up to two lines); the row grows to fit */
    lv_obj_t *row = lv_button_create(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, 62, 0);
    ui_fill(row, UI_COLOR_SURFACE);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 10, 0);
    lv_obj_set_style_pad_row(row, 2, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, (void *)arg);

    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, name);
    lv_obj_set_width(l, 160);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(l, lv_font_get_line_height(lv_obj_get_style_text_font(l, 0)));   // one line, "…" if long

    lv_obj_t *d = lv_label_create(row);
    lv_label_set_text(d, desc);
    lv_obj_set_width(d, 160);
    lv_label_set_long_mode(d, LV_LABEL_LONG_MODE_DOTS);          // up to two lines, then "…"
    lv_obj_set_style_text_font(d, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(d, UI_COLOR_DIM, 0);
    lv_obj_set_style_max_height(d, 2 * lv_font_get_line_height(&lv_font_montserrat_14), 0);
    return row;
}

/* Delete confirmation for an uploaded face */
static char s_del_id[FACE_ID_MAX];

static void face_delete_confirm_cb(lv_event_t *e)
{
    screen_watchface_delete(s_del_id);
    screen_settings_update();
    page_face();
}

static void face_delete_cancel_cb(lv_event_t *e)
{
    page_face();
}

static void face_delete_cb(lv_event_t *e)
{
    const watchface_t *f = watchface_get((int)(intptr_t)lv_event_get_user_data(e));
    if (!f) return;
    strlcpy(s_del_id, f->id, sizeof(s_del_id));

    lv_obj_t *c = ui_page_open("Delete face", NULL);
    ui_page_set_back(page_face);

    lv_obj_t *msg = lv_label_create(c);
    lv_obj_set_width(msg, LV_PCT(100));
    lv_label_set_long_mode(msg, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text_fmt(msg, "Delete \"%s\" from the watch?", f->name);

    lv_obj_t *btn = lv_button_create(c);
    lv_obj_set_size(btn, LV_PCT(100), 48);
    ui_fill(btn, UI_COLOR_DANGER);
    lv_obj_set_style_radius(btn, 24, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, face_delete_confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, LV_SYMBOL_TRASH "  Delete");
    lv_obj_center(lbl);

    btn = lv_button_create(c);
    lv_obj_set_size(btn, LV_PCT(100), 48);
    ui_fill(btn, UI_COLOR_SURFACE);
    lv_obj_set_style_radius(btn, 24, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, face_delete_cancel_cb, LV_EVENT_CLICKED, NULL);
    lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "Cancel");
    lv_obj_center(lbl);
}

/* ── Idle screen page ────────────────────────────────────────────────────── */
/*   Watchface            (always first)
 *   Activity    [▲][▼]   [on]
 *   Music       [▲][▼]   [on]
 * Pages turned off are left out of the swipe order; they can still be
 * opened from the app menu. */
static lv_obj_t *s_idle_list;

static void idle_build(void);

static void idle_save(const ui_tile_t *pages, int n)
{
    watch_settings_t *cfg = settings_get();
    memset(cfg->idle_order, 0, sizeof(cfg->idle_order));
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (pages[i] == TILE_WATCHFACE) cfg->idle_face_pos = i;
        else if (k < (int)sizeof(cfg->idle_order)) cfg->idle_order[k++] = pages[i];
    }
    watch_settings_save();
    ui_apply_idle_layout();
    screen_settings_update();
}

static void idle_move_cb(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);     // index * 2 + (1 = down)
    int i = v / 2, j = (v & 1) ? i + 1 : i - 1;
    ui_tile_t pages[TILE_SWIPE_COUNT];
    int n = ui_idle_pages(pages);
    if (j < 0 || j >= n) return;
    ui_tile_t t = pages[i];
    pages[i] = pages[j];
    pages[j] = t;
    idle_save(pages, n);
    idle_build();
}

static void idle_switch_cb(lv_event_t *e)
{
    ui_tile_t t = (ui_tile_t)(intptr_t)lv_event_get_user_data(e);
    watch_settings_t *cfg = settings_get();
    if (lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED)) cfg->idle_hidden &= ~(1u << t);
    else                                                            cfg->idle_hidden |= 1u << t;
    ui_tile_t pages[TILE_SWIPE_COUNT];
    idle_save(pages, ui_idle_pages(pages));
    idle_build();
}

static lv_obj_t *idle_row(void)
{
    lv_obj_t *row = lv_obj_create(s_idle_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 48);
    lv_obj_set_style_bg_color(row, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_pad_left(row, 8, 0);
    lv_obj_set_style_pad_right(row, 8, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 2, 0);
    lv_obj_set_scrollable(row, false);
    return row;
}

static void idle_arrow(lv_obj_t *row, const char *sym, int v, bool enabled)
{
    lv_obj_t *b = lv_button_create(row);
    lv_obj_set_size(b, 28, 28);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    ui_fill(b, UI_COLOR_RAISED);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_opa(b, LV_OPA_30, LV_STATE_DISABLED);
    lv_obj_set_state(b, LV_STATE_DISABLED, !enabled);
    lv_obj_add_event_cb(b, idle_move_cb, LV_EVENT_CLICKED, (void *)(intptr_t)v);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, sym);
    lv_obj_center(l);
}

static void idle_list_deleted_cb(lv_event_t *e)
{
    s_idle_list = NULL;
}

static void idle_build(void)
{
    if (!s_idle_list) return;
    lv_obj_clean(s_idle_list);
    const watch_settings_t *cfg = settings_get();

    ui_tile_t pages[TILE_SWIPE_COUNT];
    int n = ui_idle_pages(pages);
    for (int i = 0; i < n; i++) {
        bool face = pages[i] == TILE_WATCHFACE;
        bool on = face || !(cfg->idle_hidden & (1u << pages[i]));
        lv_obj_t *row = idle_row();
        lv_obj_t *l = lv_label_create(row);
        lv_label_set_text(l, ui_tile_name(pages[i]));
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_size(l, 0, lv_font_get_line_height(&lv_font_montserrat_16));   // one line
        lv_obj_set_flex_grow(l, 1);
        if (!on) lv_obj_set_style_text_color(l, UI_COLOR_DIM, 0);
        idle_arrow(row, LV_SYMBOL_UP, i * 2, i > 0);
        idle_arrow(row, LV_SYMBOL_DOWN, i * 2 + 1, i < n - 1);
        if (face) {
            /* Can't be turned off: a home mark where the switch would be */
            lv_obj_t *home = lv_label_create(row);
            lv_label_set_text(home, LV_SYMBOL_HOME);
            lv_obj_set_width(home, 42);
            lv_obj_set_style_text_align(home, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_color(home, UI_COLOR_ACCENT, 0);
            continue;
        }
        lv_obj_t *sw = lv_switch_create(row);
        lv_obj_set_size(sw, 40, 22);
        lv_obj_set_style_margin_left(sw, 2, 0);
        lv_obj_set_state(sw, LV_STATE_CHECKED, on);
        lv_obj_add_event_cb(sw, idle_switch_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)pages[i]);
    }
}

static void page_idle(void)
{
    lv_obj_t *c = ui_page_open("Idle screen", NULL);

    s_idle_list = lv_obj_create(c);
    lv_obj_remove_style_all(s_idle_list);
    lv_obj_set_size(s_idle_list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_idle_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_idle_list, 8, 0);
    lv_obj_set_clickable(s_idle_list, false);
    lv_obj_add_event_cb(s_idle_list, idle_list_deleted_cb, LV_EVENT_DELETE, NULL);
    idle_build();

    lv_obj_t *note = lv_label_create(c);
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(note, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(note, UI_COLOR_DIM, 0);
    lv_label_set_text(note, LV_SYMBOL_HOME " The watchface is the default screen: PWR and "
                            "waking the watch always go back to it. Swipe left / right to "
                            "go through the pages in this order. Pages turned off can "
                            "still be opened from the app menu.");
}

static void page_face(void)
{
    lv_obj_t *c = ui_page_open("Watchface", NULL);

    s_face_rows = watchface_count() < FACE_ROWS_MAX ? watchface_count() : FACE_ROWS_MAX;
    for (int i = 0; i < s_face_rows; i++) {
        const watchface_t *f = watchface_get(i);
        lv_obj_t *row = face_row(c, f->name, f->desc, face_cb, i);
        s_face_checks[i] = lv_label_create(row);
        lv_obj_set_style_text_color(s_face_checks[i], UI_COLOR_ACCENT, 0);
        lv_obj_set_floating(s_face_checks[i], true);   // outside the row's flex
        lv_obj_align(s_face_checks[i], LV_ALIGN_RIGHT_MID, 0, 0);

        if (watchface_is_custom(f)) {
            /* Narrower text; trash button on the right, tick left of it */
            lv_obj_set_width(lv_obj_get_child(row, 0), 118);
            lv_obj_set_width(lv_obj_get_child(row, 1), 118);
            lv_obj_align(s_face_checks[i], LV_ALIGN_RIGHT_MID, -42, 0);
            lv_obj_t *del = lv_button_create(row);
            lv_obj_set_floating(del, true);
            lv_obj_set_size(del, 36, 36);
            lv_obj_align(del, LV_ALIGN_RIGHT_MID, 0, 0);
            lv_obj_set_style_radius(del, LV_RADIUS_CIRCLE, 0);
            ui_fill(del, UI_COLOR_RAISED);
            lv_obj_set_style_shadow_width(del, 0, 0);
            lv_obj_add_event_cb(del, face_delete_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            lv_obj_t *ic = lv_label_create(del);
            lv_label_set_text(ic, LV_SYMBOL_TRASH);
            lv_obj_set_style_text_color(ic, UI_COLOR_DANGER, 0);
            lv_obj_center(ic);
        }
    }
    face_refresh_checks();

    lv_obj_t *up = face_row(c, LV_SYMBOL_UPLOAD "  Upload faces", "Add your own over Wi-Fi",
                            face_upload_cb, 0);
    lv_obj_set_style_bg_color(up, UI_COLOR_RAISED, 0);

    lv_obj_t *hint = lv_label_create(c);
    lv_label_set_text(hint, "Swipe right to go back");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, UI_COLOR_DIM, 0);
}

/* ── Time page ───────────────────────────────────────────────────────────── */
static lv_obj_t *s_t_clock;
static lv_obj_t *s_t_date;
static lv_obj_t *s_t_sync_status;
static lv_obj_t *s_t_sync_btn;
static lv_obj_t *s_t_zone_dd;
static lv_obj_t *s_t_zone_note;
static char      s_t_zone_text[40];  // shown on the closed dropdown

/* The dropdown always shows the current zone, which may be one not in the
 * list (auto-detected elsewhere), so its text is set explicitly. */
static void zone_refresh(void)
{
    const watch_settings_t *cfg = settings_get();
    char name[40];
    tz_current_name(name, sizeof(name));
    if (strcmp(name, s_t_zone_text) != 0) {
        strlcpy(s_t_zone_text, name, sizeof(s_t_zone_text));
        lv_dropdown_set_text(s_t_zone_dd, s_t_zone_text);
    }
    int zi = tz_current_index();
    if (zi >= 0 && !lv_dropdown_is_open(s_t_zone_dd)) lv_dropdown_set_selected(s_t_zone_dd, zi);

    if (cfg->auto_tz) {
        lv_obj_add_state(s_t_zone_dd, LV_STATE_DISABLED);
        lv_label_set_text_fmt(s_t_zone_note, "Set from your internet connection%s%s",
                              cfg->tz_name[0] ? "\n" : "", cfg->tz_name);
    } else {
        lv_obj_remove_state(s_t_zone_dd, LV_STATE_DISABLED);
        lv_label_set_text(s_t_zone_note, "Tap to choose");
    }
}

static void time_page_update(void)
{
    if (rtc_time_valid()) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        if (settings_get()->use_24h) {
            lv_label_set_text_fmt(s_t_clock, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
        } else {
            int h = tm.tm_hour % 12;
            lv_label_set_text_fmt(s_t_clock, "%d:%02d:%02d %s", h ? h : 12, tm.tm_min, tm.tm_sec,
                                  tm.tm_hour < 12 ? "AM" : "PM");
        }
        char buf[40];
        strftime(buf, sizeof(buf), "%a %d %b %Y  %Z", &tm);
        lv_label_set_text(s_t_date, buf);
    } else {
        lv_label_set_text(s_t_clock, "--:--:--");
        lv_label_set_text(s_t_date, "Time not set");
    }
    zone_refresh();

    switch (time_sync_get_status()) {
    case TIME_SYNC_DISABLED:
        lv_label_set_text(s_t_sync_status, wifi_mgr_is_enabled()
                          ? "No network saved.\nUse the Wi-Fi tab."
                          : "Wi-Fi is off.\nTurn it on in Wi-Fi.");
        lv_obj_add_state(s_t_sync_btn, LV_STATE_DISABLED);
        break;
    case TIME_SYNC_RUNNING:
        lv_label_set_text(s_t_sync_status, "Syncing...");
        lv_obj_add_state(s_t_sync_btn, LV_STATE_DISABLED);
        break;
    default: {
        time_t last = time_sync_last_success();
        if (last) {
            struct tm tm;
            localtime_r(&last, &tm);
            lv_label_set_text_fmt(s_t_sync_status, "%s, last %02d:%02d",
                                  time_sync_get_status() == TIME_SYNC_FAILED ? "Failed" : "OK",
                                  tm.tm_hour, tm.tm_min);
        } else {
            lv_label_set_text(s_t_sync_status, time_sync_get_status() == TIME_SYNC_FAILED
                                               ? "Failed. Check Wi-Fi." : "Not synced yet");
        }
        lv_obj_remove_state(s_t_sync_btn, LV_STATE_DISABLED);
        break;
    }
    }
}

static void h24_cb(lv_event_t *e)
{
    settings_get()->use_24h = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    watch_settings_save();
    time_page_update();
    screen_watchface_update();
}

static void zone_cb(lv_event_t *e)
{
    tz_select((int)lv_dropdown_get_selected(lv_event_get_target_obj(e)));
    time_page_update();
    screen_watchface_update();
    screen_settings_update();
}

static void auto_tz_cb(lv_event_t *e)
{
    settings_get()->auto_tz = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    watch_settings_save();
    if (settings_get()->auto_tz) time_sync_request();   // looks the timezone up too
    time_page_update();
}

static void sync_cb(lv_event_t *e)
{
    time_sync_request();
    time_page_update();
}

static void page_time(void)
{
    lv_obj_t *c = ui_page_open("Time", time_page_update);

    /* Live preview */
    s_t_clock = lv_label_create(c);
    lv_obj_set_style_text_font(s_t_clock, &lv_font_montserrat_24, 0);
    s_t_date = lv_label_create(c);
    lv_obj_set_style_text_font(s_t_date, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_t_date, UI_COLOR_DIM, 0);

    /* 24-hour switch */
    lv_obj_t *card = ui_card(c, NULL);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *lbl = lv_label_create(card);
    lv_label_set_text(lbl, "24-hour clock");
    lv_obj_t *sw = lv_switch_create(card);
    if (settings_get()->use_24h) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, h24_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Timezone: automatic switch, and a dropdown to pick one by hand */
    card = ui_card(c, LV_SYMBOL_GPS "  Timezone");
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lbl = lv_label_create(row);
    lv_label_set_text(lbl, "Automatic");
    sw = lv_switch_create(row);
    if (settings_get()->auto_tz) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, auto_tz_cb, LV_EVENT_VALUE_CHANGED, NULL);

    static char opts[48 * 28];
    opts[0] = 0;
    for (int i = 0; i < tz_count(); i++) {
        if (i) strlcat(opts, "\n", sizeof(opts));
        strlcat(opts, tz_get(i)->name, sizeof(opts));
    }
    s_t_zone_dd = lv_dropdown_create(card);
    lv_dropdown_set_options(s_t_zone_dd, opts);
    lv_obj_set_width(s_t_zone_dd, LV_PCT(100));
    lv_obj_set_style_max_height(lv_dropdown_get_list(s_t_zone_dd), 200, 0);
    lv_obj_add_event_cb(s_t_zone_dd, zone_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_t_zone_text[0] = 0;

    s_t_zone_note = lv_label_create(card);
    lv_obj_set_width(s_t_zone_note, LV_PCT(100));
    lv_label_set_long_mode(s_t_zone_note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(s_t_zone_note, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_t_zone_note, UI_COLOR_DIM, 0);

    /* NTP sync */
    card = ui_card(c, LV_SYMBOL_WIFI "  Internet time");
    s_t_sync_status = lv_label_create(card);
    lv_obj_set_style_text_font(s_t_sync_status, &lv_font_montserrat_14, 0);
    s_t_sync_btn = lv_button_create(card);
    lv_obj_set_width(s_t_sync_btn, LV_PCT(100));
    lv_obj_add_event_cb(s_t_sync_btn, sync_cb, LV_EVENT_CLICKED, NULL);
    lbl = lv_label_create(s_t_sync_btn);
    lv_label_set_text(lbl, LV_SYMBOL_REFRESH " Sync now");
    lv_obj_center(lbl);

    time_page_update();
}

/* ── Display page ────────────────────────────────────────────────────────── */
static lv_obj_t *slider(lv_obj_t *parent, int32_t min, int32_t max, int32_t val, lv_event_cb_t cb)
{
    lv_obj_t *s = lv_slider_create(parent);
    lv_obj_set_width(s, LV_PCT(92));
    lv_slider_set_range(s, min, max);
    lv_slider_set_value(s, val, LV_ANIM_OFF);
    lv_obj_set_style_margin_hor(s, 8, 0);
    lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s, cb, LV_EVENT_RELEASED, NULL);
    return s;
}

static void brightness_cb(lv_event_t *e)
{
    settings_get()->brightness = (uint8_t)lv_slider_get_value(lv_event_get_target_obj(e));
    display_set_backlight(settings_get()->brightness);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        watch_settings_save();
        screen_settings_update();
    }
}

static void timeout_cb(lv_event_t *e)
{
    uint32_t idx = lv_dropdown_get_selected(lv_event_get_target_obj(e));
    if (idx < sizeof(s_timeouts) / sizeof(s_timeouts[0])) {
        settings_get()->timeout_s = s_timeouts[idx];
        watch_settings_save();
    }
}

static void raise_cb(lv_event_t *e)
{
    bool on = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    settings_get()->raise_to_wake = on;
    activity_set_raise_enabled(on);
    watch_settings_save();
}

static void aod_cb(lv_event_t *e)
{
    settings_get()->aod = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    watch_settings_save();
}

/* 60 fps switch: the frame rate changes at once; the draw buffers that go with
 * it are sized at boot, so the RAM change waits for a restart */
static lv_obj_t *s_fps_hint;
static lv_obj_t *s_fps_restart;

static void fps_refresh(void)
{
    bool fps30 = settings_get()->fps30;
    bool pending = fps30 != display_buffers_fps30();
    lv_label_set_text(s_fps_hint,
        !pending ? (fps30 ? "30 fps. Saves about 15 KB of RAM; swipes are less smooth."
                          : "Smoother swipes and animations. Uses about 15 KB more RAM.")
                 : (fps30 ? "Now 30 fps. Restart to free the 15 KB of RAM."
                          : "Now 60 fps. Restart to use the larger draw buffers (about 15 KB more RAM)."));
    lv_obj_set_hidden(s_fps_restart, !pending);
}

static void fps_cb(lv_event_t *e)
{
    settings_get()->fps30 = !lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    watch_settings_save();
    display_set_fps30(settings_get()->fps30);
    fps_refresh();
}

static void fps_restart_cb(lv_event_t *e)
{
    ui_request_restart();
}

/* ── Raise-to-wake calibration ───────────────────────────────────────────
 * Two poses, each held still for a moment: arm hanging down, then looking
 * at the watch.  Beeps mark the start and end of each capture so the
 * screen needn't be watched with the arm down. */
#define CAL_COUNTDOWN_MS  3000
#define CAL_SAMPLE_MS     1500
#define CAL_TICK_MS       100
#define CAL_MAX_RANGE_G   0.35f       // per-axis movement allowed while capturing
#define CAL_MIN_ANGLE_COS 0.77f       // poses must differ by at least ~40°

typedef enum { CAL_IDLE, CAL_COUNT, CAL_SAMPLE, CAL_DONE } cal_phase_t;

static lv_timer_t *s_cal_timer;
static lv_obj_t   *s_cal_step, *s_cal_text, *s_cal_big, *s_cal_btn, *s_cal_btn_lbl;
static cal_phase_t s_cal_phase;
static int         s_cal_pose;                 // 0 = arm down, 1 = looking
static uint32_t    s_cal_elapsed;
static float       s_cal_sum[3], s_cal_min[3], s_cal_max[3];
static int         s_cal_n;
static float       s_cal_pose_g[2][3];
static bool        s_cal_ok;

static const char *const s_cal_prompt[2] = {
    "Let your arm hang down by your side and keep still.",
    "Now raise the watch and look at it, as you normally would.",
};

static lv_obj_t *small_button(lv_obj_t *parent, const char *text, lv_color_t bg, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, LV_PCT(100), 40);
    ui_fill(btn, bg);
    lv_obj_set_style_radius(btn, 20, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    return btn;
}

static void apply_raise_calibration(void)
{
    watch_settings_t *cfg = settings_get();
    if (!cfg->raise_cal) {
        activity_set_raise_calibration(NULL, NULL);
        return;
    }
    float up[3], down[3];
    for (int i = 0; i < 3; i++) {
        up[i]   = cfg->raise_up[i] / 1000.0f;
        down[i] = cfg->raise_down[i] / 1000.0f;
    }
    activity_set_raise_calibration(up, down);
}

static void cal_show_pose(void)
{
    lv_label_set_text_fmt(s_cal_step, "Step %d of 2", s_cal_pose + 1);
    lv_label_set_text(s_cal_text, s_cal_prompt[s_cal_pose]);
}

static void cal_start_pose(int pose)
{
    s_cal_pose = pose;
    s_cal_phase = CAL_COUNT;
    s_cal_elapsed = 0;
    cal_show_pose();
    lv_obj_set_hidden(s_cal_btn, true);
}

static void cal_finish(const char *error)
{
    s_cal_phase = CAL_DONE;
    s_cal_ok = error == NULL;
    lv_obj_set_hidden(s_cal_btn, false);
    if (error) {
        lv_label_set_text(s_cal_step, "Didn't work");
        lv_label_set_text(s_cal_text, error);
        lv_label_set_text(s_cal_big, "");
        lv_label_set_text(s_cal_btn_lbl, "Try again");
        audio_beep(400, 300, 0, 1);
        return;
    }
    watch_settings_t *cfg = settings_get();
    for (int i = 0; i < 3; i++) {
        cfg->raise_down[i] = (int16_t)lroundf(s_cal_pose_g[0][i] * 1000);
        cfg->raise_up[i]   = (int16_t)lroundf(s_cal_pose_g[1][i] * 1000);
    }
    cfg->raise_cal = true;
    watch_settings_save();
    apply_raise_calibration();
    lv_label_set_text(s_cal_step, "Done");
    lv_label_set_text(s_cal_text, "Raise to wake now uses your arm positions.");
    lv_label_set_text(s_cal_big, LV_SYMBOL_OK);
    lv_label_set_text(s_cal_btn_lbl, "Done");
    audio_beep(1500, 80, 80, 2);
}

/* Check one captured pose; returns an error message or NULL */
static const char *cal_take_pose(void)
{
    if (s_cal_n == 0) return "No sensor readings.";
    float *g = s_cal_pose_g[s_cal_pose];
    for (int i = 0; i < 3; i++) {
        g[i] = s_cal_sum[i] / s_cal_n;
        if (s_cal_max[i] - s_cal_min[i] > CAL_MAX_RANGE_G) return "The watch moved too much. Hold still while it beeps.";
    }
    float m = sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
    if (m < 0.7f || m > 1.3f) return "The watch moved too much. Hold still while it beeps.";
    return NULL;
}

static void cal_timer_cb(lv_timer_t *t)
{
    if (s_cal_phase == CAL_COUNT) {
        s_cal_elapsed += CAL_TICK_MS;
        uint32_t left = (CAL_COUNTDOWN_MS - s_cal_elapsed + 999) / 1000;
        lv_label_set_text_fmt(s_cal_big, "%lu", (unsigned long)left);
        if (s_cal_elapsed >= CAL_COUNTDOWN_MS) {
            s_cal_phase = CAL_SAMPLE;
            s_cal_elapsed = 0;
            s_cal_n = 0;
            for (int i = 0; i < 3; i++) { s_cal_sum[i] = 0; s_cal_min[i] = 9; s_cal_max[i] = -9; }
            lv_label_set_text(s_cal_big, "Hold still");
            audio_beep(1000, 80, 0, 1);
        }
    } else if (s_cal_phase == CAL_SAMPLE) {
        activity_debug_t d;
        activity_get_debug(&d);
        const float v[3] = { d.x, d.y, d.z };
        for (int i = 0; i < 3; i++) {
            s_cal_sum[i] += v[i];
            if (v[i] < s_cal_min[i]) s_cal_min[i] = v[i];
            if (v[i] > s_cal_max[i]) s_cal_max[i] = v[i];
        }
        s_cal_n++;
        s_cal_elapsed += CAL_TICK_MS;
        if (s_cal_elapsed < CAL_SAMPLE_MS) return;

        const char *err = cal_take_pose();
        if (err) {
            cal_finish(err);
        } else if (s_cal_pose == 0) {
            audio_beep(1500, 80, 0, 1);
            cal_start_pose(1);
        } else {
            const float *a = s_cal_pose_g[0], *b = s_cal_pose_g[1];
            float cosang = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) /
                           (sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]) *
                            sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]));
            cal_finish(cosang > CAL_MIN_ANGLE_COS
                       ? "The two positions were too similar. Let the arm hang straight down, then turn the screen towards you."
                       : NULL);
        }
    }
}

static void cal_page_deleted_cb(lv_event_t *e)
{
    if (s_cal_timer) lv_timer_delete(s_cal_timer);
    s_cal_timer = NULL;
}

static void page_display(void);

static void cal_btn_cb(lv_event_t *e)
{
    if (s_cal_phase == CAL_DONE && s_cal_ok) {
        page_display();
    } else {
        cal_start_pose(0);
    }
}

static void raise_cal_open_cb(lv_event_t *e)
{
    lv_obj_t *c = ui_page_open("Calibrate", NULL);
    ui_page_set_back(page_display);
    lv_obj_add_event_cb(c, cal_page_deleted_cb, LV_EVENT_DELETE, NULL);

    s_cal_step = lv_label_create(c);
    lv_obj_set_style_text_color(s_cal_step, UI_COLOR_DIM, 0);
    lv_label_set_text(s_cal_step, "Raise to wake");

    s_cal_text = lv_label_create(c);
    lv_obj_set_width(s_cal_text, LV_PCT(100));
    lv_label_set_long_mode(s_cal_text, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(s_cal_text, "Wear the watch. You'll hold two positions, each "
                                  "for a moment. A beep starts and ends each one.");

    s_cal_big = lv_label_create(c);
    lv_obj_set_style_text_font(s_cal_big, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(s_cal_big, UI_COLOR_ACCENT, 0);
    lv_label_set_text(s_cal_big, "");

    s_cal_btn = lv_button_create(c);
    lv_obj_set_size(s_cal_btn, LV_PCT(100), 48);
    ui_fill(s_cal_btn, UI_COLOR_ACCENT);
    lv_obj_set_style_radius(s_cal_btn, 24, 0);
    lv_obj_set_style_shadow_width(s_cal_btn, 0, 0);
    lv_obj_add_event_cb(s_cal_btn, cal_btn_cb, LV_EVENT_CLICKED, NULL);
    s_cal_btn_lbl = lv_label_create(s_cal_btn);
    lv_obj_set_style_text_color(s_cal_btn_lbl, ui_contrast(UI_COLOR_ACCENT), 0);
    lv_label_set_text(s_cal_btn_lbl, "Start");
    lv_obj_center(s_cal_btn_lbl);

    s_cal_phase = CAL_IDLE;
    s_cal_ok = false;
    s_cal_timer = lv_timer_create(cal_timer_cb, CAL_TICK_MS, NULL);
}

static void raise_cal_reset_cb(lv_event_t *e)
{
    settings_get()->raise_cal = false;
    watch_settings_save();
    apply_raise_calibration();
    page_display();             // refresh the card
}

static void page_display(void)
{
    watch_settings_t *cfg = settings_get();
    lv_obj_t *c = ui_page_open("Display", NULL);

    /* Title with the quick panel's sun icon, scaled to the 14 px title text */
    lv_obj_t *card = ui_card(c, NULL);
    lv_obj_t *title = lv_obj_create(card);
    lv_obj_remove_style_all(title);
    lv_obj_set_size(title, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(title, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(title, 9, 0);
    lv_obj_set_clickable(title, false);
    lv_obj_t *sun = ui_icon_create(title, ui_icon_sun(), UI_COLOR_DIM);
    lv_image_set_scale(sun, LV_SCALE_NONE * 16 / 22);
    lv_obj_set_size(sun, 16, 16);
    lv_image_set_inner_align(sun, LV_IMAGE_ALIGN_CENTER);
    lv_obj_t *tl = lv_label_create(title);
    lv_label_set_text(tl, "Brightness");
    lv_obj_set_style_text_color(tl, UI_COLOR_DIM, 0);
    lv_obj_set_style_text_font(tl, &lv_font_montserrat_14, 0);
    slider(card, 5, 100, cfg->brightness, brightness_cb);

    card = ui_card(c, LV_SYMBOL_EYE_CLOSE "  Screen timeout");
    lv_obj_t *dd = lv_dropdown_create(card);
    lv_dropdown_set_options_static(dd, TIMEOUT_OPTIONS);
    lv_obj_set_width(dd, LV_PCT(100));
    for (uint32_t i = 0; i < sizeof(s_timeouts) / sizeof(s_timeouts[0]); i++) {
        if (s_timeouts[i] == cfg->timeout_s) lv_dropdown_set_selected(dd, i);
    }
    lv_obj_add_event_cb(dd, timeout_cb, LV_EVENT_VALUE_CHANGED, NULL);

    card = ui_card(c, NULL);
    lv_obj_t *rrow = lv_obj_create(card);
    lv_obj_remove_style_all(rrow);
    lv_obj_set_size(rrow, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(rrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rrow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *lbl = lv_label_create(rrow);
    lv_label_set_text(lbl, "Raise to wake");
    lv_obj_t *sw = lv_switch_create(rrow);
    if (cfg->raise_to_wake) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, raise_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *cal_state = lv_label_create(card);
    lv_obj_set_style_text_font(cal_state, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cal_state, UI_COLOR_DIM, 0);
    lv_label_set_text(cal_state, cfg->raise_cal ? "Calibrated for your wrist" : "Not calibrated (default)");
    small_button(card, LV_SYMBOL_REFRESH "  Calibrate", UI_COLOR_RAISED, raise_cal_open_cb);
    if (cfg->raise_cal) {
        small_button(card, "Use default", UI_COLOR_SURFACE, raise_cal_reset_cb);
    }

    card = ui_card(c, NULL);
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    lbl = lv_label_create(row);
    lv_label_set_text(lbl, "Always-on display");
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_WRAP);   // wraps rather than running into the switch
    lv_obj_set_width(lbl, 0);
    lv_obj_set_flex_grow(lbl, 1);
    sw = lv_switch_create(row);
    if (cfg->aod) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, aod_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *hint = lv_label_create(card);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, UI_COLOR_DIM, 0);
    lv_label_set_text(hint, "Keeps the watchface on, dimmed, instead of turning the screen off. Uses more battery.");

    card = ui_card(c, NULL);
    row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    lbl = lv_label_create(row);
    lv_label_set_text(lbl, "60 fps");
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_WRAP);   // wraps rather than running into the switch
    lv_obj_set_width(lbl, 0);
    lv_obj_set_flex_grow(lbl, 1);
    sw = lv_switch_create(row);
    if (!cfg->fps30) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, fps_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_fps_hint = lv_label_create(card);
    lv_obj_set_width(s_fps_hint, LV_PCT(100));
    lv_label_set_long_mode(s_fps_hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(s_fps_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_fps_hint, UI_COLOR_DIM, 0);
    s_fps_restart = small_button(card, "Restart now", UI_COLOR_SURFACE, fps_restart_cb);
    fps_refresh();
}

/* ── Sound page ──────────────────────────────────────────────────────────── */
static void volume_cb(lv_event_t *e)
{
    settings_get()->volume = (uint8_t)lv_slider_get_value(lv_event_get_target_obj(e));
    audio_set_volume(settings_get()->volume);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        watch_settings_save();
        screen_settings_update();
        audio_beep(1000, 80, 0, 1);    // preview
    }
}

static void test_sound_cb(lv_event_t *e)
{
    audio_beep(2000, 120, 120, 3);     // same pattern as the timer alarm
}

/* ── Activity: daily step goal ──────────────────────────────────────────── */
static lv_obj_t *s_goal_lbl;

static void goal_show(void)
{
    char buf[16];
    ui_format_thousands(buf, sizeof(buf), settings_get()->step_goal);
    lv_label_set_text(s_goal_lbl, buf);
}

/* − / +: a tap moves 500 steps, holding keeps going; saved on release */
static void goal_cb(lv_event_t *e)
{
    watch_settings_t *cfg = settings_get();
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RELEASED) {
        watch_settings_save();
        screen_settings_update();
        return;
    }
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    int goal = cfg->step_goal + dir * STEP_GOAL_STEP;
    if (goal < STEP_GOAL_MIN) goal = STEP_GOAL_MIN;
    if (goal > STEP_GOAL_MAX) goal = STEP_GOAL_MAX;
    if (goal == cfg->step_goal) return;
    cfg->step_goal = (uint16_t)goal;
    goal_show();
    if (code == LV_EVENT_SHORT_CLICKED) {
        watch_settings_save();
        screen_settings_update();
    }
}

static lv_obj_t *goal_button(lv_obj_t *parent, const char *sym, int dir)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 44, 44);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    ui_fill(btn, UI_COLOR_RAISED);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, goal_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)dir);
    lv_obj_add_event_cb(btn, goal_cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)dir);
    lv_obj_add_event_cb(btn, goal_cb, LV_EVENT_RELEASED, (void *)(intptr_t)dir);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, sym);
    lv_obj_center(l);
    return btn;
}

static void page_activity(void)
{
    lv_obj_t *c = ui_page_open("Activity", NULL);

    lv_obj_t *card = ui_card(c, "Daily step goal");
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_clickable(row, false);
    goal_button(row, LV_SYMBOL_MINUS, -1);
    s_goal_lbl = lv_label_create(row);
    lv_obj_set_style_text_font(s_goal_lbl, &lv_font_montserrat_24, 0);
    goal_button(row, LV_SYMBOL_PLUS, +1);
    goal_show();

    lv_obj_t *hint = lv_label_create(card);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, UI_COLOR_DIM, 0);
    lv_label_set_text(hint, "Steps a day to fill the Activity ring. Tap - / + to change "
                            "it by 500, or hold to go faster.");
}

/* ── Theme ───────────────────────────────────────────────────────────────── */
/* Choices are held here until Apply: the theme is set up at start-up, so
 * applying one saves it and restarts the watch. */
static uint8_t   s_th_mode, s_th_bg;
static uint32_t  s_th_accent;
static bool      s_th_apps;
static lv_obj_t *s_th_prev, *s_th_prev_time, *s_th_prev_arc, *s_th_prev_btn, *s_th_prev_dim;
static lv_obj_t *s_th_mode_btn[THEME_COUNT];
static lv_obj_t *s_th_bg_btn[THEME_BG_COUNT];
static lv_obj_t *s_th_accent_card;
static lv_obj_t *s_th_swatch[THEME_SWATCH_COUNT];
static lv_obj_t *s_th_apply;

static void th_refresh(void)
{
    ui_theme_t t;
    ui_theme_build(&t, s_th_mode, s_th_bg, s_th_accent, s_th_apps);

    lv_obj_set_style_bg_color(s_th_prev, t.bg, 0);
    lv_obj_set_style_border_color(s_th_prev, t.surface2, 0);
    lv_obj_set_style_text_color(s_th_prev_time, t.text, 0);
    lv_obj_set_style_text_color(s_th_prev_dim, t.dim, 0);
    lv_obj_set_style_arc_color(s_th_prev_arc, t.accent_dim, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_th_prev_arc, t.accent, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_th_prev_btn, t.accent, 0);
    lv_obj_set_style_text_color(s_th_prev_btn, ui_contrast(t.accent), 0);

    for (int i = 0; i < THEME_COUNT; i++) {
        lv_obj_set_style_border_width(s_th_mode_btn[i], i == s_th_mode ? 2 : 0, 0);
    }
    for (int i = 0; i < THEME_BG_COUNT; i++) {
        lv_obj_set_style_border_width(s_th_bg_btn[i], i == s_th_bg ? 2 : 0, 0);
    }
    lv_obj_set_hidden(s_th_accent_card, s_th_mode != THEME_COLOUR);
    uint32_t rgb = ui_theme_accent_rgb(s_th_accent);
    for (int i = 0; i < THEME_SWATCH_COUNT; i++) {
        lv_obj_set_style_border_width(s_th_swatch[i], theme_swatches[i] == rgb ? 3 : 0, 0);
    }

    const watch_settings_t *cfg = settings_get();
    bool changed = s_th_mode != cfg->theme || s_th_bg != cfg->theme_bg ||
                   s_th_accent != cfg->theme_accent || s_th_apps != cfg->app_colors;
    lv_obj_set_hidden(s_th_apply, !changed);
}

static void th_mode_cb(lv_event_t *e)
{
    s_th_mode = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    th_refresh();
}

static void th_bg_cb(lv_event_t *e)
{
    s_th_bg = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    th_refresh();
}

static void th_swatch_cb(lv_event_t *e)
{
    s_th_accent = THEME_ACCENT_SET | theme_swatches[(int)(uintptr_t)lv_event_get_user_data(e)];
    th_refresh();
}

static void th_apps_cb(lv_event_t *e)
{
    s_th_apps = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    th_refresh();
}

static void th_apply_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    watch_settings_t *cfg = settings_get();
    cfg->theme        = s_th_mode;
    cfg->theme_bg     = s_th_bg;
    cfg->theme_accent = s_th_accent;
    cfg->app_colors   = s_th_apps;
    watch_settings_save();
    ui_request_restart();
}

static lv_obj_t *th_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return row;
}

/* One of a row of equal-width choices, outlined when selected */
static lv_obj_t *th_choice(lv_obj_t *row, const char *text, lv_color_t bg, lv_event_cb_t cb, int i)
{
    lv_obj_t *b = lv_obj_create(row);
    lv_obj_remove_style_all(b);
    lv_obj_set_height(b, 36);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_border_color(b, UI_COLOR_TEXT, 0);
    lv_obj_set_style_outline_width(b, 1, 0);
    lv_obj_set_style_outline_color(b, UI_COLOR_FAINT, 0);
    lv_obj_set_style_outline_pad(b, -1, 0);
    lv_obj_set_clickable(b, true);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, UI_COLOR_TEXT, 0);
    lv_obj_center(l);
    return b;
}

static lv_obj_t *th_hint(lv_obj_t *parent, const char *text)
{
    lv_obj_t *hint = lv_label_create(parent);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, UI_COLOR_DIM, 0);
    lv_label_set_text(hint, text);
    return hint;
}

static void page_theme(void)
{
    const watch_settings_t *cfg = settings_get();
    s_th_mode   = cfg->theme;
    s_th_bg     = cfg->theme_bg;
    s_th_accent = cfg->theme_accent;
    s_th_apps   = cfg->app_colors;

    lv_obj_t *c = ui_page_open("Theme", NULL);

    /* Preview: a little watch screen in the chosen colours */
    s_th_prev = lv_obj_create(c);
    lv_obj_remove_style_all(s_th_prev);
    lv_obj_set_size(s_th_prev, LV_PCT(100), 86);
    lv_obj_set_style_bg_opa(s_th_prev, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_th_prev, 16, 0);
    lv_obj_set_style_border_width(s_th_prev, 1, 0);
    lv_obj_set_style_pad_all(s_th_prev, 10, 0);
    lv_obj_set_scrollable(s_th_prev, false);
    lv_obj_set_clickable(s_th_prev, false);

    s_th_prev_arc = lv_arc_create(s_th_prev);
    lv_obj_set_size(s_th_prev_arc, 60, 60);
    lv_obj_align(s_th_prev_arc, LV_ALIGN_LEFT_MID, 0, 0);
    lv_arc_set_bg_angles(s_th_prev_arc, 0, 360);
    lv_arc_set_rotation(s_th_prev_arc, 270);
    lv_arc_set_range(s_th_prev_arc, 0, 100);
    lv_arc_set_value(s_th_prev_arc, 65);
    lv_obj_set_style_arc_width(s_th_prev_arc, 7, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_th_prev_arc, 7, LV_PART_INDICATOR);
    lv_obj_remove_style(s_th_prev_arc, NULL, LV_PART_KNOB);
    lv_obj_set_clickable(s_th_prev_arc, false);

    s_th_prev_time = lv_label_create(s_th_prev);
    lv_label_set_text(s_th_prev_time, "10:09");
    lv_obj_set_style_text_font(s_th_prev_time, &lv_font_montserrat_24, 0);
    lv_obj_align(s_th_prev_time, LV_ALIGN_TOP_LEFT, 70, -2);

    s_th_prev_dim = lv_label_create(s_th_prev);
    lv_label_set_text(s_th_prev_dim, "6,512 steps");
    lv_obj_set_style_text_font(s_th_prev_dim, &lv_font_montserrat_14, 0);
    lv_obj_align(s_th_prev_dim, LV_ALIGN_TOP_LEFT, 70, 26);

    s_th_prev_btn = lv_obj_create(s_th_prev);
    lv_obj_remove_style_all(s_th_prev_btn);
    lv_obj_set_size(s_th_prev_btn, 66, 22);
    lv_obj_set_style_bg_opa(s_th_prev_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_th_prev_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(s_th_prev_btn, LV_ALIGN_BOTTOM_LEFT, 70, 2);
    lv_obj_t *bl = lv_label_create(s_th_prev_btn);
    lv_label_set_text(bl, "Start");
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_14, 0);
    lv_obj_center(bl);

    /* Monochrome or Colour */
    lv_obj_t *card = ui_card(c, "Colours");
    lv_obj_t *row = th_row(card);
    lv_obj_set_style_pad_column(row, 8, 0);
    for (int i = 0; i < THEME_COUNT; i++) {
        s_th_mode_btn[i] = th_choice(row, ui_theme_mode_name(i), UI_COLOR_SURFACE2, th_mode_cb, i);
    }

    /* Background */
    card = ui_card(c, "Background");
    row = th_row(card);
    lv_obj_set_style_pad_column(row, 8, 0);
    for (int i = 0; i < THEME_BG_COUNT; i++) {
        ui_theme_t t;
        ui_theme_build(&t, THEME_MONO, i, 0, false);
        s_th_bg_btn[i] = th_choice(row, ui_theme_bg_name(i), t.bg, th_bg_cb, i);
    }

    /* Accent colour */
    card = ui_card(c, "Accent colour");
    s_th_accent_card = card;
    lv_obj_t *grid = th_row(card);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(grid, 8, 0);
    lv_obj_set_style_pad_column(grid, 6, 0);
    for (int i = 0; i < THEME_SWATCH_COUNT; i++) {
        lv_obj_t *s = lv_obj_create(grid);
        lv_obj_remove_style_all(s);
        lv_obj_set_size(s, 32, 32);
        lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(s, UI_COLOR_TEXT, 0);
        lv_obj_set_style_outline_width(s, 1, 0);
        lv_obj_set_style_outline_color(s, UI_COLOR_FAINT, 0);
        lv_obj_set_ext_click_area(s, 3);
        lv_obj_set_style_bg_color(s, lv_color_hex(theme_swatches[i]), 0);
        lv_obj_set_clickable(s, true);
        lv_obj_add_event_cb(s, th_swatch_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        s_th_swatch[i] = s;
    }

    /* App colours */
    card = ui_card(c, NULL);
    row = th_row(card);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, "App colours");
    lv_obj_t *sw = lv_switch_create(row);
    if (s_th_apps) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, th_apps_cb, LV_EVENT_VALUE_CHANGED, NULL);
    th_hint(card, "Each app in the menu gets its own colour instead of the accent.");

    /* Apply: shown once something has changed */
    s_th_apply = lv_button_create(c);
    lv_obj_set_size(s_th_apply, LV_PCT(100), 44);
    lv_obj_set_style_radius(s_th_apply, LV_RADIUS_CIRCLE, 0);
    ui_fill(s_th_apply, UI_COLOR_ACCENT);
    lv_obj_add_event_cb(s_th_apply, th_apply_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *al = lv_label_create(s_th_apply);
    lv_label_set_text(al, LV_SYMBOL_REFRESH "  Apply & restart");
    lv_obj_center(al);

    th_refresh();
}

/* ── Firmware update ───────────────────────────────────────────────────── */
/* Runs the web server while open; the new firmware is uploaded from a
 * browser (http://<ip>/update) or tools/ota_push.py */
static lv_obj_t *s_upd_url, *s_upd_hint, *s_upd_bar, *s_upd_state;

static void update_page_update(void)
{
    char ip[16];
    wifi_mgr_get_ip(ip, sizeof(ip));
    if (ip[0] && !face_server_is_running()) face_server_start();

    if (face_server_is_running()) {
        lv_label_set_text_fmt(s_upd_url, "http://%s/update", ip);
        lv_label_set_text(s_upd_hint, "Open this address in a browser on the same Wi-Fi and "
                                      "choose build/smartwatch.bin, or run\n"
                                      "tools/ota_push.py <address>");
    } else {
        lv_label_set_text(s_upd_url, LV_SYMBOL_WIFI "  Not connected");
        lv_label_set_text(s_upd_hint, "Connect to Wi-Fi (Settings > Wi-Fi) to update wirelessly.");
    }

    ota_status_t st;
    ota_get_status(&st);
    lv_obj_set_hidden(s_upd_bar, st.state != OTA_RECEIVING);
    if (st.total) lv_bar_set_value(s_upd_bar, (int32_t)((uint64_t)st.received * 100 / st.total), LV_ANIM_OFF);
    switch (st.state) {
    case OTA_RECEIVING:
        lv_label_set_text_fmt(s_upd_state, "Receiving... %lu of %lu KB",
                              (unsigned long)(st.received / 1024), (unsigned long)(st.total / 1024));
        lv_obj_set_style_text_color(s_upd_state, UI_COLOR_TEXT, 0);
        lv_display_trigger_activity(lv_display_get_default());   // stay awake
        break;
    case OTA_DONE:
        lv_label_set_text(s_upd_state, LV_SYMBOL_OK "  Done, restarting...");
        lv_obj_set_style_text_color(s_upd_state, UI_COLOR_ACCENT, 0);
        break;
    case OTA_FAILED:
        lv_label_set_text_fmt(s_upd_state, "Update failed: %s", st.message);
        lv_obj_set_style_text_color(s_upd_state, UI_COLOR_DANGER, 0);
        break;
    default:
        lv_label_set_text(s_upd_state, "");
        break;
    }
}

static void update_page_deleted_cb(lv_event_t *e)
{
    ota_status_t st;
    ota_get_status(&st);
    if (st.state != OTA_RECEIVING) face_server_stop();
}

static void page_update(void)
{
    lv_obj_t *c = ui_page_open("Firmware update", update_page_update);
    lv_obj_add_event_cb(c, update_page_deleted_cb, LV_EVENT_DELETE, NULL);

    lv_obj_t *card = ui_card(c, "Running");
    lv_obj_t *v = lv_label_create(card);
    lv_label_set_text(v, ota_version());

    card = ui_card(c, "Update over Wi-Fi");
    s_upd_url = lv_label_create(card);
    lv_obj_set_width(s_upd_url, LV_PCT(100));
    lv_label_set_long_mode(s_upd_url, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(s_upd_url, UI_COLOR_ACCENT, 0);
    s_upd_hint = lv_label_create(card);
    lv_obj_set_width(s_upd_hint, LV_PCT(100));
    lv_label_set_long_mode(s_upd_hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(s_upd_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_upd_hint, UI_COLOR_DIM, 0);

    s_upd_bar = lv_bar_create(c);
    lv_obj_set_size(s_upd_bar, LV_PCT(100), 8);
    lv_bar_set_range(s_upd_bar, 0, 100);
    lv_obj_set_style_bg_color(s_upd_bar, UI_COLOR_SURFACE2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_upd_bar, UI_COLOR_ACCENT, LV_PART_INDICATOR);
    s_upd_state = lv_label_create(c);
    lv_obj_set_width(s_upd_state, LV_PCT(100));
    lv_label_set_long_mode(s_upd_state, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(s_upd_state, &lv_font_montserrat_14, 0);

    lv_obj_t *note = lv_label_create(c);
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(note, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(note, UI_COLOR_DIM, 0);
    lv_label_set_text(note, "Keep this page open during the update. If the new firmware "
                            "doesn't start properly, the watch goes back to this one.");

    update_page_update();
}

static void page_sound(void)
{
    lv_obj_t *c = ui_page_open("Sound", NULL);

    lv_obj_t *card = ui_card(c, LV_SYMBOL_VOLUME_MAX "  Volume");
    slider(card, 0, 100, settings_get()->volume, volume_cb);

    lv_obj_t *btn = lv_button_create(c);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_add_event_cb(btn, test_sound_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, LV_SYMBOL_BELL " Play alarm sound");
    lv_obj_center(lbl);
}

/* ── Buttons page ────────────────────────────────────────────────────────── */
static void boot_action_cb(lv_event_t *e)
{
    uint8_t *field = lv_event_get_user_data(e);
    *field = (uint8_t)lv_dropdown_get_selected(lv_event_get_target_obj(e));
    const watch_settings_t *cfg = settings_get();
    button_set_double(BUTTON_PWR, cfg->pwr_double != BTN_ACT_NONE);
    button_set_double(BUTTON_BOOT, cfg->boot_double != BTN_ACT_NONE);
    watch_settings_save();
}

static void action_dropdown(lv_obj_t *card, const char *label, uint8_t *field)
{
    lv_obj_t *l = lv_label_create(card);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, UI_COLOR_DIM, 0);
    lv_obj_t *dd = lv_dropdown_create(card);
    lv_dropdown_set_options_static(dd, UI_BTN_ACTION_OPTIONS);
    lv_obj_set_width(dd, LV_PCT(100));
    lv_dropdown_set_selected(dd, *field < BTN_ACT_COUNT ? *field : 0);
    lv_obj_add_event_cb(dd, boot_action_cb, LV_EVENT_VALUE_CHANGED, field);
}

static void page_buttons(void)
{
    lv_obj_t *c = ui_page_open("Buttons", NULL);
    watch_settings_t *cfg = settings_get();

    lv_obj_t *card = ui_card(c, LV_SYMBOL_POWER "  PWR button");
    action_dropdown(card, "Press", &cfg->pwr_click);
    action_dropdown(card, "Double press", &cfg->pwr_double);
    action_dropdown(card, "Hold (1 s)", &cfg->pwr_long);

    card = ui_card(c, LV_SYMBOL_KEYBOARD "  BOOT button");
    action_dropdown(card, "Press", &cfg->boot_click);
    action_dropdown(card, "Double press", &cfg->boot_double);
    action_dropdown(card, "Hold (1 s)", &cfg->boot_long);

    lv_obj_t *note = lv_label_create(c);
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(note, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(note, UI_COLOR_DIM, 0);
    lv_label_set_text(note,
                      "With Double press on Nothing, Press acts straight away.\n"
                      "Any press silences a ringing alarm or timer, and PWR always wakes the screen.\n"
                      "Holding PWR for 5 s always opens Restart / Power off.\n"
                      "Don't hold BOOT while the watch restarts: it then starts "
                      "in flashing mode (blank screen) until the next restart.");
}

/* ── About page ──────────────────────────────────────────────────────────── */
static lv_obj_t *s_ab_battery;
static lv_obj_t *s_ab_system;
static lv_obj_t *s_ab_storage;
static lv_obj_t *s_ab_storage_bar;

/** "812 KB" below 1 MB, else "3.4 MB". */
static void format_size(char *buf, size_t len, uint64_t bytes)
{
    if (bytes < 1024 * 1024) {
        snprintf(buf, len, "%u KB", (unsigned)(bytes / 1024));
    } else {
        unsigned tenths = (unsigned)((bytes * 10 + 512 * 1024) / (1024 * 1024));
        snprintf(buf, len, "%u.%u MB", tenths / 10, tenths % 10);
    }
}

static void about_storage_update(void)
{
    uint64_t free_b = 0, total_b = 0;
    if (storage_get_space(&free_b, &total_b) != ESP_OK || total_b == 0) {
        lv_label_set_text(s_ab_storage, "Not available");
        lv_bar_set_value(s_ab_storage_bar, 0, LV_ANIM_OFF);
        return;
    }
    uint64_t used_b = total_b > free_b ? total_b - free_b : 0;
    char used[16], fr[16], total[16];
    format_size(used, sizeof(used), used_b);
    format_size(fr, sizeof(fr), free_b);
    format_size(total, sizeof(total), total_b);
    lv_label_set_text_fmt(s_ab_storage, "Used: %s\nFree: %s\nTotal: %s", used, fr, total);
    lv_bar_set_value(s_ab_storage_bar, (int32_t)(used_b * 100 / total_b), LV_ANIM_OFF);
}

static void about_page_update(void)
{
    int mv = ui_battery_mv();
    if (battery_mv_is_charging(mv)) {
        lv_label_set_text_fmt(s_ab_battery, "USB power\n%d.%02d V", mv / 1000, mv % 1000 / 10);
    } else {
        lv_label_set_text_fmt(s_ab_battery, "%u%%\n%d.%02d V", battery_percent_from_mv(mv),
                              mv / 1000, mv % 1000 / 10);
    }

    char steps[16];
    ui_format_thousands(steps, sizeof(steps), activity_get_steps());
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *slot = esp_ota_get_running_partition();
    lv_label_set_text_fmt(s_ab_system,
                          "Steps today: %s\nFree RAM: %u KB\nBuilt: %s\nSlot: %s\nIDF: %s",
                          steps,
                          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_DEFAULT) / 1024),
                          app->date, slot ? slot->label : "?", app->idf_ver);

    about_storage_update();
}

static void page_about(void)
{
    lv_obj_t *c = ui_page_open("About", about_page_update);

    lv_obj_t *card = ui_card(c, LV_SYMBOL_BATTERY_FULL "  Battery");
    s_ab_battery = lv_label_create(card);

    card = ui_card(c, LV_SYMBOL_DRIVE "  Storage");
    s_ab_storage_bar = lv_bar_create(card);
    lv_obj_set_size(s_ab_storage_bar, LV_PCT(100), 8);
    lv_bar_set_range(s_ab_storage_bar, 0, 100);
    lv_obj_set_style_bg_color(s_ab_storage_bar, UI_COLOR_RAISED, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ab_storage_bar, UI_COLOR_ACCENT, LV_PART_INDICATOR);
    s_ab_storage = lv_label_create(card);
    lv_obj_set_style_text_font(s_ab_storage, &lv_font_montserrat_14, 0);

    card = ui_card(c, LV_SYMBOL_SETTINGS "  System");
    s_ab_system = lv_label_create(card);
    lv_obj_set_style_text_font(s_ab_system, &lv_font_montserrat_14, 0);
    lv_obj_set_width(s_ab_system, LV_PCT(100));

    lv_obj_t *dev = lv_label_create(c);
    lv_label_set_text(dev, "Waveshare ESP32-C6-Touch-LCD-1.69");
    lv_obj_set_style_text_font(dev, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(dev, UI_COLOR_DIM, 0);

    about_page_update();
}
