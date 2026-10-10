/**
 * @file app/gadgetbridge.c
 * @brief Bangle.js-protocol implementation for Gadgetbridge.
 *
 * Incoming data arrives in MTU-sized pieces on the NimBLE host task and is
 * reassembled into lines here.  Outgoing reports (status, steps) are sent
 * from a small task so the host task never blocks on its own notifications.
 *
 * Strings from the phone are ISO-8859-1 (Gadgetbridge encodes them that
 * way) and are converted to UTF-8 before reaching LVGL.
 */

#include "app/gadgetbridge.h"
#include "app/contacts.h"
#include "app/ble_mgr.h"
#include "app/activity.h"
#include "app/settings.h"
#include "app/weather.h"
#include "app/calendar.h"
#include "hal/audio.h"
#include "hal/battery.h"
#include "hal/rtc.h"
#include "ui/ui_manager.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "gadgetbridge";

#define LINE_MAX            2048
#define STATUS_PERIOD_MS    (10 * 60 * 1000)
#define ACT_PERIOD_MS       (10 * 60 * 1000)

#define BIT_HELLO   (1 << 0)

static char          s_line[LINE_MAX];
static size_t        s_line_len;
static bool          s_line_overflow;

static volatile bool s_ready;
static volatile time_t s_last_sync;
static TaskHandle_t  s_task;

/* Realtime step reporting requested by the phone ({"t":"act","stp":true,"int":N}) */
static volatile uint32_t s_rt_interval_s;
static uint32_t      s_last_reported_steps;

static SemaphoreHandle_t s_music_lock;
static StaticSemaphore_t s_music_lock_buf;
static bool          s_music_playing;     // last state the phone reported
/* After a play/pause press the watch shows the requested state at once, but
 * only until this tick: if no player is running the phone never answers with
 * a musicstate, and the icon falls back to what the phone last reported. */
#define MUSIC_PENDING_MS  2000
static bool          s_music_pending;
static TickType_t    s_music_pending_until;
static char          s_artist[48];
static char          s_track[64];

/* ── Helpers ─────────────────────────────────────────────────────────────── */
/* Created on first use: the UI reads the track before gb_init() runs */
static void music_lock(void)
{
    if (!s_music_lock) s_music_lock = xSemaphoreCreateMutexStatic(&s_music_lock_buf);
    xSemaphoreTake(s_music_lock, portMAX_DELAY);
}

static void music_unlock(void)
{
    xSemaphoreGive(s_music_lock);
}

/* ISO-8859-1 → UTF-8 (each byte ≥ 0x80 becomes two bytes) */
static void latin1_to_utf8(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 3 < out_len; p++) {
        if (*p < 0x80) {
            out[o++] = (char)*p;
        } else if (*p == 0xA0) {         // no-break space ("66 m"): the fonts have no glyph
            out[o++] = ' ';
        } else if (*p == 0xAD) {         // soft hyphen: invisible
            continue;
        } else {
            out[o++] = (char)(0xC0 | (*p >> 6));
            out[o++] = (char)(0x80 | (*p & 0x3F));
        }
    }
    out[o] = 0;
}

static const char *json_str(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

static double json_num(const cJSON *obj, const char *key, double def)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) ? item->valuedouble : def;
}

static bool json_bool(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsTrue(item) || (cJSON_IsNumber(item) && item->valuedouble != 0);
}

static void send_json(cJSON *obj)
{
    char *txt = cJSON_PrintUnformatted(obj);
    if (txt) {
        ble_uart_send_line(txt);
        cJSON_free(txt);
    }
    cJSON_Delete(obj);
}

/* ── Watch → phone reports ───────────────────────────────────────────────── */
static void send_status(void)
{
    int mv = battery_read_mv();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "status");
    cJSON_AddNumberToObject(o, "bat", battery_percent_from_mv(mv));
    cJSON_AddNumberToObject(o, "volt", round(mv / 10.0) / 100.0);   // e.g. 4.18
    cJSON_AddNumberToObject(o, "chg", battery_mv_is_charging(mv) ? 1 : 0);
    send_json(o);
}

static void send_version(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "ver");
    cJSON_AddStringToObject(o, "fw", esp_app_get_description()->version);
    cJSON_AddStringToObject(o, "hw", "ESP32-C6-Touch-LCD-1.69");
    send_json(o);
}

/* Steps since the last report (the counter resets at midnight) */
static void send_activity(bool realtime)
{
    uint32_t steps = activity_get_steps();
    uint32_t delta = steps >= s_last_reported_steps ? steps - s_last_reported_steps : steps;
    s_last_reported_steps = steps;

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "act");
    cJSON_AddNumberToObject(o, "stp", delta);
    cJSON_AddNumberToObject(o, "hrm", 0);
    if (realtime) cJSON_AddNumberToObject(o, "rt", 1);
    send_json(o);
}

static void gb_task(void *arg)
{
    TickType_t last_status = 0, last_act = 0, last_rt = 0;

    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(1000));
        if (!s_ready) continue;

        TickType_t now = xTaskGetTickCount();
        if (bits & BIT_HELLO) {
            send_version();
            send_status();
            last_status = last_act = now;
            continue;
        }
        if (now - last_status >= pdMS_TO_TICKS(STATUS_PERIOD_MS)) {
            last_status = now;
            send_status();
        }
        if (now - last_act >= pdMS_TO_TICKS(ACT_PERIOD_MS)) {
            last_act = now;
            send_activity(false);
        }
        uint32_t rt = s_rt_interval_s;
        if (rt && now - last_rt >= pdMS_TO_TICKS(rt * 1000)) {
            last_rt = now;
            send_activity(true);
        }
    }
}

/* ── Phone → watch handling ──────────────────────────────────────────────── */
static int32_t local_offset_s(time_t t)
{
    struct tm lt, gt;
    localtime_r(&t, &lt);
    gmtime_r(&t, &gt);
    int day = lt.tm_yday - gt.tm_yday;
    if (day > 1) day = -1;          // year wrap
    if (day < -1) day = 1;
    return day * 86400 + (lt.tm_hour - gt.tm_hour) * 3600 + (lt.tm_min - gt.tm_min) * 60;
}

/* "setTime(1700000000);E.setTimeZone(5.5);(s=>…)" */
static void handle_time(const char *line)
{
    const char *p = strstr(line, "setTime(");
    if (p) {
        time_t ts = (time_t)strtoll(p + 8, NULL, 10);
        if (ts > 1600000000) {
            struct timeval tv = { .tv_sec = ts };
            settimeofday(&tv, NULL);
            rtc_write(ts);
            s_last_sync = ts;
            ESP_LOGI(TAG, "Time set from phone");
        }
    }

    p = strstr(line, "E.setTimeZone(");
    if (p) {
        float hours = strtof(p + 14, NULL);
        int32_t phone = (int32_t)lroundf(hours * 3600.0f);
        /* Keep the chosen city (with its DST rules) if it already matches */
        if (abs(phone - local_offset_s(time(NULL))) >= 60) {
            int mins = phone / 60;
            int m = abs(mins);
            char tz[24];
            snprintf(tz, sizeof(tz), "<%c%02d%02d>%c%d:%02d", mins >= 0 ? '+' : '-',
                     m / 60, m % 60, mins >= 0 ? '-' : '+', m / 60, m % 60);
            settings_set_timezone(tz);
            ESP_LOGI(TAG, "Timezone set from phone: %s", tz);
        }
    }
}

static void handle_json(cJSON *o)
{
    const char *t = json_str(o, "t");
    char a[160], b[96], c[512];
    ESP_LOGI(TAG, "Phone sent \"%s\"", t);

    if (strcmp(t, "notify") == 0) {
        const char *title = json_str(o, "title");
        if (!*title) title = json_str(o, "sender");
        const char *body = json_str(o, "body");
        if (!*body) body = json_str(o, "subject");
        latin1_to_utf8(json_str(o, "src"), b, sizeof(b));
        latin1_to_utf8(title, a, sizeof(a));
        latin1_to_utf8(body, c, sizeof(c));
        ui_bt_notification((int32_t)json_num(o, "id", 0), b, a, c);

    } else if (strcmp(t, "notify-") == 0) {
        ui_bt_notification_remove((int32_t)json_num(o, "id", 0));

    } else if (strcmp(t, "call") == 0) {
        const char *cmd = json_str(o, "cmd");
        latin1_to_utf8(json_str(o, "name"), a, sizeof(a));
        latin1_to_utf8(json_str(o, "number"), b, sizeof(b));
        bool incoming = strcmp(cmd, "incoming") == 0;
        if (strcmp(cmd, "accept") == 0) {
            /* An outgoing call was answered (C6 Watch device type only) */
            ui_bt_call_answered(a, b);
        } else {
            if (incoming) recents_add(a, b, false);
            ui_bt_call(incoming, a, b);
        }

    } else if (strcmp(t, "contacts") == 0) {
        /* Favourites from Gadgetbridge → Contacts: {c:[{n:"Mom", p:"+1555..."}]} */
        static contact_t list[CONTACTS_MAX];
        int n = 0;
        const cJSON *c = cJSON_GetObjectItemCaseSensitive(o, "c");
        const cJSON *item;
        cJSON_ArrayForEach(item, c) {
            if (n >= CONTACTS_MAX) break;
            latin1_to_utf8(json_str(item, "n"), list[n].name, sizeof(list[n].name));
            snprintf(list[n].number, sizeof(list[n].number), "%s", json_str(item, "p"));
            if (list[n].number[0]) n++;
        }
        contacts_set(list, n);

    } else if (strcmp(t, "dial") == 0) {
        /* Only sent back when the phone couldn't place the call */
        latin1_to_utf8(json_str(o, "err"), a, sizeof(a));
        ui_phone_dial_error(a);

    } else if (strcmp(t, "find") == 0) {
        ui_bt_find(json_bool(o, "n"));

    } else if (strcmp(t, "vibrate") == 0) {
        audio_beep(1500, 120, 80, 2);           // no vibration motor: beep instead

    } else if (strcmp(t, "musicstate") == 0) {
        music_lock();
        s_music_playing = strcmp(json_str(o, "state"), "play") == 0;
        s_music_pending_until = 0;              // the phone's answer wins
        music_unlock();

    } else if (strcmp(t, "musicinfo") == 0) {
        music_lock();
        latin1_to_utf8(json_str(o, "artist"), s_artist, sizeof(s_artist));
        latin1_to_utf8(json_str(o, "track"), s_track, sizeof(s_track));
        music_unlock();

    } else if (strcmp(t, "act") == 0) {
        s_rt_interval_s = json_bool(o, "stp") ? (uint32_t)json_num(o, "int", 10) : 0;

    } else if (strcmp(t, "nav") == 0) {
        /* Turn-by-turn from CoMaps / Google Maps / OsmAnd; a message with
         * none of these fields means navigation has stopped */
        ui_nav_info_t nav = { .left_s = -1, .pct = -1 };
        latin1_to_utf8(json_str(o, "instr"), nav.instr, sizeof(nav.instr));
        latin1_to_utf8(json_str(o, "distance"), nav.distance, sizeof(nav.distance));
        latin1_to_utf8(json_str(o, "eta"), nav.eta, sizeof(nav.eta));
        snprintf(nav.action, sizeof(nav.action), "%s", json_str(o, "action"));
        nav.left_s = (int32_t)json_num(o, "left", -1);
        nav.pct = (int8_t)json_num(o, "pct", -1);
        ui_bt_nav(&nav);

    } else if (strcmp(t, "weather") == 0) {
        /* Temperatures arrive in kelvin */
        weather_t w = { .valid = true };
        w.temp = (int8_t)lround(json_num(o, "temp", 273) - 273.15);
        w.hi = (int8_t)lround(json_num(o, "hi", 273) - 273.15);
        w.lo = (int8_t)lround(json_num(o, "lo", 273) - 273.15);
        w.humidity = (uint8_t)json_num(o, "hum", 0);
        w.rain = (uint8_t)json_num(o, "rain", 0);
        w.code = (uint16_t)json_num(o, "code", 800);
        w.wind_kmh = (float)json_num(o, "wind", 0);
        w.wind_dir = (uint16_t)json_num(o, "wdir", 0);
        w.updated = (time_t)json_num(o, "ts", (double)time(NULL));
        latin1_to_utf8(json_str(o, "txt"), w.text, sizeof(w.text));
        latin1_to_utf8(json_str(o, "loc"), w.location, sizeof(w.location));
        const cJSON *fc = cJSON_GetObjectItemCaseSensitive(o, "fc");
        const cJSON *d;
        cJSON_ArrayForEach(d, fc) {
            if (w.days >= WEATHER_FORECAST_DAYS || cJSON_GetArraySize(d) < 3) break;
            w.day[w.days].lo = (int8_t)lround(cJSON_GetArrayItem(d, 0)->valuedouble - 273.15);
            w.day[w.days].hi = (int8_t)lround(cJSON_GetArrayItem(d, 1)->valuedouble - 273.15);
            w.day[w.days].code = (uint16_t)cJSON_GetArrayItem(d, 2)->valuedouble;
            w.days++;
        }
        weather_set(&w);
        ui_weather_changed();

    } else if (strcmp(t, "calendar") == 0) {
        calendar_event_t ev = {
            .id = (int64_t)json_num(o, "id", 0),
            .start = (time_t)json_num(o, "timestamp", 0),
            .duration_s = (int32_t)json_num(o, "durationInSeconds", 0),
            .all_day = json_bool(o, "allDay"),
            .color = (uint32_t)(int64_t)json_num(o, "color", 0),
        };
        latin1_to_utf8(json_str(o, "title"), ev.title, sizeof(ev.title));
        latin1_to_utf8(json_str(o, "location"), ev.location, sizeof(ev.location));
        latin1_to_utf8(json_str(o, "calName"), ev.calendar, sizeof(ev.calendar));
        calendar_add(&ev);
        ui_calendar_changed();

    } else if (strcmp(t, "calendar-") == 0) {
        calendar_remove((int64_t)json_num(o, "id", 0));
        ui_calendar_changed();

    } else if (strcmp(t, "is_gps_active") == 0) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "t", "gps_power");
        cJSON_AddBoolToObject(r, "status", false);
        send_json(r);

    }
}

static void handle_line(char *line)
{
    while (*line == 0x10) line++;                    // DLE: "don't echo" marker
    if (!*line) return;
    ESP_LOGD(TAG, "RX line: %.60s", line);

    if (strstr(line, "setTime(") || strstr(line, "E.setTimeZone(")) {
        handle_time(line);
        return;
    }

    char *start = strstr(line, "GB(");
    char *end = strrchr(line, ')');
    if (!start || !end || end <= start + 3) {
        ESP_LOGD(TAG, "Unhandled line: %.40s", line);
        return;
    }
    start += 3;
    cJSON *o = cJSON_ParseWithLength(start, end - start);
    if (!o) {
        ESP_LOGW(TAG, "Bad JSON: %.60s", start);
        return;
    }
    handle_json(o);
    cJSON_Delete(o);
}

/* ── ble_mgr hooks ───────────────────────────────────────────────────────── */
void gb_on_rx(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char ch = (char)data[i];
        if (ch == '\n') {
            if (!s_line_overflow) {
                if (s_line_len && s_line[s_line_len - 1] == '\r') s_line_len--;
                s_line[s_line_len] = 0;
                handle_line(s_line);
            }
            s_line_len = 0;
            s_line_overflow = false;
        } else if (s_line_len < LINE_MAX - 1) {
            s_line[s_line_len++] = ch;
        } else {
            s_line_overflow = true;                  // drop over-long lines whole
        }
    }
}

void gb_on_connect(void)
{
    s_line_len = 0;
    s_line_overflow = false;
    s_rt_interval_s = 0;
    s_last_reported_steps = activity_get_steps();
}

void gb_on_ready(void)
{
    s_ready = true;
    if (s_task) xTaskNotify(s_task, BIT_HELLO, eSetBits);
}

void gb_on_disconnect(void)
{
    s_ready = false;
    s_rt_interval_s = 0;
}

void gb_init(void)
{
    xTaskCreate(gb_task, "gadgetbridge", 3072, NULL, 2, &s_task);
}

/* ── Public API ──────────────────────────────────────────────────────────── */
bool gb_is_connected(void)       { return s_ready; }
time_t gb_last_time_sync(void)   { return s_last_sync; }

void gb_send_music(const char *cmd)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "music");
    cJSON_AddStringToObject(o, "n", cmd);
    send_json(o);

    /* Show the requested state for a moment; it's kept only if the phone
     * confirms it with a musicstate message */
    if (strcmp(cmd, "play") == 0 || strcmp(cmd, "pause") == 0) {
        music_lock();
        s_music_pending = strcmp(cmd, "play") == 0;
        s_music_pending_until = xTaskGetTickCount() + pdMS_TO_TICKS(MUSIC_PENDING_MS);
        if (!s_music_pending_until) s_music_pending_until = 1;
        music_unlock();
    }
}

void gb_send_call(const char *action)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "call");
    cJSON_AddStringToObject(o, "n", action);
    send_json(o);
}

void gb_send_notify_action(int32_t id, const char *action)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "notify");
    cJSON_AddNumberToObject(o, "id", id);
    cJSON_AddStringToObject(o, "n", action);
    send_json(o);
}

bool gb_dial(const char *number)
{
    if (!gb_is_connected() || !*number) return false;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "dial");
    cJSON_AddStringToObject(o, "num", number);
    send_json(o);
    return true;
}

void gb_find_phone(bool on)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "findPhone");
    cJSON_AddBoolToObject(o, "n", on);
    send_json(o);
}

bool gb_music_playing(void)
{
    music_lock();
    bool playing = s_music_playing;
    if (s_music_pending_until) {
        if ((int32_t)(s_music_pending_until - xTaskGetTickCount()) > 0) playing = s_music_pending;
        else s_music_pending_until = 0;         // unconfirmed: back to the phone's state
    }
    music_unlock();
    return playing;
}

void gb_get_music(char *artist, size_t artist_len, char *track, size_t track_len)
{
    music_lock();
    snprintf(artist, artist_len, "%s", s_artist);
    snprintf(track, track_len, "%s", s_track);
    music_unlock();
}
