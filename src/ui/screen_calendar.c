/**
 * @file ui/screen_calendar.c
 * @brief Calendar: upcoming events from the phone, and a reminder before each.
 *
 * Gadgetbridge sends the next days' events (Sync calendar in its C6 Watch
 * settings); app/calendar.c keeps them.  A reminder pops up REMIND_MIN before
 * a timed event starts, like a notification.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "ui/ui_internal.h"
#include "app/calendar.h"
#include "app/settings.h"
#include "lvgl_port.h"

#define REMIND_MIN      10
#define CHECK_MS        20000
#define REMINDED_MAX    8

static lv_obj_t *s_root;                     // page content while it's open
static int64_t   s_reminded[REMINDED_MAX];   // events already announced
static int       s_reminded_next;

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
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    return l;
}

static void format_time(char *out, size_t len, time_t t)
{
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, len, settings_get()->use_24h ? "%H:%M" : "%I:%M %p", &tm);
    if (!settings_get()->use_24h && out[0] == '0') memmove(out, out + 1, strlen(out));
}

/* "Today", "Tomorrow" or "Thu 12 Oct" */
static void day_heading(char *out, size_t len, time_t t, time_t now)
{
    struct tm a, b;
    localtime_r(&t, &a);
    localtime_r(&now, &b);
    b.tm_hour = b.tm_min = b.tm_sec = 0;
    a.tm_hour = a.tm_min = a.tm_sec = 0;
    long days = (long)((mktime(&a) - mktime(&b) + 43200) / 86400);
    if (days <= 0)      snprintf(out, len, "Today");
    else if (days == 1) snprintf(out, len, "Tomorrow");
    else                strftime(out, len, "%a %d %b", &a);
}

static void build(void)
{
    lv_obj_clean(s_root);
    static calendar_event_t ev[CALENDAR_MAX_EVENTS];
    size_t n = calendar_upcoming(ev, CALENDAR_MAX_EVENTS);
    time_t now = time(NULL);

    if (!n) {
        lv_obj_t *icon = ui_icon_create(s_root, ui_icon_calendar(), UI_COLOR_DIM);
        lv_image_set_scale(icon, LV_SCALE_NONE * 2);
        lv_obj_set_size(icon, 44, 44);
        lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CENTER);
        lv_obj_set_style_margin_top(icon, 16, 0);
        lv_obj_t *t = text(s_root, "No upcoming events", &lv_font_montserrat_20, UI_COLOR_TEXT);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_t *h = text(s_root, "Turn on Sync calendar in Gadgetbridge's C6 Watch settings.",
                           &lv_font_montserrat_14, UI_COLOR_DIM);
        lv_label_set_long_mode(h, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }

    char last_day[24] = "";
    for (size_t i = 0; i < n; i++) {
        char day[24];
        day_heading(day, sizeof(day), ev[i].start < now ? now : ev[i].start, now);
        if (strcmp(day, last_day) != 0) {
            text(s_root, day, &lv_font_montserrat_14, UI_COLOR_DIM);
            strcpy(last_day, day);
        }

        lv_obj_t *card = ui_card(s_root, NULL);
        lv_obj_set_style_pad_row(card, 2, 0);
        if (ev[i].color & 0xFFFFFF) {
            lv_obj_set_style_border_side(card, LV_BORDER_SIDE_LEFT, 0);
            lv_obj_set_style_border_width(card, 4, 0);
            lv_obj_set_style_border_color(card, lv_color_hex(ev[i].color & 0xFFFFFF), 0);
        }

        char when[48];
        if (ev[i].all_day) {
            snprintf(when, sizeof(when), "All day");
        } else {
            char a[12], b[12];
            format_time(a, sizeof(a), ev[i].start);
            format_time(b, sizeof(b), ev[i].start + ev[i].duration_s);
            if (ev[i].start <= now) snprintf(when, sizeof(when), "Now - until %s", b);
            else if (ev[i].duration_s > 0) snprintf(when, sizeof(when), "%s - %s", a, b);
            else snprintf(when, sizeof(when), "%s", a);
        }
        text(card, when, &lv_font_montserrat_14, UI_COLOR_ACCENT);
        lv_obj_t *t = text(card, ev[i].title[0] ? ev[i].title : "(no title)",
                           &lv_font_montserrat_16, UI_COLOR_TEXT);
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_max_height(t, 2 * lv_font_get_line_height(&lv_font_montserrat_16), 0);
        if (ev[i].location[0]) text(card, ev[i].location, &lv_font_montserrat_14, UI_COLOR_DIM);
    }
}

void screen_calendar_open(void)
{
    lv_obj_t *c = ui_page_open("Calendar", NULL);
    lv_obj_add_event_cb(c, page_deleted_cb, LV_EVENT_DELETE, NULL);
    s_root = c;
    build();
}

void ui_calendar_changed(void)
{
    lvgl_lock();
    if (s_root) build();
    lvgl_unlock();
}

/* ── Reminders ───────────────────────────────────────────────────────────── */
static bool reminded(int64_t id)
{
    for (int i = 0; i < REMINDED_MAX; i++) if (s_reminded[i] == id) return true;
    return false;
}

static void remind_timer_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    time_t now = time(NULL);
    if (now < 1600000000) return;                  // clock not set yet

    calendar_event_t ev[4];
    size_t n = calendar_upcoming(ev, 4);
    for (size_t i = 0; i < n; i++) {
        if (ev[i].all_day || reminded(ev[i].id)) continue;
        long mins = (long)((ev[i].start - now + 59) / 60);
        if (mins < 0 || mins > REMIND_MIN) continue;

        s_reminded[s_reminded_next] = ev[i].id;
        s_reminded_next = (s_reminded_next + 1) % REMINDED_MAX;

        char a[12], body[96];
        format_time(a, sizeof(a), ev[i].start);
        if (mins <= 0) snprintf(body, sizeof(body), "Now (%s)", a);
        else           snprintf(body, sizeof(body), "In %ld min, at %s", mins, a);
        if (ev[i].location[0]) {
            size_t l = strlen(body);
            snprintf(body + l, sizeof(body) - l, "\n%s", ev[i].location);
        }
        /* Shown like a phone notification; the id is negative so it can't
         * clash with one, and dismissing it does nothing on the phone */
        int32_t id = -(int32_t)((uint32_t)ev[i].id & 0x3FFFFFFF) - 1;
        ui_bt_notification(id, "Calendar", ev[i].title, body);   // LVGL lock is recursive
        break;                       // one at a time
    }
}

void screen_calendar_init(void)
{
    /* Events already under way at start-up aren't announced again */
    time_t now = time(NULL);
    calendar_event_t ev[4];
    size_t n = calendar_upcoming(ev, 4);
    for (size_t i = 0; i < n && i < REMINDED_MAX; i++) {
        if (now > 1600000000 && ev[i].start <= now) s_reminded[s_reminded_next++] = ev[i].id;
    }
    lv_timer_create(remind_timer_cb, CHECK_MS, NULL);
}
