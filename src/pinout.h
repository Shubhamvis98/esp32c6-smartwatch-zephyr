#pragma once

#include "driver/gpio.h"

// ─── Display & touch ────────────────────────────────────────────────────────
// ST7789V2 (SPI) + CST816T (I2C)
#define PIN_LCD_SCLK       GPIO_NUM_1
#define PIN_LCD_MOSI       GPIO_NUM_2
#define PIN_LCD_DC         GPIO_NUM_3
#define PIN_LCD_RST        GPIO_NUM_4
#define PIN_LCD_CS         GPIO_NUM_5
#define PIN_LCD_BL         GPIO_NUM_6
#define PIN_TP_INT         GPIO_NUM_11   // CST816T interrupt (active-low)
#define PIN_TP_RST         GPIO_NUM_NC   // Tied to LCD_RST (GPIO4) on the PCB

// ─── Shared I2C bus ─────────────────────────────────────────────────────────
// Shared by: CST816T (touch), PCF85063 (RTC), QMI8658 (IMU), ES8311 (codec)
#define PIN_I2C_SCL        GPIO_NUM_7
#define PIN_I2C_SDA        GPIO_NUM_8
#define I2C_BUS_PORT       I2C_NUM_0
#define I2C_BUS_SPEED_HZ   400000       // 400 kHz Fast-mode

// I2C device addresses
#define I2C_ADDR_CST816T   0x15
#define I2C_ADDR_PCF85063  0x51
#define I2C_ADDR_QMI8658   0x6B
#define I2C_ADDR_ES8311    0x18

// ─── Interrupt / wake pins ──────────────────────────────────────────────────
#define PIN_RTC_INT        GPIO_NUM_10   // PCF85063 alarm interrupt
// NOTE: GPIO9 = BOOT button AND QMI8658 INT1 — use IMU INT2 or polling
#define PIN_IMU_INT2       GPIO_NUM_NC   // Not connected on this board revision

// ─── Buttons ────────────────────────────────────────────────────────────────
#define PIN_BTN_BOOT       GPIO_NUM_9
#define PIN_BTN_PWR        GPIO_NUM_18   // Supports single/double/long-press (active-low)

// ─── Power / battery ────────────────────────────────────────────────────────
// Drive BAT_EN high early in boot to latch battery power on; otherwise the
// board turns off on battery as soon as PWR is released. Drive low to power off.
#define PIN_BAT_EN         GPIO_NUM_15
#define PIN_BAT_ADC        GPIO_NUM_0    // ADC1_CH0, battery via 1/3 divider (V = Vadc * 3)

// ─── Audio ──────────────────────────────────────────────────────────────────
// ES8311 codec (I2S) + NS4150B Class-D amp
// NS4150B CTRL is pulled up on the PCB (no GPIO) — amp is always enabled.
#define PIN_CODEC_MCLK     GPIO_NUM_19
#define PIN_CODEC_BCLK     GPIO_NUM_20
#define PIN_CODEC_ASDOUT   GPIO_NUM_21   // ADC out (mic → ESP)
#define PIN_CODEC_LRCK     GPIO_NUM_22
#define PIN_CODEC_DSDIN    GPIO_NUM_23   // DAC in  (ESP → speaker)

// ─── Display geometry ───────────────────────────────────────────────────────
#define LCD_H_RES          240
#define LCD_V_RES          280

// ─── Backlight LEDC ─────────────────────────────────────────────────────────
#define LEDC_BL_TIMER      LEDC_TIMER_0
#define LEDC_BL_MODE       LEDC_LOW_SPEED_MODE
#define LEDC_BL_CHANNEL    LEDC_CHANNEL_0
#define LEDC_BL_DUTY_RES   LEDC_TIMER_10_BIT   // 10-bit → 0–1023
#define LEDC_BL_FREQ_HZ    5000
