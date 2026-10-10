/**
 * @file hal/touch.c
 * @brief CST816T capacitive touch controller driver.
 *
 * Register map (auto-increment read starting at 0x01):
 *   0x01 GestureID   0x02 FingerNum
 *   0x03 XposH[3:0]  0x04 XposL
 *   0x05 YposH[3:0]  0x06 YposL
 *
 * The CST816T normally drops into a standby mode after a few seconds without
 * a touch, during which it NACKs I2C reads.  We disable that (DisAutoSleep)
 * so it can be polled from the LVGL indev callback at any time.
 *
 * TP_RST is tied to LCD_RST on this board, so the chip is reset by
 * display_init(); call touch_init() afterwards.
 */

#include "hal/touch.h"
#include "hal/i2c_bus.h"
#include "pinout.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch";

#define CST816_REG_GESTURE      0x01
#define CST816_REG_CHIP_ID      0xA7
#define CST816_REG_DIS_AUTOSLEEP 0xFE

#define CST816_I2C_TIMEOUT_MS   20

static i2c_master_dev_handle_t s_dev = NULL;
static lv_point_t s_last = { 0, 0 };

static esp_err_t cst816_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len,
                                       CST816_I2C_TIMEOUT_MS);
}

static esp_err_t cst816_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), CST816_I2C_TIMEOUT_MS);
}

esp_err_t touch_init(void)
{
    esp_err_t err = i2c_bus_add_device(I2C_ADDR_CST816T, I2C_BUS_SPEED_HZ, &s_dev);
    if (err != ESP_OK) return err;

    gpio_config_t int_cfg = {
        .pin_bit_mask = 1ULL << PIN_TP_INT,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&int_cfg);

    /* The controller needs a moment after the shared reset before it ACKs */
    uint8_t chip_id = 0;
    for (int i = 0; i < 10; i++) {
        err = cst816_read(CST816_REG_CHIP_ID, &chip_id, 1);
        if (err == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CST816T not responding: %s", esp_err_to_name(err));
        return err;
    }

    err = cst816_write(CST816_REG_DIS_AUTOSLEEP, 0x01);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not disable auto-sleep: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "CST816T ready (chip id 0x%02X)", chip_id);
    return ESP_OK;
}

/* Synthetic touches from the "watch tap/swipe" shell commands (testing). */
static volatile bool    s_fake_on;
static volatile bool    s_fake_pressed;
static volatile int16_t s_fake_x, s_fake_y;

void touch_fake(bool active, bool pressed, int x, int y)
{
    s_fake_x = x;
    s_fake_y = y;
    s_fake_pressed = pressed;
    s_fake_on = active;
}

void touch_get_data(lv_indev_data_t *data)
{
    uint8_t buf[6];

    if (s_fake_on) {
        s_last.x = s_fake_x;
        s_last.y = s_fake_y;
        data->point = s_last;
        data->state = s_fake_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        return;
    }

    data->point = s_last;
    data->state = LV_INDEV_STATE_RELEASED;

    if (!s_dev || cst816_read(CST816_REG_GESTURE, buf, sizeof(buf)) != ESP_OK) {
        return;
    }

    uint8_t fingers = buf[1] & 0x0F;
    if (fingers == 0) return;

    int32_t x = ((buf[2] & 0x0F) << 8) | buf[3];
    int32_t y = ((buf[4] & 0x0F) << 8) | buf[5];
    if (x >= LCD_H_RES) x = LCD_H_RES - 1;
    if (y >= LCD_V_RES) y = LCD_V_RES - 1;

    s_last.x = x;
    s_last.y = y;
    data->point = s_last;
    data->state = LV_INDEV_STATE_PRESSED;
}
