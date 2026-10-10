/* Only the power-save calls; they go straight to the Wi-Fi blob that
 * Zephyr's esp32 Wi-Fi driver runs on. */
#pragma once
#include "esp_err.h"
typedef enum { WIFI_PS_NONE, WIFI_PS_MIN_MODEM, WIFI_PS_MAX_MODEM } wifi_ps_type_t;
esp_err_t esp_wifi_set_ps(wifi_ps_type_t type);
esp_err_t esp_wifi_get_ps(wifi_ps_type_t *type);
