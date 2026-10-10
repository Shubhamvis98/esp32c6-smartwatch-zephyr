#pragma once
#include "FreeRTOS.h"

typedef struct frt_stream *StreamBufferHandle_t;
StreamBufferHandle_t xStreamBufferCreate(size_t size, size_t trigger);
void   vStreamBufferDelete(StreamBufferHandle_t sb);
size_t xStreamBufferSend(StreamBufferHandle_t sb, const void *data, size_t len, TickType_t wait);
size_t xStreamBufferReceive(StreamBufferHandle_t sb, void *buf, size_t len, TickType_t wait);
BaseType_t xStreamBufferIsEmpty(StreamBufferHandle_t sb);
BaseType_t xStreamBufferReset(StreamBufferHandle_t sb);
#define xStreamBufferSendFromISR(sb, d, l, w) xStreamBufferSend(sb, d, l, 0)
