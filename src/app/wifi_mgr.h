#pragma once
/**
 * @file app/wifi_mgr.h
 * @brief Wi-Fi manager: scanning, connecting, saved network and NTP time sync.
 *
 * All radio work happens in one worker task, so these calls never block and
 * are safe from the LVGL task.  Results are polled (state / scan generation).
 *
 * Wi-Fi can be switched off entirely (saved in settings): the radio then
 * stays off and nothing scans, connects or syncs until it is switched on.
 *
 * Radio policy (battery first):
 *  - After the user connects from the Wi-Fi tab, the watch stays connected
 *    and reconnects automatically, until "Disconnect" or "Forget".  While the
 *    network is out of reach it retries after 3 s, backing off to once a
 *    minute; this also applies at boot and when Wi-Fi is switched on.
 *  - Otherwise the radio is only switched on briefly to scan or to sync time
 *    with the saved network.
 *
 * Credentials are stored in NVS (namespace "wifi").  If none are saved, the
 * menuconfig SSID/password (Smartwatch menu) are used for time sync only.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

#define WIFI_MGR_MAX_APS    16

typedef enum {
    WIFI_CONN_DISCONNECTED,
    WIFI_CONN_CONNECTING,
    WIFI_CONN_CONNECTED,
    WIFI_CONN_FAILED,         // last connect attempt failed, see wifi_mgr_get_fail_reason()
} wifi_conn_state_t;

typedef struct {
    char   ssid[33];
    int8_t rssi;
    bool   secure;            // needs a password
} wifi_ap_info_t;

typedef enum {
    TIME_SYNC_DISABLED,       // Wi-Fi off or no saved network
    TIME_SYNC_IDLE,
    TIME_SYNC_RUNNING,
    TIME_SYNC_OK,
    TIME_SYNC_FAILED,
} time_sync_status_t;

/** @brief Initialise Wi-Fi and start the worker.  Needs nvs_flash_init(). */
esp_err_t wifi_mgr_start(void);

/** @brief Switch Wi-Fi on/off (saved).  Off drops any connection. */
void wifi_mgr_set_enabled(bool enabled);
bool wifi_mgr_is_enabled(void);

/* ── Scanning ────────────────────────────────────────────────────────────── */
void     wifi_mgr_scan(void);
bool     wifi_mgr_is_scanning(void);
/** Incremented every time a scan finishes; compare to detect new results. */
uint32_t wifi_mgr_scan_generation(void);
/** Copy the last scan results (sorted strongest first). Returns the count. */
int      wifi_mgr_get_scan_results(wifi_ap_info_t *out, int max);

/* ── Connection ──────────────────────────────────────────────────────────── */
/** Connect (password may be "" for open networks).  Saved only on success. */
void wifi_mgr_connect(const char *ssid, const char *password);
/** Connect to the saved network and stay connected (undoes "Disconnect"). */
void wifi_mgr_connect_saved(void);
/** Disconnect and stop staying connected (the network stays saved for time sync). */
void wifi_mgr_disconnect(void);
/** Disconnect and delete the saved network. */
void wifi_mgr_forget(void);

wifi_conn_state_t wifi_mgr_get_state(void);
const char *wifi_mgr_get_fail_reason(void);
/** SSID being connected/connected to, or the saved one.  Empty if none. */
void wifi_mgr_get_ssid(char *buf, size_t len);
/** True if the saved network is kept connected (user connected from the tab). */
bool wifi_mgr_stays_connected(void);
/** True while waiting to retry the kept network after losing or failing it. */
bool wifi_mgr_is_reconnecting(void);
/** IPv4 address as text while connected; empty otherwise. */
void wifi_mgr_get_ip(char *buf, size_t len);
/** RSSI of the current connection, or 0 if not connected. */
int8_t wifi_mgr_get_rssi(void);

/* ── Time sync ───────────────────────────────────────────────────────────── */
void               time_sync_request(void);
time_sync_status_t time_sync_get_status(void);
/** Time of the last successful sync, or 0. */
time_t             time_sync_last_success(void);
