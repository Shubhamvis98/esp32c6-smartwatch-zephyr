#pragma once
#define xTaskGetCurrentTaskHandle watch_xTaskGetCurrentTaskHandle
#include "FreeRTOS.h"

typedef struct frt_task *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
typedef enum { eNoAction, eSetBits, eIncrement, eSetValueWithOverwrite, eSetValueWithoutOverwrite } eNotifyAction;

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack_bytes,
                       void *arg, UBaseType_t prio, TaskHandle_t *out);
#define xTaskCreatePinnedToCore(fn, n, s, a, p, h, core) xTaskCreate(fn, n, s, a, p, h)
void vTaskDelete(TaskHandle_t t);
TaskHandle_t xTaskGetCurrentTaskHandle(void);

static inline void vTaskDelay(TickType_t t) { k_msleep(t == portMAX_DELAY ? INT32_MAX : (int32_t)t); }
static inline TickType_t xTaskGetTickCount(void) { return k_uptime_get_32(); }
static inline void vTaskDelayUntil(TickType_t *prev, TickType_t inc)
{
    TickType_t next = *prev + inc;
    int32_t wait = (int32_t)(next - xTaskGetTickCount());
    if (wait > 0) k_msleep(wait);
    *prev = next;
}
#define xTaskDelayUntil(p, i) (vTaskDelayUntil(p, i), pdTRUE)

BaseType_t xTaskNotify(TaskHandle_t t, uint32_t value, eNotifyAction action);
#define xTaskNotifyGive(t)  xTaskNotify(t, 0, eIncrement)
#define xTaskNotifyFromISR(t, v, a, w) xTaskNotify(t, v, a)
BaseType_t xTaskNotifyWait(uint32_t clear_entry, uint32_t clear_exit, uint32_t *value, TickType_t wait);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait);
