/* ESP-IDF error codes on Zephyr. */
#pragma once
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

typedef int esp_err_t;

#define ESP_OK                    0
#define ESP_FAIL                  -1
#define ESP_ERR_NO_MEM            0x101
#define ESP_ERR_INVALID_ARG       0x102
#define ESP_ERR_INVALID_STATE     0x103
#define ESP_ERR_INVALID_SIZE      0x104
#define ESP_ERR_NOT_FOUND         0x105
#define ESP_ERR_NOT_SUPPORTED     0x106
#define ESP_ERR_TIMEOUT           0x107
#define ESP_ERR_INVALID_RESPONSE  0x108
#define ESP_ERR_INVALID_CRC       0x109
#define ESP_ERR_INVALID_VERSION   0x10A
#define ESP_ERR_NOT_FINISHED      0x10C
#define ESP_ERR_NOT_ALLOWED       0x10D
#define ESP_ERR_NVS_BASE              0x1100
#define ESP_ERR_NVS_NOT_FOUND         0x1102
#define ESP_ERR_NVS_INVALID_LENGTH    0x110c
#define ESP_ERR_NVS_NO_FREE_PAGES     0x110d
#define ESP_ERR_NVS_NEW_VERSION_FOUND 0x1110
#define ESP_ERR_OTA_VALIDATE_FAILED   0x1503

const char *esp_err_to_name(esp_err_t code);

#define ESP_ERROR_CHECK(x) do {                                         \
        esp_err_t err_rc_ = (x);                                        \
        if (err_rc_ != ESP_OK) {                                        \
            printk("ESP_ERROR_CHECK failed: %s (0x%x) at %s:%d\n",      \
                   esp_err_to_name(err_rc_), err_rc_, __FILE__, __LINE__); \
            k_panic();                                                  \
        }                                                               \
    } while (0)

#define ESP_ERROR_CHECK_WITHOUT_ABORT(x) ({ esp_err_t e__ = (x); e__; })
