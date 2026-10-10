/**
 * @file ui/faces/face_digital.c
 * @brief Digital watchface: large stacked hours and minutes, seconds and
 *        date underneath.
 */

#include "ui/faces/watchface.h"
#include "ui/ui_internal.h"

static lv_obj_t *s_m_hours, *s_m_ampm, *s_m_mins, *s_m_secs, *s_m_date;

#define AMPM_W  30

static bool create(const watchface_t *f, lv_obj_t *face)
{
    lv_obj_t *col = face_make_flex(face, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, -6, 0);
    lv_obj_align(col, LV_ALIGN_CENTER, 0, -14);

    /* Hours with AM/PM at their top right; an equal-width spacer on the
     * left keeps the hours centred */
    lv_obj_t *hrow = face_make_flex(col, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hrow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(hrow, 4, 0);
    lv_obj_t *spacer = face_make_label(hrow, &lv_font_montserrat_16, UI_COLOR_DIM);
    lv_label_set_text(spacer, "");
    lv_obj_set_width(spacer, AMPM_W);
    s_m_hours = face_make_label(hrow, &lv_font_montserrat_48, UI_COLOR_TEXT);
    s_m_ampm  = face_make_label(hrow, &lv_font_montserrat_16, UI_COLOR_DIM);
    lv_obj_set_width(s_m_ampm, AMPM_W);
    lv_obj_set_style_margin_top(s_m_ampm, 10, 0);
    s_m_mins  = face_make_label(col, &lv_font_montserrat_48, UI_COLOR_ACCENT);
    s_m_secs  = face_make_label(col, &lv_font_montserrat_24, UI_COLOR_DIM);
    lv_obj_set_style_margin_top(s_m_secs, 10, 0);

    s_m_date = face_make_label(face, &lv_font_montserrat_16, UI_COLOR_DIM);
    lv_obj_align(s_m_date, LV_ALIGN_BOTTOM_MID, 0, -UI_SAFE_MARGIN - 14);
    return true;
}

static void update(const watchface_t *f, const face_data_t *d)
{
    const struct tm *tm = d->tm;
    static const char *const wd[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    if (tm) {
        lv_label_set_text_fmt(s_m_hours, "%02d", face_display_hour(d));
        lv_label_set_text_fmt(s_m_mins, "%02d", tm->tm_min);
        lv_label_set_text_fmt(s_m_secs, "%02d", tm->tm_sec);
        lv_label_set_text(s_m_ampm, d->use_24h ? "" : tm->tm_hour < 12 ? "AM" : "PM");
        lv_label_set_text_fmt(s_m_date, "%s %d %.3s", wd[tm->tm_wday], tm->tm_mday,
                              g_face_months[tm->tm_mon]);
    } else {
        lv_label_set_text(s_m_hours, "--");
        lv_label_set_text(s_m_ampm, "");
        lv_label_set_text(s_m_mins, "--");
        lv_label_set_text(s_m_secs, "--");
        lv_label_set_text(s_m_date, "Time not set");
    }
}

const watchface_t g_face_digital = {
    .id       = "digital",
    .name     = "Digital",
    .desc     = "Hours, minutes and seconds",
    .create   = create,
    .update   = update,
};
