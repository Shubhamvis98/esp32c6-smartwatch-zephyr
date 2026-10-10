/* The ESP-IDF I2C master calls the watch uses, on Zephyr's i2c0. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

typedef struct esp_i2c_bus *i2c_master_bus_handle_t;
typedef struct esp_i2c_dev { uint16_t addr; } *i2c_master_dev_handle_t;

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *buf, size_t len, int timeout_ms);
esp_err_t i2c_master_receive(i2c_master_dev_handle_t dev, uint8_t *buf, size_t len, int timeout_ms);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *w, size_t wlen,
                                      uint8_t *r, size_t rlen, int timeout_ms);
