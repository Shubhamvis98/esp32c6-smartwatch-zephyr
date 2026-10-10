#pragma once
#include "FreeRTOS.h"

typedef struct frt_sem {
    bool is_mutex;
    union { struct k_mutex mutex; struct k_sem sem; };
} StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;

SemaphoreHandle_t frt_sem_init(StaticSemaphore_t *s, bool mutex, unsigned max, unsigned initial);
#define xSemaphoreCreateMutex()            frt_sem_init(NULL, true, 1, 1)
#define xSemaphoreCreateRecursiveMutex()   frt_sem_init(NULL, true, 1, 1)
#define xSemaphoreCreateMutexStatic(buf)   frt_sem_init(buf, true, 1, 1)
#define xSemaphoreCreateBinary()           frt_sem_init(NULL, false, 1, 0)
#define xSemaphoreCreateBinaryStatic(buf)  frt_sem_init(buf, false, 1, 0)
#define xSemaphoreCreateCounting(m, i)     frt_sem_init(NULL, false, m, i)
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t s);
void vSemaphoreDelete(SemaphoreHandle_t s);
#define xSemaphoreTakeRecursive  xSemaphoreTake
#define xSemaphoreGiveRecursive  xSemaphoreGive
#define xSemaphoreGiveFromISR(s, w)  xSemaphoreGive(s)
