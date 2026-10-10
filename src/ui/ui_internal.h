#pragma once
/**
 * @file ui/ui_internal.h
 * @brief Shared declarations between the UI manager and the individual tiles.
 *        Everything here runs in the LVGL task with the lock held.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"
#include "ui/ui_manager.h"
#include "ui/theme.h"

/* ── Palette ─────────────────────────────────────────────────────────────── */
#define UI_COLOR_BG         (g_ui_theme.bg)
#define UI_COLOR_SURFACE    (g_ui_theme.surface)
#define UI_COLOR_SURFACE2   (g_ui_theme.surface2)
#define UI_COLOR_RAISED     (g_ui_theme.raised)
#define UI_COLOR_FAINT      (g_ui_theme.faint)
#define UI_COLOR_TEXT       (g_ui_theme.text)
#define UI_COLOR_TEXT2      (g_ui_theme.text2)
#define UI_COLOR_DIM        (g_ui_theme.dim)
#define UI_COLOR_ACCENT     (g_ui_theme.accent)
#define UI_COLOR_ACCENT2    (g_ui_theme.accent2)
#define UI_COLOR_ACCENT_DIM (g_ui_theme.accent_dim)
#define UI_COLOR_WARN       (g_ui_theme.warn)
#define UI_COLOR_DANGER     (g_ui_theme.danger)
#define UI_COLOR_DANGER_DIM (g_ui_theme.danger_dim)

/* The 1.69" panel has rounded corners — keep content inside this margin */
#define UI_SAFE_MARGIN     18

/* ── Tiles ───────────────────────────────────────────────────────────────── */
void screen_watchface_create(lv_obj_t *parent);
void screen_watchface_update(void);

void screen_activity_create(lv_obj_t *parent);
void screen_activity_update(void);
void screen_music_create(lv_obj_t *parent);
void screen_music_update(void);

void screen_timer_create(lv_obj_t *parent);
bool screen_timer_is_ringing(void);
/** True while counting down (not paused); *left_ms gets the time remaining. */
bool screen_timer_is_running(uint32_t *left_ms);
void screen_timer_dismiss(void);

void screen_wifi_create(lv_obj_t *parent);
/** Called when the Wi-Fi tile becomes visible (starts a scan if stale). */
void screen_wifi_on_show(void);
/** Close the password keyboard if it is open. */
void screen_wifi_close_modal(void);
bool screen_wifi_modal_is_open(void);

void screen_bluetooth_create(lv_obj_t *parent);
/** Close the name keyboard if it is open. */
void screen_bluetooth_close_modal(void);
bool screen_bluetooth_modal_is_open(void);

/* Mouse & keyboard overlays (bt_remote.c) */
void bt_remote_open_trackpad(void);
void bt_remote_open_keyboard(void);
void bt_remote_close(void);
bool bt_remote_is_open(void);

/* Gadgetbridge popups (notify_popup.c) */
void notify_popup_close(void);
bool notify_popup_is_open(void);
bool notify_popup_is_notification(void);
/** Calls and find-my-watch keep the screen on while ringing. */
bool notify_popup_keeps_awake(void);

void screen_settings_create(lv_obj_t *parent);
void screen_settings_update(void);

/** Rebuild the watchface tile with the face in settings_get()->face_id. */
void screen_watchface_apply_style(void);
/** Re-read custom faces from storage and rebuild the watchface. */
void screen_watchface_reload_faces(void);
/** The face being shown. */
const struct watchface *screen_watchface_active(void);
/** Delete custom face @p id from storage.  If it is showing, Digital is
 *  selected instead.  Call with the LVGL lock held. */
bool screen_watchface_delete(const char *id);

/* ── Notification center (notify_center.c) ──────────────────────────────── */
void notify_center_add(int32_t id, const char *src, const char *title, const char *body);
void notify_center_set_inactive(int32_t id);   // dismissed on the phone or the watch
void notify_center_open(void);
void notify_center_close(bool anim);
bool notify_center_is_open(void);
bool notify_center_at_top(void);               // list scrolled to the top
void notify_center_clear(void);

/* ── Quick panel + torch (quick_panel.c) ─────────────────────────────────── */
void quick_panel_open(void);
void quick_panel_close(bool anim);
bool quick_panel_is_open(void);
bool quick_panel_torch_active(void);
void quick_panel_torch_exit(void);

void quick_panel_torch_start(void);

/* ── Tiles / navigation (ui_manager.c) ───────────────────────────────────── */
/* Tiles before TILE_SWIPE_COUNT are idle screen pages, in the order chosen in
 * Settings → Idle screen, reached by swiping left/right.  The watchface can
 * sit anywhere in that order but is always the default screen (PWR, wake and
 * timeouts go back to it) and can't be turned off.  A page turned off there, and every tile from
 * TILE_SWIPE_COUNT on, is hidden: opened from Settings / the app menu / the
 * quick panel, left with ← or swipe right (back to where it was opened from). */
typedef enum {
    TILE_WATCHFACE,
    TILE_ACTIVITY,
    TILE_MUSIC,
    TILE_SWIPE_COUNT,
    TILE_TIMER = TILE_SWIPE_COUNT,
    TILE_WIFI,
    TILE_BLUETOOTH,
    TILE_SETTINGS,
    TILE_COUNT,
} ui_tile_t;

/** Close any overlay and show the given tile. */
void ui_open_tile(ui_tile_t tile);

/** All idle pages, watchface included, in their saved order, on or off;
 *  returns the count (TILE_SWIPE_COUNT). */
int ui_idle_pages(ui_tile_t *out);
const char *ui_tile_name(ui_tile_t tile);
/** Re-lay out the swipe pages after Settings → Idle screen changed. */
void ui_apply_idle_layout(void);

/* ── Alarms, recorder, sound meter (full-screen pages) ───────────────────── */
/** Load alarms and start the once-a-second alarm check. */
void screen_alarms_init(void);
void screen_alarms_open(void);
void screen_nav_open(void);
void screen_weather_open(void);
void screen_calendar_open(void);
void screen_calendar_init(void);
void screen_camera_open(void);
void screen_calc_open(void);
bool alarm_ring_active(void);
void alarm_ring_dismiss(void);
/** Ring like an alarm with a custom heading and message (Snooze re-rings it
 *  through notify_rules_snooze). */
void alarm_ring_message(const char *title, const char *text);

/* ── Notification alarms (screen_rules.c) ────────────────────────────────── */
/** Load the rules and start the once-a-second check for pending rings. */
void screen_rules_init(void);
void screen_rules_open(void);
/** A notification arrived: start the countdown of every rule it matches. */
void notify_rules_check(int32_t id, const char *src, const char *title, const char *body);
/** Ring this again in 5 minutes. */
void notify_rules_snooze(const char *title, const char *text);

void screen_recorder_open(void);
void screen_soundmeter_open(void);
void screen_sensors_open(void);
void screen_stopwatch_open(void);
/** Start or stop the stopwatch (open or not); returns true if now running. */
bool screen_stopwatch_toggle(void);
bool screen_stopwatch_is_open(void);
/** Take a photo if the Camera page is open (with its timer); false if not open. */
bool screen_camera_shutter(void);
/** Ring the phone / stop; returns true if now ringing (false if not connected). */
bool screen_bluetooth_find_phone_toggle(void);
/** Phone: keypad, favourites, recent calls; calls go through the phone. */
void screen_phone_open(void);
/** Update the Calling page when the call is answered (LVGL lock held). */
void ui_phone_call_answered(void);

/* ── Drawn icons (ui_icons.c) ────────────────────────────────────────────── */
const lv_image_dsc_t *ui_icon_sun(void);
const lv_image_dsc_t *ui_icon_watch(void);
const lv_image_dsc_t *ui_icon_clock(void);
const lv_image_dsc_t *ui_icon_timer(void);
const lv_image_dsc_t *ui_icon_torch(void);
const lv_image_dsc_t *ui_icon_hourglass(void);
const lv_image_dsc_t *ui_icon_phone(void);
const lv_image_dsc_t *ui_icon_palette(void);
const lv_image_dsc_t *ui_icon_navigate(void);
const lv_image_dsc_t *ui_icon_navigate_large(void);   // 44 px
const lv_image_dsc_t *ui_icon_calendar(void);
const lv_image_dsc_t *ui_icon_calculator(void);
const lv_image_dsc_t *ui_icon_camera(void);
const lv_image_dsc_t *ui_icon_weather_menu(void);
const lv_image_dsc_t *ui_icon_camera_large(void);     // 64 px
/** Weather icon for a weather_kind_t: 56 px (large) or 24 px. */
const lv_image_dsc_t *ui_icon_weather(int kind, bool large);
/** Turn arrow for a Gadgetbridge navigation action ("left", "roundabout_right"…), or NULL. */
const lv_image_dsc_t *ui_icon_nav(const char *action);
/** Image of @p icon tinted @p color, not clickable (taps reach the parent). */
lv_obj_t *ui_icon_create(lv_obj_t *parent, const lv_image_dsc_t *icon, lv_color_t color);

/* ── PWR-button menus (menus.c) ──────────────────────────────────────────── */
void menu_apps_open(void);
void menu_power_open(void);

/* ── Sub-pages (ui_page.c) ───────────────────────────────────────────────── */
typedef void (*ui_page_update_cb_t)(void);

/**
 * Open a full-screen page above the tiles (closing any other page).
 * @param update  optional, called every second while the page is open
 * @return the scrollable content column to add widgets to
 */
lv_obj_t *ui_page_open(const char *title, ui_page_update_cb_t update);
void      ui_page_close(void);
bool      ui_page_is_open(void);

/** Override what ← / swipe-right does on the current page (default: close).
 *  Reset by the next ui_page_open / ui_page_close. */
typedef void (*ui_page_back_cb_t)(void);
void      ui_page_set_back(ui_page_back_cb_t cb);
void      ui_page_back(void);

/** Rounded surface card with an optional small title, laid out as a column. */
lv_obj_t *ui_card(lv_obj_t *parent, const char *title);

/** Lay out a hidden tile like a sub-page: title next to the ← button (which
 *  ui_manager draws on top) and a scrolling column below.  Returns the column. */
lv_obj_t *ui_tile_layout(lv_obj_t *tile, const char *title);

/* ── Shared state / helpers (ui_manager.c) ───────────────────────────────── */

/** Cached battery voltage, refreshed every 30 s. */
int  ui_battery_mv(void);

/** Called by the timer tile when it starts ringing. */
void ui_timer_alarm(void);

/** Format n with thousands separators ("12,345"). */
void ui_format_thousands(char *buf, size_t len, uint32_t n);
