#pragma once
/**
 * @file app/calendar.h
 * @brief Upcoming calendar events from the phone (Gadgetbridge "calendar" /
 *        "calendar-"), kept in storage: Gadgetbridge only sends each event once.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define CALENDAR_MAX_EVENTS  24

typedef struct {
    int64_t  id;
    time_t   start;           // UTC
    int32_t  duration_s;
    bool     all_day;
    uint32_t color;           // 0xAARRGGBB, 0 = none
    char     title[48];
    char     location[40];
    char     calendar[24];
} calendar_event_t;

void calendar_init(void);
/** Add or replace (same id) an event. */
void calendar_add(const calendar_event_t *ev);
void calendar_remove(int64_t id);
/** Events that haven't ended yet, soonest first.  Returns how many were copied. */
size_t calendar_upcoming(calendar_event_t *out, size_t max);
