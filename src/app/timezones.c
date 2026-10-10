/**
 * @file app/timezones.c
 * @brief Timezone list and automatic detection (see timezones.h).
 */

#include "app/timezones.h"
#include "app/settings.h"
#include "lvgl_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "timezones";

#define LOOKUP_URL        "http://ip-api.com/json/?fields=status,timezone,offset"
#define LOOKUP_TIMEOUT_MS 5000

static const tz_entry_t s_zones[] = {
    { "-10  Honolulu",          "Honolulu",     "HST10",
      "Pacific/Honolulu" },
    { "-9   Anchorage",         "Anchorage",    "AKST9AKDT,M3.2.0,M11.1.0",
      "America/Anchorage America/Juneau America/Nome America/Sitka" },
    { "-8   Los Angeles",       "Los Angeles",  "PST8PDT,M3.2.0,M11.1.0",
      "America/Los_Angeles America/Vancouver America/Tijuana" },
    { "-7   Denver",            "Denver",       "MST7MDT,M3.2.0,M11.1.0",
      "America/Denver America/Edmonton America/Boise" },
    { "-7   Phoenix",           "Phoenix",      "MST7",
      "America/Phoenix" },
    { "-6   Chicago",           "Chicago",      "CST6CDT,M3.2.0,M11.1.0",
      "America/Chicago America/Winnipeg America/Indiana/Knox America/Menominee" },
    { "-6   Mexico City",       "Mexico City",  "CST6",
      "America/Mexico_City America/Monterrey America/Guatemala America/Costa_Rica "
      "America/El_Salvador America/Regina" },
    { "-5   New York",          "New York",     "EST5EDT,M3.2.0,M11.1.0",
      "America/New_York America/Toronto America/Detroit America/Indiana/Indianapolis "
      "America/Kentucky/Louisville America/Nassau" },
    { "-5   Bogota / Lima",     "Bogota",       "<-05>5",
      "America/Bogota America/Lima America/Guayaquil America/Panama America/Jamaica "
      "America/Cancun" },
    { "-4   Santiago",          "Santiago",     "<-04>4<-03>,M9.1.6/24,M4.1.6/24",
      "America/Santiago" },
    { "-4   Halifax",           "Halifax",      "AST4ADT,M3.2.0,M11.1.0",
      "America/Halifax Atlantic/Bermuda" },
    { "-4   Caracas / La Paz",  "Caracas",      "<-04>4",
      "America/Caracas America/La_Paz America/Santo_Domingo America/Puerto_Rico "
      "America/Manaus" },
    { "-3   Sao Paulo",         "Sao Paulo",    "<-03>3",
      "America/Sao_Paulo America/Montevideo America/Fortaleza America/Recife" },
    { "-3   Buenos Aires",      "Buenos Aires", "<-03>3",
      "America/Argentina/Buenos_Aires America/Argentina/Cordoba" },
    { "+0   UTC",               "UTC",          "UTC0",
      "UTC Etc/UTC" },
    { "+0   London / Dublin",   "London",       "GMT0BST,M3.5.0/1,M10.5.0",
      "Europe/London Europe/Dublin Europe/Lisbon Atlantic/Canary Atlantic/Madeira "
      "Europe/Guernsey Europe/Jersey Europe/Isle_of_Man" },
    { "+0   Accra / Dakar",     "Accra",        "GMT0",
      "Africa/Accra Africa/Abidjan Africa/Dakar Atlantic/Reykjavik" },
    { "+1   Lagos",             "Lagos",        "WAT-1",
      "Africa/Lagos Africa/Kinshasa Africa/Luanda Africa/Algiers Africa/Tunis "
      "Africa/Casablanca" },
    { "+1   Paris / Berlin",    "Paris",        "CET-1CEST,M3.5.0,M10.5.0/3",
      "Europe/Paris Europe/Berlin Europe/Madrid Europe/Rome Europe/Amsterdam "
      "Europe/Brussels Europe/Vienna Europe/Zurich Europe/Stockholm Europe/Oslo "
      "Europe/Copenhagen Europe/Warsaw Europe/Prague Europe/Budapest Europe/Belgrade "
      "Europe/Zagreb Europe/Ljubljana Europe/Bratislava Europe/Luxembourg Europe/Monaco "
      "Europe/Malta Europe/Andorra Europe/Tirane Europe/Sarajevo Europe/Skopje "
      "Europe/Podgorica Europe/Vaduz Europe/San_Marino Europe/Vatican Europe/Gibraltar "
      "Africa/Ceuta Arctic/Longyearbyen" },
    { "+2   Johannesburg",      "Johannesburg", "SAST-2",
      "Africa/Johannesburg Africa/Harare Africa/Maputo Africa/Lusaka Africa/Kigali "
      "Africa/Tripoli Europe/Kaliningrad" },
    { "+2   Cairo",             "Cairo",        "EET-2EEST,M4.5.5/0,M10.5.4/24",
      "Africa/Cairo" },
    { "+2   Athens / Kyiv",     "Athens",       "EET-2EEST,M3.5.0/3,M10.5.0/4",
      "Europe/Athens Europe/Kyiv Europe/Kiev Europe/Bucharest Europe/Sofia "
      "Europe/Helsinki Europe/Riga Europe/Tallinn Europe/Vilnius Europe/Chisinau "
      "Asia/Nicosia Europe/Mariehamn" },
    { "+2   Jerusalem",         "Jerusalem",    "IST-2IDT,M3.4.4/26,M10.5.0",
      "Asia/Jerusalem Asia/Tel_Aviv" },
    { "+3   Istanbul",          "Istanbul",     "<+03>-3",
      "Europe/Istanbul Europe/Minsk Asia/Amman Asia/Damascus" },
    { "+3   Moscow",            "Moscow",       "MSK-3",
      "Europe/Moscow Europe/Simferopol Europe/Volgograd" },
    { "+3   Riyadh / Nairobi",  "Riyadh",       "<+03>-3",
      "Asia/Riyadh Asia/Kuwait Asia/Qatar Asia/Bahrain Asia/Baghdad Asia/Aden "
      "Africa/Nairobi Africa/Addis_Ababa Africa/Dar_es_Salaam Africa/Kampala" },
    { "+3:30 Tehran",           "Tehran",       "<+0330>-3:30",
      "Asia/Tehran" },
    { "+4   Dubai",             "Dubai",        "<+04>-4",
      "Asia/Dubai Asia/Muscat Asia/Baku Asia/Tbilisi Asia/Yerevan Indian/Mauritius "
      "Europe/Samara" },
    { "+4:30 Kabul",            "Kabul",        "<+0430>-4:30",
      "Asia/Kabul" },
    { "+5   Karachi",           "Karachi",      "PKT-5",
      "Asia/Karachi Asia/Tashkent Asia/Yekaterinburg Indian/Maldives" },
    { "+5:30 India",            "India",        "IST-5:30",
      "Asia/Kolkata Asia/Calcutta Asia/Colombo" },
    { "+5:45 Kathmandu",        "Kathmandu",    "<+0545>-5:45",
      "Asia/Kathmandu Asia/Katmandu" },
    { "+6   Dhaka",             "Dhaka",        "<+06>-6",
      "Asia/Dhaka Asia/Thimphu Asia/Bishkek Asia/Almaty Asia/Omsk" },
    { "+6:30 Yangon",           "Yangon",       "<+0630>-6:30",
      "Asia/Yangon Asia/Rangoon" },
    { "+7   Bangkok / Jakarta", "Bangkok",      "<+07>-7",
      "Asia/Bangkok Asia/Jakarta Asia/Ho_Chi_Minh Asia/Saigon Asia/Phnom_Penh "
      "Asia/Vientiane Asia/Novosibirsk Asia/Krasnoyarsk" },
    { "+8   Singapore / China", "Singapore",    "CST-8",
      "Asia/Singapore Asia/Shanghai Asia/Hong_Kong Asia/Taipei Asia/Kuala_Lumpur "
      "Asia/Manila Asia/Makassar Asia/Macau Asia/Irkutsk Asia/Ulaanbaatar" },
    { "+8   Perth",             "Perth",        "AWST-8",
      "Australia/Perth" },
    { "+9   Tokyo / Seoul",     "Tokyo",        "JST-9",
      "Asia/Tokyo Asia/Seoul Asia/Pyongyang Asia/Jayapura Asia/Yakutsk Pacific/Palau" },
    { "+9:30 Adelaide",         "Adelaide",     "ACST-9:30ACDT,M10.1.0,M4.1.0/3",
      "Australia/Adelaide Australia/Broken_Hill" },
    { "+9:30 Darwin",           "Darwin",       "ACST-9:30",
      "Australia/Darwin" },
    { "+10  Brisbane",          "Brisbane",     "AEST-10",
      "Australia/Brisbane Pacific/Guam Pacific/Port_Moresby Asia/Vladivostok" },
    { "+10  Sydney",            "Sydney",       "AEST-10AEDT,M10.1.0,M4.1.0/3",
      "Australia/Sydney Australia/Melbourne Australia/Hobart Australia/Canberra" },
    { "+12  Auckland",          "Auckland",     "NZST-12NZDT,M9.5.0,M4.1.0/3",
      "Pacific/Auckland Antarctica/McMurdo" },
    { "+12  Fiji",              "Fiji",         "<+12>-12",
      "Pacific/Fiji Asia/Kamchatka Pacific/Tarawa Pacific/Majuro" },
};
#define ZONE_COUNT ((int)(sizeof(s_zones) / sizeof(s_zones[0])))

int tz_count(void)               { return ZONE_COUNT; }
const tz_entry_t *tz_get(int i)  { return i >= 0 && i < ZONE_COUNT ? &s_zones[i] : NULL; }

/* Whole-word match of @p name in a space-separated list */
static bool list_has(const char *list, const char *name)
{
    size_t n = strlen(name);
    for (const char *p = list; (p = strstr(p, name)); p += n) {
        bool start = p == list || p[-1] == ' ';
        bool end = p[n] == 0 || p[n] == ' ';
        if (start && end) return true;
    }
    return false;
}

static int find_iana(const char *iana)
{
    if (!iana[0]) return -1;
    for (int i = 0; i < ZONE_COUNT; i++) {
        if (list_has(s_zones[i].iana, iana)) return i;
    }
    return -1;
}

int tz_current_index(void)
{
    const watch_settings_t *cfg = settings_get();
    int i = find_iana(cfg->tz_name);
    if (i >= 0 && strcmp(s_zones[i].tz, cfg->tz) == 0) return i;
    if (cfg->tz_name[0]) return -1;     // detected zone outside the list: show its own city
    for (i = 0; i < ZONE_COUNT; i++) {
        if (strcmp(s_zones[i].tz, cfg->tz) == 0) return i;
    }
    return -1;
}

/* "<+0530>-5:30" from an offset east of UTC */
static void fixed_posix(char *buf, size_t len, int32_t offset_s)
{
    char sign = offset_s < 0 ? '-' : '+';
    int32_t a = abs(offset_s);
    int h = a / 3600, m = a % 3600 / 60;
    /* POSIX offsets are west of UTC, so the sign flips */
    if (m) snprintf(buf, len, "<%c%02d%02d>%c%d:%02d", sign, h, m, sign == '+' ? '-' : '+', h, m);
    else   snprintf(buf, len, "<%c%02d>%c%d", sign, h, sign == '+' ? '-' : '+', h);
}

void tz_current_name(char *buf, size_t len)
{
    const watch_settings_t *cfg = settings_get();
    int i = tz_current_index();
    if (i >= 0) {
        strlcpy(buf, s_zones[i].name, len);
        return;
    }
    if (cfg->tz_name[0]) {
        /* Auto-detected zone outside the list: "+4  Tbilisi" from "<+04>-4" */
        const char *city = strrchr(cfg->tz_name, '/');
        city = city ? city + 1 : cfg->tz_name;
        char off[8] = "";
        if (cfg->tz[0] == '<') sscanf(cfg->tz + 1, "%7[^>]", off);
        snprintf(buf, len, "%s  %s", off[0] ? off : "", city);
        for (char *p = buf; *p; p++) if (*p == '_') *p = ' ';
        return;
    }
    strlcpy(buf, "Custom", len);
}

void tz_select(int index)
{
    if (index < 0 || index >= ZONE_COUNT) return;
    const tz_entry_t *z = &s_zones[index];
    watch_settings_t *cfg = settings_get();
    size_t n = strcspn(z->iana, " ");
    if (n >= sizeof(cfg->tz_name)) n = sizeof(cfg->tz_name) - 1;
    memcpy(cfg->tz_name, z->iana, n);
    cfg->tz_name[n] = 0;
    settings_set_timezone(z->tz);          // saves
}

/* ── Automatic detection ─────────────────────────────────────────────────── */
static bool lookup(char *iana, size_t len, int32_t *offset_s)
{
    char body[160];
    esp_http_client_config_t cfg = {
        .url = LOOKUP_URL,
        .timeout_ms = LOOKUP_TIMEOUT_MS,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) return false;

    int n = -1, status = 0;
    if (esp_http_client_open(h, 0) == ESP_OK) {
        esp_http_client_fetch_headers(h);
        status = esp_http_client_get_status_code(h);
        n = esp_http_client_read_response(h, body, sizeof(body) - 1);
    }
    esp_http_client_cleanup(h);
    if (status != 200 || n <= 0) {
        ESP_LOGW(TAG, "Lookup failed (HTTP %d)", status);
        return false;
    }
    body[n] = 0;

    cJSON *json = cJSON_Parse(body);
    const cJSON *st = cJSON_GetObjectItem(json, "status");
    const cJSON *tz = cJSON_GetObjectItem(json, "timezone");
    const cJSON *off = cJSON_GetObjectItem(json, "offset");
    bool ok = cJSON_IsString(st) && strcmp(st->valuestring, "success") == 0 &&
              cJSON_IsString(tz) && cJSON_IsNumber(off);
    if (ok) {
        strlcpy(iana, tz->valuestring, len);
        *offset_s = (int32_t)off->valuedouble;
    } else {
        ESP_LOGW(TAG, "Unexpected reply: %s", body);
    }
    cJSON_Delete(json);
    return ok;
}

bool tz_auto_detect(void)
{
    if (!settings_get()->auto_tz) return false;

    char iana[32];
    int32_t offset_s;
    if (!lookup(iana, sizeof(iana), &offset_s)) return false;

    char posix[48];
    int i = find_iana(iana);
    if (i >= 0) strlcpy(posix, s_zones[i].tz, sizeof(posix));
    else        fixed_posix(posix, sizeof(posix), offset_s);   // current offset, DST included

    /* The LVGL task reads the timezone (localtime) all the time */
    lvgl_lock();
    watch_settings_t *cfg = settings_get();
    bool changed = cfg->auto_tz &&
                   (strcmp(cfg->tz, posix) != 0 || strcmp(cfg->tz_name, iana) != 0);
    if (changed) {
        strlcpy(cfg->tz_name, iana, sizeof(cfg->tz_name));
        settings_set_timezone(posix);      // saves
    }
    lvgl_unlock();

    ESP_LOGI(TAG, "%s (%s)%s", iana, posix, changed ? ", applied" : "");
    return true;
}
