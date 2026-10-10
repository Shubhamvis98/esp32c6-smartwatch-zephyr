/**
 * @file lvgl_port.c
 * @brief LVGL tick source and handler task.
 */

#include "lvgl_port.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "lvgl_port";

#define LVGL_TASK_STACK      (8 * 1024)
#define LVGL_TASK_PRIO       4
#define LVGL_TASK_MIN_MS     5
#define LVGL_TASK_MAX_MS     50

SemaphoreHandle_t g_lvgl_mutex = NULL;

static uint32_t tick_get_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void lvgl_task(void *arg)
{
    for (;;) {
        lvgl_lock();
        uint32_t next_ms = lv_timer_handler();
        lvgl_unlock();

        if (next_ms < LVGL_TASK_MIN_MS) next_ms = LVGL_TASK_MIN_MS;
        if (next_ms > LVGL_TASK_MAX_MS) next_ms = LVGL_TASK_MAX_MS;
        vTaskDelay(pdMS_TO_TICKS(next_ms));
    }
}

esp_err_t lvgl_port_init(void)
{
    g_lvgl_mutex = xSemaphoreCreateRecursiveMutex();
    if (!g_lvgl_mutex) return ESP_ERR_NO_MEM;

    lv_tick_set_cb(tick_get_cb);

    if (xTaskCreate(lvgl_task, "lvgl", LVGL_TASK_STACK, NULL,
                    LVGL_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create LVGL task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
