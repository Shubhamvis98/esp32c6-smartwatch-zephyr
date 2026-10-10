/* The small part of the ESP-IDF GPIO driver the watch uses, on Zephyr's gpio0. */
#pragma once
#include <stdint.h>
#include "esp_err.h"
#include <soc/gpio_num.h>

typedef enum { GPIO_MODE_DISABLE = 0, GPIO_MODE_INPUT = 1, GPIO_MODE_OUTPUT = 2,
               GPIO_MODE_INPUT_OUTPUT = 3, GPIO_MODE_OUTPUT_OD = 6 } gpio_mode_t;
typedef enum { GPIO_PULLUP_DISABLE = 0, GPIO_PULLUP_ENABLE = 1 } gpio_pullup_t;
typedef enum { GPIO_PULLDOWN_DISABLE = 0, GPIO_PULLDOWN_ENABLE = 1 } gpio_pulldown_t;
typedef enum { GPIO_INTR_DISABLE = 0, GPIO_INTR_POSEDGE, GPIO_INTR_NEGEDGE,
               GPIO_INTR_ANYEDGE, GPIO_INTR_LOW_LEVEL, GPIO_INTR_HIGH_LEVEL } gpio_int_type_t;

typedef struct {
    uint64_t        pin_bit_mask;
    gpio_mode_t     mode;
    gpio_pullup_t   pull_up_en;
    gpio_pulldown_t pull_down_en;
    gpio_int_type_t intr_type;
} gpio_config_t;

esp_err_t esp_gpio_config(const gpio_config_t *cfg);
int       esp_gpio_get_level(gpio_num_t pin);
esp_err_t esp_gpio_set_level(gpio_num_t pin, uint32_t level);

/* Zephyr has its own gpio_* names, so map ours onto prefixed ones. */
#define gpio_config     esp_gpio_config
#define gpio_get_level  esp_gpio_get_level
#define gpio_set_level  esp_gpio_set_level
