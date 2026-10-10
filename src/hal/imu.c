/**
 * @file hal/imu.c
 * @brief QMI8658 accelerometer driver.
 *
 * Only the accelerometer is used: step counting and raise-to-wake don't need
 * the gyro, and leaving it off saves ~0.5 mA.  Samples are polled; the INT
 * pins are left disabled (INT1 shares GPIO9 with the BOOT button).
 */

#include "hal/imu.h"
#include "hal/i2c_bus.h"
#include "pinout.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "imu";

#define QMI_REG_WHO_AM_I   0x00
#define QMI_REG_CTRL1      0x02
#define QMI_REG_CTRL2      0x03
#define QMI_REG_CTRL7      0x08
#define QMI_REG_AX_L       0x35
#define QMI_REG_RESET      0x60

#define QMI_WHO_AM_I_VAL   0x05
#define QMI_RESET_CMD      0xB0

#define QMI_CTRL1_ADDR_AI  (1 << 6)   // register auto-increment, little-endian
#define QMI_ACC_FS_4G      (1 << 4)
#define QMI_ACC_ODR_56HZ   0x07       // accel-only mode: 56.05 Hz
#define QMI_CTRL7_AEN      (1 << 0)

#define QMI_LSB_PER_G      8192.0f    // ±4 g full scale
#define QMI_I2C_TIMEOUT_MS 20

static i2c_master_dev_handle_t s_dev = NULL;

static esp_err_t qmi_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), QMI_I2C_TIMEOUT_MS);
}

static esp_err_t qmi_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, QMI_I2C_TIMEOUT_MS);
}

esp_err_t imu_init(void)
{
    esp_err_t err = i2c_bus_add_device(I2C_ADDR_QMI8658, I2C_BUS_SPEED_HZ, &s_dev);
    if (err != ESP_OK) return err;

    uint8_t id = 0;
    err = qmi_read(QMI_REG_WHO_AM_I, &id, 1);
    if (err != ESP_OK || id != QMI_WHO_AM_I_VAL) {
        ESP_LOGE(TAG, "QMI8658 not found (err=%s id=0x%02X)", esp_err_to_name(err), id);
        return err != ESP_OK ? err : ESP_ERR_NOT_FOUND;
    }

    qmi_write(QMI_REG_RESET, QMI_RESET_CMD);
    vTaskDelay(pdMS_TO_TICKS(20));

    err  = qmi_write(QMI_REG_CTRL1, QMI_CTRL1_ADDR_AI);
    err |= qmi_write(QMI_REG_CTRL2, QMI_ACC_FS_4G | QMI_ACC_ODR_56HZ);
    err |= qmi_write(QMI_REG_CTRL7, QMI_CTRL7_AEN);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "QMI8658 configuration failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "QMI8658 ready (accel ±4 g @ 56 Hz)");
    return ESP_OK;
}

esp_err_t imu_read_accel(imu_accel_t *out)
{
    uint8_t b[6];
    esp_err_t err = qmi_read(QMI_REG_AX_L, b, sizeof(b));
    if (err != ESP_OK) return err;

    out->x = (int16_t)(b[0] | (b[1] << 8)) / QMI_LSB_PER_G;
    out->y = (int16_t)(b[2] | (b[3] << 8)) / QMI_LSB_PER_G;
    out->z = (int16_t)(b[4] | (b[5] << 8)) / QMI_LSB_PER_G;
    return ESP_OK;
}
