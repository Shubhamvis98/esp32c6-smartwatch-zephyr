#pragma once
/**
 * @file app/timezones.h
 * @brief Timezone list for the Time page, and automatic timezone detection.
 *
 * Automatic detection asks ip-api.com (plain HTTP, no key) for the IANA
 * timezone of the watch's public IP address after each time sync.  IANA
 * names in the list get full DST rules; any other zone gets its current
 * fixed UTC offset, refreshed at every sync.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *name;    // shown in the picker, "+5:30 India"
    const char *city;    // short name for the menu hint
    const char *tz;      // POSIX TZ string (includes DST rules)
    const char *iana;    // IANA names in this zone, space separated; first is canonical
} tz_entry_t;

int               tz_count(void);
const tz_entry_t *tz_get(int index);

/** Index of the entry for the current settings (IANA name first, then the
 *  POSIX string), or -1 if it is not in the list. */
int tz_current_index(void);

/** Display name of the current timezone ("+5:30 India", "+4 Tbilisi"). */
void tz_current_name(char *buf, size_t len);

/** Use entry @p index (manual choice).  Saves settings. */
void tz_select(int index);

/** Look up the timezone from the watch's IP address and apply it if
 *  automatic timezone is on.  Needs a working internet connection; blocks
 *  for up to a few seconds.  Call from a task, not the LVGL task. */
bool tz_auto_detect(void);
