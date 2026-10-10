/* ESP-IDF NVS on Zephyr settings: key "nvs/<namespace>/<key>". */
#pragma once

/* The Wi-Fi blob adapter has its own nvs_* symbols: keep ours apart */
#define nvs_close watch_nvs_close
#define nvs_commit watch_nvs_commit
#define nvs_erase_all watch_nvs_erase_all
#define nvs_erase_key watch_nvs_erase_key
#define nvs_get_blob watch_nvs_get_blob
#define nvs_get_str watch_nvs_get_str
#define nvs_open watch_nvs_open
#define nvs_set_blob watch_nvs_set_blob
#define nvs_set_str watch_nvs_set_str
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

typedef int nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out);
void      nvs_close(nvs_handle_t h);
esp_err_t nvs_commit(nvs_handle_t h);
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key);
esp_err_t nvs_erase_all(nvs_handle_t h);

esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *val, size_t len);
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len);
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *val);
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len);

#define NVS_INT_(sfx, type)                                                         \
    static inline esp_err_t nvs_set_##sfx(nvs_handle_t h, const char *k, type v)    \
    { return nvs_set_blob(h, k, &v, sizeof(v)); }                                   \
    static inline esp_err_t nvs_get_##sfx(nvs_handle_t h, const char *k, type *v)   \
    { size_t n = sizeof(*v); return nvs_get_blob(h, k, v, &n); }
NVS_INT_(u8, uint8_t)
NVS_INT_(i8, int8_t)
NVS_INT_(u16, uint16_t)
NVS_INT_(i16, int16_t)
NVS_INT_(u32, uint32_t)
NVS_INT_(i32, int32_t)
NVS_INT_(u64, uint64_t)
NVS_INT_(i64, int64_t)
