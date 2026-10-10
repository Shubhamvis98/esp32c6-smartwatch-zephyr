/**
 * @file ui/faces/face_common.c
 * @brief Helpers shared by watchface modules.
 */

#include "ui/faces/watchface.h"

#include <string.h>

const char *const g_face_days[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
};
const char *const g_face_months[12] = {
    "January", "February", "March", "April", "May", "June", "July",
    "August", "September", "October", "November", "December",
};

int face_display_hour(const face_data_t *d)
{
    int h24 = d->tm ? d->tm->tm_hour : 0;
    if (d->use_24h) return h24;
    int h = h24 % 12;
    return h ? h : 12;
}

lv_obj_t *face_make_layer(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_clickable(o, false);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_size(o, LV_PCT(100), LV_PCT(100));
    return o;
}

lv_obj_t *face_make_flex(lv_obj_t *parent, lv_flex_flow_t flow)
{
    lv_obj_t *o = face_make_layer(parent);
    lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(o, flow);
    return o;
}

lv_obj_t *face_make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, "");
    return l;
}

void face_set_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}
