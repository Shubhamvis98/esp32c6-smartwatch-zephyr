#pragma once
/**
 * @file hal/touch.h
 * @brief CST816T capacitive touch controller driver.
 *
 * The CST816T communicates over I2C at address 0x15.
 * It reports finger position and hardware-detected gestures (swipe up/down/
 * left/right, single-click, double-click, long-press).
 *
 * touch_init() must be called after i2c_bus_init().
 * touch_get_data() is designed to be used as the LVGL indev read callback.
 */

#include "esp_err.h"
#include "lvgl.h"

/** @brief Initialise CST816T and configure the INT pin as an input. */
esp_err_t touch_init(void);

/**
 * @brief Read the current touch state into an LVGL indev data struct.
 *
 * @param data  Filled with point coordinates and press/release state.
 *              On an I2C error the point is reported as released.
 */
void touch_get_data(lv_indev_data_t *data);
