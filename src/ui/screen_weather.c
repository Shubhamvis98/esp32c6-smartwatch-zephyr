/**
 * @file ui/screen_weather.c
 * @brief Weather: current conditions and a daily forecast from the phone.
 *
 * Gadgetbridge forwards the weather from a weather app on the phone (such as
 * Breezy Weather); app/weather.c keeps the latest copy.
 */

#include <stdio.h>
#include <time.h>
#include "ui/ui_internal.h"
#include "app/weather.h"
#include "app/settings.h"
#include "lvgl_port.h"

static lv_obj_t *s_root;            // page content while it's open

static void page_deleted_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    s_root = NULL;
}

static lv_obj_t *text(lv_obj_t *parent, const char *t, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}

static lv_obj_t *row(lv_obj_t *parent, lv_flex_align_t main_align)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, main_align, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_clickable(r, false);
    return r;
}

static void build(void)
{
    lv_obj_clean(s_root);
    weather_t w;
    weather_get(&w);

    if (!w.valid) {
        lv_obj_t *icon = ui_icon_create(s_root, ui_icon_weather(WEATHER_KIND_PARTLY, true), UI_COLOR_DIM);
        lv_obj_set_style_margin_top(icon, 16, 0);
        text(s_root, "No weather yet", &lv_font_montserrat_20, UI_COLOR_TEXT);
        lv_obj_t *h = text(s_root, "Install a weather app such as Breezy Weather on the phone "
                                   "and let it send weather to Gadgetbridge.",
                           &lv_font_montserrat_14, UI_COLOR_DIM);
        lv_obj_set_width(h, LV_PCT(100));
        lv_label_set_long_mode(h, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }

    /* Now: icon and temperature */
    lv_obj_t *now = row(s_root, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(now, 12, 0);
    ui_icon_create(now, ui_icon_weather(weather_kind(w.code), true), UI_COLOR_ACCENT);
    char buf[64];
    snprintf(buf, sizeof(buf), "%d°", w.temp);
    text(now, buf, &lv_font_montserrat_48, UI_COLOR_TEXT);

    lv_obj_t *cond = text(s_root, w.text[0] ? w.text : "", &lv_font_montserrat_20, UI_COLOR_TEXT2);
    lv_obj_set_width(cond, LV_PCT(100));
    lv_label_set_long_mode(cond, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(cond, LV_TEXT_ALIGN_CENTER, 0);

    snprintf(buf, sizeof(buf), "H %d°   L %d°", w.hi, w.lo);
    text(s_root, buf, &lv_font_montserrat_16, UI_COLOR_DIM);

    if (w.location[0]) {
        lv_obj_t *loc = text(s_root, w.location, &lv_font_montserrat_14, UI_COLOR_DIM);
        lv_obj_set_width(loc, LV_PCT(100));
        lv_label_set_long_mode(loc, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(loc, LV_TEXT_ALIGN_CENTER, 0);
    }

    /* Details */
    lv_obj_t *card = ui_card(s_root, NULL);
    static const char *const names[3] = { "Rain", "Humidity", "Wind" };
    char vals[3][16];
    snprintf(vals[0], sizeof(vals[0]), "%u%%", w.rain);
    snprintf(vals[1], sizeof(vals[1]), "%u%%", w.humidity);
    snprintf(vals[2], sizeof(vals[2]), "%.0f km/h", (double)w.wind_kmh);
    for (int i = 0; i < 3; i++) {
        lv_obj_t *r = row(card, LV_FLEX_ALIGN_SPACE_BETWEEN);
        text(r, names[i], &lv_font_montserrat_16, UI_COLOR_DIM);
        text(r, vals[i], &lv_font_montserrat_16, UI_COLOR_TEXT);
    }

    /* Forecast, from tomorrow */
    if (w.days) {
        card = ui_card(s_root, "Next days");
        time_t base = w.updated > 1600000000 ? w.updated : time(NULL);
        for (int i = 0; i < w.days; i++) {
            time_t t = base + (time_t)(i + 1) * 24 * 3600;
            struct tm tm;
            localtime_r(&t, &tm);
            char day[8];
            strftime(day, sizeof(day), "%a", &tm);

            lv_obj_t *r = row(card, LV_FLEX_ALIGN_START);
            lv_obj_set_style_pad_column(r, 10, 0);
            lv_obj_t *d = text(r, day, &lv_font_montserrat_16, UI_COLOR_TEXT);
            lv_obj_set_width(d, 44);
            ui_icon_create(r, ui_icon_weather(weather_kind(w.day[i].code), false), UI_COLOR_TEXT2);
            snprintf(buf, sizeof(buf), "%d° / %d°", w.day[i].hi, w.day[i].lo);
            lv_obj_t *hl = text(r, buf, &lv_font_montserrat_16, UI_COLOR_TEXT2);
            lv_obj_set_flex_grow(hl, 1);
            lv_obj_set_style_text_align(hl, LV_TEXT_ALIGN_RIGHT, 0);
        }
    }

    if (w.updated > 1600000000) {
        struct tm tm;
        localtime_r(&w.updated, &tm);
        char t[24];
        strftime(t, sizeof(t), settings_get()->use_24h ? "Updated %H:%M" : "Updated %I:%M %p", &tm);
        text(s_root, t, &lv_font_montserrat_14, UI_COLOR_DIM);
    }
}

void screen_weather_open(void)
{
    lv_obj_t *c = ui_page_open("Weather", NULL);
    lv_obj_add_event_cb(c, page_deleted_cb, LV_EVENT_DELETE, NULL);
    s_root = c;
    build();
}

void ui_weather_changed(void)
{
    lvgl_lock();
    if (s_root) build();
    lvgl_unlock();
}
