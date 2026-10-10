/**
 * @file app/wifi_mgr.c
 * @brief Wi-Fi worker task: scan, connect, auto-reconnect and SNTP sync.
 *
 * Only the worker task issues Wi-Fi net_mgmt requests, so operations
 * never overlap (e.g. a scan can't start while a connect is in progress).
 * The event handler only records state and wakes the worker.
 */

#include "app/wifi_mgr.h"
#include "app/settings.h"
#include "app/timezones.h"
#include "hal/rtc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/sntp.h>
#include <sys/time.h>
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "wifi_mgr";

#define CONNECT_TIMEOUT_MS      15000
#define SNTP_TIMEOUT_MS         10000
#define CONNECT_RETRIES         2
#define PASSWORD_RETRY_DELAY_MS 1000
#define RECONNECT_MAX_MS        (60 * 1000)
#define SYNC_RETRY_MS           (30 * 60 * 1000)
#define SYNC_INTERVAL_MS        ((int64_t)CONFIG_WATCH_TIME_SYNC_INTERVAL_H * 3600 * 1000)
#define SCAN_RAW_MAX            24

#define NVS_NS                  "wifi"

#define BIT_GOT_IP              (1u << 0)
#define BIT_FAILED              (1u << 1)

typedef enum {
    CMD_SCAN,
    CMD_CONNECT,
    CMD_CONNECT_SAVED,
    CMD_DISCONNECT,
    CMD_FORGET,
    CMD_SYNC,
    CMD_RECONNECT,
    CMD_ENABLE,
    CMD_DISABLE,
} cmd_type_t;

typedef struct {
    cmd_type_t type;
    char ssid[33];
    char pass[65];
} cmd_t;

static QueueHandle_t      s_queue;
static EventGroupHandle_t s_events;
static SemaphoreHandle_t  s_lock;           // guards the fields below shared with the UI

/* Shared with the UI (read under s_lock or as single words) */
static wifi_ap_info_t     s_aps[WIFI_MGR_MAX_APS];
static int                s_ap_count;
static volatile uint32_t  s_scan_gen;
static volatile bool      s_scanning;
static volatile wifi_conn_state_t  s_conn = WIFI_CONN_DISCONNECTED;
static volatile time_sync_status_t s_sync = TIME_SYNC_DISABLED;
static volatile time_t    s_last_sync;
static const char        *s_fail_reason = "";
static char               s_cur_ssid[33];   // network being used / connected

static volatile bool      s_enabled = true;

/* Worker-only state */
static char    s_saved_ssid[33];
static char    s_saved_pass[65];
static bool    s_keep;                      // stay connected to the saved network
static bool    s_radio_on;
static int64_t s_next_sync_ms;
static int64_t s_reconnect_at_ms;           // 0 = none pending
static uint8_t s_reconnect_tries;           // failed attempts since last connected

/* Event-handler state */
static volatile bool s_connecting;
static volatile bool s_stopping;
static volatile int  s_retries;
static volatile int  s_last_reason;
static struct net_if *s_iface;
static struct net_mgmt_event_callback s_wifi_cb, s_ip_cb;

/* Scan results arrive one event at a time; collected into s_raw */
#define BIT_SCAN_DONE           (1u << 2)
#define BIT_CONNECTED           (1u << 3)
#define BIT_RETRY               (1u << 4)
static wifi_ap_info_t s_raw[WIFI_MGR_MAX_APS];
static int            s_raw_count;


static void sta_disconnect(void)
{
    net_mgmt(NET_REQUEST_WIFI_DISCONNECT, s_iface, NULL, 0);
}

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void set_cur_ssid(const char *ssid)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_cur_ssid, ssid, sizeof(s_cur_ssid));
    xSemaphoreGive(s_lock);
}

/* ── Persistence ─────────────────────────────────────────────────────────── */
static void creds_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_saved_ssid);
        if (nvs_get_str(h, "ssid", s_saved_ssid, &len) != ESP_OK) s_saved_ssid[0] = 0;
        len = sizeof(s_saved_pass);
        if (nvs_get_str(h, "pass", s_saved_pass, &len) != ESP_OK) s_saved_pass[0] = 0;
        uint8_t keep = 0;
        nvs_get_u8(h, "keep", &keep);
        s_keep = keep && s_saved_ssid[0];
        nvs_close(h);
    }

    if (!s_saved_ssid[0] && strlen(CONFIG_WATCH_WIFI_SSID) > 0) {
        strlcpy(s_saved_ssid, CONFIG_WATCH_WIFI_SSID, sizeof(s_saved_ssid));
        strlcpy(s_saved_pass, CONFIG_WATCH_WIFI_PASSWORD, sizeof(s_saved_pass));
        s_keep = false;
    }
    set_cur_ssid(s_saved_ssid);
}

static void creds_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (s_saved_ssid[0]) {
        nvs_set_str(h, "ssid", s_saved_ssid);
        nvs_set_str(h, "pass", s_saved_pass);
        nvs_set_u8(h, "keep", s_keep);
    } else {
        nvs_erase_all(h);
    }
    nvs_commit(h);
    nvs_close(h);
}

/* ── Event handler ───────────────────────────────────────────────────────── */
static const char *reason_text(int reason)
{
    switch (reason) {
    case WIFI_STATUS_CONN_WRONG_PASSWORD:
        return "Wrong password";
    case WIFI_STATUS_CONN_AP_NOT_FOUND:
        return "Network not found";
    case WIFI_STATUS_CONN_TIMEOUT:
        return "Timed out";
    default:
        return "Connection failed";
    }
}

static bool reason_is_final(int reason)
{
    /* Retrying won't fix a wrong password */
    return reason == WIFI_STATUS_CONN_WRONG_PASSWORD;
}

static bool start_connect(const char *ssid, const char *pass);


static void add_scan_result(const struct wifi_scan_result *r)
{
    if (!r->ssid_length || !r->ssid[0]) return;
    char ssid[33];
    size_t n = r->ssid_length < 32 ? r->ssid_length : 32;
    memcpy(ssid, r->ssid, n);
    ssid[n] = 0;

    /* Keep only the strongest entry per SSID (mesh / multiple APs) */
    for (int j = 0; j < s_raw_count; j++) {
        if (strcmp(s_raw[j].ssid, ssid) == 0) {
            if (r->rssi > s_raw[j].rssi) s_raw[j].rssi = r->rssi;
            return;
        }
    }
    if (s_raw_count == WIFI_MGR_MAX_APS) return;
    strlcpy(s_raw[s_raw_count].ssid, ssid, sizeof(s_raw[0].ssid));
    s_raw[s_raw_count].rssi   = r->rssi;
    s_raw[s_raw_count].secure = r->security != WIFI_SECURITY_TYPE_NONE;
    s_raw_count++;
}

static void wifi_event_handler(struct net_mgmt_event_callback *cb, uint64_t event,
                               struct net_if *iface)
{
    const struct wifi_status *st = (const struct wifi_status *)cb->info;

    switch (event) {
    case NET_EVENT_WIFI_SCAN_RESULT:
        add_scan_result((const struct wifi_scan_result *)cb->info);
        break;
    case NET_EVENT_WIFI_SCAN_DONE:
        xEventGroupSetBits(s_events, BIT_SCAN_DONE);
        break;
    case NET_EVENT_WIFI_CONNECT_RESULT:
        if (st && st->status == 0) {
            xEventGroupSetBits(s_events, BIT_CONNECTED);
            break;
        }
        s_last_reason = st ? st->conn_status : WIFI_STATUS_CONN_FAIL;
        if (s_connecting) {
            if (!reason_is_final(s_last_reason) && ++s_retries <= CONNECT_RETRIES) {
                xEventGroupSetBits(s_events, BIT_RETRY);  /* worker retries */
            } else {
                xEventGroupSetBits(s_events, BIT_FAILED);
            }
        }
        break;
    case NET_EVENT_WIFI_DISCONNECT_RESULT:
        if (s_conn == WIFI_CONN_CONNECTED && !s_stopping && !s_connecting) {
            ESP_LOGW(TAG, "Connection lost");
            s_conn = WIFI_CONN_DISCONNECTED;
            cmd_t cmd = { .type = CMD_RECONNECT };
            xQueueSend(s_queue, &cmd, 0);
        }
        break;
    default:
        break;
    }
}

static void ip_event_handler(struct net_mgmt_event_callback *cb, uint64_t event,
                             struct net_if *iface)
{
    if (event == NET_EVENT_IPV4_ADDR_ADD) xEventGroupSetBits(s_events, BIT_GOT_IP);
}

/* ── Radio helpers (worker task only) ────────────────────────────────────── */
static void radio_on(void)
{
    if (s_radio_on) return;
    if (!net_if_is_admin_up(s_iface)) net_if_up(s_iface);
    s_radio_on = true;
}

static void radio_off(void)
{
    if (!s_radio_on) return;
    s_stopping = true;
    sta_disconnect();
    k_msleep(100);
    net_if_down(s_iface);      /* the driver stops the radio */
    s_stopping = false;
    s_radio_on = false;
    if (s_conn == WIFI_CONN_CONNECTED || s_conn == WIFI_CONN_CONNECTING) {
        s_conn = WIFI_CONN_DISCONNECTED;
    }
}

/* While the kept network is out of reach, try again soon at first, then
 * every minute: each attempt keeps the radio on for up to ~15 s. */
static void schedule_reconnect(void)
{
    static const uint16_t delays_s[] = { 3, 5, 10, 15, 20, 30, 30, 30, 30, 30, 30 };
    if (!s_enabled || !s_keep || !s_saved_ssid[0] || s_conn == WIFI_CONN_CONNECTED) return;

    int64_t delay = RECONNECT_MAX_MS;
    if (s_reconnect_tries < sizeof(delays_s) / sizeof(delays_s[0])) {
        delay = delays_s[s_reconnect_tries] * 1000;
        s_reconnect_tries++;
    }
    s_reconnect_at_ms = now_ms() + delay;
    ESP_LOGI(TAG, "Reconnecting in %lld s", delay / 1000);
}

/* Turn the radio off unless something still needs it */
static void radio_idle(void)
{
    if (!(s_keep && s_conn == WIFI_CONN_CONNECTED)) radio_off();
}

static bool sta_connect_once(const char *ssid, const char *pass)
{
    radio_on();

    if (s_conn == WIFI_CONN_CONNECTED) {
        s_stopping = true;
        sta_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
        s_stopping = false;
    }

    set_cur_ssid(ssid);
    s_conn = WIFI_CONN_CONNECTING;
    s_retries = 0;
    s_last_reason = WIFI_STATUS_CONN_FAIL;
    xEventGroupClearBits(s_events, BIT_GOT_IP | BIT_FAILED | BIT_CONNECTED | BIT_RETRY);
    s_connecting = true;

    ESP_LOGI(TAG, "Connecting to \"%s\"", ssid);
    EventBits_t bits = 0;
    int64_t deadline = now_ms() + CONNECT_TIMEOUT_MS;
    if (!start_connect(ssid, pass)) {
        bits = BIT_FAILED;
    }
    while (!bits) {
        int64_t left = deadline - now_ms();
        if (left <= 0) break;
        bits = xEventGroupWaitBits(s_events, BIT_GOT_IP | BIT_FAILED | BIT_RETRY, pdFALSE,
                                   pdFALSE, pdMS_TO_TICKS(left));
        if (bits & BIT_RETRY) {                 /* transient failure: try again */
            xEventGroupClearBits(s_events, BIT_RETRY);
            bits = 0;
            vTaskDelay(pdMS_TO_TICKS(500));
            if (!start_connect(ssid, pass)) bits = BIT_FAILED;
        }
    }
    s_connecting = false;

    if (bits & BIT_GOT_IP) {
        s_conn = WIFI_CONN_CONNECTED;
        s_reconnect_tries = 0;
        ESP_LOGI(TAG, "Connected to \"%s\"", ssid);
        return true;
    }

    s_fail_reason = (bits & BIT_FAILED) ? reason_text(s_last_reason) : "Timed out";
    ESP_LOGW(TAG, "Connect to \"%s\" failed: %s (reason %d)", ssid, s_fail_reason, s_last_reason);
    s_stopping = true;
    sta_disconnect();
    s_stopping = false;
    s_conn = WIFI_CONN_FAILED;
    return false;
}

static bool start_connect(const char *ssid, const char *pass)
{
    struct wifi_connect_req_params p = { 0 };
    p.ssid        = (const uint8_t *)ssid;
    p.ssid_length = strlen(ssid);
    p.band        = WIFI_FREQ_BAND_2_4_GHZ;
    p.channel     = WIFI_CHANNEL_ANY;
    p.mfp         = WIFI_MFP_OPTIONAL;
    p.timeout     = CONNECT_TIMEOUT_MS / 1000;
    if (pass && pass[0]) {
        p.psk        = (const uint8_t *)pass;
        p.psk_length = strlen(pass);
        p.security   = WIFI_SECURITY_TYPE_PSK;
    } else {
        p.security   = WIFI_SECURITY_TYPE_NONE;
    }
    int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, s_iface, &p, sizeof(p));
    if (ret) ESP_LOGW(TAG, "connect request failed (%d)", ret);
    return ret == 0;
}

/* The first attempt often fails with "wrong password" (reason 202) even
 * though the password is right; a fresh attempt a moment later works.  So a
 * password failure is only reported once it happens twice.  Not retried
 * instantly: back-to-back handshakes got the watch blocked by the router. */
static bool sta_connect(const char *ssid, const char *pass)
{
    if (sta_connect_once(ssid, pass)) return true;
    if (!reason_is_final(s_last_reason)) return false;
    ESP_LOGI(TAG, "Trying once more");
    s_conn = WIFI_CONN_CONNECTING;
    vTaskDelay(pdMS_TO_TICKS(PASSWORD_RETRY_DELAY_MS));
    return sta_connect_once(ssid, pass);
}

/* ── Operations (worker task only) ───────────────────────────────────────── */
static void do_sync(void)
{
    if (!s_enabled || !s_saved_ssid[0]) {
        s_sync = TIME_SYNC_DISABLED;
        return;
    }

    s_sync = TIME_SYNC_RUNNING;
    if (s_conn != WIFI_CONN_CONNECTED && !sta_connect(s_saved_ssid, s_saved_pass)) {
        s_sync = TIME_SYNC_FAILED;
        s_next_sync_ms = now_ms() + SYNC_RETRY_MS;
        radio_idle();
        return;
    }

    struct sntp_time ts;
    bool ok = false;
    for (int i = 0; i < 2 && !ok; i++) {
        int ret = sntp_simple(CONFIG_WATCH_NTP_SERVER, SNTP_TIMEOUT_MS / 2, &ts);
        if (ret == 0) {
            struct timeval tv = { .tv_sec = (time_t)ts.seconds,
                                  .tv_usec = ((uint64_t)ts.fraction * 1000000) >> 32 };
            settimeofday(&tv, NULL);
            ok = true;
        } else {
            ESP_LOGW(TAG, "SNTP attempt failed (%d)", ret);
        }
    }

    if (ok) {
        time_t now = time(NULL);
        rtc_write(now);
        s_last_sync = now;
        ESP_LOGI(TAG, "Time synced, RTC updated");
        tz_auto_detect();                  // while the connection is up; still "Syncing"
        s_sync = TIME_SYNC_OK;
        s_next_sync_ms = now_ms() + SYNC_INTERVAL_MS;
    } else {
        s_sync = TIME_SYNC_FAILED;
        s_next_sync_ms = now_ms() + SYNC_RETRY_MS;
        ESP_LOGW(TAG, "SNTP timed out");
    }
    radio_idle();
}

static int ap_cmp(const void *a, const void *b)
{
    return ((const wifi_ap_info_t *)b)->rssi - ((const wifi_ap_info_t *)a)->rssi;
}

static void do_scan(void)
{
    s_scanning = true;
    radio_on();

    s_raw_count = 0;
    xEventGroupClearBits(s_events, BIT_SCAN_DONE);
    struct wifi_scan_params params = { 0 };
    int ret = net_mgmt(NET_REQUEST_WIFI_SCAN, s_iface, &params, sizeof(params));
    if (ret == 0) {
        xEventGroupWaitBits(s_events, BIT_SCAN_DONE, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    } else {
        ESP_LOGW(TAG, "Scan failed to start (%d)", ret);
    }

    wifi_ap_info_t found[WIFI_MGR_MAX_APS];
    int count = s_raw_count;
    memcpy(found, s_raw, count * sizeof(found[0]));
    qsort(found, count, sizeof(found[0]), ap_cmp);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_aps, found, count * sizeof(found[0]));
    s_ap_count = count;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "Scan found %d networks", count);
    s_scan_gen++;
    s_scanning = false;
    radio_idle();
}

static void handle(const cmd_t *cmd)
{
    /* While off, only on/off and credential management are accepted */
    if (!s_enabled && cmd->type != CMD_ENABLE && cmd->type != CMD_DISABLE &&
        cmd->type != CMD_FORGET) {
        s_scanning = false;
        return;
    }

    switch (cmd->type) {
    case CMD_ENABLE:
        ESP_LOGI(TAG, "Wi-Fi on");
        s_sync = s_saved_ssid[0] ? TIME_SYNC_IDLE : TIME_SYNC_DISABLED;
        s_reconnect_tries = 0;
        if (s_keep && !sta_connect(s_saved_ssid, s_saved_pass)) schedule_reconnect();
        do_sync();
        break;

    case CMD_DISABLE:
        ESP_LOGI(TAG, "Wi-Fi off");
        s_reconnect_at_ms = 0;
        radio_off();
        s_conn = WIFI_CONN_DISCONNECTED;
        s_sync = TIME_SYNC_DISABLED;
        s_scanning = false;
        break;

    case CMD_SCAN:
        do_scan();
        break;

    case CMD_CONNECT_SAVED:
        /* Stay connected again, retrying until the network is in reach */
        if (!s_saved_ssid[0]) break;
        s_keep = true;
        creds_save();
        s_reconnect_at_ms = 0;
        s_reconnect_tries = 0;
        if (sta_connect(s_saved_ssid, s_saved_pass)) do_sync();
        else                                         schedule_reconnect();
        break;

    case CMD_CONNECT:
        s_reconnect_at_ms = 0;
        if (sta_connect(cmd->ssid, cmd->pass)) {
            strlcpy(s_saved_ssid, cmd->ssid, sizeof(s_saved_ssid));
            strlcpy(s_saved_pass, cmd->pass, sizeof(s_saved_pass));
            s_keep = true;
            creds_save();
            do_sync();
        } else if (s_keep && s_saved_ssid[0]) {
            /* Fall back to the network we were on before */
            s_reconnect_tries = 0;
            schedule_reconnect();
            set_cur_ssid(s_saved_ssid);
        } else {
            radio_off();
            s_conn = WIFI_CONN_FAILED;
        }
        break;

    case CMD_DISCONNECT:
        s_keep = false;
        s_reconnect_at_ms = 0;
        creds_save();
        radio_off();
        s_conn = WIFI_CONN_DISCONNECTED;
        set_cur_ssid(s_saved_ssid);
        break;

    case CMD_FORGET:
        s_keep = false;
        s_reconnect_at_ms = 0;
        s_saved_ssid[0] = s_saved_pass[0] = 0;
        creds_save();
        radio_off();
        s_conn = WIFI_CONN_DISCONNECTED;
        s_sync = TIME_SYNC_DISABLED;
        set_cur_ssid("");
        break;

    case CMD_SYNC:
        do_sync();
        break;

    case CMD_RECONNECT:
        s_reconnect_tries = 0;
        schedule_reconnect();
        break;
    }
}

static void worker_task(void *arg)
{
    /* Boot: reconnect to a kept network, then sync (do_sync connects briefly
     * if needed) */
    if (s_enabled) {
        if (s_keep && !sta_connect(s_saved_ssid, s_saved_pass)) schedule_reconnect();
        do_sync();
    }

    for (;;) {
        int64_t now = now_ms();
        int64_t next = s_next_sync_ms;
        if (s_reconnect_at_ms && s_reconnect_at_ms < next) next = s_reconnect_at_ms;
        TickType_t wait = next > now ? pdMS_TO_TICKS(next - now) : 0;
        if (!s_enabled || (!s_saved_ssid[0] && !s_reconnect_at_ms)) wait = portMAX_DELAY;

        cmd_t cmd;
        if (xQueueReceive(s_queue, &cmd, wait) == pdTRUE) {
            handle(&cmd);
            continue;
        }

        now = now_ms();
        if (s_reconnect_at_ms && now >= s_reconnect_at_ms) {
            s_reconnect_at_ms = 0;
            if (s_keep && s_conn != WIFI_CONN_CONNECTED &&
                !sta_connect(s_saved_ssid, s_saved_pass)) {
                schedule_reconnect();
            }
        }
        if (s_enabled && s_saved_ssid[0] && now_ms() >= s_next_sync_ms) {
            do_sync();
        }
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */
esp_err_t wifi_mgr_start(void)
{
    s_queue  = xQueueCreate(6, sizeof(cmd_t));
    s_events = xEventGroupCreate();
    s_lock   = xSemaphoreCreateMutex();
    if (!s_queue || !s_events || !s_lock) return ESP_ERR_NO_MEM;

    s_iface = net_if_get_wifi_sta();
    if (!s_iface) s_iface = net_if_get_first_wifi();
    if (!s_iface) {
        ESP_LOGE(TAG, "No Wi-Fi interface");
        return ESP_FAIL;
    }
    net_mgmt_init_event_callback(&s_wifi_cb, wifi_event_handler,
                                 NET_EVENT_WIFI_SCAN_RESULT | NET_EVENT_WIFI_SCAN_DONE |
                                 NET_EVENT_WIFI_CONNECT_RESULT |
                                 NET_EVENT_WIFI_DISCONNECT_RESULT);
    net_mgmt_add_event_callback(&s_wifi_cb);
    net_mgmt_init_event_callback(&s_ip_cb, ip_event_handler, NET_EVENT_IPV4_ADDR_ADD);
    net_mgmt_add_event_callback(&s_ip_cb);
    s_radio_on = true;          /* the interface comes up at boot */

    creds_load();
    s_enabled = settings_get()->wifi_enabled;
    s_sync = s_enabled && s_saved_ssid[0] ? TIME_SYNC_IDLE : TIME_SYNC_DISABLED;
    if (!s_enabled) ESP_LOGI(TAG, "Wi-Fi is switched off");
    if (s_saved_ssid[0]) {
        ESP_LOGI(TAG, "Saved network \"%s\"%s", s_saved_ssid, s_keep ? " (stay connected)" : "");
    } else {
        ESP_LOGI(TAG, "No saved network — connect from the Wi-Fi tab");
    }

    if (xTaskCreate(worker_task, "wifi_mgr", 4096, NULL, 2, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void post(cmd_type_t type, const char *ssid, const char *pass)
{
    if (!s_queue) return;
    cmd_t cmd = { .type = type };
    if (ssid) strlcpy(cmd.ssid, ssid, sizeof(cmd.ssid));
    if (pass) strlcpy(cmd.pass, pass, sizeof(cmd.pass));
    xQueueSend(s_queue, &cmd, 0);
}

void wifi_mgr_set_enabled(bool enabled)
{
    if (settings_get()->wifi_enabled != enabled) {
        settings_get()->wifi_enabled = enabled;
        watch_settings_save();
    }
    if (s_enabled == enabled) return;
    s_enabled = enabled;          // visible to the UI immediately
    if (!enabled) s_conn = WIFI_CONN_DISCONNECTED;
    post(enabled ? CMD_ENABLE : CMD_DISABLE, NULL, NULL);
}

bool wifi_mgr_is_enabled(void) { return s_enabled; }

void wifi_mgr_scan(void)
{
    if (s_scanning || !s_enabled) return;
    s_scanning = true;           // show the spinner immediately
    post(CMD_SCAN, NULL, NULL);
}

bool     wifi_mgr_is_scanning(void)     { return s_scanning; }
uint32_t wifi_mgr_scan_generation(void) { return s_scan_gen; }

int wifi_mgr_get_scan_results(wifi_ap_info_t *out, int max)
{
    if (!s_lock) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_ap_count < max ? s_ap_count : max;
    memcpy(out, s_aps, n * sizeof(*out));
    xSemaphoreGive(s_lock);
    return n;
}

void wifi_mgr_connect(const char *ssid, const char *password)
{
    if (!s_enabled) return;
    s_conn = WIFI_CONN_CONNECTING;
    set_cur_ssid(ssid);
    post(CMD_CONNECT, ssid, password);
}

void wifi_mgr_connect_saved(void)
{
    if (!s_enabled) return;
    s_conn = WIFI_CONN_CONNECTING;
    post(CMD_CONNECT_SAVED, NULL, NULL);
}

void wifi_mgr_disconnect(void) { post(CMD_DISCONNECT, NULL, NULL); }
void wifi_mgr_forget(void)     { post(CMD_FORGET, NULL, NULL); }

wifi_conn_state_t wifi_mgr_get_state(void) { return s_conn; }
const char *wifi_mgr_get_fail_reason(void) { return s_fail_reason; }
bool wifi_mgr_stays_connected(void)        { return s_keep; }
bool wifi_mgr_is_reconnecting(void)        { return s_reconnect_at_ms != 0 && s_conn != WIFI_CONN_CONNECTED; }

void wifi_mgr_get_ssid(char *buf, size_t len)
{
    if (!s_lock) { buf[0] = 0; return; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(buf, s_cur_ssid, len);
    xSemaphoreGive(s_lock);
}

void wifi_mgr_get_ip(char *buf, size_t len)
{
    buf[0] = 0;
    if (s_conn != WIFI_CONN_CONNECTED || !s_iface) return;
    struct net_in_addr *a = net_if_ipv4_get_global_addr(s_iface, NET_ADDR_PREFERRED);
    if (a) net_addr_ntop(NET_AF_INET, a, buf, len);
}

int8_t wifi_mgr_get_rssi(void)
{
    struct wifi_iface_status st = { 0 };
    if (s_conn != WIFI_CONN_CONNECTED ||
        net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, s_iface, &st, sizeof(st))) return 0;
    return st.rssi;
}

void time_sync_request(void)
{
    if (s_sync != TIME_SYNC_RUNNING) post(CMD_SYNC, NULL, NULL);
}

time_sync_status_t time_sync_get_status(void) { return s_sync; }
time_t time_sync_last_success(void)           { return s_last_sync; }
