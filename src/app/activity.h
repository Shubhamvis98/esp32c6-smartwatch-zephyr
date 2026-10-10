#pragma once
/**
 * @file app/activity.h
 * @brief Step counting and raise-to-wake from the QMI8658 accelerometer.
 *
 * Runs a 50 Hz sampling task.  Steps reset at local midnight and are saved
 * to NVS every few minutes (and by activity_save()) so they survive reboots.
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef void (*activity_raise_cb_t)(void);

/** @brief Start the sampling task.  Needs imu_init() and nvs_flash_init(). */
esp_err_t activity_start(void);

/** @brief Steps counted today. */
uint32_t activity_get_steps(void);

/** @brief Called from the IMU task when a wrist raise is detected. */
void activity_set_raise_cb(activity_raise_cb_t cb);

/** @brief Enable/disable raise detection (e.g. from settings). */
void activity_set_raise_enabled(bool enabled);

/** @brief Use calibrated poses for raise detection: the accelerometer
 *  reading while looking at the watch (@p up) and with the arm hanging
 *  down (@p down), in g.  NULL for either restores the built-in rule. */
void activity_set_raise_calibration(const float up[3], const float down[3]);

/** @brief Persist today's step count now (e.g. before power-off). */
void activity_save(void);

/** Live accelerometer and raise-detector state, for the Sensors app. */
typedef struct {
    float    x, y, z;            // latest sample, g
    bool     face_up;            // detector considers the screen face-up
    int32_t  vertical_ago_ms;    // since the arm was last vertical, -1 = never
    uint32_t raises;             // raises detected since boot (even if disabled)
    int32_t  raise_ago_ms;       // since the last raise, -1 = none yet
} activity_debug_t;

void activity_get_debug(activity_debug_t *out);
