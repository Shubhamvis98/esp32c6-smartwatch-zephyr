#pragma once
/**
 * @file hal/button.h
 * @brief Polled push buttons (active-low): press, double press, hold and
 *        a long hold.  Used for PWR and BOOT.
 *
 * BOOT is GPIO9, which is also the chip's boot-mode strap: holding it while
 * the watch resets starts the ROM flasher instead of the firmware.  It is
 * shared with the IMU's INT1 line, which the firmware keeps disabled.
 */

#include <stdbool.h>
#include "driver/gpio.h"
#include "esp_err.h"

typedef enum {
    BUTTON_CLICK,            // short press (after the double-press window, if on)
    BUTTON_DOUBLE,           // two short presses
    BUTTON_HOLD,             // held 1 s, fired while still held
    BUTTON_LONG_HOLD,        // held 5 s, fired while still held (after BUTTON_HOLD)
} button_event_t;

typedef enum {
    BUTTON_PWR,
    BUTTON_BOOT,
    BUTTON_COUNT,
} button_id_t;

typedef void (*button_cb_t)(button_id_t id, button_event_t event);

/** Start polling a button.  A press already held at start is ignored. */
esp_err_t button_start(button_id_t id, gpio_num_t pin, button_cb_t cb);

/** With double press off, a click fires on release instead of waiting
 *  to see whether a second press follows. */
void button_set_double(button_id_t id, bool enabled);
