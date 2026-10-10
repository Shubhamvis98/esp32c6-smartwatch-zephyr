#pragma once
/**
 * @file app/weather.h
 * @brief The latest weather from the phone (Gadgetbridge "weather"), kept in
 *        storage so it survives a restart.
 */

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define WEATHER_FORECAST_DAYS 5

typedef struct {
    int8_t   lo, hi;          // °C
    uint16_t code;            // OpenWeatherMap condition code
} weather_day_t;

typedef struct {
    bool     valid;
    time_t   updated;         // when the phone's weather provider fetched it
    int8_t   temp, hi, lo;    // °C
    uint8_t  humidity;        // %
    uint8_t  rain;            // chance of rain, %
    uint16_t code;            // OpenWeatherMap condition code
    uint16_t wind_dir;        // degrees
    float    wind_kmh;
    char     text[32];        // "Light rain"
    char     location[32];
    uint8_t  days;            // forecast days in day[], from tomorrow
    weather_day_t day[WEATHER_FORECAST_DAYS];
} weather_t;

void weather_init(void);
void weather_set(const weather_t *w);
/** Copy of the latest weather (w->valid false if there's none). */
void weather_get(weather_t *w);

/** Kinds of weather, for picking an icon. */
typedef enum {
    WEATHER_KIND_CLEAR, WEATHER_KIND_PARTLY, WEATHER_KIND_CLOUDY, WEATHER_KIND_RAIN,
    WEATHER_KIND_STORM, WEATHER_KIND_SNOW, WEATHER_KIND_FOG, WEATHER_KIND_COUNT
} weather_kind_t;
weather_kind_t weather_kind(uint16_t code);
