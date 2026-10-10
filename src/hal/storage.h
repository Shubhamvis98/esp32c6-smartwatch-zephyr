#pragma once
/**
 * @file hal/storage.h
 * @brief LittleFS on the "storage" flash partition (11 MB), mounted at
 *        STORAGE_PATH.  Formatted automatically on first use.
 */

#include <stdint.h>
#include "esp_err.h"

#define STORAGE_PATH  "/data"

esp_err_t storage_init(void);

/** @brief Free and total bytes; returns ESP_FAIL if not mounted. */
esp_err_t storage_get_space(uint64_t *free_bytes, uint64_t *total_bytes);

/** @brief Erase every file (quick format; stays mounted). */
esp_err_t storage_format(void);
