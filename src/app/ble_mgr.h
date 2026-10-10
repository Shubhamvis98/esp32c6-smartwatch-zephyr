#pragma once
/**
 * @file app/ble_mgr.h
 * @brief Bluetooth LE (NimBLE) in one of two "gadget modes":
 *
 *   BLE_MODE_WATCH — Nordic UART service speaking Gadgetbridge's Bangle.js
 *                    protocol (see app/gadgetbridge.c).  The name doesn't
 *                    match any Gadgetbridge pattern, so the app only lists
 *                    it with "Discover unsupported devices" on; it is then
 *                    added via long-press → "Add test device" → Bangle.js.
 *                    Also a media-keys-only HID service on the same link
 *                    (the camera shutter sends volume up).
 *   BLE_MODE_HID   — HID over GATT: keyboard + mouse + media keys, for
 *                    phones / PCs / tablets
 *
 * Both modes advertise the user-editable name (settings, default "C6 Watch").
 *
 * Each mode uses its own Bluetooth address, so hosts see two separate
 * devices and their pairings never mix.  Switching mode restarts the stack.
 * Bonds are stored in NVS.  Start/stop work happens on a worker task, so
 * these calls never block.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    BLE_MODE_WATCH,
    BLE_MODE_HID,
    BLE_MODE_COUNT,
} ble_mode_t;

typedef enum {
    BLE_STATE_OFF,
    BLE_STATE_STARTING,
    BLE_STATE_ADVERTISING,   // visible, waiting for a connection
    BLE_STATE_CONNECTED,
} ble_state_t;

/** @brief Start the worker and apply the saved on/off + mode settings. */
esp_err_t ble_mgr_start(void);

/** @brief Turn Bluetooth on/off and/or change mode (saved to settings). */
void ble_mgr_set(bool enabled, ble_mode_t mode);

/** @brief Delete all paired devices (both modes). */
void ble_mgr_forget_all(void);

ble_state_t ble_mgr_get_state(void);
ble_mode_t  ble_mgr_get_mode(void);
bool        ble_mgr_is_enabled(void);
/** True once the link is encrypted (paired). */
bool        ble_mgr_is_secure(void);

/* Longest name: HID advertising carries flags, the HID UUID and the
 * appearance next to the name in 31 bytes */
#define BLE_NAME_MAX_LEN  18

/** The name the watch advertises (same in both modes). */
const char *ble_mgr_get_name(void);

/**
 * Set the name (saved).  Printable ASCII, 1–BLE_NAME_MAX_LEN
 * characters after trimming.  Bluetooth restarts if it is on, so a
 * connected device reconnects.  Returns false if the name is invalid.
 */
bool ble_mgr_set_name(const char *name);

/** The watch's own Bluetooth address in a mode, as "AA:BB:CC:DD:EE:FF". */
void        ble_mgr_get_addr(ble_mode_t mode, char *buf, size_t len);

/** Connected peer address as text ("" when not connected). */
void        ble_mgr_get_peer(char *buf, size_t len);

/* ── Watch mode: Nordic UART ─────────────────────────────────────────────── */
/** Send one line to the phone ("\r\n" is appended).  False if not connected. */
bool ble_uart_send_line(const char *line);

/* ── HID mode ────────────────────────────────────────────────────────────── */
#define HID_MOUSE_LEFT    0x01
#define HID_MOUSE_RIGHT   0x02
#define HID_MOUSE_MIDDLE  0x04

#define HID_CONSUMER_PLAY_PAUSE  0x00CD
#define HID_CONSUMER_NEXT        0x00B5
#define HID_CONSUMER_PREV        0x00B6
#define HID_CONSUMER_VOL_UP      0x00E9
#define HID_CONSUMER_VOL_DOWN    0x00EA

/** True when connected, paired and the host listens for input reports. */
bool ble_hid_ready(void);
void ble_hid_mouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel);
/** Press and release one key (HID usage) with modifier bits. */
void ble_hid_key_tap(uint8_t modifier, uint8_t usage);
/** Press and release a consumer-control (media) key.  Works in both modes:
 *  in smartwatch mode the watch is also a media remote on the same link. */
void ble_hid_consumer_tap(uint16_t usage);
/** The phone is listening for media keys (bonded and subscribed). */
bool ble_media_ready(void);
/** Ask the connected phone to pair, so it starts listening for media keys. */
void ble_media_pair(void);
