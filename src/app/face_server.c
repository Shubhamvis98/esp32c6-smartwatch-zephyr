/**
 * @file app/face_server.c
 * @brief Web server for managing custom watchfaces (see face_server.h).
 *
 * Handlers run on the httpd task; anything touching the face registry or
 * LVGL takes the LVGL lock.  Uploads are written to "<file>.tmp" and renamed,
 * so an interrupted upload never leaves a half-written file in use.
 */

#include "app/face_server.h"
#include "app/settings.h"
#include "app/ota_update.h"
#include "hal/storage.h"
#include "ui/ui_internal.h"
#include "ui/faces/watchface.h"
#include "ui/faces/face_custom.h"
#include "lvgl_port.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "face_server";

#define API_PREFIX        "/api/faces/"
#define FILE_NAME_MAX     32
#define FILE_SIZE_MAX     (400 * 1024)
#define FREE_SPACE_KEEP   (256 * 1024)       // leave room for voice recordings
#define CHUNK             2048

extern const char faces_html_start[] asm("_binary_faces_html_start");
extern const char faces_html_end[]   asm("_binary_faces_html_end");

static httpd_handle_t s_server;

/* ── Helpers ─────────────────────────────────────────────────────────────── */
static bool valid_file_name(const char *name)
{
    size_t n = strlen(name);
    if (n == 0 || n >= FILE_NAME_MAX || name[0] == '.') return false;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-' || c == '.';
        if (!ok) return false;
    }
    return !strstr(name, "..") && !strstr(name, ".tmp");
}

/* Split "/api/faces/<id>[/<file>][?query]" into id and file ("" if absent) */
static bool parse_uri(const char *uri, char *id, size_t id_len, char *file, size_t file_len)
{
    const char *p = uri + strlen(API_PREFIX);
    size_t n = strcspn(p, "/?");
    if (n == 0 || n >= id_len) return false;
    memcpy(id, p, n);
    id[n] = 0;
    file[0] = 0;
    if (p[n] == '/') {
        p += n + 1;
        n = strcspn(p, "?");
        if (n >= file_len) return false;
        memcpy(file, p, n);
        file[n] = 0;
    }
    return face_custom_valid_id(id) && (!file[0] || valid_file_name(file));
}

static esp_err_t send_json(httpd_req_t *req, cJSON *json)
{
    char *s = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!s) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, s);
    free(s);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, httpd_err_code_t code, const char *msg)
{
    ESP_LOGW(TAG, "%s %s: %s", req->method == HTTP_PUT ? "PUT" : req->method == HTTP_DELETE ? "DELETE" :
             req->method == HTTP_POST ? "POST" : "GET", req->uri, msg);
    return httpd_resp_send_err(req, code, msg);
}

static void face_dir(char *out, size_t len, const char *id)
{
    snprintf(out, len, FACES_DIR "/%s", id);
}

/* ── Handlers ────────────────────────────────────────────────────────────── */
static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, faces_html_start, faces_html_end - faces_html_start);
}

static esp_err_t list_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "faces");

    lvgl_lock();
    const watchface_t *active = screen_watchface_active();
    for (int i = 0; i < watchface_count(); i++) {
        const watchface_t *f = watchface_get(i);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", f->id);
        cJSON_AddStringToObject(o, "name", f->name);
        cJSON_AddStringToObject(o, "desc", f->desc);
        cJSON_AddBoolToObject(o, "custom", !watchface_is_builtin(f));
        cJSON_AddBoolToObject(o, "active", f == active);
        cJSON_AddItemToArray(arr, o);
    }
    lvgl_unlock();

    uint64_t free_b = 0, total_b = 0;
    storage_get_space(&free_b, &total_b);
    cJSON_AddNumberToObject(root, "free_kb", (double)(free_b / 1024));
    return send_json(req, root);
}

static esp_err_t file_get(httpd_req_t *req)
{
    char id[FACE_ID_MAX], file[FILE_NAME_MAX], path[96];
    if (!parse_uri(req->uri, id, sizeof(id), file, sizeof(file)) || !file[0]) {
        return send_error(req, HTTPD_400_BAD_REQUEST, "Bad face id or file name");
    }
    snprintf(path, sizeof(path), FACES_DIR "/%s/%s", id, file);
    FILE *f = fopen(path, "rb");
    if (!f) return send_error(req, HTTPD_404_NOT_FOUND, "No such file");

    httpd_resp_set_type(req, strstr(file, ".json") ? "application/json" : "application/octet-stream");
    char *buf = malloc(CHUNK);
    if (!buf) {
        fclose(f);
        return httpd_resp_send_500(req);
    }
    size_t n;
    esp_err_t err = ESP_OK;
    while (err == ESP_OK && (n = fread(buf, 1, CHUNK, f)) > 0) {
        err = httpd_resp_send_chunk(req, buf, n);
    }
    free(buf);
    fclose(f);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    return err;
}

static esp_err_t file_put(httpd_req_t *req)
{
    char id[FACE_ID_MAX], file[FILE_NAME_MAX], dir[48], path[96], tmp[100];
    if (!parse_uri(req->uri, id, sizeof(id), file, sizeof(file)) || !file[0]) {
        return send_error(req, HTTPD_400_BAD_REQUEST,
                          "Face id must be 1-23 of a-z 0-9 _ -, file names letters, digits . _ -");
    }
    if (req->content_len > FILE_SIZE_MAX) {
        return send_error(req, HTTPD_400_BAD_REQUEST, "File too big (400 KB max)");
    }
    uint64_t free_b = 0, total_b = 0;
    if (storage_get_space(&free_b, &total_b) != ESP_OK ||
        free_b < req->content_len + FREE_SPACE_KEEP) {
        return send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Not enough storage space");
    }

    mkdir(FACES_DIR, 0755);
    face_dir(dir, sizeof(dir), id);
    mkdir(dir, 0755);
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *f = fopen(tmp, "wb");
    if (!f) return send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Can't create file");

    char *buf = malloc(CHUNK);
    size_t left = req->content_len;
    bool ok = buf != NULL;
    while (ok && left) {
        int n = httpd_req_recv(req, buf, left < CHUNK ? left : CHUNK);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        ok = n > 0 && fwrite(buf, 1, n, f) == (size_t)n;
        if (ok) left -= n;
    }
    free(buf);
    ok = fclose(f) == 0 && ok;

    if (!ok) {
        unlink(tmp);
        return send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload failed");
    }
    unlink(path);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Can't save file");
    }
    ESP_LOGI(TAG, "Saved %s (%u bytes)", path, (unsigned)req->content_len);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t face_delete(httpd_req_t *req)
{
    char id[FACE_ID_MAX], file[FILE_NAME_MAX];
    if (!parse_uri(req->uri, id, sizeof(id), file, sizeof(file)) || file[0]) {
        return send_error(req, HTTPD_400_BAD_REQUEST, "Bad face id");
    }
    lvgl_lock();
    bool ok = screen_watchface_delete(id);
    lvgl_unlock();
    if (!ok) return send_error(req, HTTPD_404_NOT_FOUND, "No such face");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t reload_post(httpd_req_t *req)
{
    char select[FACE_ID_MAX] = "";
    char query[48];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "select", select, sizeof(select));
    }

    lvgl_lock();
    if (select[0]) {
        strlcpy(settings_get()->face_id, select, sizeof(settings_get()->face_id));
        watch_settings_save();
    }
    screen_watchface_reload_faces();
    const watchface_t *active = screen_watchface_active();
    bool ok = !select[0] || (active && strcmp(active->id, select) == 0);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", ok && !face_custom_last_error()[0]);
    cJSON_AddStringToObject(root, "active", active ? active->id : "");
    cJSON_AddStringToObject(root, "error", face_custom_last_error());
    lvgl_unlock();

    ui_screen_wake();                   // show the result on the watch
    return send_json(req, root);
}

/* ── Start / stop ────────────────────────────────────────────────────────── */
esp_err_t face_server_start(void)
{
    if (s_server) return ESP_OK;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 4096;
    cfg.max_open_sockets = 3;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Start failed: %s", esp_err_to_name(err));
        s_server = NULL;
        return err;
    }

    static const httpd_uri_t uris[] = {
        { .uri = "/",              .method = HTTP_GET,    .handler = page_get },
        { .uri = "/api/faces",     .method = HTTP_GET,    .handler = list_get },
        { .uri = API_PREFIX "*",   .method = HTTP_GET,    .handler = file_get },
        { .uri = API_PREFIX "*",   .method = HTTP_PUT,    .handler = file_put },
        { .uri = API_PREFIX "*",   .method = HTTP_DELETE, .handler = face_delete },
        { .uri = "/api/reload",    .method = HTTP_POST,   .handler = reload_post },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }
    ota_register_handlers(s_server);           // /update, /api/ota
    ESP_LOGI(TAG, "Started");
    return ESP_OK;
}

static void stop_task(void *arg)
{
    httpd_stop((httpd_handle_t)arg);
    ESP_LOGI(TAG, "Stopped");
    vTaskDelete(NULL);
}

/* Called from the LVGL task (page closed).  httpd_stop() waits for running
 * handlers, which may be waiting for the LVGL lock, so stop from a task. */
void face_server_stop(void)
{
    if (!s_server) return;
    httpd_handle_t h = s_server;
    s_server = NULL;
    if (xTaskCreate(stop_task, "face_srv_stop", 3072, h, 5, NULL) != pdPASS) httpd_stop(h);
}

bool face_server_is_running(void)
{
    return s_server != NULL;
}
