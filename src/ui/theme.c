/**
 * @file ui/theme.c
 * @brief Colour themes: Monochrome or Colour, and the LVGL theme built on them.
 */

#include "ui/theme.h"
#include "app/settings.h"
#include "src/themes/lv_theme_private.h"   // lv_theme_t is opaque; a child theme needs its size

static const char *const s_mode_names[THEME_COUNT] = { "Monochrome", "Colour" };

const uint32_t theme_swatches[THEME_SWATCH_COUNT] = {
    0x3DDC84, 0x63E6BE, 0x64D2FF, 0x0A84FF, 0x5E5CE6,
    0xBF5AF2, 0xFF375F, 0xFF453A, 0xFF9F0A, 0xFFD60A,
};

static const char *const s_bg_names[THEME_BG_COUNT] = { "Black", "Graphite" };

ui_theme_t g_ui_theme;       // set by ui_theme_init() before any screen is built

static lv_style_t s_on_accent;      // readable text on accent-filled widgets
static lv_style_t s_knob_on;        // switch knob on a light accent
static lv_theme_t s_theme;

const char *ui_theme_mode_name(uint8_t mode)
{
    return mode < THEME_COUNT ? s_mode_names[mode] : "";
}

const char *ui_theme_bg_name(uint8_t bg)
{
    return bg < THEME_BG_COUNT ? s_bg_names[bg] : "";
}

uint32_t ui_theme_accent_rgb(uint32_t accent)
{
    return (accent & THEME_ACCENT_SET) ? (accent & 0xFFFFFF) : theme_swatches[0];
}

lv_color_t ui_contrast(lv_color_t bg)
{
    uint32_t y = (bg.red * 299u + bg.green * 587u + bg.blue * 114u) / 1000u;
    return y > 150 ? lv_color_black() : lv_color_white();
}

void ui_fill(lv_obj_t *obj, lv_color_t color)
{
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_text_color(obj, ui_contrast(color), 0);
}

void ui_theme_build(ui_theme_t *t, uint8_t mode, uint8_t bg, uint32_t accent, bool app_colors)
{
    bool mono = mode != THEME_COLOUR;
    bool graphite = bg == THEME_BG_GRAPHITE;

    t->bg       = lv_color_hex(graphite ? 0x141416 : 0x000000);
    t->surface  = lv_color_hex(graphite ? 0x242427 : 0x1C1C1E);
    t->surface2 = lv_color_hex(graphite ? 0x323236 : 0x2C2C2E);
    t->raised   = lv_color_hex(graphite ? 0x434347 : 0x3A3A3C);
    t->faint    = lv_color_hex(graphite ? 0x58585C : 0x48484A);
    t->text     = lv_color_hex(0xFFFFFF);
    t->text2    = lv_color_hex(0xC8C8CC);
    t->dim      = lv_color_hex(mono ? 0x8E8E93 : 0x8A8F98);   // Colour: a blue-tinted grey

    if (mono) {
        t->accent  = lv_color_hex(0xFFFFFF);
        t->accent2 = lv_color_hex(0xAEAEB2);
        t->warn    = lv_color_hex(0xD1D1D6);
        t->danger  = lv_color_hex(0xFFFFFF);
    } else {
        uint32_t rgb = ui_theme_accent_rgb(accent);
        t->accent  = lv_color_hex(rgb);
        /* Second colour: blue, or green when the accent is itself blue */
        t->accent2 = lv_color_hex(rgb == 0x0A84FF || rgb == 0x64D2FF || rgb == 0x5E5CE6
                                  ? 0x3DDC84 : 0x4FA3FF);
        t->warn    = lv_color_hex(0xFF9F0A);
        t->danger  = lv_color_hex(0xFF453A);
    }
    t->accent_dim = lv_color_mix(t->accent, t->bg, LV_OPA_30);
    t->danger_dim = mono ? t->raised : lv_color_mix(t->danger, t->bg, LV_OPA_30);
    t->app_colors = app_colors;
}

static bool is_light(lv_color_t c)
{
    return ui_contrast(c).red == 0;
}

/* Runs after LVGL's default theme: where it puts white text on the accent
 * colour, use whatever reads on it (black on a white accent) */
static void apply_cb(lv_theme_t *th, lv_obj_t *obj)
{
    LV_UNUSED(th);
    if (!is_light(g_ui_theme.accent)) return;

    if (lv_obj_check_type(obj, &lv_button_class)) {
        lv_obj_add_style(obj, &s_on_accent, 0);
    } else if (lv_obj_check_type(obj, &lv_buttonmatrix_class)) {
        lv_obj_add_style(obj, &s_on_accent, LV_PART_ITEMS | LV_STATE_CHECKED);
    } else if (lv_obj_check_type(obj, &lv_roller_class)) {
        lv_obj_add_style(obj, &s_on_accent, LV_PART_SELECTED);
    } else if (lv_obj_check_type(obj, &lv_dropdownlist_class)) {
        lv_obj_add_style(obj, &s_on_accent, LV_PART_SELECTED | LV_STATE_CHECKED);
    } else if (lv_obj_check_type(obj, &lv_checkbox_class)) {
        lv_obj_add_style(obj, &s_on_accent, LV_PART_INDICATOR | LV_STATE_CHECKED);
    } else if (lv_obj_check_type(obj, &lv_switch_class)) {
        lv_obj_add_style(obj, &s_knob_on, LV_PART_KNOB | LV_STATE_CHECKED);
    }
}

void ui_theme_init(lv_display_t *disp)
{
    const watch_settings_t *cfg = settings_get();
    ui_theme_build(&g_ui_theme, cfg->theme, cfg->theme_bg, cfg->theme_accent, cfg->app_colors);

    lv_color_t on = ui_contrast(g_ui_theme.accent);
    lv_style_init(&s_on_accent);
    lv_style_set_text_color(&s_on_accent, on);
    lv_style_init(&s_knob_on);
    lv_style_set_bg_color(&s_knob_on, on);

    lv_theme_t *base = lv_theme_default_init(disp, g_ui_theme.accent, g_ui_theme.accent2,
                                             true, &lv_font_montserrat_16);
    s_theme = *base;
    lv_theme_set_parent(&s_theme, base);
    lv_theme_set_apply_cb(&s_theme, apply_cb);
    lv_display_set_theme(disp, &s_theme);
}
