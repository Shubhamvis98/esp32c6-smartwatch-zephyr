/**
 * @file ui/screen_phone.c
 * @brief Phone app (App menu → Phone): call from the watch through the phone.
 *
 *   Phone                    Keypad                 Call
 *   [ ⌨ Keypad          ]    +1 555 0199            Mom
 *   Favourites               [1][2][3]              +15550199
 *   [ Mom   +1555… ]         [4][5][6]               ( ✆ )
 *   Recent                   [7][8][9]              Cancel
 *   [ ↓ Mom      14:05 ]     [+][0][⌫]
 *   [ ↑ +1555…   Mon   ]     [   ✆ Call   ]
 *
 * The watch has no modem: it asks Gadgetbridge ({t:"dial"}) to place the
 * call on the phone, and the conversation happens on the phone (or a
 * headset).  Favourites are set in Gadgetbridge → device settings →
 * Contacts; recent calls are recorded by the watch (app/contacts.c).
 */

#include "ui/ui_internal.h"
#include "app/contacts.h"
#include "app/gadgetbridge.h"
#include "app/settings.h"
#include "hal/rtc.h"
#include "lvgl_port.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define NUMBER_MAX   (CONTACT_NUMBER_LEN - 1)
#define COLOR_CALL   UI_COLOR_ACCENT
#define COLOR_BUTTON UI_COLOR_RAISED

static contact_t     s_favs[CONTACTS_MAX];
static int           s_fav_count;
static recent_call_t s_recents[RECENTS_MAX];
static int           s_recent_count;
static uint32_t      s_shown_gen;
static bool          s_shown_connected;
static bool          s_reopen_queued;

static char s_call_name[CONTACT_NAME_LEN];
static char s_call_number[CONTACT_NUMBER_LEN];
static char s_keypad[CONTACT_NUMBER_LEN];

static lv_obj_t *s_keypad_lbl;
static lv_obj_t *s_keypad_call;
static lv_obj_t *s_dial_status;          // on the Calling page, NULL elsewhere

static void main_open(void);
static void keypad_open(void);

/* ── Helpers ─────────────────────────────────────────────────────────────── */
static lv_obj_t *dim_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, UI_COLOR_DIM, 0);
    return l;
}

/* One line, "…" if too long: DOTS needs a fixed height */
static lv_obj_t *line_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(l, LV_PCT(100), lv_font_get_line_height(font));
    return l;
}

static lv_obj_t *phone_icon(lv_obj_t *parent, lv_color_t color)
{
    return ui_icon_create(parent, ui_icon_phone(), color);
}

static void recent_time(const recent_call_t *r, char *buf, size_t len)
{
    buf[0] = 0;
    if (!r->when || !rtc_time_valid()) return;
    time_t now = time(NULL);
    struct tm t, n;
    localtime_r(&r->when, &t);
    localtime_r(&now, &n);
    if (t.tm_year == n.tm_year && t.tm_yday == n.tm_yday) {
        if (settings_get()->use_24h) snprintf(buf, len, "%02d:%02d", t.tm_hour, t.tm_min);
        else snprintf(buf, len, "%d:%02d%s", t.tm_hour % 12 ? t.tm_hour % 12 : 12, t.tm_min,
                      t.tm_hour < 12 ? "a" : "p");
    } else if (now - r->when < 6 * 24 * 3600) {
        static const char *const wd[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
        snprintf(buf, len, "%s", wd[t.tm_wday]);
    } else {
        snprintf(buf, len, "%d/%d", t.tm_mday, t.tm_mon + 1);
    }
}

/* ── Calling (after asking the phone) ────────────────────────────────────── */
static void end_cb(lv_event_t *e)
{
    gb_send_call("END");
    main_open();
}

static void calling_deleted_cb(lv_event_t *e)
{
    s_dial_status = NULL;
}

static void calling_open(void)
{
    lv_obj_t *c = ui_page_open("Calling", NULL);
    ui_page_set_back(main_open);
    lv_obj_add_event_cb(c, calling_deleted_cb, LV_EVENT_DELETE, NULL);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *n = line_label(c, s_call_name[0] ? s_call_name : s_call_number,
                             &lv_font_montserrat_24, UI_COLOR_TEXT);
    lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
    if (s_call_name[0]) {
        lv_obj_t *num = line_label(c, s_call_number, &lv_font_montserrat_16, UI_COLOR_DIM);
        lv_obj_set_style_text_align(num, LV_TEXT_ALIGN_CENTER, 0);
    }

    s_dial_status = dim_label(c, "Calling on your phone...\nTalk on the phone or a headset.");
    lv_obj_set_style_text_align(s_dial_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_dial_status, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_pad_ver(s_dial_status, 2, 0);

    lv_obj_t *end = lv_button_create(c);
    lv_obj_set_size(end, 56, 56);
    lv_obj_set_style_radius(end, LV_RADIUS_CIRCLE, 0);
    ui_fill(end, UI_COLOR_DANGER);
    lv_obj_set_style_shadow_width(end, 0, 0);
    lv_obj_add_event_cb(end, end_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ic = phone_icon(end, UI_COLOR_TEXT);
    lv_image_set_rotation(ic, 1350);                 // hung-up handset
    lv_obj_center(ic);

}

void ui_phone_dial_error(const char *error)
{
    lvgl_lock();
    if (s_dial_status) {
        lv_label_set_text_fmt(s_dial_status, "Couldn't call:\n%s", *error ? error : "unknown error");
        lv_obj_set_style_text_color(s_dial_status, UI_COLOR_DANGER, 0);
    }
    lvgl_unlock();
}

/* Gadgetbridge says the other side picked up (C6 Watch device type) */
void ui_phone_call_answered(void)
{
    if (!s_dial_status) return;
    lv_label_set_text(s_dial_status, "Picked up\nTalk on your phone.");
    lv_obj_set_style_text_color(s_dial_status, COLOR_CALL, 0);
}

static void dial(const char *name, const char *number)
{
    snprintf(s_call_name, sizeof(s_call_name), "%s", name);
    snprintf(s_call_number, sizeof(s_call_number), "%s", number);
    if (!s_call_name[0]) contacts_name_for(number, s_call_name, sizeof(s_call_name));

    bool sent = gb_dial(number);
    if (sent) recents_add(s_call_name, number, true);
    calling_open();
    if (!sent) ui_phone_dial_error("Gadgetbridge isn't connected");
}

/* ── Confirm ─────────────────────────────────────────────────────────────── */
static void confirm_call_cb(lv_event_t *e) { dial(s_call_name, s_call_number); }
static void confirm_cancel_cb(lv_event_t *e) { main_open(); }

static void confirm_open(const char *name, const char *number)
{
    snprintf(s_call_name, sizeof(s_call_name), "%s", name);
    snprintf(s_call_number, sizeof(s_call_number), "%s", number);

    lv_obj_t *c = ui_page_open("Call", NULL);
    ui_page_set_back(main_open);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *n = line_label(c, *name ? name : number, &lv_font_montserrat_24, UI_COLOR_TEXT);
    lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
    if (*name) {
        lv_obj_t *num = line_label(c, number, &lv_font_montserrat_16, UI_COLOR_DIM);
        lv_obj_set_style_text_align(num, LV_TEXT_ALIGN_CENTER, 0);
    }

    lv_obj_t *call = lv_button_create(c);
    lv_obj_set_size(call, 72, 72);
    lv_obj_set_style_margin_top(call, 10, 0);
    lv_obj_set_style_radius(call, LV_RADIUS_CIRCLE, 0);
    ui_fill(call, COLOR_CALL);
    lv_obj_set_style_shadow_width(call, 0, 0);
    lv_obj_add_event_cb(call, confirm_call_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ic = phone_icon(call, UI_COLOR_TEXT);
    lv_image_set_scale(ic, LV_SCALE_NONE * 3 / 2);
    lv_obj_center(ic);

    lv_obj_t *cancel = lv_button_create(c);
    lv_obj_set_style_bg_opa(cancel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(cancel, 0, 0);
    lv_obj_add_event_cb(cancel, confirm_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(cancel);
    lv_label_set_text(l, "Cancel");
    lv_obj_set_style_text_color(l, UI_COLOR_DIM, 0);
}

/* ── Keypad ──────────────────────────────────────────────────────────────── */
static void keypad_show(void)
{
    lv_label_set_text(s_keypad_lbl, s_keypad[0] ? s_keypad : "Enter number");
    lv_obj_set_style_text_color(s_keypad_lbl, s_keypad[0] ? UI_COLOR_TEXT : UI_COLOR_DIM, 0);
    lv_obj_set_style_text_font(s_keypad_lbl, strlen(s_keypad) > 12 ? &lv_font_montserrat_20
                                                                    : &lv_font_montserrat_24, 0);
    lv_obj_set_state(s_keypad_call, LV_STATE_DISABLED, !s_keypad[0]);
}

static void key_cb(lv_event_t *e)
{
    lv_obj_t *m = lv_event_get_target_obj(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(m);
    const char *txt = lv_buttonmatrix_get_button_text(m, id);
    if (!txt) return;
    size_t n = strlen(s_keypad);
    if (strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) {
        if (n) s_keypad[n - 1] = 0;
    } else if (n < NUMBER_MAX && !(txt[0] == '+' && n)) {   // + only first
        s_keypad[n] = txt[0];
        s_keypad[n + 1] = 0;
    }
    keypad_show();
}

/* Holding ⌫ clears the number */
static void key_long_cb(lv_event_t *e)
{
    lv_obj_t *m = lv_event_get_target_obj(e);
    const char *txt = lv_buttonmatrix_get_button_text(m, lv_buttonmatrix_get_selected_button(m));
    if (txt && strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) {
        s_keypad[0] = 0;
        keypad_show();
    }
}

static void keypad_call_cb(lv_event_t *e)
{
    if (s_keypad[0]) dial("", s_keypad);
}

static void keypad_open(void)
{
    lv_obj_t *c = ui_page_open("Keypad", NULL);
    ui_page_set_back(main_open);
    lv_obj_set_style_pad_row(c, 6, 0);

    s_keypad_lbl = lv_label_create(c);
    lv_obj_set_size(s_keypad_lbl, LV_PCT(100), 30);
    lv_obj_set_style_text_align(s_keypad_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_keypad_lbl, LV_LABEL_LONG_MODE_CLIP);

    static const char *const map[] = {
        "1", "2", "3", "\n",
        "4", "5", "6", "\n",
        "7", "8", "9", "\n",
        "+", "0", LV_SYMBOL_BACKSPACE, "",
    };
    lv_obj_t *m = lv_buttonmatrix_create(c);
    lv_buttonmatrix_set_map(m, (const char **)map);
    lv_obj_set_size(m, LV_PCT(100), 4 * 34 + 3 * 5);
    lv_obj_set_style_pad_all(m, 0, 0);
    lv_obj_set_style_pad_gap(m, 5, 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(m, 10, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(m, UI_COLOR_SURFACE, LV_PART_ITEMS);
    lv_obj_set_style_text_color(m, UI_COLOR_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_text_font(m, &lv_font_montserrat_20, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(m, 0, LV_PART_ITEMS);
    lv_obj_add_event_cb(m, key_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(m, key_long_cb, LV_EVENT_LONG_PRESSED, NULL);

    s_keypad_call = lv_button_create(c);
    lv_obj_set_size(s_keypad_call, LV_PCT(100), 42);
    lv_obj_set_style_radius(s_keypad_call, 21, 0);
    ui_fill(s_keypad_call, COLOR_CALL);
    lv_obj_set_style_shadow_width(s_keypad_call, 0, 0);
    lv_obj_set_style_opa(s_keypad_call, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_set_flex_flow(s_keypad_call, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_keypad_call, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_keypad_call, 8, 0);
    lv_obj_add_event_cb(s_keypad_call, keypad_call_cb, LV_EVENT_CLICKED, NULL);
    phone_icon(s_keypad_call, UI_COLOR_TEXT);
    lv_obj_t *l = lv_label_create(s_keypad_call);
    lv_label_set_text(l, "Call");

    keypad_show();
}

/* ── Main page: keypad, favourites, recent calls ─────────────────────────── */
static void fav_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= 0 && i < s_fav_count) confirm_open(s_favs[i].name, s_favs[i].number);
}

static void recent_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= 0 && i < s_recent_count) confirm_open(s_recents[i].name, s_recents[i].number);
}

static void keypad_cb(lv_event_t *e) { keypad_open(); }

static void clear_cb(lv_event_t *e)
{
    recents_clear();
    main_open();
}

static void async_main_open(void *p)
{
    s_reopen_queued = false;
    main_open();
}

/* Rebuild when Gadgetbridge sends contacts, a call comes in, or the
 * connection changes (not from inside this page's own timer) */
static void main_update(void)
{
    if ((contacts_generation() != s_shown_gen || gb_is_connected() != s_shown_connected) &&
        !s_reopen_queued) {
        s_reopen_queued = true;
        lv_async_call(async_main_open, NULL);
    }
}

/* [icon] name / number [time] */
static void entry_row(lv_obj_t *parent, const char *name, const char *number, const char *icon,
                      lv_color_t icon_color, const char *right, lv_event_cb_t cb, int index)
{
    lv_obj_t *row = lv_button_create(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, 48, 0);
    ui_fill(row, UI_COLOR_SURFACE);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_pad_hor(row, 10, 0);
    lv_obj_set_style_pad_ver(row, 6, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);

    if (icon) {
        lv_obj_t *ic = lv_label_create(row);
        lv_label_set_text(ic, icon);
        lv_obj_set_style_text_font(ic, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(ic, icon_color, 0);
    }

    lv_obj_t *col = lv_obj_create(row);
    lv_obj_remove_style_all(col);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 1, 0);
    lv_obj_set_clickable(col, false);
    line_label(col, *name ? name : number, &lv_font_montserrat_16, UI_COLOR_TEXT);
    if (*name) line_label(col, number, &lv_font_montserrat_14, UI_COLOR_DIM);

    if (right && *right) {
        lv_obj_t *r = lv_label_create(row);
        lv_label_set_text(r, right);
        lv_obj_set_style_text_font(r, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(r, UI_COLOR_DIM, 0);
    }
}

static lv_obj_t *section(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = dim_label(parent, text);
    lv_obj_set_style_pad_left(l, 4, 0);
    lv_obj_set_style_pad_top(l, 4, 0);
    return l;
}

static void main_open(void)
{
    s_shown_gen = contacts_generation();
    s_shown_connected = gb_is_connected();
    s_fav_count = contacts_get(s_favs, CONTACTS_MAX);
    s_recent_count = recents_get(s_recents, RECENTS_MAX);

    lv_obj_t *c = ui_page_open("Phone", main_update);

    lv_obj_t *kp = lv_button_create(c);
    lv_obj_set_size(kp, LV_PCT(100), 44);
    lv_obj_set_style_radius(kp, 22, 0);
    ui_fill(kp, COLOR_CALL);
    lv_obj_set_style_shadow_width(kp, 0, 0);
    lv_obj_set_flex_flow(kp, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(kp, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(kp, 8, 0);
    lv_obj_add_event_cb(kp, keypad_cb, LV_EVENT_CLICKED, NULL);
    phone_icon(kp, UI_COLOR_TEXT);
    lv_obj_t *kl = lv_label_create(kp);
    lv_label_set_text(kl, "Keypad");

    if (!s_shown_connected) {
        lv_obj_t *w = dim_label(c, "Connect Gadgetbridge to make calls.");
        lv_obj_set_style_text_color(w, UI_COLOR_WARN, 0);
    }

    section(c, "Favourites");
    if (!s_fav_count) {
        dim_label(c, "Add them in Gadgetbridge: the watch's settings " LV_SYMBOL_RIGHT " Contacts.");
    }
    for (int i = 0; i < s_fav_count; i++) {
        entry_row(c, s_favs[i].name, s_favs[i].number, NULL, UI_COLOR_DIM, NULL, fav_cb, i);
    }

    if (s_recent_count) {
        section(c, "Recent");
        for (int i = 0; i < s_recent_count; i++) {
            char when[24];
            recent_time(&s_recents[i], when, sizeof(when));
            const recent_call_t *r = &s_recents[i];
            entry_row(c, r->name, r->number, r->outgoing ? LV_SYMBOL_UPLOAD : LV_SYMBOL_DOWNLOAD,
                      r->outgoing ? UI_COLOR_ACCENT : UI_COLOR_ACCENT2, when, recent_cb, i);
        }
        lv_obj_t *clr = lv_button_create(c);
        lv_obj_set_width(clr, LV_PCT(100));
        lv_obj_set_style_bg_opa(clr, LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(clr, 0, 0);
        lv_obj_add_event_cb(clr, clear_cb, LV_EVENT_LONG_PRESSED, NULL);
        lv_obj_t *l = lv_label_create(clr);
        lv_label_set_text(l, "Hold to clear recent calls");
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(l, UI_COLOR_DIM, 0);
        lv_obj_center(l);
    }
}

void screen_phone_open(void)
{
    s_keypad[0] = 0;
    main_open();
}
