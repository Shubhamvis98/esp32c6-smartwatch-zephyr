/**
 * @file ui/screen_recorder.c
 * @brief Voice recorder page.
 *
 *   Voice recorder
 *        ( ● )          ← tap to record / stop
 *        00:12
 *   ▮▮▮▮▮▮▯▯▯▯          ← input level
 *   5:21 left
 *   R0003  0:12  14:05   [▶] [🗑]
 *   R0002  1:03  Mon     [⏸] [🗑]   ← playing: tap to pause / resume
 *
 * Recording continues in the background if the page is closed; the
 * button then shows the running time when the page is reopened.
 */

#include "ui/ui_internal.h"
#include "app/recorder.h"
#include "app/settings.h"
#include "hal/audio.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define UI_RATE_MS   100

static lv_obj_t   *s_btn;
static lv_obj_t   *s_btn_icon;
static lv_obj_t   *s_time;
static lv_obj_t   *s_level;
static lv_obj_t   *s_left;
static lv_obj_t   *s_list;
static lv_timer_t *s_timer;
static bool        s_was_recording;

static rec_info_t  s_files[RECORDER_MAX_FILES];
static lv_obj_t   *s_play_icons[RECORDER_MAX_FILES];
static int         s_file_count;
static char        s_playing[sizeof(s_files[0].name)];   // clip being played, "" if none

static void build_list(void);

static void fmt_duration(char *buf, size_t len, uint32_t ms)
{
    uint32_t s = ms / 1000;
    snprintf(buf, len, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

/* ── Live state ──────────────────────────────────────────────────────────── */
/* Pause icon on the clip that is playing, play icon everywhere else */
static void update_play_icons(void)
{
    if (s_playing[0] && !audio_is_playing()) s_playing[0] = '\0';    // clip ended

    for (int i = 0; i < s_file_count; i++) {
        bool active = s_playing[0] && strcmp(s_files[i].name, s_playing) == 0 &&
                      !audio_is_paused();
        const char *icon = active ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY;
        if (strcmp(lv_label_get_text(s_play_icons[i]), icon) != 0) {
            lv_label_set_text(s_play_icons[i], icon);
        }
    }
}

static void refresh(void)
{
    bool rec = recorder_is_recording();

    lv_label_set_text(s_btn_icon, rec ? LV_SYMBOL_STOP : "");
    lv_obj_set_style_radius(s_btn_icon, rec ? 0 : LV_RADIUS_CIRCLE, 0);

    char buf[16];
    fmt_duration(buf, sizeof(buf), recorder_elapsed_ms());
    lv_label_set_text(s_time, rec ? buf : "Tap to record");
    lv_obj_set_style_text_color(s_time, rec ? UI_COLOR_DANGER : UI_COLOR_DIM, 0);

    /* Level: −60 dBFS … 0 dBFS */
    float db = audio_mic_active() ? audio_mic_rms_dbfs() : -96.0f;
    int v = (int)(db + 60.0f);
    lv_bar_set_value(s_level, v < 0 ? 0 : v > 60 ? 60 : v, LV_ANIM_OFF);

    uint32_t left = recorder_seconds_left();
    lv_label_set_text_fmt(s_left, "%lu:%02lu of recording space left",
                          (unsigned long)(left / 60), (unsigned long)(left % 60));

    if (s_was_recording && !rec) build_list();      // finished (or hit a limit)
    s_was_recording = rec;
    update_play_icons();
}

static void timer_cb(lv_timer_t *t)
{
    refresh();
}

static void page_deleted_cb(lv_event_t *e)
{
    if (s_timer) lv_timer_delete(s_timer);
    s_timer = NULL;
    if (s_playing[0] && audio_is_paused()) audio_stop();   // no way back to resume it
    s_playing[0] = '\0';
}

/* ── Actions ─────────────────────────────────────────────────────────────── */
static void record_cb(lv_event_t *e)
{
    if (recorder_is_recording()) {
        recorder_stop();
    } else if (recorder_start() != ESP_OK) {
        lv_label_set_text(s_time, "Can't record");
        return;
    }
    refresh();
    build_list();
}

static void play_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_file_count) return;
    if (s_playing[0] && strcmp(s_files[i].name, s_playing) == 0 && audio_is_playing()) {
        audio_pause(!audio_is_paused());
    } else {
        recorder_play(s_files[i].name);              // replaces any other clip
        strlcpy(s_playing, s_files[i].name, sizeof(s_playing));
    }
    update_play_icons();
}

static void delete_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_file_count) return;
    if (strcmp(s_files[i].name, s_playing) == 0) s_playing[0] = '\0';
    recorder_delete(s_files[i].name);
    build_list();
    refresh();
}

/* ── Recording list ──────────────────────────────────────────────────────── */
static lv_obj_t *icon_button(lv_obj_t *parent, const char *icon, lv_color_t color,
                             lv_event_cb_t cb, int index)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 34, 34);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    ui_fill(b, color);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, icon);
    lv_obj_center(l);
    return b;
}

static void build_list(void)
{
    lv_obj_clean(s_list);
    s_file_count = recorder_list(s_files, RECORDER_MAX_FILES);

    if (!s_file_count) {
        lv_obj_t *l = lv_label_create(s_list);
        lv_label_set_text(l, "No recordings yet");
        lv_obj_set_style_text_color(l, UI_COLOR_DIM, 0);
        return;
    }

    time_t now = time(NULL);
    for (int i = 0; i < s_file_count; i++) {
        const rec_info_t *r = &s_files[i];

        lv_obj_t *row = lv_obj_create(s_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), 48);
        lv_obj_set_style_bg_color(row, UI_COLOR_SURFACE, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_pad_hor(row, 8, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 6, 0);
        lv_obj_set_scrollable(row, false);

        /* "#3  0:12" over "Today 14:05" */
        lv_obj_t *txt = lv_obj_create(row);
        lv_obj_remove_style_all(txt);
        lv_obj_set_height(txt, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(txt, 1);
        lv_obj_set_flex_flow(txt, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_clickable(txt, false);

        int num = 0;
        sscanf(r->name, "R%4d", &num);
        char dur[12];
        fmt_duration(dur, sizeof(dur), r->duration_ms);
        lv_obj_t *l = lv_label_create(txt);
        lv_label_set_text_fmt(l, "#%d  %s", num, dur);

        lv_obj_t *when = lv_label_create(txt);
        lv_obj_set_style_text_font(when, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(when, UI_COLOR_DIM, 0);
        if (r->created) {
            struct tm tm, today;
            localtime_r(&r->created, &tm);
            localtime_r(&now, &today);
            char buf[24];
            if (tm.tm_yday == today.tm_yday && tm.tm_year == today.tm_year) {
                strftime(buf, sizeof(buf), settings_get()->use_24h ? "Today %H:%M" : "Today %I:%M %p", &tm);
            } else {
                strftime(buf, sizeof(buf), "%d %b %H:%M", &tm);
            }
            lv_label_set_text(when, buf);
        } else {
            lv_label_set_text(when, "");
        }

        lv_obj_t *play = icon_button(row, LV_SYMBOL_PLAY, UI_COLOR_ACCENT, play_cb, i);
        s_play_icons[i] = lv_obj_get_child(play, 0);
        icon_button(row, LV_SYMBOL_TRASH, UI_COLOR_RAISED, delete_cb, i);
    }
}

/* ── Page ────────────────────────────────────────────────────────────────── */
void screen_recorder_open(void)
{
    lv_obj_t *c = ui_page_open("Recorder", NULL);
    lv_obj_add_event_cb(c, page_deleted_cb, LV_EVENT_DELETE, NULL);

    /* Big round record button with a red dot / stop square inside */
    s_btn = lv_button_create(c);
    lv_obj_set_size(s_btn, 72, 72);
    lv_obj_set_style_radius(s_btn, LV_RADIUS_CIRCLE, 0);
    ui_fill(s_btn, UI_COLOR_SURFACE);
    lv_obj_set_style_border_width(s_btn, 3, 0);
    lv_obj_set_style_border_color(s_btn, UI_COLOR_TEXT, 0);
    lv_obj_add_event_cb(s_btn, record_cb, LV_EVENT_CLICKED, NULL);

    s_btn_icon = lv_label_create(s_btn);
    lv_obj_set_size(s_btn_icon, 30, 30);
    ui_fill(s_btn_icon, UI_COLOR_DANGER);
    lv_obj_set_style_bg_opa(s_btn_icon, LV_OPA_COVER, 0);
    lv_obj_set_style_text_opa(s_btn_icon, LV_OPA_TRANSP, 0);
    lv_obj_center(s_btn_icon);

    s_time = lv_label_create(c);
    lv_obj_set_style_text_font(s_time, &lv_font_montserrat_20, 0);

    s_level = lv_bar_create(c);
    lv_obj_set_size(s_level, LV_PCT(90), 8);
    lv_bar_set_range(s_level, 0, 60);
    lv_obj_set_style_bg_color(s_level, UI_COLOR_RAISED, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_level, UI_COLOR_ACCENT, LV_PART_INDICATOR);

    s_left = lv_label_create(c);
    lv_obj_set_style_text_font(s_left, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_left, UI_COLOR_DIM, 0);

    s_list = lv_obj_create(c);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 6, 0);
    lv_obj_set_scrollable(s_list, false);

    s_was_recording = recorder_is_recording();
    build_list();
    refresh();
    s_timer = lv_timer_create(timer_cb, UI_RATE_MS, NULL);
}
