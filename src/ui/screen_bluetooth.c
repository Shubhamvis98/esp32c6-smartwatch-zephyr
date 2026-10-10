/**
 * @file ui/screen_bluetooth.c
 * @brief Bluetooth tile (hidden; opened from Settings or the app menu).
 *
 *   Bluetooth                [on/off]
 *   Name   C6 Watch               [Edit]   → full-screen keyboard
 *   Mode   [Smartwatch | Mouse & Keyboard ▾]
 *   ● Connected / Waiting for connection
 *     visible as "C6 Watch"
 *   ── Smartwatch:  Gadgetbridge help, Find phone
 *   ── Mouse & Keyboard: Trackpad, Keyboard
 *   [ Forget paired devices ]
 */

#include "ui/ui_internal.h"
#include "app/ble_mgr.h"
#include "app/gadgetbridge.h"

#include <stdio.h>
#include <string.h>

#define REFRESH_MS   500

static lv_obj_t *s_switch;
static lv_obj_t *s_mode_dd;
static lv_obj_t *s_status_icon;
static lv_obj_t *s_status;
static lv_obj_t *s_detail;
static lv_obj_t *s_watch_card;
static lv_obj_t *s_hid_card;
static lv_obj_t *s_find_btn_lbl;
static lv_obj_t *s_hid_hint;
static bool      s_finding_phone;
static lv_obj_t *s_name;
static lv_obj_t *s_mac;
static lv_obj_t *s_gb_hint;

/* Name editor */
static lv_obj_t *s_name_modal;
static lv_obj_t *s_name_ta;
static lv_obj_t *s_name_err;

/* ── Name editor ─────────────────────────────────────────────────────────── */
void screen_bluetooth_close_modal(void)
{
    if (!s_name_modal) return;
    lv_obj_delete(s_name_modal);
    s_name_modal = NULL;
    s_name_ta = NULL;
    s_name_err = NULL;
}

bool screen_bluetooth_modal_is_open(void)
{
    return s_name_modal != NULL;
}

static void name_kb_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CANCEL) {
        screen_bluetooth_close_modal();
        return;
    }
    /* LV_EVENT_READY (✓) */
    if (!ble_mgr_set_name(lv_textarea_get_text(s_name_ta))) {
        lv_label_set_text_fmt(s_name_err, "1-%d letters, digits or symbols", BLE_NAME_MAX_LEN);
        lv_obj_set_style_text_color(s_name_err, UI_COLOR_DANGER, 0);
        return;
    }
    screen_bluetooth_close_modal();
}

static void name_close_cb(lv_event_t *e)
{
    screen_bluetooth_close_modal();
}

static void name_edit_cb(lv_event_t *e)
{
    screen_bluetooth_close_modal();

    s_name_modal = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_name_modal);
    lv_obj_set_size(s_name_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_name_modal, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_name_modal, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(s_name_modal, false);
    lv_obj_set_clickable(s_name_modal, true);

    lv_obj_t *close = lv_button_create(s_name_modal);
    lv_obj_set_size(close, 34, 34);
    lv_obj_set_pos(close, 16, 10);
    lv_obj_set_style_radius(close, LV_RADIUS_CIRCLE, 0);
    ui_fill(close, UI_COLOR_SURFACE);
    lv_obj_add_event_cb(close, name_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *x = lv_label_create(close);
    lv_label_set_text(x, LV_SYMBOL_CLOSE);
    lv_obj_center(x);

    lv_obj_t *title = lv_label_create(s_name_modal);
    lv_label_set_text(title, "Bluetooth name");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 60, 16);

    s_name_ta = lv_textarea_create(s_name_modal);
    lv_textarea_set_one_line(s_name_ta, true);
    lv_textarea_set_max_length(s_name_ta, BLE_NAME_MAX_LEN);
    lv_textarea_set_text(s_name_ta, ble_mgr_get_name());
    lv_obj_set_size(s_name_ta, 240 - 24, 40);
    lv_obj_set_pos(s_name_ta, 12, 50);
    lv_obj_add_state(s_name_ta, LV_STATE_FOCUSED);

    s_name_err = lv_label_create(s_name_modal);
    lv_label_set_text(s_name_err, "Press " LV_SYMBOL_OK " to save");
    lv_obj_set_style_text_font(s_name_err, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_name_err, UI_COLOR_DIM, 0);
    lv_obj_align(s_name_err, LV_ALIGN_TOP_MID, 0, 94);

    lv_obj_t *kb = lv_keyboard_create(s_name_modal);
    lv_keyboard_set_textarea(kb, s_name_ta);
    lv_keyboard_set_popovers(kb, true);
    lv_obj_set_size(kb, LV_PCT(100), 280 - 114);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_hor(kb, 6, 0);
    lv_obj_set_style_pad_bottom(kb, 14, 0);
    lv_obj_set_style_pad_top(kb, 4, 0);
    lv_obj_set_style_pad_gap(kb, 3, 0);
    lv_obj_set_style_bg_color(kb, UI_COLOR_BG, 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_add_event_cb(kb, name_kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, name_kb_cb, LV_EVENT_CANCEL, NULL);
}

/* ── Event handlers ──────────────────────────────────────────────────────── */
static void switch_cb(lv_event_t *e)
{
    bool on = lv_obj_has_state(s_switch, LV_STATE_CHECKED);
    ble_mgr_set(on, (ble_mode_t)lv_dropdown_get_selected(s_mode_dd));
}

static void mode_cb(lv_event_t *e)
{
    ble_mgr_set(ble_mgr_is_enabled(), (ble_mode_t)lv_dropdown_get_selected(s_mode_dd));
}

static void forget_cb(lv_event_t *e)
{
    ble_mgr_forget_all();
}

static void find_phone_cb(lv_event_t *e)
{
    s_finding_phone = !s_finding_phone;
    gb_find_phone(s_finding_phone);
}

bool screen_bluetooth_find_phone_toggle(void)
{
    if (!gb_is_connected()) return s_finding_phone = false;
    find_phone_cb(NULL);
    return s_finding_phone;
}

static void trackpad_cb(lv_event_t *e) { bt_remote_open_trackpad(); }
static void keyboard_cb(lv_event_t *e) { bt_remote_open_keyboard(); }

/* ── Layout helpers ──────────────────────────────────────────────────────── */
static lv_obj_t *row_layout(lv_obj_t *card)
{
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return card;
}

static lv_obj_t *wide_button(lv_obj_t *parent, const char *text, lv_color_t color,
                             lv_event_cb_t cb, lv_obj_t **label_out)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_width(btn, LV_PCT(100));
    ui_fill(btn, color);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    if (label_out) *label_out = lbl;
    return btn;
}

static lv_obj_t *small_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, UI_COLOR_DIM, 0);
    return l;
}

/* ── Refresh ─────────────────────────────────────────────────────────────── */
static void refresh(void)
{
    bool enabled = ble_mgr_is_enabled();
    ble_mode_t mode = ble_mgr_get_mode();
    ble_state_t st = ble_mgr_get_state();

    if (enabled != lv_obj_has_state(s_switch, LV_STATE_CHECKED)) {
        if (enabled) lv_obj_add_state(s_switch, LV_STATE_CHECKED);
        else         lv_obj_remove_state(s_switch, LV_STATE_CHECKED);
    }
    if (!lv_dropdown_is_open(s_mode_dd) && lv_dropdown_get_selected(s_mode_dd) != (uint32_t)mode) {
        lv_dropdown_set_selected(s_mode_dd, mode);
    }

    lv_color_t color = UI_COLOR_DIM;
    char peer[24];
    ble_mgr_get_peer(peer, sizeof(peer));

    if (!enabled) {
        lv_label_set_text(s_status, "Off");
        lv_label_set_text(s_detail, "Turn on to pair with a phone or computer");
    } else if (st == BLE_STATE_CONNECTED) {
        color = UI_COLOR_ACCENT;
        if (mode == BLE_MODE_WATCH) {
            lv_label_set_text(s_status, gb_is_connected() ? "Gadgetbridge connected" : "Connected");
        } else {
            lv_label_set_text(s_status, ble_mgr_is_secure() ? "Connected" : "Pairing...");
        }
        lv_label_set_text_fmt(s_detail, "%s", peer);
    } else if (st == BLE_STATE_ADVERTISING) {
        color = UI_COLOR_ACCENT2;
        lv_label_set_text(s_status, "Waiting for connection");
        lv_label_set_text_fmt(s_detail, "Visible as \"%s\"", ble_mgr_get_name());
    } else {
        color = UI_COLOR_WARN;
        lv_label_set_text(s_status, "Starting...");
        lv_label_set_text(s_detail, "");
    }
    lv_obj_set_style_text_color(s_status_icon, color, 0);
    lv_obj_set_style_text_color(s_status, color, 0);

    lv_obj_set_hidden(s_watch_card, mode != BLE_MODE_WATCH);
    lv_obj_set_hidden(s_hid_card, mode != BLE_MODE_HID);

    lv_label_set_text(s_name, ble_mgr_get_name());
    char mac[20];
    ble_mgr_get_addr(mode, mac, sizeof(mac));
    lv_label_set_text_fmt(s_mac, "MAC  %s", mac);

    if (mode == BLE_MODE_WATCH) {
        lv_label_set_text_fmt(s_gb_hint, "Gadgetbridge: turn on Settings > Discovery and "
                              "Pairing options > Discover unsupported devices. Then tap +, "
                              "long-press \"%s\" and choose Add test device > Bangle.js.",
                              ble_mgr_get_name());
        lv_label_set_text(s_find_btn_lbl, s_finding_phone ? LV_SYMBOL_STOP " Stop ringing phone"
                                                          : LV_SYMBOL_BELL " Find my phone");
        if (!gb_is_connected()) s_finding_phone = false;
    } else {
        if (ble_hid_ready()) {
            lv_label_set_text(s_hid_hint, "Ready. Open the trackpad or keyboard.");
        } else {
            lv_label_set_text_fmt(s_hid_hint, "Pair \"%s\" from your phone or computer's "
                                  "Bluetooth settings.", ble_mgr_get_name());
        }
    }
}

static void refresh_cb(lv_timer_t *t)
{
    refresh();
}

/* ── Tile ────────────────────────────────────────────────────────────────── */
void screen_bluetooth_create(lv_obj_t *parent)
{
    lv_obj_t *col = ui_tile_layout(parent, "Bluetooth");

    /* On / off */
    lv_obj_t *card = row_layout(ui_card(col, NULL));
    lv_obj_t *lbl = lv_label_create(card);
    lv_label_set_text(lbl, "Bluetooth");
    s_switch = lv_switch_create(card);
    lv_obj_add_event_cb(s_switch, switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Name */
    card = ui_card(col, "Name");
    lv_obj_t *name_row = lv_obj_create(card);
    lv_obj_remove_style_all(name_row);
    lv_obj_set_size(name_row, LV_PCT(100), LV_SIZE_CONTENT);
    row_layout(name_row);
    lv_obj_set_style_pad_column(name_row, 10, 0);
    card = name_row;
    s_name = lv_label_create(card);
    lv_label_set_long_mode(s_name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(s_name, 1);
    lv_obj_t *edit = lv_button_create(card);
    lv_obj_set_size(edit, 64, 34);
    ui_fill(edit, UI_COLOR_RAISED);
    lv_obj_add_event_cb(edit, name_edit_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *edit_lbl = lv_label_create(edit);
    lv_label_set_text(edit_lbl, LV_SYMBOL_EDIT);
    lv_obj_center(edit_lbl);

    /* Gadget mode */
    card = ui_card(col, "Gadget mode");
    s_mode_dd = lv_dropdown_create(card);
    lv_dropdown_set_options_static(s_mode_dd, "Smartwatch\nMouse & Keyboard");
    lv_obj_set_width(s_mode_dd, LV_PCT(100));
    lv_dropdown_set_selected(s_mode_dd, ble_mgr_get_mode());
    lv_obj_add_event_cb(s_mode_dd, mode_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Status */
    card = ui_card(col, NULL);
    lv_obj_t *line = lv_obj_create(card);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(line, 6, 0);
    s_status_icon = lv_label_create(line);
    lv_label_set_text(s_status_icon, LV_SYMBOL_BLUETOOTH);
    s_status = lv_label_create(line);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(s_status, 1);
    s_detail = small_label(card, "");
    s_mac = small_label(card, "");

    /* Smartwatch mode tools */
    s_watch_card = ui_card(col, "Gadgetbridge");
    s_gb_hint = small_label(s_watch_card, "");
    wide_button(s_watch_card, LV_SYMBOL_BELL " Find my phone", UI_COLOR_RAISED,
                find_phone_cb, &s_find_btn_lbl);

    /* Mouse & keyboard mode tools */
    s_hid_card = ui_card(col, "Mouse & Keyboard");
    s_hid_hint = small_label(s_hid_card, "");
    wide_button(s_hid_card, LV_SYMBOL_GPS " Trackpad", UI_COLOR_RAISED, trackpad_cb, NULL);
    wide_button(s_hid_card, LV_SYMBOL_KEYBOARD " Keyboard", UI_COLOR_RAISED, keyboard_cb, NULL);

    wide_button(col, "Forget paired devices", UI_COLOR_DANGER, forget_cb, NULL);

    lv_timer_create(refresh_cb, REFRESH_MS, NULL);
    refresh();
}
