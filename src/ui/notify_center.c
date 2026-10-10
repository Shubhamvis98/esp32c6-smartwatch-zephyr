/**
 * @file ui/notify_center.c
 * @brief Notification center: swipe up on the watchface for the phone's
 *        notifications and their history.
 *
 *   swipe up on the watchface
 *   ┌──────────────────────────┐
 *   │           ────           │  ← swipe down / tap to close
 *   │       Notifications      │
 *   │ ON PHONE                 │
 *   │ ┌──────────────────────┐ │
 *   │ │ WhatsApp       10:42 │ │  tap a card for the full text,
 *   │ │ Alice                │ │  Dismiss (on the phone) and Delete
 *   │ │ See you at 6?        │ │
 *   │ └──────────────────────┘ │
 *   │ EARLIER                  │
 *   │ …                        │
 *   │       [ Clear all ]      │
 *   └──────────────────────────┘
 *
 * Every notification from Gadgetbridge is kept here (newest first, up to
 * NC_MAX).  "On phone" ones are still showing on Android; once dismissed on
 * either side they move to "Earlier".  The list is saved to flash, so the
 * history survives a restart (everything loads as "Earlier", since the
 * phone's state isn't known then).
 *
 * All access happens under the LVGL lock: Gadgetbridge calls in through
 * notify_popup.c, which holds it.
 */

#include "ui/ui_internal.h"
#include "app/gadgetbridge.h"
#include "app/settings.h"
#include "hal/storage.h"

#include "esp_log.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define NC_MAX       20
#define NC_FILE      STORAGE_PATH "/notify.bin"
#define NC_MAGIC     0x4E43            // "NC"
#define NC_VERSION   1
#define ANIM_MS      220

static const char *TAG = "notify_center";

typedef struct {
    int32_t id;
    int64_t when;               // time() when it arrived
    bool    active;             // still showing on the phone
    char    src[24];            // app name
    char    title[64];
    char    body[300];
} notif_t;

typedef struct {
    uint16_t magic;
    uint16_t version;
    uint16_t count;
    uint16_t entry_size;
} nc_header_t;

static notif_t s_items[NC_MAX];      // newest first
static int     s_count;
static bool    s_loaded;

static lv_obj_t *s_root;             // full-screen panel, NULL while closed
static lv_obj_t *s_list;
static bool      s_open;

static void rebuild(void);

/* ── Store ───────────────────────────────────────────────────────────────── */
/* Copy UTF-8 without cutting a multi-byte character in half */
static void utf8_copy(char *dst, size_t len, const char *src)
{
    size_t n = strlen(src);
    if (n >= len) {
        n = len - 1;
        while (n > 0 && ((uint8_t)src[n] & 0xC0) == 0x80) n--;   // back to a lead byte
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void load(void)
{
    s_loaded = true;
    FILE *f = fopen(NC_FILE, "rb");
    if (!f) return;
    nc_header_t h;
    if (fread(&h, sizeof(h), 1, f) == 1 && h.magic == NC_MAGIC && h.version == NC_VERSION &&
        h.entry_size == sizeof(notif_t) && h.count <= NC_MAX) {
        s_count = (int)fread(s_items, sizeof(notif_t), h.count, f);
        for (int i = 0; i < s_count; i++) s_items[i].active = false;   // phone state unknown
    }
    fclose(f);
    ESP_LOGI(TAG, "Loaded %d notifications", s_count);
}

static void save(void)
{
    FILE *f = fopen(NC_FILE, "wb");
    if (!f) {
        ESP_LOGW(TAG, "Can't write %s", NC_FILE);
        return;
    }
    nc_header_t h = { NC_MAGIC, NC_VERSION, (uint16_t)s_count, sizeof(notif_t) };
    fwrite(&h, sizeof(h), 1, f);
    fwrite(s_items, sizeof(notif_t), s_count, f);
    fclose(f);
}

static int find(int32_t id)
{
    for (int i = 0; i < s_count; i++) {
        if (s_items[i].id == id) return i;
    }
    return -1;
}

static void remove_at(int i)
{
    memmove(&s_items[i], &s_items[i + 1], (s_count - i - 1) * sizeof(notif_t));
    s_count--;
}

static void changed(void)
{
    save();
    if (s_open) rebuild();
}

void notify_center_add(int32_t id, const char *src, const char *title, const char *body)
{
    if (!s_loaded) load();

    /* An update to a notification (same id) replaces it and moves it to the top */
    int i = find(id);
    if (i >= 0) remove_at(i);
    if (s_count == NC_MAX) s_count--;                 // drop the oldest
    memmove(&s_items[1], &s_items[0], s_count * sizeof(notif_t));
    s_count++;

    notif_t *n = &s_items[0];
    memset(n, 0, sizeof(*n));
    n->id = id;
    n->when = (int64_t)time(NULL);
    n->active = true;
    utf8_copy(n->src, sizeof(n->src), src);
    utf8_copy(n->title, sizeof(n->title), title);
    utf8_copy(n->body, sizeof(n->body), body);
    changed();
}

void notify_center_set_inactive(int32_t id)
{
    if (!s_loaded) load();
    int i = find(id);
    if (i < 0 || !s_items[i].active) return;
    s_items[i].active = false;
    changed();
}

/* ── Formatting ──────────────────────────────────────────────────────────── */
/* "10:42" today, "Mon 10:42" this week, "3 Oct" before that */
static void format_when(char *buf, size_t len, int64_t when)
{
    time_t now = time(NULL), t = (time_t)when;
    struct tm tn, tw;
    localtime_r(&now, &tn);
    localtime_r(&t, &tw);

    char hm[12];
    if (settings_get()->use_24h) {
        strftime(hm, sizeof(hm), "%H:%M", &tw);
    } else {
        int h = tw.tm_hour % 12;
        snprintf(hm, sizeof(hm), "%d:%02d %s", h ? h : 12, tw.tm_min, tw.tm_hour < 12 ? "AM" : "PM");
    }

    bool same_day = tn.tm_year == tw.tm_year && tn.tm_yday == tw.tm_yday;
    if (same_day)                        snprintf(buf, len, "%s", hm);
    else if (now - t < 6 * 24 * 3600L && t <= now) {
        char d[8];
        strftime(d, sizeof(d), "%a", &tw);
        snprintf(buf, len, "%s %s", d, hm);
    } else                               strftime(buf, len, "%e %b", &tw);
}

/* ── Detail page ─────────────────────────────────────────────────────────── */
static int32_t s_detail_id;

static void detail_dismiss_cb(lv_event_t *e)
{
    int i = find(s_detail_id);
    if (i >= 0 && s_items[i].active) {
        if (s_detail_id >= 0) gb_send_notify_action(s_detail_id, "DISMISS");   // < 0: made on the watch
        s_items[i].active = false;
        changed();
    }
    ui_page_close();
}

static void detail_delete_cb(lv_event_t *e)
{
    int i = find(s_detail_id);
    if (i >= 0) {
        remove_at(i);
        changed();
    }
    ui_page_close();
}

static lv_obj_t *page_button(lv_obj_t *parent, const char *text, lv_color_t color, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_height(btn, 44);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_radius(btn, 22, 0);
    ui_fill(btn, color);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return btn;
}

static void open_detail(int i)
{
    const notif_t *n = &s_items[i];
    s_detail_id = n->id;

    lv_obj_t *c = ui_page_open(n->src[0] ? n->src : "Notification", NULL);

    char when[24];
    format_when(when, sizeof(when), n->when);
    lv_obj_t *w = lv_label_create(c);
    lv_obj_set_width(w, LV_PCT(100));
    lv_label_set_text_fmt(w, "%s%s", when, n->active ? ", still on phone" : "");
    lv_obj_set_style_text_font(w, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(w, UI_COLOR_DIM, 0);

    if (n->title[0]) {
        lv_obj_t *t = lv_label_create(c);
        lv_obj_set_width(t, LV_PCT(100));
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
        lv_label_set_text(t, n->title);
    }
    if (n->body[0]) {
        lv_obj_t *b = lv_label_create(c);
        lv_obj_set_width(b, LV_PCT(100));
        lv_label_set_long_mode(b, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_font(b, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(b, UI_COLOR_TEXT2, 0);
        lv_label_set_text(b, n->body);
    }

    lv_obj_t *row = lv_obj_create(c);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_style_pad_top(row, 6, 0);
    if (n->active) page_button(row, "Dismiss", UI_COLOR_SURFACE, detail_dismiss_cb);
    page_button(row, "Delete", UI_COLOR_DANGER_DIM, detail_delete_cb);
}

/* ── Panel ───────────────────────────────────────────────────────────────── */
static void card_cb(lv_event_t *e)
{
    int32_t id = (int32_t)(intptr_t)lv_event_get_user_data(e);
    int i = find(id);
    if (i >= 0) open_detail(i);
}

static void clear_all_cb(lv_event_t *e)
{
    s_count = 0;
    changed();
}

static void handle_cb(lv_event_t *e)
{
    notify_center_close(true);
}

static void section_label(const char *text)
{
    lv_obj_t *l = lv_label_create(s_list);
    lv_label_set_text(l, text);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, UI_COLOR_DIM, 0);
    lv_obj_set_style_pad_left(l, 4, 0);
    lv_obj_set_style_pad_top(l, 4, 0);
}

static void add_card(const notif_t *n)
{
    lv_obj_t *card = lv_obj_create(s_list);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, UI_COLOR_RAISED, LV_STATE_PRESSED);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_style_pad_row(card, 2, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(card, false);
    lv_obj_add_event_cb(card, card_cb, LV_EVENT_CLICKED, (void *)(intptr_t)n->id);
    if (!n->active) lv_obj_set_style_opa(card, LV_OPA_80, 0);

    /* App name and time on one line: the time keeps its width and the
     * app name is cut short with "…" to fit what's left */
    lv_obj_t *top = lv_obj_create(card);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top, 8, 0);
    lv_obj_set_clickable(top, false);
    lv_obj_t *app = lv_label_create(top);
    lv_label_set_text(app, n->src[0] ? n->src : "Notification");
    lv_label_set_long_mode(app, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(app, 0);                     // takes the space the time leaves
    lv_obj_set_height(app, lv_font_get_line_height(&lv_font_montserrat_14));   // one line
    lv_obj_set_flex_grow(app, 1);
    lv_obj_set_style_text_font(app, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(app, n->active ? UI_COLOR_ACCENT : UI_COLOR_DIM, 0);
    char when[24];
    format_when(when, sizeof(when), n->when);
    lv_obj_t *w = lv_label_create(top);
    lv_label_set_text(w, when);
    lv_obj_set_style_text_font(w, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(w, UI_COLOR_DIM, 0);

    if (n->title[0]) {
        lv_obj_t *t = lv_label_create(card);
        lv_obj_set_width(t, LV_PCT(100));
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(t, lv_font_get_line_height(&lv_font_montserrat_16));   // one line
        lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
        lv_label_set_text(t, n->title);
    }
    if (n->body[0]) {
        /* At most two lines of the body */
        lv_obj_t *b = lv_label_create(card);
        lv_obj_set_width(b, LV_PCT(100));
        lv_label_set_long_mode(b, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_font(b, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(b, UI_COLOR_DIM, 0);
        lv_obj_set_style_max_height(b, 2 * lv_font_get_line_height(&lv_font_montserrat_14), 0);
        lv_label_set_text(b, n->body);
    }
}

static void rebuild(void)
{
    if (!s_list) return;
    int32_t scroll = lv_obj_get_scroll_y(s_list);
    lv_obj_clean(s_list);

    lv_obj_t *title = lv_label_create(s_list);
    lv_label_set_text(title, "Notifications");
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_pad_bottom(title, 4, 0);

    if (s_count == 0) {
        lv_obj_t *e = lv_label_create(s_list);
        lv_obj_set_width(e, LV_PCT(100));
        lv_obj_set_style_text_align(e, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(e, UI_COLOR_DIM, 0);
        lv_obj_set_style_pad_top(e, 40, 0);
        lv_label_set_text(e, gb_is_connected() ? "No notifications"
                                               : "No notifications\n\nConnect Gadgetbridge\nto receive them");
        return;
    }

    bool any_active = false, any_old = false;
    for (int i = 0; i < s_count; i++) {
        if (s_items[i].active) any_active = true;
        else                   any_old = true;
    }
    if (any_active) {
        section_label("ON PHONE");
        for (int i = 0; i < s_count; i++) if (s_items[i].active) add_card(&s_items[i]);
    }
    if (any_old) {
        section_label("EARLIER");
        for (int i = 0; i < s_count; i++) if (!s_items[i].active) add_card(&s_items[i]);
    }

    lv_obj_t *clear = lv_button_create(s_list);
    lv_obj_set_size(clear, 130, 40);
    lv_obj_set_style_radius(clear, 20, 0);
    ui_fill(clear, UI_COLOR_SURFACE);
    lv_obj_set_style_shadow_width(clear, 0, 0);
    lv_obj_set_style_margin_top(clear, 6, 0);
    lv_obj_add_event_cb(clear, clear_all_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(clear);
    lv_label_set_text(cl, "Clear all");
    lv_obj_center(cl);

    lv_obj_update_layout(s_list);
    lv_obj_scroll_to_y(s_list, scroll, LV_ANIM_OFF);
}

static void anim_y(void *obj, int32_t v) { lv_obj_set_y(obj, v); }

static void slide(int32_t from, int32_t to, lv_anim_completed_cb_t done)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_root);
    lv_anim_set_exec_cb(&a, anim_y);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    if (done) lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);
}

static void destroy(void)
{
    if (!s_root) return;
    lv_anim_delete(s_root, anim_y);
    lv_obj_delete(s_root);
    s_root = NULL;
    s_list = NULL;
}

static void close_done_cb(lv_anim_t *a)
{
    if (!s_open) destroy();
}

void notify_center_open(void)
{
    if (s_open) return;
    if (!s_loaded) load();
    destroy();                                   // a close still animating
    s_open = true;

    int32_t h = lv_display_get_vertical_resolution(lv_display_get_default());
    s_root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_root, 0, h);
    lv_obj_set_style_bg_color(s_root, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_clickable(s_root, true);          // block touches to the watchface
    lv_obj_set_scrollable(s_root, false);

    /* Grab handle at the top: tap (or swipe down) to close */
    lv_obj_t *grab = lv_obj_create(s_root);
    lv_obj_remove_style_all(grab);
    lv_obj_set_size(grab, 120, 24);
    lv_obj_align(grab, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_add_event_cb(grab, handle_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bar = lv_obj_create(grab);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 36, 4);
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, 2);
    lv_obj_set_style_radius(bar, 2, 0);
    lv_obj_set_style_bg_color(bar, UI_COLOR_DIM, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_clickable(bar, false);

    s_list = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, LV_PCT(100), h - 24);
    lv_obj_set_pos(s_list, 0, 24);
    lv_obj_set_style_pad_hor(s_list, 12, 0);
    lv_obj_set_style_pad_bottom(s_list, UI_SAFE_MARGIN, 0);
    lv_obj_set_style_pad_row(s_list, 8, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);

    rebuild();
    slide(h, 0, NULL);
}

void notify_center_close(bool anim)
{
    if (!s_open) return;
    s_open = false;
    if (anim && s_root) {
        lv_anim_delete(s_root, anim_y);
        slide(lv_obj_get_y(s_root), lv_display_get_vertical_resolution(lv_display_get_default()), close_done_cb);
    } else {
        destroy();
    }
}

bool notify_center_is_open(void)
{
    return s_open;
}

bool notify_center_at_top(void)
{
    return !s_list || lv_obj_get_scroll_y(s_list) <= 0;
}

void notify_center_clear(void)
{
    s_count = 0;
    s_loaded = true;
    remove(NC_FILE);
    if (s_open) rebuild();
}
