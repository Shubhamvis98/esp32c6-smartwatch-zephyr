/**
 * @file ui/screen_camera.c
 * @brief Camera shutter: take a photo with whatever camera app is open.
 *
 * The watch is also a Bluetooth media remote (see ble_mgr.h), and camera apps
 * take a photo on volume up.  The big button presses volume up, straight away
 * or after a 3-second countdown.  The first time, it asks the phone to pair.
 */

#include <stdio.h>
#include "ui/ui_internal.h"
#include "app/ble_mgr.h"
#include "hal/audio.h"

#define COUNTDOWN_S  3
#define CHECK_MS     1000

static lv_obj_t   *s_root, *s_status, *s_shutter, *s_timer_sw;
static lv_timer_t *s_countdown, *s_check;
static int         s_count;

static void set_status(const char *t, lv_color_t color)
{
    if (!s_status) return;
    lv_label_set_text(s_status, t);
    lv_obj_set_style_text_color(s_status, color, 0);
}

static void stop_countdown(void)
{
    if (s_countdown) {
        lv_timer_delete(s_countdown);
        s_countdown = NULL;
    }
}

static void show_link(void)
{
    if (ble_media_ready())             set_status("Open the camera on the phone", UI_COLOR_DIM);
    else if (ble_mgr_get_state() != BLE_STATE_CONNECTED)
        set_status("Connect the phone in Bluetooth settings", UI_COLOR_WARN);
    else if (!ble_mgr_is_secure()) set_status("Accept the pairing requests on the phone", UI_COLOR_WARN);
    else set_status("Paired. In the phone's Bluetooth settings, open C6 Watch and tap Connect",
                    UI_COLOR_WARN);
}

static void shoot(void)
{
    if (!ble_media_ready()) {
        ble_media_pair();
        show_link();
        return;
    }
    ble_hid_consumer_tap(HID_CONSUMER_VOL_UP);
    set_status("Click", UI_COLOR_ACCENT);
    audio_beep(2400, 40, 40, 2);
}

static void countdown_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    if (--s_count > 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", s_count);
        set_status(buf, UI_COLOR_TEXT);
        audio_beep(1500, 60, 0, 1);
        return;
    }
    stop_countdown();
    shoot();
}

static void shutter_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (s_countdown) {                       // tap again to cancel
        stop_countdown();
        set_status("Cancelled", UI_COLOR_DIM);
        return;
    }
    if (lv_obj_has_state(s_timer_sw, LV_STATE_CHECKED)) {
        s_count = COUNTDOWN_S + 1;
        countdown_cb(NULL);
        s_countdown = lv_timer_create(countdown_cb, 1000, NULL);
    } else {
        shoot();
    }
}

bool screen_camera_shutter(void)
{
    if (!s_root) return false;
    shutter_cb(NULL);
    return true;
}

/* Follow pairing while nothing else is shown */
static void check_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    static bool was_ready;
    bool ready = ble_media_ready();
    if (!s_countdown && ready != was_ready) show_link();
    was_ready = ready;
}

static void page_deleted_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    stop_countdown();
    if (s_check) {
        lv_timer_delete(s_check);
        s_check = NULL;
    }
    s_root = s_status = s_shutter = s_timer_sw = NULL;
}

void screen_camera_open(void)
{
    lv_obj_t *c = ui_page_open("Camera", NULL);
    lv_obj_add_event_cb(c, page_deleted_cb, LV_EVENT_DELETE, NULL);
    lv_obj_set_scrollable(c, false);
    lv_obj_set_style_pad_row(c, 10, 0);
    s_root = c;

    s_shutter = lv_button_create(c);
    lv_obj_set_size(s_shutter, 120, 120);
    lv_obj_set_style_radius(s_shutter, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(s_shutter, 0, 0);
    lv_obj_set_style_margin_top(s_shutter, 4, 0);
    ui_fill(s_shutter, UI_COLOR_ACCENT);
    lv_obj_set_style_border_width(s_shutter, 4, 0);
    lv_obj_set_style_border_color(s_shutter, UI_COLOR_SURFACE2, 0);
    lv_obj_add_event_cb(s_shutter, shutter_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *icon = ui_icon_create(s_shutter, ui_icon_camera_large(), ui_contrast(UI_COLOR_ACCENT));
    lv_obj_center(icon);

    s_status = lv_label_create(c);
    lv_obj_set_width(s_status, LV_PCT(100));
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_16, 0);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_MODE_WRAP);

    lv_obj_t *row = lv_obj_create(c);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, "3 s timer");
    s_timer_sw = lv_switch_create(row);

    show_link();
    if (ble_mgr_get_state() == BLE_STATE_CONNECTED && !ble_media_ready()) ble_media_pair();
    s_check = lv_timer_create(check_cb, CHECK_MS, NULL);
}
