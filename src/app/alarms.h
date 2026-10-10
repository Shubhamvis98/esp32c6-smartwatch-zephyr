#pragma once
/**
 * @file app/alarms.h
 * @brief Alarm list (up to ALARM_MAX), saved in NVS.
 *
 * Each alarm has a time, a weekday mask and an on/off flag.  An alarm with
 * no weekdays rings once, at the next occurrence of its time, and then
 * switches itself off.  Times are local (Settings → Time zone).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define ALARM_MAX         8
#define ALARM_SNOOZE_MIN  5

/* days: bit 0 = Sunday … bit 6 = Saturday (struct tm tm_wday order) */
#define ALARM_DAYS_WEEKDAYS  0x3E
#define ALARM_DAYS_WEEKEND   0x41
#define ALARM_DAYS_ALL       0x7F

typedef struct {
    uint8_t hour;       // 0–23
    uint8_t minute;     // 0–59
    uint8_t days;       // weekday mask, 0 = once
    bool    enabled;
} alarm_t;

void alarms_load(void);
void alarms_save(void);

int      alarms_count(void);
alarm_t *alarms_get(int index);
/** @brief Append an alarm.  Returns its index, or -1 if the list is full. */
int      alarms_add(const alarm_t *a);
void     alarms_remove(int index);
/** @brief Sort by time of day (call after editing). */
void     alarms_sort(void);

/** @brief Any alarm switched on. */
bool     alarms_any_enabled(void);

/**
 * @brief Check whether an alarm (or a snooze) is due at local time `now`.
 *        Call at least once per minute.  Each alarm fires once per minute
 *        it matches; one-off alarms are switched off when they fire.
 * @return true if something should ring now.
 */
bool     alarms_check(time_t now, int *index_out);

/** @brief Ring again in ALARM_SNOOZE_MIN minutes. */
void     alarms_snooze(time_t now, int index);
void     alarms_cancel_snooze(void);

/** @brief Next time any enabled alarm (or snooze) will ring, or 0 if none. */
time_t   alarms_next(time_t now);

/** @brief "Mon–Fri", "Every day", "Weekends", "Once", or "Mon Wed Fri". */
void     alarms_days_text(uint8_t days, char *buf, size_t len);
