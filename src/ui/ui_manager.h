#pragma once
/**
 * @file ui/ui_manager.h
 * @brief Smartwatch UI: swipeable tiles (watchface, activity, timer,
 *        settings), touch input and screen on/off management.
 *
 * All functions here take the LVGL lock themselves, so they can be called
 * from any task (button task, IMU task, ...).
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/** @brief Build the UI and register the touch input.  Needs display_init(),
 *         touch_init() and lvgl_port_init(). */
esp_err_t ui_init(void);

/** @brief Turn the screen on (if off) and reset the idle timer.  The screen
 *         shows whatever was open when it turned off. */
void ui_screen_wake(void);

/** @brief Turn the screen off now. */
void ui_screen_off(void);

bool ui_screen_is_on(void);

/** @brief Jump back to the watchface. */
void ui_go_home(void);

/**
 * @brief Handle a PWR button click:
 *        screen off → wake on the watchface; timer ringing → dismiss;
 *        anything other than the plain watchface → back to the watchface;
 *        on the watchface → open the app menu.
 */
void ui_handle_power_click(void);

/** @brief Handle a PWR long press: show the Restart / Power off menu. */
void ui_handle_power_long_press(void);

/** What the PWR and BOOT buttons do (Settings → Buttons).  Stored in settings, so
 *  only add new values at the end. */
typedef enum {
    BTN_ACT_NONE,
    BTN_ACT_BACK,          // back one page; on an idle page, the watchface
    BTN_ACT_HOME,          // watchface
    BTN_ACT_APPS,          // app menu
    BTN_ACT_NOTIFICATIONS, // notification center
    BTN_ACT_QUICK_PANEL,
    BTN_ACT_TORCH,         // screen torch on / off
    BTN_ACT_PLAY_PAUSE,    // phone music
    BTN_ACT_NEXT,
    BTN_ACT_PREVIOUS,
    BTN_ACT_CAMERA,        // camera shutter (volume up over Bluetooth)
    BTN_ACT_FIND_PHONE,    // ring the phone / stop
    BTN_ACT_STOPWATCH,     // start / stop the stopwatch
    BTN_ACT_TIMER,         // open the timer
    BTN_ACT_SCREEN_OFF,
    BTN_ACT_SILENT,        // toggle the watch's sound
    BTN_ACT_HOME_OR_APPS,  // PWR's original press: watchface, or the app menu from it
    BTN_ACT_POWER_MENU,    // Restart / Power off
    BTN_ACT_COUNT,
} btn_action_t;

/** Dropdown options for the actions above, "\n"-separated, in order. */
extern const char UI_BTN_ACTION_OPTIONS[];
/** Run one of the actions above (from the button task).  pwr = it came from
 *  PWR, which always wakes the screen. */
void ui_handle_button_action(btn_action_t action, bool pwr);

/** @brief Show "Powering off", save step count and cut battery power. */
void ui_request_power_off(void);

/** @brief Show "Restarting", save step count and reboot. */
void ui_request_restart(void);

/** @brief Erase all settings, alarms, steps, Wi-Fi networks, Bluetooth
 *         pairings and recordings, then reboot. */
void ui_request_factory_reset(void);

/* ── Gadgetbridge popups (ui/notify_popup.c) ───────────────────────────── */
void ui_bt_notification(int32_t id, const char *src, const char *title, const char *body);
void ui_bt_notification_remove(int32_t id);

/** Turn-by-turn navigation from the phone (Gadgetbridge "nav"). */
typedef struct {
    char    action[24];       // "left", "roundabout_right", "finish"… ("" = none)
    char    instr[128];       // street / instruction
    char    distance[24];     // to the turn, as the phone formats it ("350 m")
    char    eta[16];          // arrival time ("10:42")
    int32_t left_s;           // seconds to go, -1 if unknown
    int8_t  pct;              // route completed, -1 if unknown
} ui_nav_info_t;
void ui_bt_nav(const ui_nav_info_t *nav);

/** New weather / calendar data from the phone (refresh what shows it). */
void ui_weather_changed(void);
void ui_calendar_changed(void);
/** incoming = true shows the ringing call screen; false closes it. */
void ui_bt_call(bool incoming, const char *name, const char *number);
/** Someone picked up a call made from the phone (Notify alarms: Call answered). */
void ui_bt_call_answered(const char *name, const char *number);
void ui_bt_find(bool on);
/** The phone couldn't place a call asked for by the Phone app. */
void ui_phone_dial_error(const char *error);

/** @brief Show a full-screen message (e.g. "Powering off") and wake the screen. */
void ui_show_message(const char *text);
