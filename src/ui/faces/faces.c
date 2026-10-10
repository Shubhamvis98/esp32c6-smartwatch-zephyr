/**
 * @file ui/faces/faces.c
 * @brief Watchface registry: built-in code faces, then custom faces found in
 *        /data/faces.
 *
 * To add a code face, write face_<name>.c defining a `const watchface_t`,
 * add it to SRCS in main/CMakeLists.txt and to s_builtin[] below.
 */

#include "ui/faces/watchface.h"
#include "ui/faces/face_custom.h"

#include <string.h>

extern const watchface_t g_face_digital;

/* The first entry is the default and the fallback when a face fails */
static const watchface_t *const s_builtin[] = {
    &g_face_digital,
};
#define BUILTIN_COUNT ((int)(sizeof(s_builtin) / sizeof(s_builtin[0])))

static watchface_t s_custom[FACE_CUSTOM_MAX];
static int         s_custom_count;
static bool        s_scanned;

void watchface_registry_scan(void)
{
    face_custom_free(s_custom, s_custom_count);
    s_custom_count = face_custom_scan(s_custom, FACE_CUSTOM_MAX);
    s_scanned = true;
}

int watchface_count(void)
{
    if (!s_scanned) watchface_registry_scan();
    return BUILTIN_COUNT + s_custom_count;
}

const watchface_t *watchface_get(int index)
{
    if (index < 0 || index >= watchface_count()) return NULL;
    return index < BUILTIN_COUNT ? s_builtin[index] : &s_custom[index - BUILTIN_COUNT];
}

const watchface_t *watchface_find(const char *id)
{
    for (int i = 0; i < watchface_count(); i++) {
        const watchface_t *f = watchface_get(i);
        if (strcmp(f->id, id) == 0) return f;
    }
    return NULL;
}

bool watchface_is_builtin(const watchface_t *face)
{
    for (int i = 0; i < BUILTIN_COUNT; i++) {
        if (s_builtin[i] == face) return true;
    }
    return false;
}

bool watchface_is_custom(const watchface_t *face)
{
    return face >= s_custom && face < s_custom + s_custom_count;
}
