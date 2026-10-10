#pragma once
/**
 * @file hal/imu.h
 * @brief QMI8658 6-axis IMU — accelerometer only (gyro left off to save power).
 */

#include "esp_err.h"

typedef struct {
    float x, y, z;      // acceleration in g
} imu_accel_t;

/** @brief Reset the QMI8658 and enable the accelerometer (±4 g, ~56 Hz). */
esp_err_t imu_init(void);

/** @brief Read the latest accelerometer sample. */
esp_err_t imu_read_accel(imu_accel_t *out);
