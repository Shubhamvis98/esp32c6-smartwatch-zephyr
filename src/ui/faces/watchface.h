#pragma once
/**
 * @file ui/faces/watchface.h
 * @brief Watchface module interface.
 *
 * A watchface is a watchface_t: it builds its widgets inside a full-screen
 * root object and refreshes them once a second from a face_data_t.  Only the
 * selected face exists; switching faces deletes the old root and builds the
 * new one.
 *
 * Two kinds of faces:
 *   - code faces (face_*.c): listed in s_builtin[] in faces.c
 *   - custom faces: /data/faces/<id>/face.json, uploaded over Wi-Fi
 *     (face_custom.c, app/face_server.c)
 *
 * See main/ui/faces/README.md for how to add either kind.
 */

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "lvgl.h"

#define FACE_ID_MAX         24          // including the terminator

/** Everything a face may show; filled in by the host once a second. */
typedef struct {
    const struct tm *tm;        // local time, NULL until the clock has been set
    bool     use_24h;
    uint32_t steps;
    uint32_t step_goal;
    uint8_t  battery_pct;
    bool     charging;
} face_data_t;

typedef struct watchface watchface_t;

struct watchface {
    char        id[FACE_ID_MAX];    // stable; saved in settings
    const char *name;               // shown in Settings → Watchface
    const char *desc;

    /** Build the face inside @p root (full screen, transparent, not
     *  clickable).  Return false if it can't be shown; the host then falls
     *  back to the first built-in face. */
    bool (*create)(const watchface_t *face, lv_obj_t *root);

    /** Refresh the widgets; called right after create() and once a second. */
    void (*update)(const watchface_t *face, const face_data_t *d);

    /** Optional: release resources.  Called after root has been deleted. */
    void (*destroy)(const watchface_t *face);

    void *user;                     // module data
};

/* ── Registry (faces.c) ──────────────────────────────────────────────────── */
/** Number of faces: built-in first, then custom faces sorted by id. */
int watchface_count(void);
const watchface_t *watchface_get(int index);
const watchface_t *watchface_find(const char *id);
/** True for faces loaded from storage (these can be deleted). */
bool watchface_is_custom(const watchface_t *face);

bool watchface_is_builtin(const watchface_t *face);

/** Re-read /data/faces.  Invalidates custom watchface_t pointers, so only
 *  screen_watchface_reload_faces() calls it (it rebuilds the active face). */
void watchface_registry_scan(void);

/* ── Helpers for face modules (face_common.c) ────────────────────────────── */
extern const char *const g_face_days[7];     // "Sunday" …
extern const char *const g_face_months[12];  // "January" …

/** Hour for display, honouring the 12/24 h setting */
int face_display_hour(const face_data_t *d);

/** Transparent, non-clickable container so swipes reach the tileview */
lv_obj_t *face_make_layer(lv_obj_t *parent);
/** face_make_layer() sized to its content, laid out as a flex @p flow */
lv_obj_t *face_make_flex(lv_obj_t *parent, lv_flex_flow_t flow);
lv_obj_t *face_make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color);

/** Set label text only if it changed (avoids redrawing unchanged areas) */
void face_set_text(lv_obj_t *label, const char *text);
