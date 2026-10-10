/* Minimal blocking HTTP/1.0 GET client with the ESP-IDF call names (plain http only). */
#pragma once
#include "esp_err.h"

typedef struct {
    const char *url;
    int         timeout_ms;
} esp_http_client_config_t;

typedef struct esp_http_client *esp_http_client_handle_t;

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg);
esp_err_t esp_http_client_open(esp_http_client_handle_t h, int write_len);
int       esp_http_client_fetch_headers(esp_http_client_handle_t h);
int       esp_http_client_get_status_code(esp_http_client_handle_t h);
int       esp_http_client_read_response(esp_http_client_handle_t h, char *buf, int len);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t h);
