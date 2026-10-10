#pragma once
/**
 * @file hal/battery.h
 * @brief Li-ion battery voltage via ADC1_CH0 (GPIO0) behind a 1/3 divider.
 *
 * The ETA6098 STAT pin only drives an LED on this board, so charging can't
 * be detected directly.  A reading above ~4.25 V means USB is supplying VSYS.
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t battery_init(void);

/** @brief Battery voltage in millivolts (averaged over several samples). */
int battery_read_mv(void);

/** @brief Map a voltage to 0–100 % using a typical Li-ion discharge curve. */
uint8_t battery_percent_from_mv(int mv);

/** @brief True when the voltage suggests USB power / charging. */
bool battery_mv_is_charging(int mv);
