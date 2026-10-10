/**
 * @file ui/screen_stopwatch.c
 * @brief Stopwatch app (App menu → Stopwatch).
 *
 *   ┌──────────────────────────┐
 *   │ ‹  Stopwatch             │
 *   │       01:23.45           │
 *   │   [ Lap ]   [ Stop ]     │   stopped: [ Reset ] [ Start ]
 *   │  Lap 3   00:21.07  01:23 │   newest lap first; the fastest is
 *   │  Lap 2   00:30.92  …     │   green and the slowest red
 *   └──────────────────────────┘
 *
 * It keeps running after the page is closed (the time is worked out from
 * esp_timer, not counted by the UI), so you can leave and come back.
 */

#include "ui/ui_internal.h"

#include "esp_timer.h"

#include <stdio.h>
#include <string.h>

#define UI_RATE_MS   33
#define MAX_LAPS     99

#define COLOR_GO     UI_COLOR_ACCENT
#define COLOR_STOP   UI_COLOR_DANGER

/* State: survives the page being closed */
static bool     s_running;
static int64_t  s_start_us;          // when the current run began
static int64_t  s_acc_us;            // time from earlier runs (before Stop)
static int64_t  s_laps[MAX_LAPS];    // total time at each lap
static int      s_lap_count;

/* UI: only while the page is open */
static lv_timer_t *s_timer;
static lv_obj_t   *s_main;           // "01:23"
static lv_obj_t   *s_frac;           // ".45"
static lv_obj_t   *s_left_btn, *s_left_lbl;    // Lap / Reset
static lv_obj_t   *s_right_btn, *s_right_lbl;  // Start / Stop
static lv_obj_t   *s_lap_list;

static int64_t elapsed_us(void)
{
    return s_acc_us + (s_running ? esp_timer_get_time() - s_start_us : 0);
}

/* "01:23" + ".45", or "1:02:03" + "" from an hour on */
static void format_split(int64_t us, char *main, size_t mlen, char *frac, size_t flen)
{
    long cs = (long)(us / 10000);
    long h = cs / 360000, m = cs / 6000 % 60, s = cs / 100 % 60, c = cs % 100;
    if (h) {
        snprintf(main, mlen, "%ld:%02ld:%02ld", h, m, s);
        frac[0] = '\0';
    } else {
        snprintf(main, mlen, "%02ld:%02ld", m, s);
        snprintf(frac, flen, ".%02ld", c);
    }
}

static void format_time(int64_t us, char *buf, size_t len)
{
    char m[16], f[8];
    format_split(us, m, sizeof(m), f, sizeof(f));
    snprintf(buf, len, "%s%s", m, f);
}

static void set_text_if_changed(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

/* ── UI ──────────────────────────────────────────────────────────────────── */
static void update_time(void)
{
    char m[16], f[8];
    format_split(elapsed_us(), m, sizeof(m), f, sizeof(f));
    set_text_if_changed(s_main, m);
    set_text_if_changed(s_frac, f);
}

static void update_buttons(void)
{
    lv_label_set_text(s_right_lbl, s_running ? "Stop" : "Start");
    ui_fill(s_right_btn, s_running ? COLOR_STOP : COLOR_GO);

    bool can_lap = s_running && s_lap_count < MAX_LAPS;
    lv_label_set_text(s_left_lbl, s_running ? "Lap" : "Reset");
    bool enabled = s_running ? can_lap : elapsed_us() > 0;
    lv_obj_set_state(s_left_btn, LV_STATE_DISABLED, !enabled);
}

static void lap_row(int i, int fastest, int slowest)
{
    int64_t split = s_laps[i] - (i ? s_laps[i - 1] : 0);
    char num[16], sp[24], tot[24];
    snprintf(num, sizeof(num), "Lap %d", i + 1);
    format_time(split, sp, sizeof(sp));
    format_time(s_laps[i], tot, sizeof(tot));

    lv_obj_t *row = lv_obj_create(s_lap_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(row, 4, 0);
    lv_obj_set_clickable(row, false);

    lv_color_t color = i == fastest ? COLOR_GO : i == slowest ? COLOR_STOP : UI_COLOR_TEXT;
    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, num);
    lv_obj_set_style_text_color(l, UI_COLOR_DIM, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    l = lv_label_create(row);
    lv_label_set_text(l, sp);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 62, 0);
    l = lv_label_create(row);
    lv_label_set_text(l, tot);
    lv_obj_set_style_text_color(l, UI_COLOR_DIM, 0);
    lv_obj_align(l, LV_ALIGN_RIGHT_MID, 0, 0);
}

static void rebuild_laps(void)
{
    lv_obj_clean(s_lap_list);
    lv_obj_set_hidden(s_lap_list, s_lap_count == 0);

    /* Fastest / slowest only mean something with 3+ laps */
    int fastest = -1, slowest = -1;
    if (s_lap_count >= 3) {
        fastest = slowest = 0;
        for (int i = 1; i < s_lap_count; i++) {
            int64_t d = s_laps[i] - s_laps[i - 1];
            int64_t df = s_laps[fastest] - (fastest ? s_laps[fastest - 1] : 0);
            int64_t ds = s_laps[slowest] - (slowest ? s_laps[slowest - 1] : 0);
            if (d < df) fastest = i;
            if (d > ds) slowest = i;
        }
    }
    for (int i = s_lap_count - 1; i >= 0; i--) lap_row(i, fastest, slowest);
}

static void timer_cb(lv_timer_t *t)
{
    update_time();
}

static void right_cb(lv_event_t *e)          // Start / Stop
{
    int64_t now = esp_timer_get_time();
    if (s_running) {
        s_acc_us += now - s_start_us;
        s_running = false;
    } else {
        s_start_us = now;
        s_running = true;
    }
    update_time();
    update_buttons();
}

static void left_cb(lv_event_t *e)           // Lap / Reset
{
    if (s_running) {
        if (s_lap_count < MAX_LAPS) s_laps[s_lap_count++] = elapsed_us();
    } else {
        s_acc_us = 0;
        s_lap_count = 0;
    }
    update_time();
    update_buttons();
    rebuild_laps();
}

static void page_deleted_cb(lv_event_t *e)
{
    if (s_timer) lv_timer_delete(s_timer);
    s_timer = NULL;
    s_main = NULL;
}

bool screen_stopwatch_is_open(void) { return s_main != NULL; }

bool screen_stopwatch_toggle(void)
{
    if (s_main) {
        right_cb(NULL);
    } else {                                 // page closed: just keep time
        int64_t now = esp_timer_get_time();
        if (s_running) s_acc_us += now - s_start_us;
        else s_start_us = now;
        s_running = !s_running;
    }
    return s_running;
}

static lv_obj_t *pill_button(lv_obj_t *parent, lv_color_t color, lv_event_cb_t cb, lv_obj_t **label)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_height(btn, 48);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_radius(btn, 24, 0);
    ui_fill(btn, color);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_opa(btn, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    *label = lv_label_create(btn);
    lv_obj_set_style_text_font(*label, &lv_font_montserrat_20, 0);
    lv_obj_center(*label);
    return btn;
}

void screen_stopwatch_open(void)
{
    lv_obj_t *c = ui_page_open("Stopwatch", NULL);
    lv_obj_add_event_cb(c, page_deleted_cb, LV_EVENT_DELETE, NULL);

    /* Big minutes:seconds, smaller hundredths, sharing a baseline */
    lv_obj_t *time = lv_obj_create(c);
    lv_obj_remove_style_all(time);
    lv_obj_set_size(time, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(time, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_ver(time, 10, 0);
    lv_obj_set_clickable(time, false);
    s_main = lv_label_create(time);
    lv_label_set_text(s_main, "");
    lv_obj_set_style_text_font(s_main, &lv_font_montserrat_48, 0);
    s_frac = lv_label_create(time);
    lv_label_set_text(s_frac, "");
    lv_obj_set_style_text_font(s_frac, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_frac, UI_COLOR_DIM, 0);
    lv_obj_set_style_pad_bottom(s_frac, 6, 0);     // line up with the big digits' baseline

    lv_obj_t *row = lv_obj_create(c);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_clickable(row, false);
    s_left_btn  = pill_button(row, UI_COLOR_SURFACE, left_cb, &s_left_lbl);
    s_right_btn = pill_button(row, COLOR_GO, right_cb, &s_right_lbl);

    s_lap_list = lv_obj_create(c);
    lv_obj_remove_style_all(s_lap_list);
    lv_obj_set_size(s_lap_list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_lap_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(s_lap_list, 4, 0);
    lv_obj_set_style_text_font(s_lap_list, &lv_font_montserrat_16, 0);
    lv_obj_set_clickable(s_lap_list, false);

    update_time();
    update_buttons();
    rebuild_laps();
    s_timer = lv_timer_create(timer_cb, UI_RATE_MS, NULL);
}
