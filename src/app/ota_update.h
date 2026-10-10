#pragma once
/**
 * @file app/ota_update.h
 * @brief Wireless firmware updates: upload a new firmware over Wi-Fi.
 *
 * The web server (app/face_server.c) serves an upload page at /update and
 * takes the firmware as POST /api/ota.  It is written to the spare app slot;
 * once it checks out the watch restarts into it.  If the new firmware doesn't
 * reach "running" (crash, hang), the bootloader goes back to the old one.
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_http_server.h"

typedef enum {
    OTA_IDLE,
    OTA_RECEIVING,
    OTA_DONE,            // restarting into the new firmware
    OTA_FAILED,
} ota_state_t;

typedef struct {
    ota_state_t state;
    uint32_t    received, total;   // bytes
    char        message[64];       // why it failed
} ota_status_t;

/** Add /update and /api/ota to the web server. */
void ota_register_handlers(httpd_handle_t server);
void ota_get_status(ota_status_t *st);
/** Call once the firmware is up: confirms an update so it isn't rolled back. */
void ota_confirm_running(void);
/** Running firmware version, e.g. "fe25faf". */
const char *ota_version(void);
