#pragma once
/**
 * @file ui/theme.h
 * @brief The watch's colour theme: Monochrome or Colour (with a chosen accent)
 *        and a background shade, set in Settings → Theme.
 *
 * Every screen takes its colours from the UI_COLOR_* roles (ui_internal.h),
 * which read the active theme.  The theme is fixed at start-up: Settings
 * saves a new one and restarts the watch to apply it.
 */

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

typedef enum {
    THEME_MONO,         // greys and white
    THEME_COLOUR,       // an accent colour, with blue, orange and red alongside
    THEME_COUNT
} theme_mode_t;

typedef enum {
    THEME_BG_BLACK,
    THEME_BG_GRAPHITE,
    THEME_BG_COUNT
} theme_bg_t;

/** Accent colours offered for Colour mode (0xRRGGBB); the first is the default. */
#define THEME_SWATCH_COUNT 10
extern const uint32_t theme_swatches[THEME_SWATCH_COUNT];

/** settings.theme_accent: 0 = the default (green), else THEME_ACCENT_SET | 0xRRGGBB */
#define THEME_ACCENT_SET  0x01000000u

typedef struct {
    lv_color_t bg;          // screen background
    lv_color_t surface;     // cards, rows
    lv_color_t surface2;    // subtle fills: tracks, grid lines
    lv_color_t raised;      // secondary buttons
    lv_color_t faint;       // inactive marks on a surface
    lv_color_t text;
    lv_color_t text2;       // softer text
    lv_color_t dim;         // hints, captions
    lv_color_t accent;      // main colour: highlights, primary buttons
    lv_color_t accent2;     // second colour: secondary data, info
    lv_color_t accent_dim;  // accent on the background, for ring tracks
    lv_color_t warn;
    lv_color_t danger;
    lv_color_t danger_dim;  // dark fill for delete buttons
    bool       app_colors;  // menu icons in each app's own colour
} ui_theme_t;

/** The active theme (read through the UI_COLOR_* macros). */
extern ui_theme_t g_ui_theme;

/** Work out a theme from its settings, without applying it. */
void ui_theme_build(ui_theme_t *t, uint8_t mode, uint8_t bg, uint32_t accent, bool app_colors);
/** Load the theme from the settings and install it as the display's LVGL theme. */
void ui_theme_init(lv_display_t *disp);

const char *ui_theme_mode_name(uint8_t mode);
const char *ui_theme_bg_name(uint8_t bg);
/** The Colour-mode accent a settings.theme_accent value stands for, as 0xRRGGBB. */
uint32_t ui_theme_accent_rgb(uint32_t accent);

/** Black or white, whichever reads better on @p bg. */
lv_color_t ui_contrast(lv_color_t bg);
/** Set @p obj's background to @p color and its text to a readable colour. */
void ui_fill(lv_obj_t *obj, lv_color_t color);
