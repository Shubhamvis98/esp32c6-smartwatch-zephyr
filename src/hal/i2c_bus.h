#pragma once
/**
 * @file hal/i2c_bus.h
 * @brief Shared I2C master bus (GPIO7/GPIO8).
 *
 * All I2C peripherals (CST816T, PCF85063, QMI8658, ES8311) share a single
 * I2C_NUM_0 master bus.  Call i2c_bus_init() once at startup, then call
 * i2c_bus_add_device() for each peripheral.
 */

#include "driver/i2c_master.h"
#include "esp_err.h"

/**
 * @brief Initialise the shared I2C master bus.
 *        Must be called before any other i2c_bus_* function.
 */
esp_err_t i2c_bus_init(void);

/** @brief Return the master bus handle (valid after i2c_bus_init). */
i2c_master_bus_handle_t i2c_bus_get_handle(void);

/**
 * @brief Add a device to the shared bus.
 *
 * @param addr       7-bit I2C device address
 * @param speed_hz   SCL frequency in Hz (usually 100000 or 400000)
 * @param out_handle Returned device handle
 */
esp_err_t i2c_bus_add_device(uint16_t addr,
                               uint32_t speed_hz,
                               i2c_master_dev_handle_t *out_handle);
