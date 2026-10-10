/**
 * @file ui/notify_popup.c
 * @brief Full-screen popups driven by Gadgetbridge: notifications, incoming
 *        calls and "find my watch".  Each wakes the screen and beeps; calls
 *        and find-my-watch keep ringing (and the screen on) until handled.
 *
 * Public entry points (ui_bt_*) are called from the NimBLE host task and
 * take the LVGL lock themselves.
 */

#include "ui/ui_internal.h"
#include "app/gadgetbridge.h"
#include "hal/audio.h"
#include "lvgl_port.h"

#include <string.h>

#define RING_TIMEOUT_MS   60000
#define NOTIFY_SHOW_MS    6000      // a notification closes itself after this
                                    // long without a touch (it stays in the
                                    // notification center's history)

typedef enum { POPUP_NONE, POPUP_NOTIFY, POPUP_CALL, POPUP_FIND } popup_kind_t;

static lv_obj_t    *s_popup;
static popup_kind_t s_kind = POPUP_NONE;
static int32_t      s_notify_id;
static lv_timer_t  *s_ring_timer;
static uint32_t     s_ring_start;
static lv_timer_t  *s_auto_close;

/* ── Common ──────────────────────────────────────────────────────────────── */
void notify_popup_close(void)
{
    if (s_auto_close) {
        lv_timer_delete(s_auto_close);
        s_auto_close = NULL;
    }
    if (s_ring_timer) {
        lv_timer_delete(s_ring_timer);
        s_ring_timer = NULL;
        audio_stop();
    }
    if (s_popup) {
        lv_obj_delete(s_popup);
        s_popup = NULL;
    }
    s_kind = POPUP_NONE;
}

bool notify_popup_is_open(void)        { return s_popup != NULL; }
bool notify_popup_is_notification(void){ return s_kind == POPUP_NOTIFY; }
bool notify_popup_keeps_awake(void)    { return s_kind == POPUP_CALL || s_kind == POPUP_FIND; }

static lv_obj_t *popup_create(popup_kind_t kind)
{
    notify_popup_close();
    s_kind = kind;

    s_popup = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_popup);
    lv_obj_set_size(s_popup, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_popup, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_popup, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(s_popup, 16, 0);
    lv_obj_set_style_pad_top(s_popup, UI_SAFE_MARGIN, 0);
    lv_obj_set_style_pad_bottom(s_popup, UI_SAFE_MARGIN, 0);
    lv_obj_set_style_pad_row(s_popup, 6, 0);
    lv_obj_set_flex_flow(s_popup, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_popup, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(s_popup, false);
    lv_obj_set_clickable(s_popup, true);          // block touches to what's below
    return s_popup;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                       lv_color_t color, bool wrap)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, wrap ? LV_LABEL_LONG_MODE_WRAP : LV_LABEL_LONG_MODE_DOTS);
    return l;
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_color_t color, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_height(btn, 46);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_radius(btn, 23, 0);
    ui_fill(btn, color);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return btn;
}

static lv_obj_t *button_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_clickable(row, false);
    return row;
}

static void ring_timer_cb(lv_timer_t *t)
{
    if (lv_tick_elaps(s_ring_start) > RING_TIMEOUT_MS) {
        notify_popup_close();
        return;
    }
    if (s_kind == POPUP_CALL) audio_beep(1000, 300, 150, 2);
    else                      audio_beep(2500, 150, 100, 3);
}

static void start_ringing(uint32_t period_ms)
{
    s_ring_start = lv_tick_get();
    s_ring_timer = lv_timer_create(ring_timer_cb, period_ms, NULL);
    ring_timer_cb(s_ring_timer);
}

/* ── Notification ────────────────────────────────────────────────────────── */
static void dismiss_cb(lv_event_t *e)
{
    if (s_notify_id >= 0) gb_send_notify_action(s_notify_id, "DISMISS");   // < 0: made on the watch
    notify_center_set_inactive(s_notify_id);
    notify_popup_close();
}

static void close_cb(lv_event_t *e)
{
    notify_popup_close();
}

/* Close once it has been up NOTIFY_SHOW_MS and untouched for as long, so
 * scrolling a long message keeps it open */
static void auto_close_cb(lv_timer_t *t)
{
    if (lv_display_get_inactive_time(lv_display_get_default()) >= NOTIFY_SHOW_MS) {
        notify_popup_close();
    }
}

void ui_bt_notification(int32_t id, const char *src, const char *title, const char *body)
{
    lvgl_lock();
    notify_center_add(id, src, title, body);
    lv_obj_t *p = popup_create(POPUP_NOTIFY);
    s_notify_id = id;

    label(p, *src ? src : "Notification", &lv_font_montserrat_14, UI_COLOR_ACCENT, false);
    if (*title) label(p, title, &lv_font_montserrat_20, UI_COLOR_TEXT, false);

    /* Body scrolls if it is long */
    lv_obj_t *box = lv_obj_create(p);
    lv_obj_remove_style_all(box);
    lv_obj_set_width(box, LV_PCT(100));
    lv_obj_set_flex_grow(box, 1);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    lv_obj_t *b = label(box, body, &lv_font_montserrat_16, UI_COLOR_DIM, true);
    lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_LEFT, 0);

    lv_obj_t *row = button_row(p);
    button(row, "Close", UI_COLOR_SURFACE, close_cb);
    button(row, "Dismiss", UI_COLOR_RAISED, dismiss_cb);

    lv_display_trigger_activity(lv_display_get_default());   // count from now
    s_auto_close = lv_timer_create(auto_close_cb, 500, NULL);

    lvgl_unlock();
    ui_screen_wake();
    audio_beep(1800, 60, 60, 2);

    lvgl_lock();
    notify_rules_check(id, src, title, body);    // Notify alarms: may ring now
    lvgl_unlock();
}

void ui_bt_notification_remove(int32_t id)
{
    lvgl_lock();
    if (s_kind == POPUP_NOTIFY && s_notify_id == id) notify_popup_close();
    notify_center_set_inactive(id);
    lvgl_unlock();
}

/* ── Incoming call ───────────────────────────────────────────────────────── */
static void accept_cb(lv_event_t *e)
{
    gb_send_call("ACCEPT");
    notify_popup_close();
}

static void reject_cb(lv_event_t *e)
{
    gb_send_call("REJECT");
    notify_popup_close();
}

void ui_bt_call(bool incoming, const char *name, const char *number)
{
    lvgl_lock();
    if (!incoming) {
        /* answered / ended / rejected on the phone */
        if (s_kind == POPUP_CALL) notify_popup_close();
        lvgl_unlock();
        return;
    }

    lv_obj_t *p = popup_create(POPUP_CALL);
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    label(p, LV_SYMBOL_BELL "  Incoming call", &lv_font_montserrat_16, UI_COLOR_ACCENT, false);
    label(p, *name ? name : number, &lv_font_montserrat_24, UI_COLOR_TEXT, true);
    if (*name && *number) label(p, number, &lv_font_montserrat_16, UI_COLOR_DIM, false);

    lv_obj_t *row = button_row(p);
    button(row, LV_SYMBOL_CLOSE, UI_COLOR_DANGER, reject_cb);
    button(row, LV_SYMBOL_OK, UI_COLOR_ACCENT, accept_cb);

    start_ringing(1500);
    lvgl_unlock();
    ui_screen_wake();
}

/* ── Find my watch ───────────────────────────────────────────────────────── */
static void find_stop_cb(lv_event_t *e)
{
    notify_popup_close();
}

void ui_bt_find(bool on)
{
    lvgl_lock();
    if (!on) {
        if (s_kind == POPUP_FIND) notify_popup_close();
        lvgl_unlock();
        return;
    }

    lv_obj_t *p = popup_create(POPUP_FIND);
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    label(p, LV_SYMBOL_BELL, &lv_font_montserrat_48, UI_COLOR_WARN, false);
    label(p, "Here I am!", &lv_font_montserrat_24, UI_COLOR_TEXT, false);
    lv_obj_t *row = button_row(p);
    button(row, "Stop", UI_COLOR_SURFACE, find_stop_cb);

    start_ringing(1000);
    lvgl_unlock();
    ui_screen_wake();
}
