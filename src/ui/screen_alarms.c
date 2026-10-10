/**
 * @file ui/screen_alarms.c
 * @brief Alarm pages (list + editor) and the ringing screen.
 *
 *   Alarms                      Edit alarm
 *   Next: Tue 07:30 (in 9 h)    [07] : [30]      ← rollers
 *   07:30  Mon-Fri      [on]    S M T W T F S    ← weekday toggles
 *   09:00  Weekends     [off]   [Delete] [Save]
 *   [+ Add alarm]
 *
 * A once-per-second LVGL timer (running even with the screen off) checks
 * for due alarms and shows the ringing screen: Snooze (5 min) or Stop.
 */

#include "ui/ui_internal.h"
#include "app/alarms.h"
#include "app/settings.h"
#include "hal/audio.h"
#include "hal/rtc.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define RING_TIMEOUT_MS   60000
#define RING_PERIOD_MS    1500

/* ── Helpers ─────────────────────────────────────────────────────────────── */
static void format_time(char *buf, size_t len, int h, int m)
{
    if (settings_get()->use_24h) {
        snprintf(buf, len, "%02d:%02d", h, m);
    } else {
        int h12 = h % 12;
        snprintf(buf, len, "%d:%02d %s", h12 ? h12 : 12, m, h < 12 ? "AM" : "PM");
    }
}

/* ── Ringing screen ──────────────────────────────────────────────────────── */
static lv_obj_t   *s_ring;
static lv_timer_t *s_ring_timer;
static uint32_t    s_ring_start;
static int         s_ring_index = -1;     // alarm, or -1 for a notification rule
static char        s_ring_title[48];       // rule rings: what to show again on Snooze
static char        s_ring_text[96];

bool alarm_ring_active(void)
{
    return s_ring != NULL;
}

void alarm_ring_dismiss(void)
{
    if (!s_ring) return;
    lv_timer_delete(s_ring_timer);
    s_ring_timer = NULL;
    lv_obj_delete(s_ring);
    s_ring = NULL;
    s_ring_index = -1;
    audio_stop();
}

static void ring_tick_cb(lv_timer_t *t)
{
    if (lv_tick_elaps(s_ring_start) > RING_TIMEOUT_MS) {
        alarm_ring_dismiss();
        return;
    }
    audio_beep(1800, 150, 100, 4);
}

static void snooze_cb(lv_event_t *e)
{
    if (s_ring_index >= 0) alarms_snooze(time(NULL), s_ring_index);
    else notify_rules_snooze(s_ring_title, s_ring_text);
    alarm_ring_dismiss();
}

static void stop_cb(lv_event_t *e)
{
    alarm_ring_dismiss();
}

static lv_obj_t *ring_button(lv_obj_t *parent, const char *text, lv_color_t color, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_height(b, 50);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_style_radius(b, 25, 0);
    ui_fill(b, color);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}

/* An alarm (index >= 0), or a notification rule (title / text set) */
static void ring_show_ex(int index, const char *title, const char *text)
{
    alarm_ring_dismiss();
    s_ring_index = index;
    snprintf(s_ring_title, sizeof(s_ring_title), "%s", title ? title : "");
    snprintf(s_ring_text, sizeof(s_ring_text), "%s", text ? text : "");

    s_ring = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_ring);
    lv_obj_set_size(s_ring, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_ring, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_ring, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(s_ring, 16, 0);
    lv_obj_set_style_pad_ver(s_ring, UI_SAFE_MARGIN + 6, 0);
    lv_obj_set_flex_flow(s_ring, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ring, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(s_ring, false);
    lv_obj_set_clickable(s_ring, true);

    lv_obj_t *icon = lv_label_create(s_ring);
    lv_label_set_text(icon, LV_SYMBOL_BELL "  Alarm");
    lv_obj_set_style_text_color(icon, UI_COLOR_WARN, 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);
    if (title) {
        lv_label_set_text_fmt(icon, LV_SYMBOL_BELL "  %s", title);
        lv_obj_set_width(icon, LV_PCT(100));
        lv_label_set_long_mode(icon, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
    }

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char buf[16];
    format_time(buf, sizeof(buf), tm.tm_hour, tm.tm_min);
    lv_obj_t *t = lv_label_create(s_ring);
    lv_label_set_text(t, buf);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_48, 0);

    if (text && *text) {
        lv_obj_t *msg = lv_label_create(s_ring);
        lv_label_set_text(msg, text);
        lv_obj_set_width(msg, LV_PCT(100));
        lv_label_set_long_mode(msg, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_max_height(msg, 60, 0);
        lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(msg, UI_COLOR_DIM, 0);
        lv_obj_set_style_text_font(msg, &lv_font_montserrat_16, 0);
    }

    lv_obj_t *row = lv_obj_create(s_ring);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_clickable(row, false);
    ring_button(row, "Snooze", UI_COLOR_RAISED, snooze_cb);
    ring_button(row, "Stop", UI_COLOR_DANGER, stop_cb);

    s_ring_start = lv_tick_get();
    s_ring_timer = lv_timer_create(ring_tick_cb, RING_PERIOD_MS, NULL);
    ring_tick_cb(s_ring_timer);

    ui_screen_wake();
}

static void ring_show(int index) { ring_show_ex(index, NULL, NULL); }

void alarm_ring_message(const char *title, const char *text)
{
    ring_show_ex(-1, title, text);
}

static void scheduler_cb(lv_timer_t *t)
{
    if (!rtc_time_valid()) return;
    int index;
    if (alarms_check(time(NULL), &index)) ring_show(index);
}

void screen_alarms_init(void)
{
    alarms_load();
    lv_timer_create(scheduler_cb, 1000, NULL);
}

/* ── Editor ──────────────────────────────────────────────────────────────── */
static alarm_t   s_edit;
static int       s_edit_index;       // -1 = new
static lv_obj_t *s_roll_h, *s_roll_m;
static lv_obj_t *s_days;

static void list_open(void);

static void save_cb(lv_event_t *e)
{
    s_edit.hour = (uint8_t)lv_roller_get_selected(s_roll_h);
    s_edit.minute = (uint8_t)lv_roller_get_selected(s_roll_m);
    s_edit.days = 0;
    for (int k = 0; k < 7; k++) {                    // buttons are Mon … Sun
        if (lv_buttonmatrix_has_button_ctrl(s_days, k, LV_BUTTONMATRIX_CTRL_CHECKED)) {
            s_edit.days |= 1 << ((k + 1) % 7);
        }
    }
    s_edit.enabled = true;

    if (s_edit_index < 0) alarms_add(&s_edit);
    else                  *alarms_get(s_edit_index) = s_edit;
    alarms_sort();
    alarms_save();
    list_open();
}

static void delete_cb(lv_event_t *e)
{
    if (s_edit_index >= 0) {
        alarms_remove(s_edit_index);
        alarms_save();
    }
    list_open();
}

static void edit_open(int index)
{
    s_edit_index = index;
    if (index >= 0) {
        s_edit = *alarms_get(index);
    } else {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        s_edit = (alarm_t){ .hour = rtc_time_valid() ? (tm.tm_hour + 1) % 24 : 7,
                            .minute = 0, .days = 0, .enabled = true };
    }

    lv_obj_t *c = ui_page_open(index < 0 ? "New alarm" : "Edit alarm", NULL);
    ui_page_set_back(list_open);

    /* Hour : minute rollers */
    lv_obj_t *row = lv_obj_create(c);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 6, 0);

    static char hours[24 * 6];
    hours[0] = 0;
    for (int h = 0; h < 24; h++) {
        char item[8];
        if (settings_get()->use_24h) {
            snprintf(item, sizeof(item), "%02d", h);
        } else {
            snprintf(item, sizeof(item), "%d%s", h % 12 ? h % 12 : 12, h < 12 ? "a" : "p");
        }
        strlcat(hours, item, sizeof(hours));
        if (h < 23) strlcat(hours, "\n", sizeof(hours));
    }
    static char minutes[60 * 3];
    minutes[0] = 0;
    for (int m = 0; m < 60; m++) {
        char item[4];
        snprintf(item, sizeof(item), "%02d", m);
        strlcat(minutes, item, sizeof(minutes));
        if (m < 59) strlcat(minutes, "\n", sizeof(minutes));
    }

    lv_obj_t **rollers[2] = { &s_roll_h, &s_roll_m };
    const char *opts[2] = { hours, minutes };
    uint8_t sel[2] = { s_edit.hour, s_edit.minute };
    for (int i = 0; i < 2; i++) {
        lv_obj_t *r = lv_roller_create(row);
        lv_roller_set_options(r, opts[i], LV_ROLLER_MODE_INFINITE);
        lv_roller_set_visible_row_count(r, 3);
        lv_obj_set_width(r, 78);
        lv_obj_set_style_text_font(r, &lv_font_montserrat_24, 0);
        lv_obj_set_style_bg_color(r, UI_COLOR_SURFACE, 0);
        lv_obj_set_style_border_width(r, 0, 0);
        lv_obj_set_style_bg_color(r, UI_COLOR_ACCENT, LV_PART_SELECTED);
        lv_obj_set_style_text_color(r, ui_contrast(UI_COLOR_ACCENT), LV_PART_SELECTED);
        lv_roller_set_selected(r, sel[i], LV_ANIM_OFF);
        *rollers[i] = r;
        if (i == 0) {
            lv_obj_t *colon = lv_label_create(row);
            lv_label_set_text(colon, ":");
            lv_obj_set_style_text_font(colon, &lv_font_montserrat_24, 0);
        }
    }

    /* Weekdays, Monday first */
    lv_obj_t *lbl = lv_label_create(c);
    lv_label_set_text(lbl, "Repeat (none = once)");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, UI_COLOR_DIM, 0);

    static const char *const day_map[] = { "M", "T", "W", "T", "F", "S", "S", "" };
    s_days = lv_buttonmatrix_create(c);
    lv_buttonmatrix_set_map(s_days, (const char **)day_map);
    lv_buttonmatrix_set_button_ctrl_all(s_days, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    for (int k = 0; k < 7; k++) {
        if (s_edit.days & (1 << ((k + 1) % 7))) {
            lv_buttonmatrix_set_button_ctrl(s_days, k, LV_BUTTONMATRIX_CTRL_CHECKED);
        }
    }
    lv_obj_set_size(s_days, LV_PCT(100), 40);
    lv_obj_set_style_pad_all(s_days, 0, 0);
    lv_obj_set_style_pad_gap(s_days, 3, 0);
    lv_obj_set_style_border_width(s_days, 0, 0);
    lv_obj_set_style_bg_opa(s_days, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(s_days, UI_COLOR_SURFACE, LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_days, UI_COLOR_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_days, UI_COLOR_ACCENT, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(s_days, ui_contrast(UI_COLOR_ACCENT), LV_PART_ITEMS | LV_STATE_CHECKED);

    /* Buttons */
    row = lv_obj_create(c);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);
    if (index >= 0) ring_button(row, "Delete", UI_COLOR_DANGER, delete_cb);
    ring_button(row, "Save", UI_COLOR_ACCENT, save_cb);
}

/* ── List ────────────────────────────────────────────────────────────────── */
static lv_obj_t *s_next;

static void list_update(void)
{
    if (!rtc_time_valid()) {
        lv_label_set_text(s_next, "Clock not set: alarms are paused");
        return;
    }
    time_t now = time(NULL);
    time_t next = alarms_next(now);
    if (!next) {
        lv_label_set_text(s_next, "No alarms on");
        return;
    }
    struct tm tm;
    localtime_r(&next, &tm);
    char t[16];
    format_time(t, sizeof(t), tm.tm_hour, tm.tm_min);
    static const char *const wd[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    uint32_t mins = (uint32_t)((next - now + 59) / 60);
    if (mins < 60) {
        lv_label_set_text_fmt(s_next, "Next: %s %s (in %lu min)", wd[tm.tm_wday], t, (unsigned long)mins);
    } else {
        lv_label_set_text_fmt(s_next, "Next: %s %s (in %lu h %lu min)", wd[tm.tm_wday], t,
                              (unsigned long)(mins / 60), (unsigned long)(mins % 60));
    }
}

static void row_cb(lv_event_t *e)
{
    edit_open((int)(intptr_t)lv_event_get_user_data(e));
}

static void switch_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    alarm_t *a = alarms_get(i);
    if (!a) return;
    a->enabled = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    alarms_save();
    list_update();
}

static void add_cb(lv_event_t *e)
{
    edit_open(-1);
}

static void list_open(void)
{
    lv_obj_t *c = ui_page_open("Alarms", list_update);

    s_next = lv_label_create(c);
    lv_obj_set_width(s_next, LV_PCT(100));
    lv_label_set_long_mode(s_next, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(s_next, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_next, UI_COLOR_ACCENT, 0);

    for (int i = 0; i < alarms_count(); i++) {
        const alarm_t *a = alarms_get(i);

        lv_obj_t *row = lv_button_create(c);
        lv_obj_set_size(row, LV_PCT(100), 56);
        ui_fill(row, UI_COLOR_SURFACE);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_pad_hor(row, 10, 0);
        lv_obj_add_event_cb(row, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        char buf[24];
        format_time(buf, sizeof(buf), a->hour, a->minute);
        lv_obj_t *t = lv_label_create(row);
        lv_label_set_text(t, buf);
        lv_obj_set_style_text_font(t, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(t, a->enabled ? UI_COLOR_TEXT : UI_COLOR_DIM, 0);
        lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 2);

        alarms_days_text(a->days, buf, sizeof(buf));
        lv_obj_t *d = lv_label_create(row);
        lv_label_set_text(d, buf);
        lv_obj_set_style_text_font(d, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(d, UI_COLOR_DIM, 0);
        lv_obj_align(d, LV_ALIGN_BOTTOM_LEFT, 0, -2);

        lv_obj_t *sw = lv_switch_create(row);
        lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
        if (a->enabled) lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_add_event_cb(sw, switch_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
    }

    if (alarms_count() < ALARM_MAX) {
        lv_obj_t *add = lv_button_create(c);
        lv_obj_set_width(add, LV_PCT(100));
        lv_obj_add_event_cb(add, add_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(add);
        lv_label_set_text(l, LV_SYMBOL_PLUS " Add alarm");
        lv_obj_center(l);
    }

    list_update();
}

void screen_alarms_open(void)
{
    list_open();
}
