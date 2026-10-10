#pragma once
/**
 * @file lvgl_port.h
 * @brief LVGL tick source, handler task and thread-safety helpers.
 *
 * All tasks that create or modify LVGL objects MUST wrap their calls with
 * lvgl_lock() / lvgl_unlock().  The LVGL handler task already holds the
 * lock while calling lv_timer_handler(), so LVGL callbacks (events, lv_timer)
 * must NOT take it again — although the mutex is recursive, so doing so is
 * harmless.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_err.h"

/** Recursive mutex protecting all LVGL API calls. Created by lvgl_port_init(). */
extern SemaphoreHandle_t g_lvgl_mutex;

/**
 * @brief Install the LVGL tick source and start the LVGL handler task.
 *        Call after display_init() (which calls lv_init()).
 */
esp_err_t lvgl_port_init(void);

/* ui_init() holds the lock for about 0.5 s while it builds every screen,
 * so the LVGL task waits that long at boot.  At 500 ms this left a few ms of
 * margin and small UI additions caused a boot loop. */
#define LVGL_LOCK_TIMEOUT_MS  3000

/** Wait up to LVGL_LOCK_TIMEOUT_MS for the LVGL lock; assert on timeout. */
static inline void lvgl_lock(void)
{
    if (xSemaphoreTakeRecursive(g_lvgl_mutex, pdMS_TO_TICKS(LVGL_LOCK_TIMEOUT_MS)) != pdTRUE) {
        // Deadlock guard – should never happen in normal operation
        configASSERT(0);
    }
}

static inline void lvgl_unlock(void)
{
    xSemaphoreGiveRecursive(g_lvgl_mutex);
}
