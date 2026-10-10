/**
 * @file app/calendar.c
 * @brief Calendar events from the phone, saved to storage (see calendar.h).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "app/calendar.h"
#include "hal/storage.h"

#define CALENDAR_FILE  STORAGE_PATH "/calendar.bin"
#define FILE_MAGIC     0x434C            // "CL"
#define FILE_VERSION   1

static calendar_event_t s_ev[CALENDAR_MAX_EVENTS];
static size_t           s_n;
static SemaphoreHandle_t s_mutex;

static bool ended(const calendar_event_t *e, time_t now)
{
    time_t end = e->start + (e->duration_s > 0 ? e->duration_s : 0);
    if (e->all_day && e->duration_s <= 0) end = e->start + 24 * 3600;
    return end < now;
}

static int by_start(const void *a, const void *b)
{
    const calendar_event_t *x = a, *y = b;
    return (x->start > y->start) - (x->start < y->start);
}

/* Drop finished events (when the clock is set) and keep the list sorted */
static void tidy(void)
{
    time_t now = time(NULL);
    if (now > 1600000000) {
        size_t o = 0;
        for (size_t i = 0; i < s_n; i++) if (!ended(&s_ev[i], now)) s_ev[o++] = s_ev[i];
        s_n = o;
    }
    qsort(s_ev, s_n, sizeof(s_ev[0]), by_start);
}

static void save(void)
{
    FILE *f = fopen(CALENDAR_FILE, "wb");
    if (!f) return;
    const uint16_t hdr[2] = { FILE_MAGIC, FILE_VERSION };
    const uint16_t n = (uint16_t)s_n;
    fwrite(hdr, sizeof(hdr), 1, f);
    fwrite(&n, sizeof(n), 1, f);
    fwrite(s_ev, sizeof(s_ev[0]), s_n, f);
    fclose(f);
}

void calendar_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    FILE *f = fopen(CALENDAR_FILE, "rb");
    if (!f) return;
    uint16_t hdr[2], n = 0;
    if (fread(hdr, sizeof(hdr), 1, f) == 1 && hdr[0] == FILE_MAGIC && hdr[1] == FILE_VERSION &&
        fread(&n, sizeof(n), 1, f) == 1) {
        if (n > CALENDAR_MAX_EVENTS) n = CALENDAR_MAX_EVENTS;
        s_n = fread(s_ev, sizeof(s_ev[0]), n, f);
        for (size_t i = 0; i < s_n; i++) {
            s_ev[i].title[sizeof(s_ev[i].title) - 1] = '\0';
            s_ev[i].location[sizeof(s_ev[i].location) - 1] = '\0';
            s_ev[i].calendar[sizeof(s_ev[i].calendar) - 1] = '\0';
        }
    }
    fclose(f);
}

void calendar_add(const calendar_event_t *ev)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t i;
    for (i = 0; i < s_n && s_ev[i].id != ev->id; i++) {}
    if (i == s_n) {
        tidy();
        if (s_n == CALENDAR_MAX_EVENTS) {
            /* Full: replace the event furthest away, if this one is sooner */
            if (ev->start >= s_ev[s_n - 1].start) {
                xSemaphoreGive(s_mutex);
                return;
            }
            i = s_n - 1;
        } else {
            i = s_n++;
        }
    }
    s_ev[i] = *ev;
    tidy();
    save();
    xSemaphoreGive(s_mutex);
}

void calendar_remove(int64_t id)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t o = 0;
    for (size_t i = 0; i < s_n; i++) if (s_ev[i].id != id) s_ev[o++] = s_ev[i];
    bool changed = o != s_n;
    s_n = o;
    if (changed) save();
    xSemaphoreGive(s_mutex);
}

size_t calendar_upcoming(calendar_event_t *out, size_t max)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    tidy();
    size_t n = s_n < max ? s_n : max;
    memcpy(out, s_ev, n * sizeof(out[0]));
    xSemaphoreGive(s_mutex);
    return n;
}
