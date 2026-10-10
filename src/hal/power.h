#pragma once
/**
 * @file hal/power.h
 * @brief Battery power latch (BAT_EN).  The PWR button is read by hal/button.c.
 *
 * On battery, the PWR button only powers the board while it is held.
 * power_hold() drives BAT_EN high to keep the regulator on after release;
 * power_off() drives it low.  When USB is connected the board stays powered
 * regardless, so power_off() then just blanks everything and waits for the
 * PWR button to reboot.
 */

#include "esp_err.h"

/** @brief Latch battery power on.  Call as the very first thing in app_main. */
void power_hold(void);

/** @brief Cut battery power.  Does not return. */
void power_off(void) __attribute__((noreturn));
