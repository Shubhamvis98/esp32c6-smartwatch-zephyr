/* A FreeRTOS subset on Zephyr kernel objects. One tick = 1 ms. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <zephyr/kernel.h>

typedef uint32_t TickType_t;
typedef int      BaseType_t;
typedef unsigned UBaseType_t;

#define pdTRUE   1
#define pdFALSE  0
#define pdPASS   1
#define pdFAIL   0
#define portMAX_DELAY        UINT32_MAX
#define configTICK_RATE_HZ   1000
#define portTICK_PERIOD_MS   1
#define pdMS_TO_TICKS(ms)    ((TickType_t)(ms))
#define pdTICKS_TO_MS(t)     ((uint32_t)(t))
#define configASSERT(x)      do { if (!(x)) { printk("assert %s:%d\n", __FILE__, __LINE__); k_panic(); } } while (0)

static inline k_timeout_t frt_timeout(TickType_t t)
{
    return t == portMAX_DELAY ? K_FOREVER : K_MSEC(t);
}

typedef struct { struct k_spinlock lock; k_spinlock_key_t key; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}
#define portENTER_CRITICAL(m)  do { (m)->key = k_spin_lock(&(m)->lock); } while (0)
#define portEXIT_CRITICAL(m)   k_spin_unlock(&(m)->lock, (m)->key)
#define taskENTER_CRITICAL     portENTER_CRITICAL
#define taskEXIT_CRITICAL      portEXIT_CRITICAL
#define portYIELD_FROM_ISR(x)  do { (void)(x); } while (0)
