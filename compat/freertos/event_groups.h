#pragma once
#define xEventGroupCreate watch_xEventGroupCreate
#include "FreeRTOS.h"

typedef uint32_t EventBits_t;
typedef struct k_event *EventGroupHandle_t;
EventGroupHandle_t xEventGroupCreate(void);
static inline EventBits_t xEventGroupSetBits(EventGroupHandle_t e, EventBits_t b) { k_event_post(e, b); return k_event_test(e, ~0u); }
static inline EventBits_t xEventGroupClearBits(EventGroupHandle_t e, EventBits_t b) { EventBits_t o = k_event_test(e, ~0u); k_event_clear(e, b); return o; }
static inline EventBits_t xEventGroupGetBits(EventGroupHandle_t e) { return k_event_test(e, ~0u); }
EventBits_t xEventGroupWaitBits(EventGroupHandle_t e, EventBits_t bits, BaseType_t clear,
                                BaseType_t all, TickType_t wait);
