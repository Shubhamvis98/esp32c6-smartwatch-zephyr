#pragma once
#include "FreeRTOS.h"

typedef struct k_msgq *QueueHandle_t;
QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item_size);
void vQueueDelete(QueueHandle_t q);
static inline BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait)
{
    return k_msgq_put(q, item, frt_timeout(wait)) == 0 ? pdPASS : pdFAIL;
}
#define xQueueSendToBack xQueueSend
#define xQueueSendFromISR(q, i, w) xQueueSend(q, i, 0)
static inline BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait)
{
    return k_msgq_get(q, item, frt_timeout(wait)) == 0 ? pdTRUE : pdFALSE;
}
static inline UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q) { return k_msgq_num_used_get(q); }
static inline BaseType_t xQueueReset(QueueHandle_t q) { k_msgq_purge(q); return pdPASS; }
