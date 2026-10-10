/**
 * @file ui/screen_wifi.c
 * @brief Wi-Fi tile: connection status, nearby networks, and a full-screen
 *        password entry with an on-screen keyboard.
 *
 *   ┌──────────────────────┐        ┌──────────────────────┐
 *   │ Wi-Fi        [on/off]│        │ ✕   MyNetwork        │
 *   │ ● Connected  MyNet   │  tap   │ [••••••••••]    👁   │
 *   │ [Disconnect][Forget] │ ─────► │ ┌──────────────────┐ │
 *   │ Networks         ⟳   │ secure │ │  q w e r t y ... │ │
 *   │ MyNet          ▂▄▆█  │        │ │   keyboard       │ │
 *   │ Neighbour      ▂▄    │        │ └──────────────────┘ │
 *   └──────────────────────┘        └──────────────────────┘
 */

#include "ui/ui_internal.h"
#include "app/wifi_mgr.h"

#include <string.h>

#define WIFI_REFRESH_MS      400
#define RESCAN_AFTER_MS      30000
#define MIN_WPA_PASS_LEN     8

static lv_obj_t *s_switch;
static lv_obj_t *s_off_hint;
static lv_obj_t *s_card;
static lv_obj_t *s_hdr;
static lv_obj_t *s_status_icon;
static lv_obj_t *s_status;
static lv_obj_t *s_detail;
static lv_obj_t *s_btn_row;
static lv_obj_t *s_btn_disconnect;
static lv_obj_t *s_btn_connect;
static lv_obj_t *s_scan_btn;
static lv_obj_t *s_spinner;
static lv_obj_t *s_list;
static lv_obj_t *s_empty;

static wifi_ap_info_t s_aps[WIFI_MGR_MAX_APS];
static int      s_ap_count;
static uint32_t s_shown_gen = 0;
static uint32_t s_last_scan_tick = 0;
static bool     s_scanned_once = false;

/* Password modal */
static lv_obj_t *s_modal;
static lv_obj_t *s_ta;
static lv_obj_t *s_hint;
static char      s_modal_ssid[33];

/* ── Helpers ─────────────────────────────────────────────────────────────── */
static int rssi_bars(int8_t rssi)
{
    if (rssi >= -55) return 4;
    if (rssi >= -67) return 3;
    if (rssi >= -78) return 2;
    return 1;
}

/* Four little bars of increasing height, `level` of them lit */
static lv_obj_t *signal_icon(lv_obj_t *parent, int level)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 4 * 4 + 3 * 2, 14);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(box, 2, 0);
    lv_obj_set_clickable(box, false);

    for (int i = 0; i < 4; i++) {
        lv_obj_t *bar = lv_obj_create(box);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 4, 5 + i * 3);
        lv_obj_set_style_radius(bar, 1, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(bar, i < level ? UI_COLOR_TEXT : UI_COLOR_FAINT, 0);
    }
    return box;
}

static lv_obj_t *small_button(lv_obj_t *parent, const char *text, lv_color_t color, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_height(btn, 34);
    lv_obj_set_flex_grow(btn, 1);
    ui_fill(btn, color);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl);
    return btn;
}

/* ── Password modal ──────────────────────────────────────────────────────── */
void screen_wifi_close_modal(void)
{
    if (!s_modal) return;
    lv_obj_delete(s_modal);
    s_modal = NULL;
    s_ta = NULL;
    s_hint = NULL;
}

bool screen_wifi_modal_is_open(void)
{
    return s_modal != NULL;
}

static void modal_submit(void)
{
    const char *pass = lv_textarea_get_text(s_ta);
    size_t len = strlen(pass);
    if (len < MIN_WPA_PASS_LEN) {
        lv_label_set_text_fmt(s_hint, "At least %d characters", MIN_WPA_PASS_LEN);
        lv_obj_set_style_text_color(s_hint, UI_COLOR_DANGER, 0);
        return;
    }
    wifi_mgr_connect(s_modal_ssid, pass);
    screen_wifi_close_modal();
}

static void keyboard_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        modal_submit();
    } else if (code == LV_EVENT_CANCEL) {
        screen_wifi_close_modal();
    }
}

static void close_cb(lv_event_t *e)
{
    screen_wifi_close_modal();
}

static void eye_cb(lv_event_t *e)
{
    bool hidden = lv_textarea_get_password_mode(s_ta);
    lv_textarea_set_password_mode(s_ta, !hidden);
    lv_label_set_text(lv_obj_get_child(lv_event_get_target_obj(e), 0),
                      hidden ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
}

static void open_password_modal(const char *ssid)
{
    screen_wifi_close_modal();
    strlcpy(s_modal_ssid, ssid, sizeof(s_modal_ssid));

    s_modal = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_modal);
    lv_obj_set_size(s_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_modal, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_modal, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(s_modal, false);
    lv_obj_set_clickable(s_modal, true);           // block touches to the tiles

    /* Title row: close button + SSID */
    lv_obj_t *close = lv_button_create(s_modal);
    lv_obj_set_size(close, 34, 34);
    lv_obj_set_pos(close, 16, 10);
    lv_obj_set_style_radius(close, LV_RADIUS_CIRCLE, 0);
    ui_fill(close, UI_COLOR_SURFACE);
    lv_obj_add_event_cb(close, close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *x = lv_label_create(close);
    lv_label_set_text(x, LV_SYMBOL_CLOSE);
    lv_obj_center(x);

    lv_obj_t *title = lv_label_create(s_modal);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(title, 160);
    lv_label_set_text(title, ssid);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 60, 16);

    /* Password field + show/hide */
    s_ta = lv_textarea_create(s_modal);
    lv_textarea_set_one_line(s_ta, true);
    lv_textarea_set_password_mode(s_ta, true);
    lv_textarea_set_placeholder_text(s_ta, "Password");
    lv_textarea_set_max_length(s_ta, 63);
    lv_obj_set_size(s_ta, 170, 40);
    lv_obj_set_pos(s_ta, 12, 50);
    lv_obj_add_state(s_ta, LV_STATE_FOCUSED);     // show the cursor

    lv_obj_t *eye = lv_button_create(s_modal);
    lv_obj_set_size(eye, 40, 40);
    lv_obj_set_pos(eye, 188, 50);
    ui_fill(eye, UI_COLOR_SURFACE);
    lv_obj_add_event_cb(eye, eye_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *eye_lbl = lv_label_create(eye);
    lv_label_set_text(eye_lbl, LV_SYMBOL_EYE_OPEN);
    lv_obj_center(eye_lbl);

    s_hint = lv_label_create(s_modal);
    lv_label_set_text(s_hint, "Press " LV_SYMBOL_OK " to connect");
    lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_hint, UI_COLOR_DIM, 0);
    lv_obj_align(s_hint, LV_ALIGN_TOP_MID, 0, 94);

    /* Keyboard fills the rest; padded so the rounded corners don't clip keys */
    lv_obj_t *kb = lv_keyboard_create(s_modal);
    lv_keyboard_set_textarea(kb, s_ta);
    lv_keyboard_set_popovers(kb, true);
    lv_obj_set_size(kb, LV_PCT(100), 280 - 114);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_hor(kb, 6, 0);
    lv_obj_set_style_pad_bottom(kb, 14, 0);
    lv_obj_set_style_pad_top(kb, 4, 0);
    lv_obj_set_style_pad_gap(kb, 3, 0);
    lv_obj_set_style_bg_color(kb, UI_COLOR_BG, 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_add_event_cb(kb, keyboard_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, keyboard_cb, LV_EVENT_CANCEL, NULL);
}

/* ── Status card ─────────────────────────────────────────────────────────── */
static void connect_cb(lv_event_t *e)    { wifi_mgr_connect_saved(); }
static void disconnect_cb(lv_event_t *e) { wifi_mgr_disconnect(); }
static void forget_cb(lv_event_t *e)     { wifi_mgr_forget(); }

static void update_status(void)
{
    char ssid[33];
    wifi_mgr_get_ssid(ssid, sizeof(ssid));
    wifi_conn_state_t st = wifi_mgr_get_state();

    lv_color_t color = UI_COLOR_DIM;
    bool show_buttons = ssid[0] != 0;

    switch (st) {
    case WIFI_CONN_CONNECTED: {
        color = UI_COLOR_ACCENT;
        lv_label_set_text(s_status, "Connected");
        lv_label_set_text(s_detail, ssid);
        break;
    }
    case WIFI_CONN_CONNECTING:
        color = UI_COLOR_WARN;
        lv_label_set_text(s_status, "Connecting...");
        lv_label_set_text(s_detail, ssid);
        show_buttons = false;
        break;
    case WIFI_CONN_FAILED:
        color = UI_COLOR_DANGER;
        lv_label_set_text(s_status, wifi_mgr_get_fail_reason());
        if (wifi_mgr_is_reconnecting()) {
            color = UI_COLOR_WARN;
            lv_label_set_text_fmt(s_detail, "%s - retrying", ssid);
        } else {
            lv_label_set_text(s_detail, ssid);
        }
        break;
    default:
        if (ssid[0]) {
            lv_label_set_text(s_status, "Saved, not connected");
            lv_label_set_text(s_detail, ssid);
        } else {
            lv_label_set_text(s_status, "Not connected");
            lv_label_set_text(s_detail, "Pick a network below");
        }
        break;
    }
    lv_obj_set_style_text_color(s_status_icon, color, 0);
    lv_obj_set_style_text_color(s_status, color, 0);

    lv_obj_set_hidden(s_btn_row, !show_buttons);
    lv_obj_set_hidden(s_btn_disconnect, st != WIFI_CONN_CONNECTED);
    lv_obj_set_hidden(s_btn_connect, st == WIFI_CONN_CONNECTED);   // also "retry now" after a failure

    bool scanning = wifi_mgr_is_scanning();
    lv_obj_set_hidden(s_spinner, !scanning);
    lv_obj_set_hidden(s_scan_btn, scanning);
}

/* ── Network list ────────────────────────────────────────────────────────── */
static void network_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_ap_count) return;
    const wifi_ap_info_t *ap = &s_aps[idx];

    char cur[33];
    wifi_mgr_get_ssid(cur, sizeof(cur));
    if (wifi_mgr_get_state() == WIFI_CONN_CONNECTED && strcmp(cur, ap->ssid) == 0) return;

    if (ap->secure) {
        open_password_modal(ap->ssid);
    } else {
        wifi_mgr_connect(ap->ssid, "");
    }
}

static void rebuild_list(void)
{
    lv_obj_clean(s_list);
    s_ap_count = wifi_mgr_get_scan_results(s_aps, WIFI_MGR_MAX_APS);

    lv_obj_set_hidden(s_empty, s_ap_count > 0 || !s_scanned_once);

    for (int i = 0; i < s_ap_count; i++) {
        lv_obj_t *row = lv_button_create(s_list);
        lv_obj_set_size(row, LV_PCT(100), 40);
        ui_fill(row, UI_COLOR_SURFACE);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_pad_hor(row, 10, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 6, 0);
        lv_obj_add_event_cb(row, network_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *name = lv_label_create(row);
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_text(name, s_aps[i].ssid);
        lv_obj_set_flex_grow(name, 1);

        if (!s_aps[i].secure) {
            lv_obj_t *open = lv_label_create(row);
            lv_label_set_text(open, "open");
            lv_obj_set_style_text_font(open, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(open, UI_COLOR_DIM, 0);
        }
        signal_icon(row, rssi_bars(s_aps[i].rssi));
    }
}

static void scan_cb(lv_event_t *e)
{
    wifi_mgr_scan();
    s_last_scan_tick = lv_tick_get();
    update_status();
}

/* Show the switch state, and hide everything else while Wi-Fi is off */
static void update_enabled(void)
{
    bool on = wifi_mgr_is_enabled();
    if (on != lv_obj_has_state(s_switch, LV_STATE_CHECKED)) {
        if (on) lv_obj_add_state(s_switch, LV_STATE_CHECKED);
        else    lv_obj_remove_state(s_switch, LV_STATE_CHECKED);
    }
    lv_obj_set_hidden(s_off_hint, on);
    lv_obj_set_hidden(s_card, !on);
    lv_obj_set_hidden(s_hdr, !on);
    lv_obj_set_hidden(s_list, !on);
    if (!on) lv_obj_set_hidden(s_empty, true);
}

static void switch_cb(lv_event_t *e)
{
    bool on = lv_obj_has_state(s_switch, LV_STATE_CHECKED);
    if (!on) screen_wifi_close_modal();
    wifi_mgr_set_enabled(on);
    update_enabled();
    if (on) {
        s_scanned_once = false;        // fresh scan for the newly powered radio
        screen_wifi_on_show();
    }
}

static void refresh_cb(lv_timer_t *t)
{
    update_enabled();
    uint32_t gen = wifi_mgr_scan_generation();
    if (gen != s_shown_gen) {
        s_shown_gen = gen;
        s_scanned_once = true;
        rebuild_list();
    }
    update_status();
}

void screen_wifi_on_show(void)
{
    if (!wifi_mgr_is_enabled() || wifi_mgr_is_scanning()) return;
    if (!s_scanned_once || lv_tick_elaps(s_last_scan_tick) > RESCAN_AFTER_MS) {
        scan_cb(NULL);
    }
}

/* ── Tile ────────────────────────────────────────────────────────────────── */
void screen_wifi_create(lv_obj_t *parent)
{
    lv_obj_t *col = ui_tile_layout(parent, "Wi-Fi");

    /* On / off */
    lv_obj_t *sw_card = ui_card(col, NULL);
    lv_obj_set_flex_flow(sw_card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sw_card, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *sw_lbl = lv_label_create(sw_card);
    lv_label_set_text(sw_lbl, "Wi-Fi");
    s_switch = lv_switch_create(sw_card);
    lv_obj_add_event_cb(s_switch, switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_off_hint = lv_label_create(col);
    lv_label_set_text(s_off_hint, "Wi-Fi is off. Internet time sync is paused.");
    lv_obj_set_width(s_off_hint, LV_PCT(100));
    lv_label_set_long_mode(s_off_hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(s_off_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_off_hint, UI_COLOR_DIM, 0);

    /* Status card */
    lv_obj_t *card = lv_obj_create(col);
    s_card = card;
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_style_pad_row(card, 4, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(card, false);

    lv_obj_t *line = lv_obj_create(card);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(line, 6, 0);
    s_status_icon = lv_label_create(line);
    lv_label_set_text(s_status_icon, LV_SYMBOL_WIFI);
    s_status = lv_label_create(line);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(s_status, 1);

    s_detail = lv_label_create(card);
    lv_label_set_long_mode(s_detail, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s_detail, LV_PCT(100));
    lv_obj_set_style_text_font(s_detail, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_detail, UI_COLOR_DIM, 0);

    s_btn_row = lv_obj_create(card);
    lv_obj_remove_style_all(s_btn_row);
    lv_obj_set_size(s_btn_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(s_btn_row, 6, 0);
    lv_obj_set_style_pad_top(s_btn_row, 4, 0);
    s_btn_connect = small_button(s_btn_row, "Connect", UI_COLOR_ACCENT, connect_cb);
    s_btn_disconnect = small_button(s_btn_row, "Disconnect", UI_COLOR_RAISED, disconnect_cb);
    small_button(s_btn_row, "Forget", UI_COLOR_DANGER, forget_cb);

    /* "Networks" header with scan button / spinner */
    lv_obj_t *hdr = lv_obj_create(col);
    s_hdr = hdr;
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, LV_PCT(100), 34);
    lv_obj_t *hl = lv_label_create(hdr);
    lv_label_set_text(hl, "Networks");
    lv_obj_set_style_text_color(hl, UI_COLOR_DIM, 0);
    lv_obj_align(hl, LV_ALIGN_LEFT_MID, 2, 0);

    s_scan_btn = lv_button_create(hdr);
    lv_obj_set_size(s_scan_btn, 34, 34);
    lv_obj_align(s_scan_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(s_scan_btn, LV_RADIUS_CIRCLE, 0);
    ui_fill(s_scan_btn, UI_COLOR_SURFACE);
    lv_obj_add_event_cb(s_scan_btn, scan_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(s_scan_btn);
    lv_label_set_text(sl, LV_SYMBOL_REFRESH);
    lv_obj_center(sl);

    s_spinner = lv_spinner_create(hdr);
    lv_obj_set_size(s_spinner, 26, 26);
    lv_obj_align(s_spinner, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_arc_width(s_spinner, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_spinner, 3, LV_PART_INDICATOR);

    s_list = lv_obj_create(col);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 6, 0);
    lv_obj_set_scrollable(s_list, false);

    s_empty = lv_label_create(col);
    lv_label_set_text(s_empty, "No networks found");
    lv_obj_set_style_text_color(s_empty, UI_COLOR_DIM, 0);
    lv_obj_set_hidden(s_empty, true);

    lv_timer_create(refresh_cb, WIFI_REFRESH_MS, NULL);
    update_status();
    update_enabled();
}
