/**
 * @file hal/i2c_bus.c
 * @brief Shared I2C bus (touch, IMU, RTC, codec) on Zephyr's i2c0.
 */
#include "hal/i2c_bus.h"

#include <stdlib.h>
#include <zephyr/drivers/i2c.h>

static const struct device *const s_i2c = DEVICE_DT_GET(DT_NODELABEL(i2c0));

static esp_err_t map(int r)
{
    return r == 0 ? ESP_OK : r == -ETIMEDOUT ? ESP_ERR_TIMEOUT : ESP_FAIL;
}

esp_err_t i2c_bus_init(void)
{
    return device_is_ready(s_i2c) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

i2c_master_bus_handle_t i2c_bus_get_handle(void)
{
    return (i2c_master_bus_handle_t)s_i2c;
}

esp_err_t i2c_bus_add_device(uint16_t addr, uint32_t speed_hz, i2c_master_dev_handle_t *out)
{
    (void)speed_hz;                    /* the bus speed is set in the devicetree */
    struct esp_i2c_dev *d = malloc(sizeof(*d));
    if (!d) return ESP_ERR_NO_MEM;
    d->addr = addr;
    *out = d;
    return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *buf, size_t len, int timeout_ms)
{
    return map(i2c_write(s_i2c, buf, len, dev->addr));
}

esp_err_t i2c_master_receive(i2c_master_dev_handle_t dev, uint8_t *buf, size_t len, int timeout_ms)
{
    return map(i2c_read(s_i2c, buf, len, dev->addr));
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *w, size_t wlen,
                                      uint8_t *r, size_t rlen, int timeout_ms)
{
    return map(i2c_write_read(s_i2c, dev->addr, w, wlen, r, rlen));
}
