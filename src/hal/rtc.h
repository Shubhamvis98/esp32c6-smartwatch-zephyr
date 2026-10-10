#pragma once
/**
 * @file hal/rtc.h
 * @brief PCF85063 real-time clock (I2C 0x51).
 *
 * The RTC always holds UTC.  At boot, rtc_init() copies it into the system
 * clock, so the rest of the firmware just uses time()/localtime_r().
 */

#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

/**
 * @brief Probe the RTC and, if it holds a valid time, set the system clock.
 *        Must be called after i2c_bus_init().
 */
esp_err_t rtc_init(void);

/** @brief True if the RTC had a valid time at boot or has been set since. */
bool rtc_time_valid(void);

/** @brief Read the RTC (UTC) as a UNIX timestamp. */
esp_err_t rtc_read(time_t *out);

/** @brief Write a UNIX timestamp (UTC) to the RTC. */
esp_err_t rtc_write(time_t t);
