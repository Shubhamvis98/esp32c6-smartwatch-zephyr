/**
 * @file ui/screen_watchface.c
 * @brief Watchface tile: hosts the selected watchface module (ui/faces/).
 *
 * Only the selected face is built.  Switching faces (Settings → Watchface)
 * or uploading new ones rebuilds it.
 */

#include "ui/ui_internal.h"
#include "ui/faces/watchface.h"
#include "ui/faces/face_custom.h"
#include "hal/battery.h"
#include "hal/rtc.h"
#include "app/activity.h"
#include "app/settings.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include <string.h>
#include <time.h>

static const char *TAG = "watchface";

static lv_obj_t          *s_tile;
static lv_obj_t          *s_root;           // the active face's widgets
static const watchface_t *s_face;

/* ── Face lifecycle ──────────────────────────────────────────────────────── */
static void destroy_face(void)
{
    if (s_root) lv_obj_delete(s_root);
    s_root = NULL;
    if (s_face && s_face->destroy) s_face->destroy(s_face);
    s_face = NULL;
}

static bool build_face(const watchface_t *face)
{
    s_root = face_make_layer(s_tile);
    s_face = face;
    if (face->create(face, s_root)) return true;
    ESP_LOGW(TAG, "Face \"%s\" failed to load", face->id);
    destroy_face();
    return false;
}

/* Build the face saved in settings, falling back to the first built-in one */
static void show_selected(void)
{
    destroy_face();
    const watchface_t *face = watchface_find(settings_get()->face_id);
    if (!face || !build_face(face)) build_face(watchface_get(0));
    screen_watchface_update();
}

/* ── Tile ────────────────────────────────────────────────────────────────── */
void screen_watchface_create(lv_obj_t *parent)
{
    s_tile = parent;
    lv_obj_set_style_bg_color(parent, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    show_selected();
}

void screen_watchface_apply_style(void)
{
    show_selected();
}

void screen_watchface_reload_faces(void)
{
    destroy_face();               // custom face pointers die with the rescan
    watchface_registry_scan();
    show_selected();
}

const watchface_t *screen_watchface_active(void)
{
    return s_face;
}

void screen_watchface_update(void)
{
    if (!s_face) return;

    struct tm tm;
    face_data_t d = {
        .use_24h   = settings_get()->use_24h,
        .steps     = activity_get_steps(),
        .step_goal = settings_get()->step_goal,
    };
    if (rtc_time_valid()) {
        time_t now = time(NULL);
        localtime_r(&now, &tm);
        d.tm = &tm;
    }
    int mv = ui_battery_mv();
    d.battery_pct = battery_percent_from_mv(mv);
    d.charging = battery_mv_is_charging(mv);

    s_face->update(s_face, &d);
}

bool screen_watchface_delete(const char *id)
{
    char deleting[FACE_ID_MAX];
    strlcpy(deleting, id, sizeof(deleting));      // id may point into the registry

    /* Nothing may read the face's files while they go */
    watch_settings_t *cfg = settings_get();
    bool was_selected = strcmp(cfg->face_id, deleting) == 0;
    destroy_face();
    bool ok = face_custom_remove(deleting);

    if (was_selected) {
        strlcpy(cfg->face_id, watchface_get(0)->id, sizeof(cfg->face_id));
        watch_settings_save();
    }
    watchface_registry_scan();
    show_selected();
    return ok;
}
