/**
 * @file ui/screen_music.c
 * @brief Music tile (swipe left from Activity): what's playing on the phone
 *        and playback / volume controls, through Gadgetbridge.
 */

#include "ui/ui_internal.h"
#include "app/gadgetbridge.h"

#include <string.h>

static lv_obj_t *s_track;
static lv_obj_t *s_artist;
static lv_obj_t *s_play_icon;
static lv_obj_t *s_controls;     // dimmed while not connected

static void set_text_if_changed(lv_obj_t *label, const char *text)
{
    /* Re-setting the same text would restart the scrolling animation */
    if (strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

static void control_cb(lv_event_t *e)
{
    if (!gb_is_connected()) return;
    const char *cmd = lv_event_get_user_data(e);
    if (strcmp(cmd, "playpause") == 0) cmd = gb_music_playing() ? "pause" : "play";
    gb_send_music(cmd);
    screen_music_update();
}

static lv_obj_t *round_button(lv_obj_t *parent, const char *icon, int size, lv_color_t bg,
                              const char *cmd)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    ui_fill(btn, bg);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, control_cb, LV_EVENT_CLICKED, (void *)cmd);
    lv_obj_t *ic = lv_label_create(btn);
    lv_label_set_text(ic, icon);
    lv_obj_center(ic);
    return ic;
}

static lv_obj_t *button_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_clickable(row, false);
    return row;
}

void screen_music_create(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, LV_SYMBOL_AUDIO "  Music");
    lv_obj_set_style_text_color(title, UI_COLOR_DIM, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, UI_SAFE_MARGIN - 4);

    /* Track (scrolls if long) and artist */
    s_track = lv_label_create(parent);
    lv_obj_set_width(s_track, 200);
    lv_label_set_long_mode(s_track, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_style_text_font(s_track, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(s_track, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_track, "");
    lv_obj_align(s_track, LV_ALIGN_TOP_MID, 0, 56);

    s_artist = lv_label_create(parent);
    lv_obj_set_width(s_artist, 200);
    lv_label_set_long_mode(s_artist, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_color(s_artist, UI_COLOR_DIM, 0);
    lv_obj_set_style_text_align(s_artist, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_artist, "");
    lv_obj_align(s_artist, LV_ALIGN_TOP_MID, 0, 86);

    /* Controls */
    s_controls = lv_obj_create(parent);
    lv_obj_remove_style_all(s_controls);
    lv_obj_set_size(s_controls, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_controls, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_controls, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_controls, 14, 0);
    lv_obj_set_clickable(s_controls, false);
    lv_obj_align(s_controls, LV_ALIGN_TOP_MID, 0, 124);

    lv_obj_t *row = button_row(s_controls);
    round_button(row, LV_SYMBOL_PREV, 52, UI_COLOR_SURFACE, "previous");
    s_play_icon = round_button(row, LV_SYMBOL_PLAY, 68, UI_COLOR_ACCENT, "playpause");
    lv_obj_set_style_text_color(s_play_icon, ui_contrast(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_text_font(s_play_icon, &lv_font_montserrat_24, 0);
    round_button(row, LV_SYMBOL_NEXT, 52, UI_COLOR_SURFACE, "next");

    row = button_row(s_controls);
    lv_obj_set_style_pad_hor(row, 30, 0);
    round_button(row, LV_SYMBOL_VOLUME_MID, 44, UI_COLOR_SURFACE, "volumedown");
    round_button(row, LV_SYMBOL_VOLUME_MAX, 44, UI_COLOR_SURFACE, "volumeup");

    screen_music_update();
}

void screen_music_update(void)
{
    if (!s_track) return;

    bool connected = gb_is_connected();
    lv_obj_set_style_opa(s_controls, connected ? LV_OPA_COVER : LV_OPA_40, 0);

    if (!connected) {
        set_text_if_changed(s_track, "Not connected");
        set_text_if_changed(s_artist, "Connect Gadgetbridge");
        set_text_if_changed(s_play_icon, LV_SYMBOL_PLAY);
        return;
    }

    char artist[48], track[64];
    gb_get_music(artist, sizeof(artist), track, sizeof(track));
    set_text_if_changed(s_track, track[0] ? track : "Nothing playing");
    set_text_if_changed(s_artist, artist);
    set_text_if_changed(s_play_icon, gb_music_playing() ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}
