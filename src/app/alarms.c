/**
 * @file app/alarms.c
 * @brief Alarm list, persistence and scheduling.
 */

#include "app/alarms.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "alarms";

#define NVS_NS   "alarms"
#define NVS_KEY  "list"

static alarm_t s_alarms[ALARM_MAX];
static int     s_count;

static time_t  s_last_fired_minute[ALARM_MAX];   // minute (time/60) each alarm last rang
static time_t  s_snooze_at;                      // 0 = no snooze
static int     s_snooze_index = -1;

/* ── Persistence ─────────────────────────────────────────────────────────── */
void alarms_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_alarms);
    if (nvs_get_blob(h, NVS_KEY, s_alarms, &len) == ESP_OK) {
        s_count = (int)(len / sizeof(alarm_t));
    }
    nvs_close(h);
    ESP_LOGI(TAG, "%d alarm(s) loaded", s_count);
}

void alarms_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (s_count) {
        nvs_set_blob(h, NVS_KEY, s_alarms, s_count * sizeof(alarm_t));
    } else {
        nvs_erase_key(h, NVS_KEY);
    }
    nvs_commit(h);
    nvs_close(h);
}

/* ── List ────────────────────────────────────────────────────────────────── */
int alarms_count(void)        { return s_count; }
alarm_t *alarms_get(int i)    { return (i >= 0 && i < s_count) ? &s_alarms[i] : NULL; }

int alarms_add(const alarm_t *a)
{
    if (s_count >= ALARM_MAX) return -1;
    s_alarms[s_count] = *a;
    s_last_fired_minute[s_count] = 0;
    return s_count++;
}

void alarms_remove(int i)
{
    if (i < 0 || i >= s_count) return;
    memmove(&s_alarms[i], &s_alarms[i + 1], (s_count - i - 1) * sizeof(alarm_t));
    memmove(&s_last_fired_minute[i], &s_last_fired_minute[i + 1],
            (s_count - i - 1) * sizeof(time_t));
    s_count--;
    if (s_snooze_index == i) alarms_cancel_snooze();
    else if (s_snooze_index > i) s_snooze_index--;
}

static int by_time(const void *a, const void *b)
{
    const alarm_t *x = a, *y = b;
    return (x->hour * 60 + x->minute) - (y->hour * 60 + y->minute);
}

void alarms_sort(void)
{
    /* Fired-minute markers are per position; after a re-sort they would
     * point at the wrong alarm, and the same minute can't fire twice anyway */
    qsort(s_alarms, s_count, sizeof(alarm_t), by_time);
    memset(s_last_fired_minute, 0, sizeof(s_last_fired_minute));
    alarms_cancel_snooze();
}

bool alarms_any_enabled(void)
{
    for (int i = 0; i < s_count; i++) {
        if (s_alarms[i].enabled) return true;
    }
    return s_snooze_at != 0;
}

/* ── Scheduling ──────────────────────────────────────────────────────────── */
bool alarms_check(time_t now, int *index_out)
{
    if (s_snooze_at && now >= s_snooze_at) {
        *index_out = s_snooze_index;
        alarms_cancel_snooze();
        return true;
    }

    struct tm tm;
    localtime_r(&now, &tm);
    time_t minute = now / 60;

    for (int i = 0; i < s_count; i++) {
        alarm_t *a = &s_alarms[i];
        if (!a->enabled || a->hour != tm.tm_hour || a->minute != tm.tm_min) continue;
        if (a->days && !(a->days & (1 << tm.tm_wday))) continue;
        if (s_last_fired_minute[i] == minute) continue;

        s_last_fired_minute[i] = minute;
        if (!a->days) {                       // one-off
            a->enabled = false;
            alarms_save();
        }
        *index_out = i;
        return true;
    }
    return false;
}

void alarms_snooze(time_t now, int index)
{
    s_snooze_at = now + ALARM_SNOOZE_MIN * 60;
    s_snooze_index = index;
}

void alarms_cancel_snooze(void)
{
    s_snooze_at = 0;
    s_snooze_index = -1;
}

time_t alarms_next(time_t now)
{
    time_t best = s_snooze_at;

    struct tm base;
    localtime_r(&now, &base);

    for (int i = 0; i < s_count; i++) {
        const alarm_t *a = &s_alarms[i];
        if (!a->enabled) continue;

        /* Try today and the next 7 days */
        for (int d = 0; d <= 7; d++) {
            struct tm t = base;
            t.tm_mday += d;
            t.tm_hour = a->hour;
            t.tm_min = a->minute;
            t.tm_sec = 0;
            t.tm_isdst = -1;
            time_t when = mktime(&t);              // normalises tm_wday too
            if (when <= now) continue;
            if (a->days && !(a->days & (1 << t.tm_wday))) continue;
            if (!best || when < best) best = when;
            break;
        }
    }
    return best;
}

void alarms_days_text(uint8_t days, char *buf, size_t len)
{
    static const char *const names[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

    switch (days & ALARM_DAYS_ALL) {
    case 0:                    snprintf(buf, len, "Once");      return;
    case ALARM_DAYS_ALL:       snprintf(buf, len, "Every day"); return;
    case ALARM_DAYS_WEEKDAYS:  snprintf(buf, len, "Mon-Fri");   return;
    case ALARM_DAYS_WEEKEND:   snprintf(buf, len, "Weekends");  return;
    }

    /* List them Monday first */
    buf[0] = 0;
    for (int k = 0; k < 7; k++) {
        int d = (k + 1) % 7;
        if (!(days & (1 << d))) continue;
        if (buf[0]) strlcat(buf, " ", len);
        strlcat(buf, names[d], len);
    }
}
