/**
 * @file ui/screen_activity.c
 * @brief Activity tile: step ring towards the daily goal, distance, calories.
 */

#include "ui/ui_internal.h"
#include "app/activity.h"
#include "app/settings.h"

#include <stdio.h>

/* Rough averages; good enough for a watch summary */
#define STRIDE_CM        75
#define KCAL_PER_1000    40

static lv_obj_t *s_arc;
static lv_obj_t *s_steps;
static lv_obj_t *s_detail;
static lv_obj_t *s_goal;
static uint16_t  s_shown_goal;

void screen_activity_create(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "Activity");
    lv_obj_set_style_text_color(title, UI_COLOR_DIM, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, UI_SAFE_MARGIN - 4);

    s_arc = lv_arc_create(parent);
    lv_obj_set_size(s_arc, 170, 170);
    lv_obj_align(s_arc, LV_ALIGN_CENTER, 0, -10);
    lv_arc_set_rotation(s_arc, 135);
    lv_arc_set_bg_angles(s_arc, 0, 270);
    lv_arc_set_range(s_arc, 0, 1000);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
    lv_obj_set_clickable(s_arc, false);
    lv_obj_set_style_arc_width(s_arc, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 14, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, UI_COLOR_ACCENT_DIM, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, UI_COLOR_ACCENT, LV_PART_INDICATOR);

    s_steps = lv_label_create(s_arc);
    lv_obj_set_style_text_font(s_steps, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(s_steps, UI_COLOR_TEXT, 0);
    lv_obj_align(s_steps, LV_ALIGN_CENTER, 0, -8);

    s_goal = lv_label_create(s_arc);
    lv_label_set_text(s_goal, "");
    lv_obj_set_style_text_color(s_goal, UI_COLOR_DIM, 0);
    lv_obj_align(s_goal, LV_ALIGN_CENTER, 0, 24);
    s_shown_goal = 0;

    s_detail = lv_label_create(parent);
    lv_obj_set_style_text_color(s_detail, UI_COLOR_TEXT, 0);
    lv_obj_align(s_detail, LV_ALIGN_BOTTOM_MID, 0, -UI_SAFE_MARGIN - 14);
}

void screen_activity_update(void)
{
    uint32_t n = activity_get_steps();

    char buf[16];
    ui_format_thousands(buf, sizeof(buf), n);
    lv_label_set_text(s_steps, buf);

    uint16_t goal = settings_get()->step_goal;     // changed in Settings → Activity
    if (goal != s_shown_goal) {
        s_shown_goal = goal;
        ui_format_thousands(buf, sizeof(buf), goal);
        lv_label_set_text_fmt(s_goal, "of %s", buf);
    }
    uint32_t permille = n * 1000ull / goal;
    lv_arc_set_value(s_arc, permille > 1000 ? 1000 : (int32_t)permille);

    uint32_t dist_m = n * STRIDE_CM / 100;
    uint32_t kcal   = n * KCAL_PER_1000 / 1000;
    lv_label_set_text_fmt(s_detail, "%lu.%02lu km    %lu kcal",
                          (unsigned long)(dist_m / 1000),
                          (unsigned long)(dist_m % 1000 / 10),
                          (unsigned long)kcal);
}
