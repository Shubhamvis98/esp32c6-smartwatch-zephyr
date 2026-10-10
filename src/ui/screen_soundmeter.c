/**
 * @file ui/screen_soundmeter.c
 * @brief Sound level meter page.
 *
 *     Sound level
 *        54 dB            ← smoothed level, coloured by loudness
 *      Normal talk
 *   ▮▮▮▮▮▮▮▯▯▯▯▯          ← 30–110 dB
 *   ╱╲_╱‾╲__ (last 10 s)
 *   Min 38  Avg 51  Max 72
 *   [ Reset ]
 *
 * The microphone gives dB relative to full scale; MIC_SPL_OFFSET_DB maps
 * that to approximate dB SPL.  It is not a calibrated instrument.
 */

#include "ui/ui_internal.h"
#include "hal/audio.h"

#include <math.h>

#define UI_RATE_MS          100
#define HISTORY_POINTS      100          // 10 s at 10 Hz
#define SMOOTHING           0.35f        // "fast" response, per 100 ms
#define DB_MIN              30
#define DB_MAX              110

/* dBFS → approximate dB SPL for the board's analog mic with +30 dB PGA and
 * +30 dB digital gain (hal/audio.c).  Was 94 at +18 dB digital gain, where a
 * quiet room read about 32 dB; lowered by the 12 dB of extra gain so the
 * readings stay the same. */
#define MIC_SPL_OFFSET_DB   82.0f

static lv_obj_t   *s_value;
static lv_obj_t   *s_desc;
static lv_obj_t   *s_bar;
static lv_obj_t   *s_chart;
static lv_chart_series_t *s_series;
static lv_obj_t   *s_stats;
static lv_timer_t *s_timer;
static bool        s_mic_ok;

static float  s_smooth;
static float  s_min, s_max;
static double s_sum;
static uint32_t s_n;

static void reset_stats(void)
{
    s_min = 1000.0f;
    s_max = 0.0f;
    s_sum = 0;
    s_n = 0;
}

static const char *describe(float db, lv_color_t *color)
{
    if (db < 40) { *color = UI_COLOR_ACCENT2; return "Very quiet"; }
    if (db < 55) { *color = UI_COLOR_ACCENT;  return "Quiet room"; }
    if (db < 70) { *color = UI_COLOR_ACCENT;  return "Conversation"; }
    if (db < 85) { *color = UI_COLOR_WARN;    return "Loud"; }
    *color = UI_COLOR_DANGER;
    return "Very loud - protect your hearing";
}

static void timer_cb(lv_timer_t *t)
{
    if (!s_mic_ok) return;

    float db = audio_mic_rms_dbfs() + MIC_SPL_OFFSET_DB;
    if (db < 0) db = 0;
    s_smooth = s_n ? s_smooth + SMOOTHING * (db - s_smooth) : db;

    if (s_smooth < s_min) s_min = s_smooth;
    if (s_smooth > s_max) s_max = s_smooth;
    s_sum += s_smooth;
    s_n++;

    lv_color_t color;
    const char *text = describe(s_smooth, &color);
    lv_label_set_text_fmt(s_value, "%d dB", (int)lroundf(s_smooth));
    lv_obj_set_style_text_color(s_value, color, 0);
    lv_label_set_text(s_desc, text);

    int v = (int)s_smooth;
    lv_bar_set_value(s_bar, v < DB_MIN ? DB_MIN : v > DB_MAX ? DB_MAX : v, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar, color, LV_PART_INDICATOR);

    lv_chart_set_next_value(s_chart, s_series, v < DB_MIN ? DB_MIN : v > DB_MAX ? DB_MAX : v);

    lv_label_set_text_fmt(s_stats, "Min %d   Avg %d   Max %d", (int)lroundf(s_min),
                          (int)lroundf((float)(s_sum / s_n)), (int)lroundf(s_max));
}

static void reset_cb(lv_event_t *e)
{
    reset_stats();
    lv_chart_set_all_values(s_chart, s_series, LV_CHART_POINT_NONE);
}

static void page_deleted_cb(lv_event_t *e)
{
    if (s_timer) lv_timer_delete(s_timer);
    s_timer = NULL;
    if (s_mic_ok) audio_mic_stop();
    s_mic_ok = false;
}

void screen_soundmeter_open(void)
{
    lv_obj_t *c = ui_page_open("Sound meter", NULL);
    lv_obj_add_event_cb(c, page_deleted_cb, LV_EVENT_DELETE, NULL);
    lv_obj_set_style_pad_row(c, 6, 0);

    s_value = lv_label_create(c);
    lv_obj_set_style_text_font(s_value, &lv_font_montserrat_48, 0);
    lv_label_set_text(s_value, "-- dB");

    s_desc = lv_label_create(c);
    lv_obj_set_style_text_color(s_desc, UI_COLOR_DIM, 0);
    lv_label_set_text(s_desc, "");

    s_bar = lv_bar_create(c);
    lv_obj_set_size(s_bar, LV_PCT(100), 10);
    lv_bar_set_range(s_bar, DB_MIN, DB_MAX);
    lv_obj_set_style_bg_color(s_bar, UI_COLOR_RAISED, LV_PART_MAIN);

    s_chart = lv_chart_create(c);
    lv_obj_set_size(s_chart, LV_PCT(100), 60);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, HISTORY_POINTS);
    lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, DB_MIN, DB_MAX);
    lv_chart_set_update_mode(s_chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_div_line_count(s_chart, 3, 0);
    lv_obj_set_style_bg_color(s_chart, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_border_width(s_chart, 0, 0);
    lv_obj_set_style_pad_all(s_chart, 4, 0);
    lv_obj_set_style_line_color(s_chart, UI_COLOR_SURFACE2, LV_PART_MAIN);
    lv_obj_set_style_width(s_chart, 0, LV_PART_INDICATOR);        // no point dots
    lv_obj_set_style_height(s_chart, 0, LV_PART_INDICATOR);
    lv_obj_set_scrollable(s_chart, false);
    s_series = lv_chart_add_series(s_chart, UI_COLOR_ACCENT, LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_values(s_chart, s_series, LV_CHART_POINT_NONE);

    s_stats = lv_label_create(c);
    lv_obj_set_style_text_font(s_stats, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_stats, "");

    lv_obj_t *btn = lv_button_create(c);
    lv_obj_set_width(btn, LV_PCT(100));
    ui_fill(btn, UI_COLOR_RAISED);
    lv_obj_add_event_cb(btn, reset_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, LV_SYMBOL_REFRESH " Reset");
    lv_obj_center(l);

    lv_obj_t *note = lv_label_create(c);
    lv_label_set_text(note, "Approximate, uncalibrated");
    lv_obj_set_style_text_font(note, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(note, UI_COLOR_DIM, 0);

    reset_stats();
    s_mic_ok = audio_mic_start(NULL) == ESP_OK;
    if (!s_mic_ok) {
        lv_label_set_text(s_desc, "Microphone busy (recording?)");
    }
    s_timer = lv_timer_create(timer_cb, UI_RATE_MS, NULL);
}
