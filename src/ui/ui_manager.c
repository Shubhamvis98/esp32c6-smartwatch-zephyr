/**
 * @file ui/ui_manager.c
 * @brief Tileview navigation, touch input, idle timeout and screen power.
 *
 *   ┌───────────┬──────────┐   ┌───────┬───────┬───────────┬──────────┐
 *   │ Watchface │ Activity │   │ Timer │ Wi-Fi │ Bluetooth │ Settings │
 *   └───────────┴──────────┘   └───────┴───────┴───────────┴──────────┘
 *     swipe left/right           hidden: opened from Settings / app menu /
 *                                quick panel, ← or swipe right goes back
 */

#include "ui/ui_manager.h"
#include "ui/ui_internal.h"

#include "hal/display.h"
#include "hal/touch.h"
#include "hal/battery.h"
#include "hal/power.h"
#include "hal/audio.h"
#include "hal/storage.h"
#include "app/settings.h"
#include "app/activity.h"
#include "app/recorder.h"
#include "app/gadgetbridge.h"
#include "app/ble_mgr.h"
#include "lvgl_port.h"
#include "lvgl_private.h"        // lv_tileview_tile_t: per-tile swipe directions

#include <stdio.h>
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ui";

#define SWIPE_MIN_PX            40      // travel needed to count as a swipe

#define UI_REFRESH_MS           1000
#define BATTERY_REFRESH_MS      30000
#define AOD_BRIGHTNESS          5        // backlight % while the always-on display shows

static lv_obj_t *s_tileview;
static lv_obj_t *s_tiles[TILE_COUNT];
static lv_obj_t *s_back_btn;                 // shown on hidden tiles
static ui_tile_t s_back_to[TILE_COUNT];      // where ← goes from each hidden tile
static lv_obj_t *s_message;

static bool s_screen_on = false;
static bool s_swallow_touch = false;     // touch that woke the screen
static bool s_aod = false;               // "off", but showing the dimmed watchface
static bool s_powering_off = false;
static int  s_battery_mv = 0;

/* ── Helpers ─────────────────────────────────────────────────────────────── */
void ui_format_thousands(char *buf, size_t len, uint32_t n)
{
    if (n >= 1000000) {
        snprintf(buf, len, "%lu,%03lu,%03lu", (unsigned long)(n / 1000000),
                 (unsigned long)(n / 1000 % 1000), (unsigned long)(n % 1000));
    } else if (n >= 1000) {
        snprintf(buf, len, "%lu,%03lu", (unsigned long)(n / 1000), (unsigned long)(n % 1000));
    } else {
        snprintf(buf, len, "%lu", (unsigned long)n);
    }
}

int ui_battery_mv(void)
{
    return s_battery_mv;
}

static void close_overlays_locked(void);
static void set_tile_locked(ui_tile_t tile, lv_anim_enable_t anim, bool remember_back);
static void go_back_locked(void);
static ui_tile_t active_tile(void);
static bool in_swipe(ui_tile_t t);

/* ── Screen power (call with LVGL lock held) ─────────────────────────────── */
static void screen_on_locked(void)
{
    lv_display_trigger_activity(lv_display_get_default());
    if (s_screen_on) return;

    display_set_sleep(false);
    display_set_backlight(settings_get()->brightness);
    s_screen_on = true;
    s_aod = false;
}

static void screen_off_locked(void)
{
    if (!s_screen_on) return;

    /* Leave the torch first: exiting it restores the backlight.  Everything
     * else stays as it is, so waking shows the same screen; only the PWR
     * button returns to the watchface.  The quick panel and torch close. */
    quick_panel_torch_exit();
    quick_panel_close(false);
    notify_center_close(false);
    if (notify_popup_is_notification()) notify_popup_close();

    display_set_backlight(0);
    display_set_sleep(true);
    s_screen_on = false;
    s_aod = false;
}

/* Always-on display: counts as "off" for every wake path (touch, PWR,
 * raise, notifications), but the panel stays awake at low brightness and
 * the watchface keeps updating. */
static void screen_aod_locked(void)
{
    quick_panel_torch_exit();
    quick_panel_close(false);
    notify_center_close(false);
    if (notify_popup_is_notification()) notify_popup_close();
    display_set_backlight(AOD_BRIGHTNESS);
    s_screen_on = false;
    s_aod = true;
}

/* ── Swipe detection ─────────────────────────────────────────────────────────
 * Done on the raw touch points rather than with LVGL gesture events: LVGL
 * only sends a gesture when nothing is scrolling and delivers it to the first
 * parent with gesture-bubbling off, so swipes over scrollable tiles never
 * arrived.  A swipe fires once per touch, as soon as the finger has moved
 * SWIPE_MIN_PX mostly along one axis. */
typedef enum { SWIPE_NONE, SWIPE_UP, SWIPE_DOWN, SWIPE_LEFT, SWIPE_RIGHT } swipe_t;

/* The current touch began on a slider or arc: its left/right drag sets the
 * value, so it mustn't also count as a swipe (back, next page) */
static bool s_touch_on_drag_widget;

static bool point_on_drag_widget(lv_point_t p)
{
    lv_obj_t *o = lv_indev_search_obj(lv_layer_top(), &p);
    if (!o) o = lv_indev_search_obj(lv_screen_active(), &p);
    for (; o; o = lv_obj_get_parent(o)) {
        if (lv_obj_check_type(o, &lv_slider_class) || lv_obj_check_type(o, &lv_arc_class)) return true;
    }
    return false;
}

static swipe_t swipe_track(const lv_indev_data_t *d)
{
    static bool       down = false;
    static bool       fired = false;
    static lv_point_t start;

    if (d->state != LV_INDEV_STATE_PRESSED) {
        down = false;
        return SWIPE_NONE;
    }
    if (!down) {
        down = true;
        fired = false;
        start = d->point;
        s_touch_on_drag_widget = point_on_drag_widget(start);
        return SWIPE_NONE;
    }
    if (fired) return SWIPE_NONE;

    int32_t dx = d->point.x - start.x;
    int32_t dy = d->point.y - start.y;
    if (LV_ABS(dy) >= SWIPE_MIN_PX && LV_ABS(dy) > 2 * LV_ABS(dx)) {
        fired = true;
        return dy > 0 ? SWIPE_DOWN : SWIPE_UP;
    }
    if (LV_ABS(dx) >= SWIPE_MIN_PX && LV_ABS(dx) > 2 * LV_ABS(dy)) {
        fired = true;
        return dx > 0 ? SWIPE_RIGHT : SWIPE_LEFT;
    }
    return SWIPE_NONE;
}

/* Deferred actions: run outside the indev read callback so objects that the
 * finger is on can be safely created/deleted. */
static void async_torch_exit(void *p)  { quick_panel_torch_exit(); }
static void async_page_back(void *p)   { ui_page_back(); }
static void async_panel_close(void *p) { quick_panel_close(true); }
static void async_panel_open(void *p)  { quick_panel_open(); }
static void async_go_back(void *p)     { go_back_locked(); }
static void async_center_open(void *p) { notify_center_open(); }
static void async_center_close(void *p){ notify_center_close(true); }

/* Returns true if the swipe was consumed (the rest of the touch is ignored) */
static void async_popup_close(void *p)  { notify_popup_close(); }

static bool handle_swipe(swipe_t sw)
{
    if (alarm_ring_active()) return true;         // needs Snooze / Stop
    if (notify_popup_is_open()) {                 // swipe away a notification;
        if (notify_popup_is_notification()) {     // calls need a button press
            lv_async_call(async_popup_close, NULL);
        }
        return true;
    }
    if (bt_remote_is_open()) return false;        // drags are pointer moves
    if (s_touch_on_drag_widget && (sw == SWIPE_LEFT || sw == SWIPE_RIGHT)) {
        return false;                             // adjusting a slider
    }
    if (quick_panel_torch_active()) {             // any swipe leaves the torch
        lv_async_call(async_torch_exit, NULL);
        return true;
    }
    if (ui_page_is_open()) {                      // swipe right = back
        if (sw != SWIPE_RIGHT) return false;
        lv_async_call(async_page_back, NULL);
        return true;
    }
    if (quick_panel_is_open()) {                  // swipe up closes the panel
        if (sw != SWIPE_UP) return false;
        lv_async_call(async_panel_close, NULL);
        return true;
    }
    if (notify_center_is_open()) {                // swipe down (at the top) or right closes it
        if (sw == SWIPE_RIGHT || (sw == SWIPE_DOWN && notify_center_at_top())) {
            lv_async_call(async_center_close, NULL);
            return true;
        }
        return false;                             // the list scrolls
    }
    if (sw == SWIPE_RIGHT && !in_swipe(active_tile())) {
        lv_async_call(async_go_back, NULL);
        return true;
    }
    if (sw == SWIPE_DOWN && active_tile() == TILE_WATCHFACE) {
        lv_async_call(async_panel_open, NULL);
        return true;
    }
    if (sw == SWIPE_UP && active_tile() == TILE_WATCHFACE) {
        lv_async_call(async_center_open, NULL);
        return true;
    }
    return false;                                 // let LVGL scroll as usual
}

/* ── Touch input with tap-to-wake ────────────────────────────────────────── */
static void indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    touch_get_data(data);
    swipe_t sw = swipe_track(data);

    if (s_powering_off) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    if (!s_screen_on && data->state == LV_INDEV_STATE_PRESSED) {
        screen_on_locked();
        s_swallow_touch = true;
    }

    if (!s_swallow_touch && sw != SWIPE_NONE && handle_swipe(sw)) {
        s_swallow_touch = true;
    }

    /* Don't let the waking tap (or a handled swipe) click what's under the finger */
    if (s_swallow_touch) {
        if (data->state == LV_INDEV_STATE_RELEASED) s_swallow_touch = false;
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/* ── Periodic refresh + idle timeout ─────────────────────────────────────── */
/* The screen only times out on the watchface (the quick panel and
 * notifications over it included).  Other tiles, pages and menus stay on
 * until PWR takes the watch back to the watchface. */
static bool on_idle_screen(void)
{
    return active_tile() == TILE_WATCHFACE && !ui_page_is_open() && !bt_remote_is_open();
}

static void refresh_timer_cb(lv_timer_t *t)
{
    static uint32_t last_battery = 0;
    uint32_t now = lv_tick_get();

    if (last_battery == 0 || now - last_battery >= BATTERY_REFRESH_MS) {
        last_battery = now;
        s_battery_mv = battery_read_mv();
    }

    if (s_screen_on && !s_powering_off && on_idle_screen() &&
        !screen_timer_is_ringing() && !alarm_ring_active() &&
        !quick_panel_torch_active() && !notify_popup_keeps_awake() &&
        lv_display_get_inactive_time(lv_display_get_default()) > settings_get()->timeout_s * 1000u) {
        if (settings_get()->aod) screen_aod_locked();
        else                     screen_off_locked();
    }

    if (s_aod) {
        screen_watchface_update();
    } else if (s_screen_on) {
        screen_watchface_update();
        screen_activity_update();
        screen_music_update();
        screen_settings_update();
    }
}

/* ── Navigation ──────────────────────────────────────────────────────────── */
static void close_overlays_locked(void)
{
    screen_wifi_close_modal();
    screen_bluetooth_close_modal();
    ui_page_close();
    quick_panel_close(false);
    quick_panel_torch_exit();
    notify_center_close(false);
    bt_remote_close();
    notify_popup_close();
}

static bool overlay_open(void)
{
    return screen_wifi_modal_is_open() || screen_bluetooth_modal_is_open() || ui_page_is_open() ||
           quick_panel_is_open() || quick_panel_torch_active() ||
           notify_popup_is_open() || notify_center_is_open() || bt_remote_is_open();
}

/* Idle screen pages that can be swiped through, in order, watchface first */
static ui_tile_t s_swipe[TILE_SWIPE_COUNT];
static int       s_swipe_n;

static int swipe_index(ui_tile_t t)
{
    for (int i = 0; i < s_swipe_n; i++) {
        if (s_swipe[i] == t) return i;
    }
    return -1;
}

/* Reached by swiping (else it's opened from somewhere and has a ← back) */
static bool in_swipe(ui_tile_t t)
{
    return swipe_index(t) >= 0;
}

static ui_tile_t active_tile(void)
{
    lv_obj_t *act = lv_tileview_get_tile_active(s_tileview);
    for (int i = 0; i < TILE_COUNT; i++) {
        if (s_tiles[i] == act) return (ui_tile_t)i;
    }
    return TILE_WATCHFACE;
}

/* Everything that depends on which tile is showing.  Called after every tile
 * change, since a change made from code doesn't reliably send the tileview's
 * VALUE_CHANGED event. */
static void on_tile_changed(void)
{
    ui_tile_t t = active_tile();
    lv_obj_set_hidden(s_back_btn, in_swipe(t));             // hidden tiles only

    if (t != TILE_WATCHFACE) {
        quick_panel_close(false);
        notify_center_close(false);
    }
    if (t == TILE_WIFI) screen_wifi_on_show();
}

/* remember_back: record the current tile as where ← should return to.
 * False when going back, so the trail isn't overwritten. */
static void set_tile_locked(ui_tile_t tile, lv_anim_enable_t anim, bool remember_back)
{
    ui_tile_t cur = active_tile();
    if (remember_back && !in_swipe(tile) && tile != cur) s_back_to[tile] = cur;

    /* Only animate between neighbouring swipe tiles; a jump further would
     * otherwise scroll visibly across the ones in between. */
    int a = swipe_index(tile), b = swipe_index(cur);
    if (a < 0 || b < 0 || LV_ABS(a - b) > 1) anim = LV_ANIM_OFF;

    lv_tileview_set_tile(s_tileview, s_tiles[tile], anim);
    on_tile_changed();
}

static void go_back_locked(void)
{
    ui_tile_t t = active_tile();
    if (in_swipe(t)) return;
    close_overlays_locked();
    set_tile_locked(s_back_to[t], LV_ANIM_OFF, false);
}

void ui_open_tile(ui_tile_t tile)
{
    close_overlays_locked();
    set_tile_locked(tile, LV_ANIM_ON, true);
}

/* ── Idle screen layout ──────────────────────────────────────────────────── */
static const char *const s_tile_names[TILE_SWIPE_COUNT] = {
    [TILE_WATCHFACE] = "Watchface",
    [TILE_ACTIVITY]  = "Activity",
    [TILE_MUSIC]     = "Music",
};

const char *ui_tile_name(ui_tile_t tile)
{
    return tile < TILE_SWIPE_COUNT ? s_tile_names[tile] : "";
}

int ui_idle_pages(ui_tile_t *out)
{
    const watch_settings_t *cfg = settings_get();
    bool seen[TILE_SWIPE_COUNT] = { [TILE_WATCHFACE] = true };   // placed below
    ui_tile_t others[TILE_SWIPE_COUNT];
    int n = 0;
    for (int i = 0; i < (int)sizeof(cfg->idle_order) && cfg->idle_order[i]; i++) {
        uint8_t t = cfg->idle_order[i];
        if (t < TILE_SWIPE_COUNT && !seen[t]) {
            seen[t] = true;
            others[n++] = (ui_tile_t)t;
        }
    }
    for (int t = 1; t < TILE_SWIPE_COUNT; t++) {         // pages new since the list was saved
        if (!seen[t]) others[n++] = (ui_tile_t)t;
    }

    int face = cfg->idle_face_pos > n ? n : cfg->idle_face_pos;
    int k = 0;
    for (int i = 0; i <= n; i++) {
        if (i == face) out[k++] = TILE_WATCHFACE;
        if (i < n)     out[k++] = others[i];
    }
    return k;
}

/* Put a tile in a column of the tileview and say where it can be swiped to */
static void place_tile(ui_tile_t t, int col, lv_dir_t dir)
{
    lv_obj_set_pos(s_tiles[t], lv_pct(col * 100), 0);
    ((lv_tileview_tile_t *)s_tiles[t])->dir = dir;
}

void ui_apply_idle_layout(void)
{
    ui_tile_t pages[TILE_SWIPE_COUNT];
    int n = ui_idle_pages(pages);
    uint8_t hidden = settings_get()->idle_hidden;

    hidden &= ~(1u << TILE_WATCHFACE);                  // the watchface is always there
    s_swipe_n = 0;
    for (int i = 0; i < n; i++) {
        if (!(hidden & (1u << pages[i]))) s_swipe[s_swipe_n++] = pages[i];
    }

    /* Swipe pages first, then everything that's opened some other way */
    int col = 0;
    for (int i = 0; i < s_swipe_n; i++, col++) {
        lv_dir_t dir = LV_DIR_NONE;
        if (i > 0) dir |= LV_DIR_LEFT;
        if (i < s_swipe_n - 1) dir |= LV_DIR_RIGHT;
        place_tile(s_swipe[i], col, dir);
    }
    for (int i = 0; i < n; i++) {
        if (hidden & (1u << pages[i])) place_tile(pages[i], col++, LV_DIR_NONE);
    }
    for (int t = TILE_SWIPE_COUNT; t < TILE_COUNT; t++) place_tile((ui_tile_t)t, col++, LV_DIR_NONE);

    /* Stay on the same tile, at its new place */
    lv_obj_update_layout(s_tileview);
    lv_tileview_set_tile(s_tileview, s_tiles[active_tile()], LV_ANIM_OFF);
    on_tile_changed();
}

/* ── Page indicator ──────────────────────────────────────────────────────── */
/* Swipes between the visible tiles end here */
static void tileview_event_cb(lv_event_t *e)
{
    on_tile_changed();
}

static void back_btn_cb(lv_event_t *e)
{
    go_back_locked();
}

static void create_back_button(void)
{
    /* Same look and place as the sub-pages' back button */
    s_back_btn = lv_button_create(lv_layer_top());
    lv_obj_set_size(s_back_btn, 34, 34);
    lv_obj_set_pos(s_back_btn, 16, 10);
    lv_obj_set_style_radius(s_back_btn, LV_RADIUS_CIRCLE, 0);
    ui_fill(s_back_btn, UI_COLOR_SURFACE);
    lv_obj_add_event_cb(s_back_btn, back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *arrow = lv_label_create(s_back_btn);
    lv_label_set_text(arrow, LV_SYMBOL_LEFT);
    lv_obj_center(arrow);

    for (int i = 0; i < TILE_COUNT; i++) s_back_to[i] = TILE_WATCHFACE;
}

/* ── Public API ──────────────────────────────────────────────────────────── */
esp_err_t ui_init(void)
{
    lvgl_lock();

    lv_display_t *disp = display_get_lvgl_disp();
    ui_theme_init(disp);
    /* The theme colours text on screens only.  Pages, popups and overlays live
     * on the top layer, where unstyled labels would otherwise be black. */
    lv_obj_set_style_text_color(lv_layer_top(), UI_COLOR_TEXT, 0);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, indev_read_cb);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, UI_COLOR_BG, 0);

    s_tileview = lv_tileview_create(scr);
    lv_obj_set_style_bg_color(s_tileview, UI_COLOR_BG, 0);
    lv_obj_set_scrollbar_mode(s_tileview, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_tileview, tileview_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    for (int i = 0; i < TILE_COUNT; i++) {
        s_tiles[i] = lv_tileview_add_tile(s_tileview, i, 0, LV_DIR_NONE);   // placed below
    }
    screen_watchface_create(s_tiles[TILE_WATCHFACE]);
    screen_activity_create(s_tiles[TILE_ACTIVITY]);
    screen_music_create(s_tiles[TILE_MUSIC]);
    screen_timer_create(s_tiles[TILE_TIMER]);
    screen_wifi_create(s_tiles[TILE_WIFI]);
    screen_bluetooth_create(s_tiles[TILE_BLUETOOTH]);
    screen_alarms_init();
    screen_rules_init();
    screen_calendar_init();
    screen_settings_create(s_tiles[TILE_SETTINGS]);
    /* The watchface has nothing to scroll; without this LVGL "scrolls" it
     * elastically on vertical drags instead of leaving them to the swipe */
    lv_obj_set_scroll_dir(s_tiles[TILE_WATCHFACE], LV_DIR_NONE);
    create_back_button();
    ui_apply_idle_layout();
    /* The tileview has no active tile until set_tile or a scroll ends */
    set_tile_locked(TILE_WATCHFACE, LV_ANIM_OFF, true);

    s_battery_mv = battery_read_mv();
    lv_timer_create(refresh_timer_cb, UI_REFRESH_MS, NULL);
    refresh_timer_cb(NULL);

    /* Render the first frame before lighting the backlight */
    lv_refr_now(disp);
    screen_on_locked();
    screen_watchface_update();

    lvgl_unlock();
    ESP_LOGI(TAG, "UI ready");
    return ESP_OK;
}

void ui_screen_wake(void)
{
    lvgl_lock();
    if (!s_powering_off) screen_on_locked();
    lvgl_unlock();
}

void ui_screen_off(void)
{
    lvgl_lock();
    screen_off_locked();
    lvgl_unlock();
}

bool ui_screen_is_on(void)
{
    return s_screen_on;
}

void ui_go_home(void)
{
    lvgl_lock();
    if (!s_powering_off) {
        screen_on_locked();
        ui_open_tile(TILE_WATCHFACE);
    }
    lvgl_unlock();
}

void ui_handle_power_click(void)
{
    lvgl_lock();
    if (s_powering_off) {
        /* ignore */
    } else if (!s_screen_on) {
        screen_on_locked();                 // PWR wakes to the watchface
        ui_open_tile(TILE_WATCHFACE);
    } else if (alarm_ring_active()) {
        alarm_ring_dismiss();
        screen_on_locked();
    } else if (screen_timer_is_ringing()) {
        screen_timer_dismiss();
        screen_on_locked();
    } else if (overlay_open() ||
               lv_tileview_get_tile_active(s_tileview) != s_tiles[TILE_WATCHFACE]) {
        screen_on_locked();
        ui_open_tile(TILE_WATCHFACE);
    } else {
        screen_on_locked();
        menu_apps_open();
    }
    lvgl_unlock();
}

void ui_handle_power_long_press(void)
{
    lvgl_lock();
    if (!s_powering_off) {
        screen_on_locked();
        screen_timer_dismiss();
        close_overlays_locked();
        menu_power_open();
    }
    lvgl_unlock();
}

/* ── BOOT button actions ─────────────────────────────────────────────────── */
const char UI_BTN_ACTION_OPTIONS[] =
    "Nothing\n"
    "Back\n"
    "Watchface\n"
    "App menu\n"
    "Notifications\n"
    "Quick panel\n"
    "Torch\n"
    "Play / pause\n"
    "Next track\n"
    "Previous track\n"
    "Camera shutter\n"
    "Find my phone\n"
    "Stopwatch\n"
    "Timer\n"
    "Screen off\n"
    "Sound on / off\n"
    "Watchface / apps\n"
    "Power menu";

/* A short note at the bottom of the screen, gone after a moment */
static lv_obj_t *s_toast;

static void toast_done_cb(lv_timer_t *t)
{
    if (s_toast) lv_obj_delete(s_toast);
    s_toast = NULL;
}

static void toast_locked(const char *text)
{
    static lv_timer_t *timer;
    if (!s_toast) {
        s_toast = lv_label_create(lv_layer_top());
        lv_obj_set_style_bg_color(s_toast, UI_COLOR_RAISED, 0);
        lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(s_toast, UI_COLOR_TEXT, 0);
        lv_obj_set_style_text_font(s_toast, &lv_font_montserrat_16, 0);
        lv_obj_set_style_radius(s_toast, 16, 0);
        lv_obj_set_style_pad_hor(s_toast, 14, 0);
        lv_obj_set_style_pad_ver(s_toast, 8, 0);
        lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -24);
        timer = NULL;
    }
    lv_label_set_text(s_toast, text);
    if (timer) lv_timer_delete(timer);
    timer = lv_timer_create(toast_done_cb, 1500, NULL);
    lv_timer_set_repeat_count(timer, 1);
}

/* Actions that make sense with the screen off, so they don't wake it */
static bool runs_in_background(btn_action_t a)
{
    return a == BTN_ACT_PLAY_PAUSE || a == BTN_ACT_NEXT || a == BTN_ACT_PREVIOUS ||
           a == BTN_ACT_FIND_PHONE || a == BTN_ACT_SCREEN_OFF;
}

static bool phone_ok_locked(void)
{
    if (gb_is_connected()) return true;
    toast_locked("Phone not connected");
    return false;
}

void ui_handle_button_action(btn_action_t a, bool pwr)
{
    if (a >= BTN_ACT_COUNT) a = BTN_ACT_NONE;
    if (a == BTN_ACT_HOME_OR_APPS) {
        ui_handle_power_click();
        return;
    }
    if (a == BTN_ACT_POWER_MENU) {
        ui_handle_power_long_press();
        return;
    }
    if (a == BTN_ACT_NONE && !pwr) return;
    lvgl_lock();
    if (s_powering_off) {
        lvgl_unlock();
        return;
    }
    /* A ringing alarm or timer: any action silences it first */
    if (alarm_ring_active()) {
        alarm_ring_dismiss();
        screen_on_locked();
        lvgl_unlock();
        return;
    }
    if (screen_timer_is_ringing()) {
        screen_timer_dismiss();
        screen_on_locked();
        lvgl_unlock();
        return;
    }
    /* PWR always wakes; BOOT wakes unless the action works in the dark */
    if (!s_screen_on && (pwr || !runs_in_background(a))) {
        screen_on_locked();
        if (a == BTN_ACT_BACK || a == BTN_ACT_NONE || a == BTN_ACT_SCREEN_OFF) {   // waking is enough
            lvgl_unlock();
            return;
        }
    } else if (s_screen_on && a != BTN_ACT_SCREEN_OFF) {
        screen_on_locked();                          // restart the timeout
    }

    switch (a) {
    case BTN_ACT_BACK:
        if (ui_page_is_open())                      ui_page_back();
        else if (quick_panel_torch_active())       quick_panel_torch_exit();
        else if (overlay_open())                   close_overlays_locked();
        else if (!in_swipe(active_tile()))         go_back_locked();
        else if (active_tile() != TILE_WATCHFACE)  ui_open_tile(TILE_WATCHFACE);
        break;
    case BTN_ACT_HOME:
        ui_open_tile(TILE_WATCHFACE);
        break;
    case BTN_ACT_APPS:
        close_overlays_locked();
        menu_apps_open();
        break;
    case BTN_ACT_NOTIFICATIONS:
        if (notify_center_is_open()) {
            notify_center_close(true);
        } else {
            close_overlays_locked();
            notify_center_open();
        }
        break;
    case BTN_ACT_QUICK_PANEL:
        if (quick_panel_is_open()) {
            quick_panel_close(true);
        } else {
            close_overlays_locked();
            quick_panel_open();
        }
        break;
    case BTN_ACT_TORCH:
        if (quick_panel_torch_active()) {
            quick_panel_torch_exit();
        } else {
            close_overlays_locked();
            quick_panel_torch_start();
        }
        break;
    case BTN_ACT_PLAY_PAUSE:
        if (phone_ok_locked()) gb_send_music(gb_music_playing() ? "pause" : "play");
        break;
    case BTN_ACT_NEXT:
        if (phone_ok_locked()) gb_send_music("next");
        break;
    case BTN_ACT_PREVIOUS:
        if (phone_ok_locked()) gb_send_music("previous");
        break;
    case BTN_ACT_CAMERA:
        if (screen_camera_shutter()) break;          // Camera page: its own timer
        if (ble_media_ready()) {
            ble_hid_consumer_tap(HID_CONSUMER_VOL_UP);
            audio_beep(2400, 40, 40, 2);
        } else {
            toast_locked("Camera remote not paired");
        }
        break;
    case BTN_ACT_FIND_PHONE:
        if (phone_ok_locked()) {
            bool ringing = screen_bluetooth_find_phone_toggle();
            if (s_screen_on) toast_locked(ringing ? "Ringing the phone" : "Stopped ringing");
        }
        break;
    case BTN_ACT_STOPWATCH:
        /* Shows the stopwatch page too, unless it is already open */
        if (!screen_stopwatch_is_open()) {
            close_overlays_locked();
            screen_stopwatch_open();
        }
        screen_stopwatch_toggle();
        break;
    case BTN_ACT_TIMER:
        ui_open_tile(TILE_TIMER);
        break;
    case BTN_ACT_SCREEN_OFF:
        if (s_screen_on) screen_off_locked();
        break;
    case BTN_ACT_SILENT: {
        static uint8_t s_last_volume = 70;
        watch_settings_t *cfg = settings_get();
        if (cfg->volume) {
            s_last_volume = cfg->volume;
            cfg->volume = 0;
        } else {
            cfg->volume = s_last_volume ? s_last_volume : 70;
        }
        audio_set_volume(cfg->volume);
        watch_settings_save();
        if (cfg->volume) audio_beep(2000, 60, 0, 1);
        toast_locked(cfg->volume ? "Sound on" : "Sound off");
        break;
    }
    default:
        break;
    }
    lvgl_unlock();
}

void ui_timer_alarm(void)
{
    screen_on_locked();
    close_overlays_locked();
    set_tile_locked(TILE_TIMER, LV_ANIM_OFF, true);
}

void ui_show_message(const char *text)
{
    lvgl_lock();
    if (!s_message) {
        s_message = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_message);
        lv_obj_set_size(s_message, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(s_message, UI_COLOR_BG, 0);
        lv_obj_set_style_bg_opa(s_message, LV_OPA_COVER, 0);
        lv_obj_t *lbl = lv_label_create(s_message);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(lbl, UI_COLOR_TEXT, 0);
        lv_obj_center(lbl);
    }
    lv_label_set_text(lv_obj_get_child(s_message, 0), text);
    screen_on_locked();
    lv_refr_now(lv_display_get_default());
    lvgl_unlock();
}

typedef enum { SHUTDOWN_POWER_OFF, SHUTDOWN_RESTART, SHUTDOWN_FACTORY_RESET } shutdown_t;

/* Erase everything the user has set up: NVS (settings, alarms, steps, Wi-Fi
 * networks, Bluetooth pairings) and the recordings partition.  The RTC keeps
 * the time.  Runs just before a restart, so nothing writes the data back. */
static void factory_reset(void)
{
    recorder_stop();
    esp_err_t err = storage_format();
    if (err != ESP_OK) ESP_LOGE(TAG, "Storage format failed: %s", esp_err_to_name(err));
    err = nvs_flash_erase();          // de-initialises NVS first, closing open handles
    if (err != ESP_OK) ESP_LOGE(TAG, "NVS erase failed: %s", esp_err_to_name(err));
    ESP_LOGW(TAG, "Factory reset done");
}

static void shutdown_task(void *arg)
{
    shutdown_t mode = (shutdown_t)(intptr_t)arg;

    ui_show_message(mode == SHUTDOWN_FACTORY_RESET ? LV_SYMBOL_TRASH "  Resetting" :
                    mode == SHUTDOWN_RESTART       ? LV_SYMBOL_REFRESH "  Restarting" :
                                                     LV_SYMBOL_POWER "  Powering off");
    audio_stop();
    if (mode == SHUTDOWN_FACTORY_RESET) factory_reset();
    else                                activity_save();
    vTaskDelay(pdMS_TO_TICKS(mode == SHUTDOWN_POWER_OFF ? 1000 : 600));

    lvgl_lock();
    screen_off_locked();
    lvgl_unlock();

    if (mode != SHUTDOWN_POWER_OFF) esp_restart();
    power_off();   // does not return
}

static void request_shutdown(shutdown_t mode)
{
    if (s_powering_off) return;
    s_powering_off = true;
    /* 8 KB: ui_show_message() renders a frame on this task (LVGL text drawing) */
    xTaskCreate(shutdown_task, "shutdown", 8192, (void *)(intptr_t)mode, 6, NULL);
}

void ui_request_power_off(void)     { request_shutdown(SHUTDOWN_POWER_OFF); }
void ui_request_restart(void)       { request_shutdown(SHUTDOWN_RESTART); }
void ui_request_factory_reset(void) { request_shutdown(SHUTDOWN_FACTORY_RESET); }
