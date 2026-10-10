#pragma once
/**
 * @file app/gadgetbridge.h
 * @brief Gadgetbridge (Android, the F-Droid app) support over the Nordic UART
 *        service (smartwatch gadget mode of app/ble_mgr.c).  Gadgetbridge has
 *        no generic watch protocol; this speaks the one its Bangle.js driver
 *        uses, which is plain-text JSON and so the simplest to implement.
 *
 * Phone → watch: lines starting with 0x10, either JavaScript for the time
 *   ("setTime(…);E.setTimeZone(…);…") or "GB({json})".
 * Watch → phone: one JSON object per line, e.g. {"t":"status","bat":80}.
 *
 * Supported: time + timezone sync, battery status, step reports,
 * notifications (show / dismiss), incoming calls (accept / reject),
 * find-my-watch, find-my-phone, music state and controls.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* Called by ble_mgr */
void gb_init(void);
void gb_on_connect(void);
void gb_on_ready(void);          // phone subscribed to our TX characteristic
void gb_on_disconnect(void);
void gb_on_rx(const uint8_t *data, size_t len);

/** True when Gadgetbridge is connected and listening. */
bool gb_is_connected(void);
/** Time of the last time sync from the phone, or 0. */
time_t gb_last_time_sync(void);

/* Watch → phone */
void gb_send_music(const char *cmd);                 // play pause next previous volumeup volumedown
void gb_send_call(const char *action);               // ACCEPT REJECT END IGNORE
void gb_send_notify_action(int32_t id, const char *action);   // DISMISS …
/** Ask the phone to call @p number (Phone app); false if not connected.
 *  Needs the C6 Watch device type in Gadgetbridge; a failure comes back
 *  through ui_phone_dial_error(). */
bool gb_dial(const char *number);
void gb_find_phone(bool on);

/* Music state reported by the phone */
bool gb_music_playing(void);
/** Artist and track separately; either may be "". */
void gb_get_music(char *artist, size_t artist_len, char *track, size_t track_len);
