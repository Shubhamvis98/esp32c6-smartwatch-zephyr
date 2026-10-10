/**
 * @file ui/screen_timer.c
 * @brief Countdown timer tile.  Keeps running while the screen is off; when it
 *        expires the screen wakes, this tile is shown and the speaker beeps
 *        until dismissed (Stop button or PWR click).
 *
 * The duration is one of the presets (1 / 3 / 5 / 10 min) or a custom time
 * up to 23:59:59 picked with hour / minute / second wheels (the ✎ button, or
 * a tap on the time while the timer is stopped).
 */

#include "ui/ui_internal.h"
#include "hal/audio.h"

#include "esp_timer.h"

#include <stdio.h>

#define TIMER_TICK_MS        200
#define RING_TIMEOUT_MS      30000
#define BEEP_FREQ_HZ         2000

static const uint8_t s_presets_min[] = { 1, 3, 5, 10 };

static lv_obj_t *s_time;
static lv_obj_t *s_start_lbl;
static lv_obj_t *s_presets;

/* Custom time picker */
static lv_obj_t *s_roll_h, *s_roll_m, *s_roll_s, *s_set_btn;

static uint32_t s_duration_ms = 5 * 60 * 1000;
static uint32_t s_remaining_ms = 5 * 60 * 1000;
static int64_t  s_end_us = 0;
static bool     s_running = false;
static bool     s_ringing = false;
static int64_t  s_ring_start_us = 0;
static int64_t  s_ring_beep_sec = 0;

static void refresh(void)
{
    uint32_t secs = (s_remaining_ms + 999) / 1000;
    if (s_ringing) {
        lv_label_set_text(s_time, "00:00");
        lv_obj_set_style_text_color(s_time, UI_COLOR_DANGER, 0);
    } else {
        if (secs >= 3600) {
            lv_label_set_text_fmt(s_time, "%lu:%02lu:%02lu", (unsigned long)(secs / 3600),
                                  (unsigned long)(secs / 60 % 60), (unsigned long)(secs % 60));
        } else {
            lv_label_set_text_fmt(s_time, "%02lu:%02lu",
                                  (unsigned long)(secs / 60), (unsigned long)(secs % 60));
        }
        lv_obj_set_style_text_color(s_time, s_running ? UI_COLOR_ACCENT : UI_COLOR_TEXT, 0);
    }

    lv_label_set_text(s_start_lbl, s_ringing ? LV_SYMBOL_STOP " Stop"
                                 : s_running ? LV_SYMBOL_PAUSE " Pause"
                                             : LV_SYMBOL_PLAY " Start");

    if (s_running || s_ringing) lv_obj_add_state(s_presets, LV_STATE_DISABLED);
    else                        lv_obj_remove_state(s_presets, LV_STATE_DISABLED);
}

bool screen_timer_is_ringing(void)
{
    return s_ringing;
}

bool screen_timer_is_running(uint32_t *left_ms)
{
    if (left_ms) *left_ms = s_remaining_ms;
    return s_running;
}

void screen_timer_dismiss(void)
{
    if (!s_ringing) return;
    audio_stop();
    s_ringing = false;
    s_remaining_ms = s_duration_ms;
    refresh();
}

static void tick_cb(lv_timer_t *t)
{
    int64_t now = esp_timer_get_time();

    if (s_running) {
        int64_t left_us = s_end_us - now;
        if (left_us <= 0) {
            s_running = false;
            s_remaining_ms = 0;
            s_ringing = true;
            s_ring_start_us = now;
            s_ring_beep_sec = 0;
            audio_beep(BEEP_FREQ_HZ, 120, 120, 3);
            ui_timer_alarm();
        } else {
            s_remaining_ms = (uint32_t)(left_us / 1000);
        }
        refresh();
    } else if (s_ringing) {
        int64_t ringing_ms = (now - s_ring_start_us) / 1000;
        if (ringing_ms >= RING_TIMEOUT_MS) {
            screen_timer_dismiss();
        } else if (ringing_ms / 1000 != s_ring_beep_sec) {
            s_ring_beep_sec = ringing_ms / 1000;     // re-arm the pattern each second
            audio_beep(BEEP_FREQ_HZ, 120, 120, 3);
        }
    }
}

static void start_cb(lv_event_t *e)
{
    if (s_ringing) {
        screen_timer_dismiss();
        return;
    }
    if (s_running) {
        s_running = false;          // pause: s_remaining_ms holds the time left
    } else {
        if (s_remaining_ms == 0) s_remaining_ms = s_duration_ms;
        s_end_us = esp_timer_get_time() + (int64_t)s_remaining_ms * 1000;
        s_running = true;
    }
    refresh();
}

static void reset_cb(lv_event_t *e)
{
    screen_timer_dismiss();
    s_running = false;
    s_remaining_ms = s_duration_ms;
    refresh();
}

/* ── Custom time picker ──────────────────────────────────────────────────── */
static uint32_t picked_secs(void)
{
    return lv_roller_get_selected(s_roll_h) * 3600u + lv_roller_get_selected(s_roll_m) * 60u +
           lv_roller_get_selected(s_roll_s);
}

static void roller_cb(lv_event_t *e)
{
    lv_obj_set_state(s_set_btn, LV_STATE_DISABLED, picked_secs() == 0);
}

static void set_cb(lv_event_t *e)
{
    uint32_t secs = picked_secs();
    if (secs == 0 || s_running || s_ringing) return;
    s_duration_ms = secs * 1000u;
    s_remaining_ms = s_duration_ms;
    ui_page_close();
    refresh();
}

static lv_obj_t *wheel(lv_obj_t *parent, const char *opts, uint32_t sel, const char *unit)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 4, 0);
    lv_obj_set_clickable(col, false);

    lv_obj_t *r = lv_roller_create(col);
    lv_roller_set_options(r, opts, LV_ROLLER_MODE_INFINITE);
    lv_roller_set_visible_row_count(r, 3);
    lv_roller_set_selected(r, sel, LV_ANIM_OFF);
    lv_obj_set_width(r, 58);
    lv_obj_set_style_text_font(r, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(r, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(r, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_text_color(r, UI_COLOR_DIM, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_bg_color(r, UI_COLOR_ACCENT, LV_PART_SELECTED);
    lv_obj_set_style_text_color(r, ui_contrast(UI_COLOR_ACCENT), LV_PART_SELECTED);
    lv_obj_add_event_cb(r, roller_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *l = lv_label_create(col);
    lv_label_set_text(l, unit);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, UI_COLOR_DIM, 0);
    return r;
}

static void picker_open(void)
{
    if (s_running || s_ringing) return;

    /* "00\n01\n…": built once */
    static char opts_h[24 * 3 + 1], opts_60[60 * 3 + 1];
    if (!opts_h[0]) {
        for (int i = 0; i < 60; i++) {
            if (i < 24) snprintf(opts_h + i * 3, 4, "%02d\n", i);
            snprintf(opts_60 + i * 3, 4, "%02d\n", i);
        }
        opts_h[24 * 3 - 1] = 0;                   // no newline after the last one
        opts_60[60 * 3 - 1] = 0;
    }

    uint32_t secs = s_duration_ms / 1000;
    lv_obj_t *c = ui_page_open("Set timer", NULL);

    lv_obj_t *row = lv_obj_create(c);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_clickable(row, false);
    s_roll_h = wheel(row, opts_h, secs / 3600 % 24, "hours");
    s_roll_m = wheel(row, opts_60, secs / 60 % 60, "min");
    s_roll_s = wheel(row, opts_60, secs % 60, "sec");

    s_set_btn = lv_button_create(c);
    lv_obj_set_size(s_set_btn, LV_PCT(100), 44);
    lv_obj_set_style_radius(s_set_btn, 22, 0);
    ui_fill(s_set_btn, UI_COLOR_ACCENT);
    lv_obj_set_style_shadow_width(s_set_btn, 0, 0);
    lv_obj_set_style_opa(s_set_btn, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_add_event_cb(s_set_btn, set_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(s_set_btn);
    lv_label_set_text(l, LV_SYMBOL_OK " Set");
    lv_obj_set_style_text_color(l, ui_contrast(UI_COLOR_ACCENT), 0);
    lv_obj_center(l);
    roller_cb(NULL);
}

static void time_click_cb(lv_event_t *e) { picker_open(); }

static void preset_cb(lv_event_t *e)
{
    uint32_t idx = lv_buttonmatrix_get_selected_button(lv_event_get_target_obj(e));
    if (idx == sizeof(s_presets_min)) {           // ✎: custom time
        picker_open();
        return;
    }
    if (idx >= sizeof(s_presets_min)) return;
    s_duration_ms = s_presets_min[idx] * 60u * 1000u;
    s_remaining_ms = s_duration_ms;
    refresh();
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, lv_color_t color,
                             lv_event_cb_t cb, lv_obj_t **label_out)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 96, 44);
    ui_fill(btn, color);
    lv_obj_set_style_radius(btn, 22, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    if (label_out) *label_out = lbl;
    return btn;
}

void screen_timer_create(lv_obj_t *parent)
{
    static const char *const map[] = { "1m", "3m", "5m", "10m", LV_SYMBOL_EDIT, "" };

    lv_obj_set_style_bg_color(parent, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, LV_SYMBOL_BELL " Timer");
    lv_obj_set_style_text_color(title, UI_COLOR_DIM, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, UI_SAFE_MARGIN - 4);

    s_time = lv_label_create(parent);
    lv_obj_set_style_text_font(s_time, &lv_font_montserrat_48, 0);
    lv_obj_align(s_time, LV_ALIGN_CENTER, 0, -62);
    lv_obj_set_clickable(s_time, true);                       // tap: custom time
    lv_obj_set_ext_click_area(s_time, 10);
    lv_obj_add_event_cb(s_time, time_click_cb, LV_EVENT_CLICKED, NULL);

    s_presets = lv_buttonmatrix_create(parent);
    lv_buttonmatrix_set_map(s_presets, (const char **)map);
    lv_obj_set_size(s_presets, 224, 52);
    lv_obj_align(s_presets, LV_ALIGN_CENTER, 0, 4);
    lv_obj_set_style_bg_opa(s_presets, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_presets, 0, 0);
    lv_obj_set_style_pad_all(s_presets, 4, 0);
    lv_obj_set_style_pad_gap(s_presets, 5, 0);
    lv_obj_set_style_bg_color(s_presets, UI_COLOR_SURFACE, LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_presets, UI_COLOR_TEXT, LV_PART_ITEMS);
    lv_obj_add_event_cb(s_presets, preset_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 210, LV_SIZE_CONTENT);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -UI_SAFE_MARGIN - 18);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    make_button(row, LV_SYMBOL_PLAY " Start", UI_COLOR_ACCENT, start_cb, &s_start_lbl);
    make_button(row, LV_SYMBOL_REFRESH " Reset", UI_COLOR_SURFACE, reset_cb, NULL);

    lv_timer_create(tick_cb, TIMER_TICK_MS, NULL);
    refresh();
}
