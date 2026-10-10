/**
 * @file ui/screen_rules.c
 * @brief Notify alarms (App menu → Notify alarms): ring when a phone
 *        notification matches a rule, right away or after a countdown.
 *
 *   Notify alarms               Edit rule
 *   Pending                     From app      [ WhatsApp      ]
 *   [ WhatsApp     4:32   ✕ ]   Text contains [ urgent        ]
 *   Rules                       Ring          [ After 5 min ▾ ]
 *   [ WhatsApp          [on]]   [Delete] [Save]
 *   [ "urgent", 5 min       ]
 *   [+ Add rule]
 *
 * A rule matches when the notification's app name contains "From app" and
 * its title or text contains "Text contains" (either may be left empty,
 * not both; case is ignored).  A match starts a countdown; when it runs out
 * the watch rings like an alarm with the notification's app and text.
 * Snooze rings again 5 minutes later.
 *
 * A "Call answered" rule instead rings when someone picks up a call you
 * made from the phone (optionally only for callers whose name or number
 * contains some text), so you notice it with the phone away from your ear.
 * Gadgetbridge sees that moment through its companion InCallService and
 * sends {t:"call", cmd:"accept"}.
 *
 * Rules are saved in /data/rules.bin.  Countdowns are not: a restart drops
 * them.
 */

#include "ui/ui_internal.h"
#include "hal/storage.h"
#include "app/contacts.h"
#include "lvgl_port.h"

#include "esp_log.h"
#include "esp_timer.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "rules";

#define RULES_FILE     STORAGE_PATH "/rules.bin"
#define RULES_MAGIC    0x524C              // "RL"
#define RULES_VERSION  1
#define RULE_MAX       10
#define PENDING_MAX    8
#define SEEN_MAX       16
#define SNOOZE_MIN     5
#define APP_LEN        24
#define TEXT_LEN       32

typedef enum {
    RULE_NOTIFICATION,                     // app + text
    RULE_CALL_ANSWERED,                    // text = caller name / number
} rule_type_t;

typedef struct {
    bool     enabled;
    uint16_t delay_min;                    // 0 = ring right away
    char     app[APP_LEN];                 // "" = any app
    char     text[TEXT_LEN];               // "" = any text / caller
    uint8_t  type;                         // rule_type_t (added later: 0 in old files)
} rule_t;

typedef struct {
    uint16_t magic, version, count, size;
} rules_header_t;

typedef struct {
    int64_t due_us;                        // esp_timer time
    char    title[48];                     // the notification's app
    char    text[96];                      // its title and text
} pending_t;

static rule_t    s_rules[RULE_MAX];
static int       s_rule_count;
static pending_t s_pending[PENDING_MAX];
static int       s_pending_count;
static int32_t   s_seen[SEEN_MAX];         // notification ids already matched
static int       s_seen_next;

static const uint16_t s_delays[] = { 0, 1, 2, 5, 10, 15, 30, 45, 60, 120 };
#define DELAY_COUNT ((int)(sizeof(s_delays) / sizeof(s_delays[0])))
static const char *const DELAY_OPTIONS =
    "Right away\nAfter 1 min\nAfter 2 min\nAfter 5 min\nAfter 10 min\nAfter 15 min\n"
    "After 30 min\nAfter 45 min\nAfter 1 hour\nAfter 2 hours";

/* ── Storage ─────────────────────────────────────────────────────────────── */
static void rules_load(void)
{
    s_rule_count = 0;
    FILE *f = fopen(RULES_FILE, "rb");
    if (!f) return;
    rules_header_t h;
    /* Records may be shorter than rule_t (written before .type existed):
     * read each over a zeroed rule, so new fields get 0 */
    if (fread(&h, sizeof(h), 1, f) == 1 && h.magic == RULES_MAGIC && h.version == RULES_VERSION &&
        h.size >= offsetof(rule_t, type) && h.size <= sizeof(rule_t) && h.count <= RULE_MAX) {
        for (int i = 0; i < h.count; i++) {
            memset(&s_rules[i], 0, sizeof(rule_t));
            if (fread(&s_rules[i], h.size, 1, f) != 1) break;
            s_rule_count++;
        }
    }
    fclose(f);
    for (int i = 0; i < s_rule_count; i++) {
        s_rules[i].app[APP_LEN - 1] = 0;
        s_rules[i].text[TEXT_LEN - 1] = 0;
    }
    ESP_LOGI(TAG, "Loaded %d rules", s_rule_count);
}

static void rules_save(void)
{
    FILE *f = fopen(RULES_FILE, "wb");
    if (!f) {
        ESP_LOGW(TAG, "Can't write %s", RULES_FILE);
        return;
    }
    rules_header_t h = { RULES_MAGIC, RULES_VERSION, (uint16_t)s_rule_count, sizeof(rule_t) };
    fwrite(&h, sizeof(h), 1, f);
    fwrite(s_rules, sizeof(rule_t), s_rule_count, f);
    fclose(f);
}

/* ── Matching and countdowns ─────────────────────────────────────────────── */
/* Case-insensitive "haystack contains needle"; an empty needle matches */
static bool contains(const char *hay, const char *needle)
{
    if (!*needle) return true;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) { h++; n++; }
        if (!*n) return true;
    }
    return false;
}

static void pending_add(int64_t due_us, const char *title, const char *text)
{
    if (s_pending_count >= PENDING_MAX) {          // drop the oldest
        memmove(&s_pending[0], &s_pending[1], sizeof(pending_t) * (PENDING_MAX - 1));
        s_pending_count--;
    }
    pending_t *p = &s_pending[s_pending_count++];
    p->due_us = due_us;
    snprintf(p->title, sizeof(p->title), "%s", title);
    snprintf(p->text, sizeof(p->text), "%s", text);
}

static void pending_remove(int i)
{
    if (i < 0 || i >= s_pending_count) return;
    memmove(&s_pending[i], &s_pending[i + 1], sizeof(pending_t) * (s_pending_count - i - 1));
    s_pending_count--;
}

void notify_rules_check(int32_t id, const char *src, const char *title, const char *body)
{
    /* Gadgetbridge sends the same id again when a notification is updated */
    for (int i = 0; i < SEEN_MAX; i++) {
        if (s_seen[i] == id && id != 0) return;
    }

    const rule_t *hit = NULL;
    for (int i = 0; i < s_rule_count; i++) {
        const rule_t *r = &s_rules[i];
        if (!r->enabled || r->type != RULE_NOTIFICATION || (!r->app[0] && !r->text[0])) continue;
        if (!contains(src, r->app)) continue;
        if (!contains(title, r->text) && !contains(body, r->text)) continue;
        if (!hit || r->delay_min < hit->delay_min) hit = r;    // the soonest wins
    }
    if (!hit) return;

    s_seen[s_seen_next] = id;
    s_seen_next = (s_seen_next + 1) % SEEN_MAX;

    char text[96];
    if (*title && *body) snprintf(text, sizeof(text), "%s: %s", title, body);
    else                 snprintf(text, sizeof(text), "%s", *title ? title : body);
    const char *heading = *src ? src : "Notification";

    ESP_LOGI(TAG, "\"%s\" matched a rule: ring in %u min", heading, hit->delay_min);
    if (hit->delay_min == 0) {
        alarm_ring_message(heading, text);
    } else {
        pending_add(esp_timer_get_time() + hit->delay_min * 60LL * 1000000LL, heading, text);
    }
}

void ui_bt_call_answered(const char *name, const char *number)
{
    char who[CONTACT_NAME_LEN] = "";
    if (*name) snprintf(who, sizeof(who), "%s", name);
    else contacts_name_for(number, who, sizeof(who));

    lvgl_lock();
    ui_phone_call_answered();
    const rule_t *hit = NULL;
    for (int i = 0; i < s_rule_count; i++) {
        const rule_t *r = &s_rules[i];
        if (!r->enabled || r->type != RULE_CALL_ANSWERED) continue;
        if (!contains(who, r->text) && !contains(number, r->text)) continue;
        if (!hit || r->delay_min < hit->delay_min) hit = r;
    }
    if (hit) {
        char text[96];
        if (*who && *number) snprintf(text, sizeof(text), "%s (%s) picked up", who, number);
        else snprintf(text, sizeof(text), "%s picked up", *who ? who : *number ? number : "The call was");
        ESP_LOGI(TAG, "Call answered: ring in %u min", hit->delay_min);
        if (hit->delay_min == 0) alarm_ring_message("Call answered", text);
        else pending_add(esp_timer_get_time() + hit->delay_min * 60LL * 1000000LL, "Call answered", text);
    }
    lvgl_unlock();
}

void notify_rules_snooze(const char *title, const char *text)
{
    pending_add(esp_timer_get_time() + SNOOZE_MIN * 60LL * 1000000LL, title, text);
}

static void check_cb(lv_timer_t *t)
{
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < s_pending_count; i++) {
        if (s_pending[i].due_us <= now && !alarm_ring_active()) {
            pending_t p = s_pending[i];
            pending_remove(i);
            alarm_ring_message(p.title, p.text);
            return;                                // one at a time
        }
    }
}

void screen_rules_init(void)
{
    rules_load();
    lv_timer_create(check_cb, 1000, NULL);
}

/* ── UI helpers ──────────────────────────────────────────────────────────── */
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

static lv_obj_t *pill(lv_obj_t *parent, const char *text, lv_color_t color, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_height(b, 46);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_style_radius(b, 23, 0);
    ui_fill(b, color);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}

static void format_left(char *buf, size_t len, int64_t us)
{
    long s = us > 0 ? (long)((us + 999999) / 1000000) : 0;
    if (s >= 3600) snprintf(buf, len, "%ld:%02ld:%02ld", s / 3600, s / 60 % 60, s % 60);
    else           snprintf(buf, len, "%ld:%02ld", s / 60, s % 60);
}

static void delay_text(char *buf, size_t len, uint16_t min)
{
    if (min == 0)            snprintf(buf, len, "right away");
    else if (min % 60 == 0)  snprintf(buf, len, "after %u h", min / 60);
    else                     snprintf(buf, len, "after %u min", min);
}

/* ── Editor ──────────────────────────────────────────────────────────────── */
static rule_t    s_edit;
static int       s_edit_index;             // -1 = new
static lv_obj_t *s_delay_dd;
static lv_obj_t *s_type_dd;
static lv_obj_t *s_edit_hint;
static char     *s_entry_field;            // s_edit.app or s_edit.text
static size_t    s_entry_len;
static lv_obj_t *s_entry_ta;

static void list_open(void);
static void edit_show(void);

static void entry_kb_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY) {
        snprintf(s_entry_field, s_entry_len, "%s", lv_textarea_get_text(s_entry_ta));
        /* Trim spaces at the ends: " urgent " would rarely match */
        char *f = s_entry_field;
        size_t n = strlen(f);
        while (n && f[n - 1] == ' ') f[--n] = 0;
        size_t lead = strspn(f, " ");
        if (lead) memmove(f, f + lead, n - lead + 1);
    }
    edit_show();
}

/* Full-page text entry with the on-screen keyboard */
static void entry_open(const char *title, char *field, size_t len)
{
    s_edit.delay_min = s_delays[lv_dropdown_get_selected(s_delay_dd)];
    s_entry_field = field;
    s_entry_len = len;

    lv_obj_t *c = ui_page_open(title, NULL);
    ui_page_set_back(edit_show);
    lv_obj_set_style_pad_row(c, 6, 0);

    s_entry_ta = lv_textarea_create(c);
    lv_textarea_set_one_line(s_entry_ta, true);
    lv_textarea_set_max_length(s_entry_ta, len - 1);
    lv_textarea_set_text(s_entry_ta, field);
    lv_textarea_set_placeholder_text(s_entry_ta, "Empty = any");
    lv_obj_set_width(s_entry_ta, LV_PCT(100));
    lv_obj_add_state(s_entry_ta, LV_STATE_FOCUSED);

    lv_obj_t *kb = lv_keyboard_create(c);
    lv_keyboard_set_textarea(kb, s_entry_ta);
    lv_keyboard_set_popovers(kb, true);
    lv_obj_set_size(kb, LV_PCT(100), 160);
    lv_obj_set_style_pad_all(kb, 0, 0);
    lv_obj_set_style_pad_gap(kb, 3, 0);
    lv_obj_set_style_bg_opa(kb, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_add_event_cb(kb, entry_kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, entry_kb_cb, LV_EVENT_CANCEL, NULL);
}

static void app_field_cb(lv_event_t *e)  { entry_open("From app", s_edit.app, sizeof(s_edit.app)); }
static void text_field_cb(lv_event_t *e)
{
    entry_open(s_edit.type == RULE_CALL_ANSWERED ? "Caller" : "Text contains",
               s_edit.text, sizeof(s_edit.text));
}

static void type_cb(lv_event_t *e)
{
    s_edit.delay_min = s_delays[lv_dropdown_get_selected(s_delay_dd)];
    s_edit.type = (uint8_t)lv_dropdown_get_selected(s_type_dd);
    edit_show();
}

static void save_cb(lv_event_t *e)
{
    s_edit.delay_min = s_delays[lv_dropdown_get_selected(s_delay_dd)];
    if (s_edit.type == RULE_CALL_ANSWERED) {
        s_edit.app[0] = 0;                          // not used
    } else if (!s_edit.app[0] && !s_edit.text[0]) {
        lv_label_set_text(s_edit_hint, "Fill in the app, the text, or both");
        lv_obj_set_style_text_color(s_edit_hint, UI_COLOR_DANGER, 0);
        return;
    }
    if (s_edit_index < 0) {
        if (s_rule_count >= RULE_MAX) return;
        s_edit.enabled = true;
        s_rules[s_rule_count++] = s_edit;
    } else {
        s_rules[s_edit_index] = s_edit;
    }
    rules_save();
    list_open();
}

static void delete_cb(lv_event_t *e)
{
    if (s_edit_index >= 0 && s_edit_index < s_rule_count) {
        memmove(&s_rules[s_edit_index], &s_rules[s_edit_index + 1],
                sizeof(rule_t) * (s_rule_count - s_edit_index - 1));
        s_rule_count--;
        rules_save();
    }
    list_open();
}

static void field(lv_obj_t *parent, const char *title, const char *value, const char *empty,
                  lv_event_cb_t cb)
{
    lv_obj_t *card = ui_card(parent, title);
    lv_obj_t *b = lv_button_create(card);
    lv_obj_set_size(b, LV_PCT(100), 40);
    ui_fill(b, UI_COLOR_SURFACE2);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_hor(b, 10, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(l, *value ? value : empty);
    lv_obj_set_style_text_color(l, *value ? UI_COLOR_TEXT : UI_COLOR_DIM, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
}

static void edit_show(void)
{
    lv_obj_t *c = ui_page_open(s_edit_index < 0 ? "New rule" : "Edit rule", NULL);
    ui_page_set_back(list_open);

    lv_obj_t *tc = ui_card(c, "When");
    s_type_dd = lv_dropdown_create(tc);
    lv_dropdown_set_options_static(s_type_dd, "Notification\nCall answered");
    lv_obj_set_width(s_type_dd, LV_PCT(100));
    lv_dropdown_set_selected(s_type_dd, s_edit.type);
    lv_obj_add_event_cb(s_type_dd, type_cb, LV_EVENT_VALUE_CHANGED, NULL);

    bool call = s_edit.type == RULE_CALL_ANSWERED;
    if (call) {
        field(c, "Caller (name or number)", s_edit.text, "Any caller", text_field_cb);
    } else {
        field(c, "From app", s_edit.app, "Any app", app_field_cb);
        field(c, "Text contains", s_edit.text, "Any text", text_field_cb);
    }

    lv_obj_t *card = ui_card(c, LV_SYMBOL_BELL "  Ring");
    s_delay_dd = lv_dropdown_create(card);
    lv_dropdown_set_options_static(s_delay_dd, DELAY_OPTIONS);
    lv_obj_set_width(s_delay_dd, LV_PCT(100));
    lv_obj_set_style_max_height(lv_dropdown_get_list(s_delay_dd), 180, 0);
    int sel = 0;
    for (int i = 0; i < DELAY_COUNT; i++) if (s_delays[i] == s_edit.delay_min) sel = i;
    lv_dropdown_set_selected(s_delay_dd, sel);

    s_edit_hint = dim_label(c, call ? "Rings when someone picks up a call you made from the phone."
                                    : "Case is ignored. An empty field matches anything.");

    lv_obj_t *row = lv_obj_create(c);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_clickable(row, false);
    if (s_edit_index >= 0) pill(row, "Delete", UI_COLOR_DANGER, delete_cb);
    pill(row, "Save", UI_COLOR_ACCENT, save_cb);
}

static void edit_open(int index)
{
    s_edit_index = index;
    if (index >= 0) s_edit = s_rules[index];
    else s_edit = (rule_t){ .enabled = true, .delay_min = 0 };
    edit_show();
}

/* ── List ────────────────────────────────────────────────────────────────── */
static lv_obj_t *s_left_lbl[PENDING_MAX];
static int       s_shown_pending;
static bool      s_reopen_queued;

static void async_list_open(void *p)
{
    s_reopen_queued = false;
    list_open();
}

static void list_update(void)
{
    if (s_shown_pending != s_pending_count) {     // one went off or was added:
        if (!s_reopen_queued) {                    // rebuild, but not from inside
            s_reopen_queued = true;                // this page's own timer
            lv_async_call(async_list_open, NULL);
        }
        return;
    }
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < s_pending_count; i++) {
        char buf[16];
        format_left(buf, sizeof(buf), s_pending[i].due_us - now);
        lv_label_set_text(s_left_lbl[i], buf);
    }
}

static void cancel_cb(lv_event_t *e)
{
    pending_remove((int)(intptr_t)lv_event_get_user_data(e));
    list_open();
}

static void row_cb(lv_event_t *e) { edit_open((int)(intptr_t)lv_event_get_user_data(e)); }
static void add_cb(lv_event_t *e) { edit_open(-1); }

static void switch_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_rule_count) return;
    s_rules[i].enabled = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    rules_save();
}

static lv_obj_t *section(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = dim_label(parent, text);
    lv_obj_set_style_pad_left(l, 4, 0);
    return l;
}

static void list_open(void)
{
    lv_obj_t *c = ui_page_open("Notify alarms", list_update);

    if (s_rule_count == 0) {
        dim_label(c, "Ring when a phone notification matches a rule (from an app, "
                     "containing some text, or both), or when someone picks up a call "
                     "you made. Right away or after a countdown.");
    }

    if (s_pending_count) {
        section(c, "Ringing in");
        int64_t now = esp_timer_get_time();
        for (int i = 0; i < s_pending_count; i++) {
            lv_obj_t *row = lv_obj_create(c);
            lv_obj_remove_style_all(row);
            lv_obj_set_size(row, LV_PCT(100), 44);
            ui_fill(row, UI_COLOR_SURFACE);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(row, 12, 0);
            lv_obj_set_style_pad_left(row, 10, 0);
            lv_obj_set_style_pad_right(row, 4, 0);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(row, 6, 0);
            lv_obj_set_clickable(row, false);

            lv_obj_t *n = lv_label_create(row);
            lv_label_set_text(n, s_pending[i].title);
            lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_flex_grow(n, 1);

            char buf[16];
            format_left(buf, sizeof(buf), s_pending[i].due_us - now);
            s_left_lbl[i] = lv_label_create(row);
            lv_label_set_text(s_left_lbl[i], buf);
            lv_obj_set_style_text_color(s_left_lbl[i], UI_COLOR_WARN, 0);

            lv_obj_t *x = lv_button_create(row);
            lv_obj_set_size(x, 36, 36);
            lv_obj_set_style_radius(x, LV_RADIUS_CIRCLE, 0);
            ui_fill(x, UI_COLOR_RAISED);
            lv_obj_set_style_shadow_width(x, 0, 0);
            lv_obj_add_event_cb(x, cancel_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            lv_obj_t *xl = lv_label_create(x);
            lv_label_set_text(xl, LV_SYMBOL_CLOSE);
            lv_obj_center(xl);
        }
    }
    s_shown_pending = s_pending_count;

    if (s_rule_count) section(c, "Rules");
    for (int i = 0; i < s_rule_count; i++) {
        const rule_t *r = &s_rules[i];
        /* [ app name      (switch) ]
         * [ "text", when           ]   stacked by flex, so they can't overlap */
        lv_obj_t *row = lv_button_create(c);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(row, 56, 0);
        ui_fill(row, UI_COLOR_SURFACE);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_pad_hor(row, 10, 0);
        lv_obj_set_style_pad_ver(row, 8, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 8, 0);
        lv_obj_add_event_cb(row, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *col = lv_obj_create(row);
        lv_obj_remove_style_all(col);
        lv_obj_set_height(col, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(col, 1);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(col, 2, 0);
        lv_obj_set_clickable(col, false);              // taps open the rule

        lv_obj_t *t = lv_label_create(col);
        bool call = r->type == RULE_CALL_ANSWERED;
        lv_label_set_text(t, call ? "Call answered" : r->app[0] ? r->app : "Any app");
        lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(t, call || r->app[0] ? UI_COLOR_TEXT : UI_COLOR_DIM, 0);
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_size(t, LV_PCT(100), lv_font_get_line_height(&lv_font_montserrat_16));

        char when[24], buf[64];
        delay_text(when, sizeof(when), r->delay_min);
        if (r->text[0]) snprintf(buf, sizeof(buf), "\"%s\", %s", r->text, when);
        else            snprintf(buf, sizeof(buf), call ? "Any caller, %s" : "Any text, %s", when);
        lv_obj_t *d = lv_label_create(col);
        lv_label_set_text(d, buf);
        lv_obj_set_style_text_font(d, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(d, UI_COLOR_DIM, 0);
        lv_label_set_long_mode(d, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_size(d, LV_PCT(100), lv_font_get_line_height(&lv_font_montserrat_14));

        lv_obj_t *sw = lv_switch_create(row);
        if (r->enabled) lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_add_event_cb(sw, switch_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
    }

    if (s_rule_count < RULE_MAX) {
        lv_obj_t *add = lv_button_create(c);
        lv_obj_set_width(add, LV_PCT(100));
        lv_obj_add_event_cb(add, add_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(add);
        lv_label_set_text(l, LV_SYMBOL_PLUS " Add rule");
        lv_obj_center(l);
    }
}

void screen_rules_open(void)
{
    list_open();
}
