#pragma once
#include <stdint.h>
typedef enum { ESP_PARTITION_TYPE_APP = 0, ESP_PARTITION_TYPE_DATA = 1 } esp_partition_type_t;
#define ESP_PARTITION_SUBTYPE_ANY 0xff
typedef struct {
    esp_partition_type_t type;
    uint8_t  id;            /* flash map partition id */
    uint32_t address;
    uint32_t size;
    char     label[17];
} esp_partition_t;
