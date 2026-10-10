/**
 * @file app/contacts.c
 * @brief Favourite contacts and recent calls, saved in /data.
 */

#include "app/contacts.h"
#include "hal/storage.h"
#include "hal/rtc.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "contacts";

#define CONTACTS_FILE   STORAGE_PATH "/contacts.bin"
#define RECENTS_FILE    STORAGE_PATH "/recents.bin"
#define FILE_MAGIC      0x4354              // "CT"
#define FILE_VERSION    1

typedef struct {
    uint16_t magic, version, count, size;
} file_header_t;

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static contact_t     s_contacts[CONTACTS_MAX];
static int           s_contact_count;
static recent_call_t s_recents[RECENTS_MAX];
static int           s_recent_count;
static volatile uint32_t s_gen;

static void lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

static int load(const char *path, void *items, size_t size, int max)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    int n = 0;
    file_header_t h;
    if (fread(&h, sizeof(h), 1, f) == 1 && h.magic == FILE_MAGIC && h.version == FILE_VERSION &&
        h.size == size && h.count <= max) {
        n = (int)fread(items, size, h.count, f);
    }
    fclose(f);
    return n;
}

static void save(const char *path, const void *items, size_t size, int count)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGW(TAG, "Can't write %s", path);
        return;
    }
    file_header_t h = { FILE_MAGIC, FILE_VERSION, (uint16_t)count, (uint16_t)size };
    fwrite(&h, sizeof(h), 1, f);
    fwrite(items, size, count, f);
    fclose(f);
}

void contacts_init(void)
{
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    s_contact_count = load(CONTACTS_FILE, s_contacts, sizeof(contact_t), CONTACTS_MAX);
    s_recent_count = load(RECENTS_FILE, s_recents, sizeof(recent_call_t), RECENTS_MAX);
    for (int i = 0; i < s_contact_count; i++) {
        s_contacts[i].name[CONTACT_NAME_LEN - 1] = 0;
        s_contacts[i].number[CONTACT_NUMBER_LEN - 1] = 0;
    }
    for (int i = 0; i < s_recent_count; i++) {
        s_recents[i].name[CONTACT_NAME_LEN - 1] = 0;
        s_recents[i].number[CONTACT_NUMBER_LEN - 1] = 0;
    }
    ESP_LOGI(TAG, "%d contacts, %d recent calls", s_contact_count, s_recent_count);
}

static int name_cmp(const void *a, const void *b)
{
    return strcasecmp(((const contact_t *)a)->name, ((const contact_t *)b)->name);
}

void contacts_set(const contact_t *list, int count)
{
    if (count > CONTACTS_MAX) count = CONTACTS_MAX;
    lock();
    bool same = count == s_contact_count &&
                memcmp(list, s_contacts, sizeof(contact_t) * count) == 0;
    if (!same) {
        memcpy(s_contacts, list, sizeof(contact_t) * count);
        s_contact_count = count;
        qsort(s_contacts, count, sizeof(contact_t), name_cmp);
        save(CONTACTS_FILE, s_contacts, sizeof(contact_t), s_contact_count);
        s_gen++;
    }
    unlock();
    if (!same) ESP_LOGI(TAG, "Got %d contacts", count);
}

int contacts_get(contact_t *out, int max)
{
    lock();
    int n = s_contact_count < max ? s_contact_count : max;
    memcpy(out, s_contacts, sizeof(contact_t) * n);
    unlock();
    return n;
}

uint32_t contacts_generation(void) { return s_gen; }

/* Compare numbers by their digits only, so "+1 555-0199" matches "15550199"
 * and a local "05550199" matches "+15550199" on the last 8 digits */
static bool same_number(const char *a, const char *b)
{
    char da[CONTACT_NUMBER_LEN], db[CONTACT_NUMBER_LEN];
    int na = 0, nb = 0;
    for (; *a && na < CONTACT_NUMBER_LEN - 1; a++) if (isdigit((unsigned char)*a)) da[na++] = *a;
    for (; *b && nb < CONTACT_NUMBER_LEN - 1; b++) if (isdigit((unsigned char)*b)) db[nb++] = *b;
    if (na == nb) return na > 0 && memcmp(da, db, na) == 0;
    int n = na < nb ? na : nb;
    if (n < 7) return false;                 // too short to tell apart
    if (n > 8) n = 8;
    return memcmp(da + na - n, db + nb - n, n) == 0;
}

bool contacts_name_for(const char *number, char *name, size_t len)
{
    bool found = false;
    lock();
    for (int i = 0; i < s_contact_count && !found; i++) {
        if (same_number(number, s_contacts[i].number)) {
            snprintf(name, len, "%s", s_contacts[i].name);
            found = true;
        }
    }
    unlock();
    return found;
}

void recents_add(const char *name, const char *number, bool outgoing)
{
    if (!*number && !*name) return;
    recent_call_t r = { .outgoing = outgoing, .when = rtc_time_valid() ? time(NULL) : 0 };
    snprintf(r.number, sizeof(r.number), "%s", number);
    if (*name) snprintf(r.name, sizeof(r.name), "%s", name);
    else       contacts_name_for(number, r.name, sizeof(r.name));

    lock();
    /* The phone can report one call twice (ringing again): don't list it twice */
    bool dup = s_recent_count && s_recents[0].outgoing == outgoing &&
               strcmp(s_recents[0].number, r.number) == 0 &&
               (!r.when || !s_recents[0].when || r.when - s_recents[0].when < 60);
    if (dup) {
        s_recents[0].when = r.when;
    } else {
        int n = s_recent_count < RECENTS_MAX ? s_recent_count : RECENTS_MAX - 1;
        memmove(&s_recents[1], &s_recents[0], sizeof(recent_call_t) * n);
        s_recents[0] = r;
        s_recent_count = n + 1;
    }
    save(RECENTS_FILE, s_recents, sizeof(recent_call_t), s_recent_count);
    s_gen++;
    unlock();
}

int recents_get(recent_call_t *out, int max)
{
    lock();
    int n = s_recent_count < max ? s_recent_count : max;
    memcpy(out, s_recents, sizeof(recent_call_t) * n);
    unlock();
    return n;
}

void recents_clear(void)
{
    lock();
    s_recent_count = 0;
    remove(RECENTS_FILE);
    s_gen++;
    unlock();
}
