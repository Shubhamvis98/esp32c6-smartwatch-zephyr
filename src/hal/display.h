#pragma once
/**
 * @file hal/display.h
 * @brief ST7789V2 display driver with LVGL 9 integration.
 *
 * Call display_init() once.  After that LVGL handles all rendering;
 * use lvgl_lock()/lvgl_unlock() around any lv_* calls from your tasks.
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

/**
 * @brief Initialise the SPI bus, ST7789V2 panel, LVGL, and DMA draw buffers.
 *        The backlight is left off; turn it on with display_set_backlight()
 *        once the first frame has been rendered.
 */
esp_err_t display_init(bool fps30);

/**
 * @brief Switch between 60 fps and 30 fps redraws (takes effect at once).
 *        The draw-buffer size that goes with it is chosen at display_init().
 */
void display_set_fps30(bool fps30);

/** @brief The mode the draw buffers were allocated for at boot. */
bool display_buffers_fps30(void);

/**
 * @brief Set backlight brightness.
 * @param percent 0 (off) … 100 (full brightness)
 */
void display_set_backlight(uint8_t percent);

/** @brief Last brightness set with display_set_backlight(). */
uint8_t display_get_backlight(void);

/** @brief Put the panel in display-off mode (true) or turn it back on. */
void display_set_sleep(bool sleep);

/** @brief Return the LVGL display handle (valid after display_init). */
lv_display_t *display_get_lvgl_disp(void);
