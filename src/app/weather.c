/**
 * @file app/weather.c
 * @brief Weather from the phone, saved to storage (see weather.h).
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "app/weather.h"
#include "hal/storage.h"

#define WEATHER_FILE   STORAGE_PATH "/weather.bin"
#define FILE_MAGIC     0x5754            // "WT"
#define FILE_VERSION   1

static weather_t   s_weather;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

void weather_init(void)
{
    FILE *f = fopen(WEATHER_FILE, "rb");
    if (!f) return;
    uint16_t hdr[2];
    weather_t w;
    if (fread(hdr, sizeof(hdr), 1, f) == 1 && hdr[0] == FILE_MAGIC && hdr[1] == FILE_VERSION &&
        fread(&w, sizeof(w), 1, f) == 1) {
        w.text[sizeof(w.text) - 1] = '\0';
        w.location[sizeof(w.location) - 1] = '\0';
        if (w.days > WEATHER_FORECAST_DAYS) w.days = WEATHER_FORECAST_DAYS;
        taskENTER_CRITICAL(&s_lock);
        s_weather = w;
        taskEXIT_CRITICAL(&s_lock);
    }
    fclose(f);
}

void weather_set(const weather_t *w)
{
    taskENTER_CRITICAL(&s_lock);
    s_weather = *w;
    taskEXIT_CRITICAL(&s_lock);

    FILE *f = fopen(WEATHER_FILE, "wb");
    if (!f) return;
    const uint16_t hdr[2] = { FILE_MAGIC, FILE_VERSION };
    fwrite(hdr, sizeof(hdr), 1, f);
    fwrite(w, sizeof(*w), 1, f);
    fclose(f);
}

void weather_get(weather_t *w)
{
    taskENTER_CRITICAL(&s_lock);
    *w = s_weather;
    taskEXIT_CRITICAL(&s_lock);
}

/* OpenWeatherMap codes: 2xx thunderstorm, 3xx drizzle, 5xx rain, 6xx snow,
 * 7xx mist/fog/haze, 800 clear, 801-802 partly cloudy, 803-804 cloudy */
weather_kind_t weather_kind(uint16_t code)
{
    if (code >= 200 && code < 300) return WEATHER_KIND_STORM;
    if (code >= 300 && code < 600) return WEATHER_KIND_RAIN;
    if (code >= 600 && code < 700) return WEATHER_KIND_SNOW;
    if (code >= 700 && code < 800) return WEATHER_KIND_FOG;
    if (code == 801 || code == 802) return WEATHER_KIND_PARTLY;
    if (code > 802 && code < 900)  return WEATHER_KIND_CLOUDY;
    return WEATHER_KIND_CLEAR;
}
