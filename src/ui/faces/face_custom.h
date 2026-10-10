#pragma once
/**
 * @file ui/faces/face_custom.h
 * @brief Watchfaces described by /data/faces/<id>/face.json (format in
 *        main/ui/faces/README.md).
 */

#include "ui/faces/watchface.h"

#define FACE_CUSTOM_MAX   16
#define FACES_DIR         "/data/faces"

/** Fill @p out with the custom faces on storage, sorted by id.  Returns the
 *  count.  Only name/description are read here; the layout is parsed when
 *  the face is shown. */
int face_custom_scan(watchface_t *out, int max);

/** Free what face_custom_scan() allocated. */
void face_custom_free(watchface_t *faces, int count);

/** Why the last custom face failed to load ("" if it loaded).  Problems that
 *  don't stop the face (e.g. a missing image) are reported here too. */
const char *face_custom_last_error(void);

/** True if @p id is a valid face id: 1–23 of a-z 0-9 _ - */
bool face_custom_valid_id(const char *id);

/** Remove /data/faces/<id> and its files.  The face must not be on screen
 *  (use screen_watchface_delete()). */
bool face_custom_remove(const char *id);
