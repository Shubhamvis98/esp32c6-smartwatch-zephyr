/**
 * @file hal/rtc.c
 * @brief PCF85063 real-time clock driver.
 *
 * Time registers 0x04..0x0A are BCD:
 *   0x04 seconds (bit 7 = OS, oscillator stopped → time invalid)
 *   0x05 minutes  0x06 hours (24 h)  0x07 days  0x08 weekday
 *   0x09 months   0x0A years (00–99, we use 2000–2099)
 */

#include "hal/rtc.h"
#include "hal/i2c_bus.h"
#include "pinout.h"

#include <stdint.h>
#include <string.h>
#include <sys/time.h>
#include "esp_log.h"

static const char *TAG = "rtc";

#define PCF_REG_CTRL1    0x00
#define PCF_REG_SECONDS  0x04
#define PCF_CTRL1_12_24  (1 << 1)
#define PCF_SECONDS_OS   (1 << 7)

#define PCF_I2C_TIMEOUT_MS 50

static i2c_master_dev_handle_t s_dev = NULL;
static bool s_valid = false;

static uint8_t rtc_bcd2bin(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t rtc_bin2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

/* Days since 1970-01-01 for a proleptic Gregorian date (H. Hinnant's
 * algorithm).  Used instead of mktime(), which depends on the TZ variable. */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static esp_err_t pcf_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, PCF_I2C_TIMEOUT_MS);
}

static esp_err_t pcf_write(uint8_t reg, const uint8_t *buf, size_t len)
{
    uint8_t tx[8];
    tx[0] = reg;
    memcpy(&tx[1], buf, len);
    return i2c_master_transmit(s_dev, tx, len + 1, PCF_I2C_TIMEOUT_MS);
}

esp_err_t rtc_read(time_t *out)
{
    uint8_t r[7];
    esp_err_t err = pcf_read(PCF_REG_SECONDS, r, sizeof(r));
    if (err != ESP_OK) return err;
    if (r[0] & PCF_SECONDS_OS) return ESP_ERR_INVALID_STATE;

    int year = rtc_bcd2bin(r[6]) + 2000;
    int mon  = rtc_bcd2bin(r[5] & 0x1F);
    int day  = rtc_bcd2bin(r[3] & 0x3F);

    *out = (time_t)days_from_civil(year, mon, day) * 86400
         + rtc_bcd2bin(r[2] & 0x3F) * 3600
         + rtc_bcd2bin(r[1] & 0x7F) * 60
         + rtc_bcd2bin(r[0] & 0x7F);
    return ESP_OK;
}

esp_err_t rtc_write(time_t t)
{
    struct tm tm;
    gmtime_r(&t, &tm);

    uint8_t r[7] = {
        rtc_bin2bcd(tm.tm_sec),            /* also clears the OS flag */
        rtc_bin2bcd(tm.tm_min),
        rtc_bin2bcd(tm.tm_hour),
        rtc_bin2bcd(tm.tm_mday),
        (uint8_t)tm.tm_wday,
        rtc_bin2bcd(tm.tm_mon + 1),
        rtc_bin2bcd(tm.tm_year % 100),
    };
    esp_err_t err = pcf_write(PCF_REG_SECONDS, r, sizeof(r));
    if (err == ESP_OK) s_valid = true;
    return err;
}

bool rtc_time_valid(void)
{
    return s_valid;
}

esp_err_t rtc_init(void)
{
    esp_err_t err = i2c_bus_add_device(I2C_ADDR_PCF85063, I2C_BUS_SPEED_HZ, &s_dev);
    if (err != ESP_OK) return err;

    /* Force 24 h mode */
    uint8_t ctrl1;
    err = pcf_read(PCF_REG_CTRL1, &ctrl1, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PCF85063 not responding: %s", esp_err_to_name(err));
        return err;
    }
    if (ctrl1 & PCF_CTRL1_12_24) {
        ctrl1 &= ~PCF_CTRL1_12_24;
        pcf_write(PCF_REG_CTRL1, &ctrl1, 1);
    }

    time_t now;
    err = rtc_read(&now);
    if (err == ESP_OK) {
        struct timeval tv = { .tv_sec = now };
        settimeofday(&tv, NULL);
        s_valid = true;
        ESP_LOGI(TAG, "System clock set from RTC: %lld", (long long)now);
    } else {
        ESP_LOGW(TAG, "RTC time invalid (battery lost?) — waiting for NTP");
    }
    return ESP_OK;
}
