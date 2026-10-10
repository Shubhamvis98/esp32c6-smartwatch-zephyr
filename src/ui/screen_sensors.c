/**
 * @file ui/screen_sensors.c
 * @brief Sensors app: live accelerometer readings and the raise-to-wake
 *        detector's state, for checking how the IMU is mounted and why
 *        raise to wake does or doesn't fire.
 */

#include "ui/ui_internal.h"
#include "app/activity.h"
#include "app/settings.h"

#include <math.h>
#include <stdio.h>

#define UI_RATE_MS   100
#define AXIS_RANGE   200          // bars show -2 g … +2 g in hundredths

static lv_timer_t *s_timer;
static lv_obj_t   *s_bar[3];
static lv_obj_t   *s_val[3];
static lv_obj_t   *s_total;
static lv_obj_t   *s_orient;
static lv_obj_t   *s_raise;

/* "1.2 s ago", "never" */
static void format_ago(char *buf, size_t len, int32_t ms)
{
    if (ms < 0)          snprintf(buf, len, "never");
    else if (ms < 60000) snprintf(buf, len, "%ld.%ld s ago", (long)(ms / 1000), (long)(ms % 1000 / 100));
    else                 snprintf(buf, len, "%ld min ago", (long)(ms / 60000));
}

/* "+0.95" — LVGL's printf has no float support */
static void format_g(char *buf, size_t len, float g, bool sign)
{
    long c = lroundf(g * 100);
    long a = c < 0 ? -c : c;
    snprintf(buf, len, "%s%ld.%02ld", c < 0 ? "-" : sign ? "+" : "", a / 100, a % 100);
}

/* Which way the screen faces, from the raw axes (no sign assumptions) */
static const char *orientation(const activity_debug_t *d)
{
    float ax = fabsf(d->x), ay = fabsf(d->y), az = fabsf(d->z);
    if (az >= ax && az >= ay) return d->z > 0 ? "Z+ (flat)" : "Z- (flat)";
    if (ax >= ay)             return d->x > 0 ? "X+ (on edge)" : "X- (on edge)";
    return                           d->y > 0 ? "Y+ (on edge)" : "Y- (on edge)";
}

static void timer_cb(lv_timer_t *t)
{
    activity_debug_t d;
    activity_get_debug(&d);

    const float v[3] = { d.x, d.y, d.z };
    for (int i = 0; i < 3; i++) {
        int32_t c = (int32_t)lroundf(v[i] * 100);
        lv_bar_set_value(s_bar[i], c < -AXIS_RANGE ? -AXIS_RANGE : c > AXIS_RANGE ? AXIS_RANGE : c,
                         LV_ANIM_OFF);
        char g[24];
        format_g(g, sizeof(g), v[i], true);
        lv_label_set_text(s_val[i], g);
    }
    float mag = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    char g[24];
    format_g(g, sizeof(g), mag, false);
    lv_label_set_text_fmt(s_total, "Total  %s g", g);
    lv_label_set_text_fmt(s_orient, "Gravity on  %s", orientation(&d));

    char vert[20], last[20];
    format_ago(vert, sizeof(vert), d.vertical_ago_ms);
    format_ago(last, sizeof(last), d.raise_ago_ms);
    lv_label_set_text_fmt(s_raise,
                          "Raise to wake: %s\n"
                          "Face up: %s\n"
                          "Arm down: %s\n"
                          "Raises: %lu (%s)",
                          settings_get()->raise_to_wake ? "on" : "off",
                          d.face_up ? "yes" : "no",
                          vert, (unsigned long)d.raises, last);
}

static void page_deleted_cb(lv_event_t *e)
{
    if (s_timer) lv_timer_delete(s_timer);
    s_timer = NULL;
}

static void axis_row(lv_obj_t *card, int i, const char *name, lv_color_t color)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);

    lv_obj_t *n = lv_label_create(row);
    lv_label_set_text(n, name);
    lv_obj_set_style_text_color(n, color, 0);
    lv_obj_set_width(n, 14);

    /* Symmetric bar: fills from the middle (0 g) towards ±2 g */
    s_bar[i] = lv_bar_create(row);
    lv_bar_set_mode(s_bar[i], LV_BAR_MODE_SYMMETRICAL);
    lv_bar_set_range(s_bar[i], -AXIS_RANGE, AXIS_RANGE);
    lv_obj_set_height(s_bar[i], 10);
    lv_obj_set_flex_grow(s_bar[i], 1);
    lv_obj_set_style_bg_color(s_bar[i], UI_COLOR_RAISED, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar[i], color, LV_PART_INDICATOR);

    s_val[i] = lv_label_create(row);
    lv_obj_set_width(s_val[i], 52);
    lv_obj_set_style_text_align(s_val[i], LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(s_val[i], &lv_font_montserrat_16, 0);
}

void screen_sensors_open(void)
{
    lv_obj_t *c = ui_page_open("Sensors", NULL);
    lv_obj_add_event_cb(c, page_deleted_cb, LV_EVENT_DELETE, NULL);

    lv_obj_t *card = ui_card(c, "Accelerometer (g)");
    lv_obj_set_style_pad_row(card, 6, 0);
    axis_row(card, 0, "X", UI_COLOR_WARN);
    axis_row(card, 1, "Y", UI_COLOR_ACCENT);
    axis_row(card, 2, "Z", UI_COLOR_ACCENT2);
    s_total = lv_label_create(card);
    lv_obj_set_style_text_font(s_total, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_total, UI_COLOR_DIM, 0);
    s_orient = lv_label_create(card);
    lv_obj_set_style_text_font(s_orient, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_orient, UI_COLOR_DIM, 0);

    card = ui_card(c, "Raise detector");
    s_raise = lv_label_create(card);
    lv_obj_set_style_text_font(s_raise, &lv_font_montserrat_14, 0);

    lv_obj_t *note = lv_label_create(c);
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(note, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(note, UI_COLOR_DIM, 0);
    lv_label_set_text(note, "Lying flat, screen up, one axis should read about "
                            "1 g. A raise needs the arm down, then the screen "
                            "facing up for a moment.");

    timer_cb(NULL);
    s_timer = lv_timer_create(timer_cb, UI_RATE_MS, NULL);
}
