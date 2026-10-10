/**
 * @file ui/screen_nav.c
 * @brief Navigation: turn-by-turn directions from the phone.
 *
 * Gadgetbridge forwards the route from CoMaps, Google Maps or OsmAnd as "nav"
 * messages: the next turn, the distance to it, the street and the arrival
 * time.  This shows them as a big arrow and the distance counting down.
 *
 * The page opens by itself when navigation starts, and the watch wakes and
 * beeps as each turn comes near.  It closes again when navigation stops.
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "ui/ui_internal.h"
#include "hal/audio.h"
#include "lvgl_port.h"

#define ALERT_M        150    // wake and beep this close to a turn
#define ARROW_PX       64

static ui_nav_info_t s_nav;         // latest route state
static bool          s_active;      // a route is being followed
static char          s_alerted[sizeof(s_nav.action) + sizeof(s_nav.instr)];  // turn already announced

static lv_obj_t *s_root;            // page content while it's open
static lv_obj_t *s_arrow, *s_dist, *s_instr, *s_eta, *s_bar, *s_idle;

/* "350 m", "1.2 km", "0,4 mi", "500 ft" → metres; -1 if it can't be read */
static int32_t distance_m(const char *s)
{
    char buf[sizeof(s_nav.distance)];
    size_t n = 0;
    for (; *s && n < sizeof(buf) - 1; s++) buf[n++] = *s == ',' ? '.' : *s;
    buf[n] = '\0';

    char *end;
    double v = strtod(buf, &end);
    if (end == buf) return -1;
    while (*end == ' ') end++;
    char u0 = (char)tolower((unsigned char)end[0]);
    char u1 = (char)tolower((unsigned char)end[1]);
    if (u0 == 'k')              v *= 1000.0;     // km
    else if (u0 == 'm' && u1 == 'i') v *= 1609.3; // mi
    else if (u0 == 'f')         v *= 0.3048;     // ft
    else if (u0 == 'y')         v *= 0.9144;     // yd
    return (int32_t)v;
}

static void format_left(char *out, size_t len, int32_t s)
{
    int32_t min = (s + 59) / 60;
    if (min < 60) snprintf(out, len, "%ld min", (long)min);
    else          snprintf(out, len, "%ld h %02ld", (long)(min / 60), (long)(min % 60));
}

/* Fill the page from s_nav (LVGL locked) */
static void refresh(void)
{
    if (!s_root) return;

    lv_obj_set_hidden(s_idle, s_active);
    lv_obj_set_hidden(s_arrow, !s_active);
    lv_obj_set_hidden(s_dist, !s_active);
    lv_obj_set_hidden(s_instr, !s_active);
    lv_obj_set_hidden(s_eta, !s_active);
    lv_obj_set_hidden(s_bar, !s_active || s_nav.pct < 0);
    if (!s_active) return;

    const lv_image_dsc_t *img = ui_icon_nav(s_nav.action);
    lv_image_set_src(s_arrow, img ? img : ui_icon_nav("continue"));
    lv_obj_set_style_image_recolor(s_arrow,
        strcmp(s_nav.action, "offroute") == 0 ? UI_COLOR_WARN : UI_COLOR_ACCENT, 0);

    bool finish = strcmp(s_nav.action, "finish") == 0;
    lv_label_set_text(s_dist, s_nav.distance[0] ? s_nav.distance : (finish ? "Arrived" : ""));
    lv_obj_set_hidden(s_dist, !lv_label_get_text(s_dist)[0]);
    lv_label_set_text(s_instr, s_nav.instr[0] ? s_nav.instr
                             : strcmp(s_nav.action, "offroute") == 0 ? "Off route" : "");

    char left[16] = "", eta[48];
    if (s_nav.left_s > 0 && !finish) format_left(left, sizeof(left), s_nav.left_s);
    if (s_nav.eta[0] && left[0]) snprintf(eta, sizeof(eta), "Arrive %s  (%s)", s_nav.eta, left);
    else if (s_nav.eta[0])       snprintf(eta, sizeof(eta), "Arrive %s", s_nav.eta);
    else                         snprintf(eta, sizeof(eta), "%s", left);
    lv_label_set_text(s_eta, eta);

    if (s_nav.pct >= 0) lv_bar_set_value(s_bar, s_nav.pct, LV_ANIM_OFF);
}

static void page_deleted_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    s_root = NULL;
}

static lv_obj_t *centred_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, "");
    return l;
}

/* Build the page (LVGL locked) */
static void page_open(void)
{
    lv_obj_t *c = ui_page_open("Navigation", NULL);
    lv_obj_add_event_cb(c, page_deleted_cb, LV_EVENT_DELETE, NULL);
    lv_obj_set_style_pad_row(c, 4, 0);
    lv_obj_set_scrollable(c, false);
    s_root = c;

    s_arrow = ui_icon_create(c, ui_icon_nav("continue"), UI_COLOR_ACCENT);
    lv_obj_set_size(s_arrow, ARROW_PX, ARROW_PX);

    s_dist = centred_label(c, &lv_font_montserrat_36, UI_COLOR_TEXT);
    lv_label_set_long_mode(s_dist, LV_LABEL_LONG_MODE_DOTS);

    s_instr = centred_label(c, &lv_font_montserrat_20, UI_COLOR_TEXT2);
    lv_label_set_long_mode(s_instr, LV_LABEL_LONG_MODE_DOTS);     // two lines, then "…"
    lv_obj_set_height(s_instr, 2 * lv_font_get_line_height(&lv_font_montserrat_20));

    s_eta = centred_label(c, &lv_font_montserrat_16, UI_COLOR_DIM);
    lv_label_set_long_mode(s_eta, LV_LABEL_LONG_MODE_DOTS);

    s_bar = lv_bar_create(c);
    lv_obj_set_size(s_bar, LV_PCT(80), 4);
    lv_bar_set_range(s_bar, 0, 100);
    lv_obj_set_style_bg_color(s_bar, UI_COLOR_SURFACE2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, UI_COLOR_ACCENT, LV_PART_INDICATOR);

    /* Shown when there's no route */
    s_idle = lv_obj_create(c);
    lv_obj_remove_style_all(s_idle);
    lv_obj_set_size(s_idle, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_idle, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_idle, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(s_idle, 24, 0);
    lv_obj_set_style_pad_row(s_idle, 14, 0);
    ui_icon_create(s_idle, ui_icon_navigate_large(), UI_COLOR_DIM);
    lv_obj_t *t = centred_label(s_idle, &lv_font_montserrat_20, UI_COLOR_TEXT);
    lv_label_set_text(t, "No route");
    lv_obj_t *hint = centred_label(s_idle, &lv_font_montserrat_14, UI_COLOR_DIM);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(hint, "Start navigation in CoMaps or Google Maps on your phone.");

    refresh();
}

void screen_nav_open(void)
{
    page_open();
}

void ui_bt_nav(const ui_nav_info_t *nav)
{
    bool stopped = !nav->action[0] && !nav->instr[0] && !nav->distance[0];
    int beeps = 0;

    lvgl_lock();
    if (stopped) {
        bool was = s_active;
        s_active = false;
        s_alerted[0] = '\0';
        if (was && s_root) ui_page_close();        // route over: leave the page
        lvgl_unlock();
        return;
    }

    bool started = !s_active;
    s_nav = *nav;
    s_active = true;

    /* Announce each turn once, when it first comes within ALERT_M, or the
     * arrival / leaving the route straight away */
    char key[sizeof(s_alerted)];
    snprintf(key, sizeof(key), "%s|%s", nav->action, nav->instr);
    int32_t m = distance_m(nav->distance);
    bool urgent = strcmp(nav->action, "finish") == 0 || strcmp(nav->action, "offroute") == 0;
    if (strcmp(key, s_alerted) != 0 && (urgent || (m >= 0 && m <= ALERT_M))) {
        snprintf(s_alerted, sizeof(s_alerted), "%s", key);
        beeps = urgent ? 3 : 2;
    }
    if (started && !beeps) beeps = 1;

    if (beeps && !s_root) page_open();             // bring the directions up
    else refresh();
    lvgl_unlock();

    if (beeps) {
        ui_screen_wake();
        audio_beep(2000, 70, 90, (uint8_t)beeps);
    }
}
