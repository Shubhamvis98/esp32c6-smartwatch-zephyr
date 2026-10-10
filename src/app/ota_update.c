/**
 * @file app/ota_update.c
 * @brief Wireless firmware updates over the Wi-Fi web server (see ota_update.h).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app/ota_update.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui/ui_manager.h"

static const char *TAG = "ota";

#define CHUNK  4096

static ota_status_t  s_status;
static portMUX_TYPE  s_lock = portMUX_INITIALIZER_UNLOCKED;

static void set_status(ota_state_t state, uint32_t received, uint32_t total, const char *msg)
{
    taskENTER_CRITICAL(&s_lock);
    s_status.state = state;
    s_status.received = received;
    s_status.total = total;
    snprintf(s_status.message, sizeof(s_status.message), "%s", msg ? msg : "");
    taskEXIT_CRITICAL(&s_lock);
}

void ota_get_status(ota_status_t *st)
{
    taskENTER_CRITICAL(&s_lock);
    *st = s_status;
    taskEXIT_CRITICAL(&s_lock);
}

const char *ota_version(void)
{
    return esp_app_get_description()->version;
}

void ota_confirm_running(void)
{
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "New firmware is running, keeping it");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));            // let the reply reach the browser
    ui_request_restart();
    vTaskDelete(NULL);
}

static esp_err_t fail(httpd_req_t *req, esp_ota_handle_t h, uint32_t got, const char *msg)
{
    if (h) esp_ota_abort(h);
    ESP_LOGE(TAG, "Update failed: %s", msg);
    set_status(OTA_FAILED, got, req->content_len, msg);
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, msg);
    return ESP_OK;
}

/* POST /api/ota, body = the firmware .bin */
static esp_err_t ota_post(httpd_req_t *req)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) return fail(req, 0, 0, "No update slot (needs the OTA partition table)");
    if (s_status.state == OTA_RECEIVING) return fail(req, 0, 0, "An update is already running");
    if (req->content_len == 0 || req->content_len > part->size)
        return fail(req, 0, 0, "Wrong size for a firmware file");

    char *buf = malloc(CHUNK);
    if (!buf) return fail(req, 0, 0, "Out of memory");

    esp_ota_handle_t h = 0;
    /* Erase as the data arrives: erasing the whole slot up front stalls the
     * web server long enough to trip the task watchdog */
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (err != ESP_OK) {
        free(buf);
        return fail(req, 0, 0, esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "Receiving %u bytes into %s", (unsigned)req->content_len, part->label);
    set_status(OTA_RECEIVING, 0, req->content_len, NULL);
    ui_screen_wake();
    /* Wi-Fi power saving makes the upload several times slower */
    wifi_ps_type_t ps = WIFI_PS_MIN_MODEM;
    esp_wifi_get_ps(&ps);
    esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "Power saving off for the upload: %s", esp_err_to_name(ps_err));
    int64_t t0 = esp_timer_get_time();

    uint32_t got = 0;
    int timeouts = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, buf, CHUNK);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) continue;
        if (n <= 0) {
            free(buf);
            if (ps_err == ESP_OK) esp_wifi_set_ps(ps);
            return fail(req, h, got, "Upload interrupted");
        }
        timeouts = 0;
        err = esp_ota_write(h, buf, n);
        if (err != ESP_OK) {
            free(buf);
            if (ps_err == ESP_OK) esp_wifi_set_ps(ps);
            return fail(req, h, got, err == ESP_ERR_OTA_VALIDATE_FAILED ? "Not a firmware file"
                                                                       : esp_err_to_name(err));
        }
        got += n;
        set_status(OTA_RECEIVING, got, req->content_len, NULL);
    }
    free(buf);
    if (ps_err == ESP_OK) esp_wifi_set_ps(ps);
    ESP_LOGI(TAG, "Received in %lld s", (esp_timer_get_time() - t0) / 1000000);

    err = esp_ota_end(h);                       // checks the image
    if (err != ESP_OK) {
        return fail(req, 0, got, err == ESP_ERR_OTA_VALIDATE_FAILED ? "Firmware check failed"
                                                                    : esp_err_to_name(err));
    }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) return fail(req, 0, got, esp_err_to_name(err));

    ESP_LOGI(TAG, "Update written, restarting");
    set_status(OTA_DONE, got, req->content_len, NULL);
    httpd_resp_sendstr(req, "OK - the watch is restarting into the new firmware");
    xTaskCreate(restart_task, "ota_restart", 3072, NULL, 5, NULL);
    return ESP_OK;
}

static const char UPDATE_PAGE[] =
    "<!doctype html><meta name=viewport content='width=device-width'>"
    "<title>Watch update</title>"
    "<style>body{font-family:sans-serif;max-width:28em;margin:2em auto;padding:0 1em}"
    "progress{width:100%%}button{font-size:1em;padding:.5em 1em}</style>"
    "<h2>Firmware update</h2><p>Running: <b>%s</b></p>"
    "<p>Choose <code>build/smartwatch.bin</code>:</p>"
    "<input type=file id=f accept=.bin> <button onclick=go()>Update</button>"
    "<p><progress id=p value=0 max=1></progress></p><p id=s></p>"
    "<script>function go(){var f=document.getElementById('f').files[0],s=document.getElementById('s');"
    "if(!f){s.textContent='Choose a file first';return}"
    "var x=new XMLHttpRequest();x.open('POST','/api/ota');"
    "x.upload.onprogress=function(e){document.getElementById('p').value=e.loaded/e.total};"
    "x.onload=function(){s.textContent=x.responseText};"
    "x.onerror=function(){s.textContent='Upload failed'};"
    "s.textContent='Uploading...';x.send(f)}</script>";

static esp_err_t update_page_get(httpd_req_t *req)
{
    char *page = malloc(sizeof(UPDATE_PAGE) + 40);
    if (!page) return httpd_resp_send_500(req);
    snprintf(page, sizeof(UPDATE_PAGE) + 40, UPDATE_PAGE, ota_version());
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, page);
    free(page);
    return ESP_OK;
}

void ota_register_handlers(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {
        { .uri = "/update",  .method = HTTP_GET,  .handler = update_page_get },
        { .uri = "/api/ota", .method = HTTP_POST, .handler = ota_post },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
}
