/**
 * @file ui/bt_remote.c
 * @brief Mouse & keyboard gadget screens (HID mode):
 *
 *   Trackpad — drag to move the pointer, tap to click, strip on the right
 *              scrolls, L / R buttons at the bottom.  Long-press L or R to
 *              hold it down (for dragging or selecting); tap it, or tap the
 *              pad, to let go
 *   Keyboard — every key is sent as it is pressed (US layout)
 *
 * Both are full-screen overlays with a back button.  Swipe detection in
 * ui_manager.c is suspended while they are open (drags are pointer moves).
 */

#include "ui/ui_internal.h"
#include "app/ble_mgr.h"

#include <string.h>

#define POINTER_GAIN   2        // pointer px per touch px
#define SCROLL_DIV     6        // touch px per wheel step
#define SCROLL_W       34

#define MOD_LSHIFT     0x02

static lv_obj_t *s_overlay;
static uint8_t   s_held;            // mouse buttons latched down by a long press
static lv_obj_t *s_btn[2];          // L, R

/* ── Common ──────────────────────────────────────────────────────────────── */
void bt_remote_close(void)
{
    if (!s_overlay) return;
    if (s_held) {                    // don't leave a button stuck down on the host
        s_held = 0;
        ble_hid_mouse(0, 0, 0, 0);
    }
    s_btn[0] = s_btn[1] = NULL;
    lv_obj_delete(s_overlay);
    s_overlay = NULL;
}

bool bt_remote_is_open(void)
{
    return s_overlay != NULL;
}

static void back_cb(lv_event_t *e)
{
    bt_remote_close();
}

static lv_obj_t *overlay_create(const char *title)
{
    bt_remote_close();

    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_overlay, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(s_overlay, false);
    lv_obj_set_clickable(s_overlay, true);

    lv_obj_t *back = lv_button_create(s_overlay);
    lv_obj_set_size(back, 34, 34);
    lv_obj_set_pos(back, 16, 10);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    ui_fill(back, UI_COLOR_SURFACE);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *arrow = lv_label_create(back);
    lv_label_set_text(arrow, LV_SYMBOL_LEFT);
    lv_obj_center(arrow);

    lv_obj_t *lbl = lv_label_create(s_overlay);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 60, 16);
    return s_overlay;
}

static int8_t clamp8(int32_t v)
{
    return v > 127 ? 127 : v < -127 ? -127 : (int8_t)v;
}

/* ── Trackpad ────────────────────────────────────────────────────────────── */
static const uint8_t s_btn_mask[2] = { HID_MOUSE_LEFT, HID_MOUSE_RIGHT };
static const char *const s_btn_text[2]      = { "L", "R" };
static const char *const s_btn_held_text[2] = { "L  " LV_SYMBOL_DOWN " held", "R  " LV_SYMBOL_DOWN " held" };

/* Latch buttons down (or release them) and show which are held */
static void set_held(uint8_t held)
{
    s_held = held;
    for (int i = 0; i < 2; i++) {
        if (!s_btn[i]) continue;
        bool on = held & s_btn_mask[i];
        ui_fill(s_btn[i], on ? UI_COLOR_ACCENT : UI_COLOR_RAISED);
        lv_label_set_text(lv_obj_get_child(s_btn[i], 0), on ? s_btn_held_text[i] : s_btn_text[i]);
    }
    ble_hid_mouse(s_held, 0, 0, 0);
}

static void pad_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSING) {
        lv_point_t v;
        lv_indev_get_vect(lv_indev_active(), &v);
        if (v.x || v.y) ble_hid_mouse(s_held, clamp8(v.x * POINTER_GAIN), clamp8(v.y * POINTER_GAIN), 0);
    } else if (code == LV_EVENT_SHORT_CLICKED) {
        if (s_held) {                // tap = drop what's being dragged
            set_held(0);
            return;
        }
        ble_hid_mouse(HID_MOUSE_LEFT, 0, 0, 0);
        ble_hid_mouse(0, 0, 0, 0);
    }
}

static void scroll_cb(lv_event_t *e)
{
    static int32_t acc;
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        acc = 0;
        return;
    }
    lv_point_t v;
    lv_indev_get_vect(lv_indev_active(), &v);
    acc += v.y;
    int32_t steps = acc / SCROLL_DIV;
    if (steps) {
        acc -= steps * SCROLL_DIV;
        ble_hid_mouse(s_held, 0, 0, clamp8(-steps));  // drag down = scroll down
    }
}

/* Press and release click as before; a long press keeps the button down
 * after release, and the next tap on it lets go */
static void mouse_btn_cb(lv_event_t *e)
{
    static bool s_latched_now;       // this press turned the hold on
    uint8_t mask = (uint8_t)(uintptr_t)lv_event_get_user_data(e);

    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        s_latched_now = false;
        ble_hid_mouse(s_held | mask, 0, 0, 0);
        break;
    case LV_EVENT_LONG_PRESSED:
        if (!(s_held & mask)) {
            s_latched_now = true;
            set_held(s_held | mask);
        }
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        if (s_latched_now) break;    // stays held
        set_held(s_held & ~mask);
        break;
    default:
        break;
    }
}

static lv_obj_t *surface(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_color(o, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, 14, 0);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_clickable(o, true);
    return o;
}

void bt_remote_open_trackpad(void)
{
    lv_obj_t *p = overlay_create("Trackpad");

    lv_obj_t *pad = surface(p);
    lv_obj_set_size(pad, 240 - 28 - SCROLL_W - 6, 150);
    lv_obj_set_pos(pad, 14, 52);
    lv_obj_add_event_cb(pad, pad_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(pad, pad_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_t *hint = lv_label_create(pad);
    lv_label_set_text(hint, "Drag to move\nTap to click\nHold L/R to drag");
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, UI_COLOR_FAINT, 0);
    lv_obj_center(hint);

    lv_obj_t *strip = surface(p);
    lv_obj_set_size(strip, SCROLL_W, 150);
    lv_obj_set_pos(strip, 240 - 14 - SCROLL_W, 52);
    lv_obj_add_event_cb(strip, scroll_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(strip, scroll_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_t *arrows = lv_label_create(strip);
    lv_label_set_text(arrows, LV_SYMBOL_UP "\n\n" LV_SYMBOL_DOWN);
    lv_obj_set_style_text_color(arrows, UI_COLOR_DIM, 0);
    lv_obj_center(arrows);

    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = lv_button_create(p);
        lv_obj_set_size(b, 98, 44);
        lv_obj_set_pos(b, 14 + i * (98 + 16), 210);
        ui_fill(b, UI_COLOR_RAISED);
        lv_obj_set_style_radius(b, 14, 0);
        void *mask = (void *)(uintptr_t)s_btn_mask[i];
        lv_obj_add_event_cb(b, mouse_btn_cb, LV_EVENT_PRESSED, mask);
        lv_obj_add_event_cb(b, mouse_btn_cb, LV_EVENT_LONG_PRESSED, mask);
        lv_obj_add_event_cb(b, mouse_btn_cb, LV_EVENT_RELEASED, mask);
        lv_obj_add_event_cb(b, mouse_btn_cb, LV_EVENT_PRESS_LOST, mask);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, s_btn_text[i]);
        lv_obj_center(l);
        s_btn[i] = b;
    }
}

/* ── Keyboard ────────────────────────────────────────────────────────────── */
/* ASCII → (modifier, usage) for a US layout.  Returns false if unmapped. */
static bool ascii_to_hid(char c, uint8_t *mod, uint8_t *usage)
{
    static const char shifted_digits[] = ")!@#$%^&*(";
    static const struct { char plain, shifted; uint8_t usage; } sym[] = {
        { ' ',  ' ',  0x2C }, { '-', '_', 0x2D }, { '=', '+', 0x2E }, { '[', '{', 0x2F },
        { ']',  '}',  0x30 }, { '\\', '|', 0x31 }, { ';', ':', 0x33 }, { '\'', '"', 0x34 },
        { '`',  '~',  0x35 }, { ',', '<', 0x36 }, { '.', '>', 0x37 }, { '/', '?', 0x38 },
    };

    *mod = 0;
    if (c >= 'a' && c <= 'z') { *usage = 0x04 + (c - 'a'); return true; }
    if (c >= 'A' && c <= 'Z') { *usage = 0x04 + (c - 'A'); *mod = MOD_LSHIFT; return true; }
    if (c >= '1' && c <= '9') { *usage = 0x1E + (c - '1'); return true; }
    if (c == '0')             { *usage = 0x27; return true; }

    const char *d = strchr(shifted_digits, c);
    if (d && c) {
        int n = d - shifted_digits;                  // ')' is 0, '!' is 1, …
        *usage = n == 0 ? 0x27 : 0x1E + (n - 1);
        *mod = MOD_LSHIFT;
        return true;
    }
    for (size_t i = 0; i < sizeof(sym) / sizeof(sym[0]); i++) {
        if (c == sym[i].plain)   { *usage = sym[i].usage; return true; }
        if (c == sym[i].shifted) { *usage = sym[i].usage; *mod = MOD_LSHIFT; return true; }
    }
    return false;
}

static void kb_cb(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_target_obj(e);
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_CANCEL) {                   // keyboard-hide key
        bt_remote_close();
        return;
    }
    if (code == LV_EVENT_READY) {                    // ✓ key
        ble_hid_key_tap(0, 0x28);                    // Enter
        return;
    }

    uint32_t idx = lv_buttonmatrix_get_selected_button(kb);
    const char *txt = lv_buttonmatrix_get_button_text(kb, idx);
    if (!txt) return;

    if (strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) ble_hid_key_tap(0, 0x2A);
    else if (strcmp(txt, LV_SYMBOL_NEW_LINE) == 0) ble_hid_key_tap(0, 0x28);
    else if (strcmp(txt, LV_SYMBOL_LEFT) == 0) ble_hid_key_tap(0, 0x50);
    else if (strcmp(txt, LV_SYMBOL_RIGHT) == 0) ble_hid_key_tap(0, 0x4F);
    else if (strlen(txt) == 1) {
        uint8_t mod, usage;
        if (ascii_to_hid(txt[0], &mod, &usage)) ble_hid_key_tap(mod, usage);
    }
    /* "ABC", "abc", "1#": layout switches handled by LVGL itself */
}

void bt_remote_open_keyboard(void)
{
    lv_obj_t *p = overlay_create("Keyboard");

    /* Local echo of what was typed */
    lv_obj_t *ta = lv_textarea_create(p);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, "Keys are sent as you type");
    lv_obj_set_size(ta, 240 - 24, 40);
    lv_obj_set_pos(ta, 12, 50);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);

    lv_obj_t *kb = lv_keyboard_create(p);
    lv_keyboard_set_textarea(kb, ta);
    lv_keyboard_set_popovers(kb, true);
    lv_obj_set_size(kb, LV_PCT(100), 280 - 96);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_hor(kb, 6, 0);
    lv_obj_set_style_pad_bottom(kb, 14, 0);
    lv_obj_set_style_pad_top(kb, 4, 0);
    lv_obj_set_style_pad_gap(kb, 3, 0);
    lv_obj_set_style_bg_color(kb, UI_COLOR_BG, 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_add_event_cb(kb, kb_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(kb, kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, kb_cb, LV_EVENT_CANCEL, NULL);
}
