#pragma once
/**
 * @file app/face_server.h
 * @brief Web server for managing custom watchfaces over Wi-Fi.
 *
 * Runs only while Settings → Watchface → Upload faces is open.  Serves a
 * page (main/web/faces.html) and a small API:
 *
 *   GET    /api/faces                 list faces (built-in and custom)
 *   GET    /api/faces/<id>/<file>     download a custom face file
 *   PUT    /api/faces/<id>/<file>     upload a file (body = file contents)
 *   DELETE /api/faces/<id>            delete a custom face
 *   POST   /api/reload[?select=<id>]  re-read faces, optionally switch to one;
 *                                     replies {"ok", "active", "error"}
 */

#include <stdbool.h>
#include "esp_err.h"

esp_err_t face_server_start(void);
void      face_server_stop(void);
bool      face_server_is_running(void);
