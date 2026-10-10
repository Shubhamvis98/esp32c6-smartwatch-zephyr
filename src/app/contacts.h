#pragma once
/**
 * @file app/contacts.h
 * @brief Favourite contacts (sent by Gadgetbridge) and recent calls, for the
 *        Phone app.  Both are kept in /data so they survive a restart.
 *
 * Favourites come from Gadgetbridge → device settings → Contacts, which
 * sends the whole list whenever it changes and at each connection.  Recent
 * calls are recorded on the watch: incoming calls the phone reports, and
 * calls placed from the watch.
 *
 * Safe to call from any task.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define CONTACTS_MAX        20      // = C6WATCH_CONTACT_SLOTS in Gadgetbridge
#define RECENTS_MAX         15
#define CONTACT_NAME_LEN    28      // UTF-8 bytes
#define CONTACT_NUMBER_LEN  24

typedef struct {
    char name[CONTACT_NAME_LEN];
    char number[CONTACT_NUMBER_LEN];
} contact_t;

typedef struct {
    char   name[CONTACT_NAME_LEN];  // "" if the phone sent none
    char   number[CONTACT_NUMBER_LEN];
    time_t when;                    // 0 if the clock wasn't set
    bool   outgoing;                // placed from the watch
} recent_call_t;

void contacts_init(void);

/** Replace the favourites (from Gadgetbridge). */
void contacts_set(const contact_t *list, int count);
/** Copy up to @p max favourites, A–Z; returns how many. */
int  contacts_get(contact_t *out, int max);
/** Changes whenever the favourites or the recent calls change. */
uint32_t contacts_generation(void);

/** Add a call at the top of the recent list (merged with an identical top entry). */
void recents_add(const char *name, const char *number, bool outgoing);
/** Copy up to @p max recent calls, newest first; returns how many. */
int  recents_get(recent_call_t *out, int max);
void recents_clear(void);

/** The favourite's name for @p number, or NULL. */
bool contacts_name_for(const char *number, char *name, size_t len);
