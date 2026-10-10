/**
 * @file ui/quick_panel.c
 * @brief Pull-down quick panel on the watchface, plus the torch screen.
 *
 *   swipe down on the watchface
 *   ┌──────────────────────────┐
 *   │ 10:42   Sat 3 Oct    85% │
 *   │  (☀)     (⚙)     (⚡)     │
 *   │  60%   Settings   Torch  │
 *   │          ────            │  ← swipe up / tap below to close
 *   └──────────────────────────┘
 *
 * Brightness cycles through 5 levels.  Torch shows a full-white screen
 * (kept on, no idle timeout); tapping it toggles between the normal
 * brightness and 100 %, holding it (or a PWR click / swipe) exits.
 *
 * Swipes (open / close / exit torch) are detected in ui_manager.c.
 */

#include "ui/ui_internal.h"
#include "hal/battery.h"
#include "hal/display.h"
#include "hal/rtc.h"
#include "app/settings.h"

#include <time.h>

#define PANEL_H          150
#define ANIM_MS          220
#define TORCH_MAX_PCT    100

static const uint8_t s_levels[] = { 20, 40, 60, 80, 100 };
#define LEVEL_COUNT (sizeof(s_levels) / sizeof(s_levels[0]))

static lv_obj_t   *s_backdrop;       // dims the watchface, tap to close
static lv_obj_t   *s_panel;
static lv_obj_t   *s_time;
static lv_obj_t   *s_batt;
static lv_obj_t   *s_bright_caption;
static lv_obj_t   *s_aod_btn;
static lv_timer_t *s_timer;
static bool        s_open;

static lv_obj_t   *s_torch;
static bool        s_torch_max;      // torch currently at 100 %

/* ── Helpers ─────────────────────────────────────────────────────────────── */
static void anim_y(void *obj, int32_t v)   { lv_obj_set_y(obj, v); }
static void anim_opa(void *obj, int32_t v) { lv_obj_set_style_bg_opa(obj, (lv_opa_t)v, 0); }

static void animate(lv_obj_t *obj, lv_anim_exec_xcb_t cb, int32_t from, int32_t to,
                    lv_anim_completed_cb_t done)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    if (done) lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);
}

static int level_index(uint8_t pct)
{
    /* Highest level at or below pct (pct may be any 5–100 from the slider) */
    int idx = 0;
    for (int i = 0; i < (int)LEVEL_COUNT; i++) {
        if (pct >= s_levels[i]) idx = i;
    }
    return idx;
}

/* ── Panel content ───────────────────────────────────────────────────────── */
static void panel_update(void)
{
    if (rtc_time_valid()) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        char buf[32];
        if (settings_get()->use_24h) {
            strftime(buf, sizeof(buf), "%H:%M  %a %d %b", &tm);
        } else {
            strftime(buf, sizeof(buf), "%I:%M %p  %a %d", &tm);
        }
        lv_label_set_text(s_time, buf);
    } else {
        lv_label_set_text(s_time, "--:--");
    }

    int mv = ui_battery_mv();
    if (battery_mv_is_charging(mv)) {
        lv_label_set_text(s_batt, LV_SYMBOL_CHARGE " USB");
    } else {
        lv_label_set_text_fmt(s_batt, "%u%%", battery_percent_from_mv(mv));
    }

    uint8_t pct = settings_get()->brightness;
    lv_label_set_text_fmt(s_bright_caption, "%u%%", pct);
    lv_obj_set_style_bg_color(s_aod_btn, settings_get()->aod ? UI_COLOR_ACCENT2 : UI_COLOR_RAISED, 0);
}

static void panel_timer_cb(lv_timer_t *t)
{
    panel_update();
}

static void brightness_cb(lv_event_t *e)
{
    watch_settings_t *cfg = settings_get();
    int next = level_index(cfg->brightness) + 1;
    /* If the slider left us between levels, step to the next level up */
    if (cfg->brightness < s_levels[next - 1]) next--;
    cfg->brightness = s_levels[next % LEVEL_COUNT];
    display_set_backlight(cfg->brightness);
    watch_settings_save();
    panel_update();
}

static void settings_cb(lv_event_t *e)
{
    ui_open_tile(TILE_SETTINGS);
}

static void aod_cb(lv_event_t *e)
{
    settings_get()->aod = !settings_get()->aod;
    watch_settings_save();
    panel_update();
}

static void torch_cb(lv_event_t *e)
{
    quick_panel_torch_start();
}

static void backdrop_cb(lv_event_t *e)
{
    quick_panel_close(true);
}

static lv_obj_t *round_button(lv_obj_t *parent, const char *icon, lv_color_t bg,
                              lv_event_cb_t cb, lv_obj_t **caption_out, const char *caption)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);   // as wide as the button or caption
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 4, 0);
    lv_obj_set_clickable(col, false);

    lv_obj_t *btn = lv_button_create(col);
    lv_obj_set_size(btn, 48, 48);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    ui_fill(btn, bg);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    if (icon) {
        lv_obj_t *ic = lv_label_create(btn);
        lv_label_set_text(ic, icon);
        lv_obj_set_style_text_font(ic, &lv_font_montserrat_20, 0);
        lv_obj_center(ic);
    }

    lv_obj_t *cap = lv_label_create(col);
    lv_label_set_text(cap, caption);
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cap, UI_COLOR_DIM, 0);
    if (caption_out) *caption_out = cap;
    return btn;
}

static void create_panel(void)
{
    s_backdrop = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_backdrop);
    lv_obj_set_size(s_backdrop, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_backdrop, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_backdrop, LV_OPA_TRANSP, 0);
    lv_obj_set_clickable(s_backdrop, true);
    lv_obj_set_scrollable(s_backdrop, false);
    lv_obj_add_event_cb(s_backdrop, backdrop_cb, LV_EVENT_CLICKED, NULL);

    s_panel = lv_obj_create(s_backdrop);
    lv_obj_remove_style_all(s_panel);
    lv_obj_set_size(s_panel, LV_PCT(100), PANEL_H);
    lv_obj_set_pos(s_panel, 0, -PANEL_H);
    lv_obj_set_style_bg_color(s_panel, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_panel, 24, 0);
    lv_obj_set_style_pad_top(s_panel, 14, 0);
    lv_obj_set_style_pad_hor(s_panel, 12, 0);
    lv_obj_set_clickable(s_panel, true);           // taps on the panel don't close it
    lv_obj_set_scrollable(s_panel, false);

    s_time = lv_label_create(s_panel);
    lv_obj_align(s_time, LV_ALIGN_TOP_LEFT, 0, 0);
    s_batt = lv_label_create(s_panel);
    lv_obj_set_style_text_color(s_batt, UI_COLOR_DIM, 0);
    lv_obj_align(s_batt, LV_ALIGN_TOP_RIGHT, 0, 0);

    lv_obj_t *row = lv_obj_create(s_panel);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_clickable(row, false);

    lv_obj_t *bbtn = round_button(row, NULL, UI_COLOR_RAISED,
                                  brightness_cb, &s_bright_caption, "");
    lv_obj_t *sun = ui_icon_create(bbtn, ui_icon_sun(), UI_COLOR_TEXT);
    lv_obj_center(sun);
    s_aod_btn = round_button(row, LV_SYMBOL_EYE_OPEN, UI_COLOR_RAISED, aod_cb, NULL, "AOD");
    round_button(row, LV_SYMBOL_SETTINGS, UI_COLOR_RAISED, settings_cb, NULL, "Settings");
    lv_obj_t *tbtn = round_button(row, NULL, UI_COLOR_WARN, torch_cb, NULL, "Torch");
    lv_obj_center(ui_icon_create(tbtn, ui_icon_torch(), UI_COLOR_TEXT));

    /* Grab handle */
    lv_obj_t *handle = lv_obj_create(s_panel);
    lv_obj_remove_style_all(handle);
    lv_obj_set_size(handle, 36, 4);
    lv_obj_align(handle, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_radius(handle, 2, 0);
    lv_obj_set_style_bg_color(handle, UI_COLOR_DIM, 0);
    lv_obj_set_style_bg_opa(handle, LV_OPA_COVER, 0);
    lv_obj_set_clickable(handle, false);

    lv_obj_set_hidden(s_backdrop, true);
}

/* ── Panel open / close ──────────────────────────────────────────────────── */
void quick_panel_open(void)
{
    if (!s_panel) create_panel();
    if (s_open) return;
    s_open = true;

    panel_update();
    lv_obj_set_hidden(s_backdrop, false);
    lv_obj_move_foreground(s_backdrop);
    animate(s_panel, anim_y, -PANEL_H, 0, NULL);
    animate(s_backdrop, anim_opa, LV_OPA_TRANSP, LV_OPA_50, NULL);
    s_timer = lv_timer_create(panel_timer_cb, 1000, NULL);
}

static void close_done_cb(lv_anim_t *a)
{
    if (!s_open) lv_obj_set_hidden(s_backdrop, true);
}

void quick_panel_close(bool anim)
{
    if (!s_open) return;
    s_open = false;
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }

    lv_anim_delete(s_panel, anim_y);
    lv_anim_delete(s_backdrop, anim_opa);
    if (anim) {
        animate(s_panel, anim_y, lv_obj_get_y(s_panel), -PANEL_H, NULL);
        animate(s_backdrop, anim_opa, lv_obj_get_style_bg_opa(s_backdrop, 0), LV_OPA_TRANSP,
                close_done_cb);
    } else {
        lv_obj_set_y(s_panel, -PANEL_H);
        lv_obj_set_hidden(s_backdrop, true);
    }
}

bool quick_panel_is_open(void)
{
    return s_open;
}

/* ── Torch ───────────────────────────────────────────────────────────────── */
static void torch_tap_cb(lv_event_t *e)
{
    s_torch_max = !s_torch_max;
    display_set_backlight(s_torch_max ? TORCH_MAX_PCT : settings_get()->brightness);
}

static void torch_hold_cb(lv_event_t *e)
{
    quick_panel_torch_exit();
}

void quick_panel_torch_start(void)
{
    if (s_torch) return;
    quick_panel_close(false);

    s_torch = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_torch);
    lv_obj_set_size(s_torch, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_torch, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_torch, LV_OPA_COVER, 0);
    lv_obj_set_clickable(s_torch, true);
    lv_obj_set_scrollable(s_torch, false);
    lv_obj_add_event_cb(s_torch, torch_tap_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(s_torch, torch_hold_cb, LV_EVENT_LONG_PRESSED, NULL);

    lv_obj_t *hint = lv_label_create(s_torch);
    lv_label_set_text(hint, "Tap: max brightness\nHold or swipe: exit");
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, UI_COLOR_TEXT2, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -UI_SAFE_MARGIN - 6);

    s_torch_max = false;
    display_set_backlight(settings_get()->brightness);
}

bool quick_panel_torch_active(void)
{
    return s_torch != NULL;
}

void quick_panel_torch_exit(void)
{
    if (!s_torch) return;
    lv_obj_delete(s_torch);
    s_torch = NULL;
    s_torch_max = false;
    display_set_backlight(settings_get()->brightness);
    lv_display_trigger_activity(lv_display_get_default());   // fresh idle timeout
}
