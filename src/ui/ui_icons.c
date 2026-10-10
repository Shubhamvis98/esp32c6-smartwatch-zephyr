/**
 * @file ui/ui_icons.c
 * @brief Icons the Montserrat symbol font doesn't have: anti-aliased A8 masks
 *        (ui_icons_data.h, from tools/gen_icons.py) tinted with image_recolor.
 *
 * The masks are generated offline: drawing them on the C6 (no FPU) inside
 * the LVGL lock at boot was slow enough to trip the lock timeout.
 */

#include <string.h>
#include "ui/ui_internal.h"
#include "ui/ui_icons_data.h"

#define ICON_DSC(name, px) {                                    \
        .header = {                                             \
            .magic = LV_IMAGE_HEADER_MAGIC,                     \
            .cf = LV_COLOR_FORMAT_A8,                           \
            .w = px, .h = px, .stride = px,                     \
        },                                                      \
        .data_size = sizeof(s_icon_##name),                     \
        .data = s_icon_##name,                                  \
    }

static const lv_image_dsc_t s_sun   = ICON_DSC(sun, ICON_SUN_PX);
static const lv_image_dsc_t s_watch = ICON_DSC(watch, ICON_WATCH_PX);
static const lv_image_dsc_t s_clock = ICON_DSC(clock, ICON_CLOCK_PX);
static const lv_image_dsc_t s_timer = ICON_DSC(timer, ICON_TIMER_PX);
static const lv_image_dsc_t s_torch = ICON_DSC(torch, ICON_TORCH_PX);
static const lv_image_dsc_t s_hourglass = ICON_DSC(hourglass, ICON_HOURGLASS_PX);
static const lv_image_dsc_t s_phone = ICON_DSC(phone, ICON_PHONE_PX);
static const lv_image_dsc_t s_palette = ICON_DSC(palette, ICON_PALETTE_PX);
static const lv_image_dsc_t s_navigate = ICON_DSC(navigate, ICON_NAVIGATE_PX);
static const lv_image_dsc_t s_navigate_lg = ICON_DSC(navigate_lg, ICON_NAVIGATE_LG_PX);
static const lv_image_dsc_t s_calendar = ICON_DSC(calendar, ICON_CALENDAR_PX);
static const lv_image_dsc_t s_calculator = ICON_DSC(calculator, ICON_CALCULATOR_PX);
static const lv_image_dsc_t s_camera = ICON_DSC(camera, ICON_CAMERA_PX);
static const lv_image_dsc_t s_camera_lg = ICON_DSC(camera_lg, ICON_CAMERA_LG_PX);

/* Weather, large and small, in weather_kind_t order */
static const lv_image_dsc_t s_wx[] = {
    ICON_DSC(wx_clear, ICON_WX_CLEAR_PX), ICON_DSC(wx_partly, ICON_WX_CLEAR_PX),
    ICON_DSC(wx_cloudy, ICON_WX_CLEAR_PX), ICON_DSC(wx_rain, ICON_WX_CLEAR_PX),
    ICON_DSC(wx_storm, ICON_WX_CLEAR_PX), ICON_DSC(wx_snow, ICON_WX_CLEAR_PX),
    ICON_DSC(wx_fog, ICON_WX_CLEAR_PX),
};
static const lv_image_dsc_t s_wxs[] = {
    ICON_DSC(wxs_clear, ICON_WXS_CLEAR_PX), ICON_DSC(wxs_partly, ICON_WXS_CLEAR_PX),
    ICON_DSC(wxs_cloudy, ICON_WXS_CLEAR_PX), ICON_DSC(wxs_rain, ICON_WXS_CLEAR_PX),
    ICON_DSC(wxs_storm, ICON_WXS_CLEAR_PX), ICON_DSC(wxs_snow, ICON_WXS_CLEAR_PX),
    ICON_DSC(wxs_fog, ICON_WXS_CLEAR_PX),
};

/* Turn arrows, in the order of s_nav_action_names */
#define NAV_DSC(name) ICON_DSC(nav_##name, ICON_NAV_CONTINUE_PX)
static const lv_image_dsc_t s_nav[ICON_NAV_COUNT] = {
    NAV_DSC(continue), NAV_DSC(left), NAV_DSC(left_slight), NAV_DSC(left_sharp),
    NAV_DSC(right), NAV_DSC(right_slight), NAV_DSC(right_sharp),
    NAV_DSC(keep_left), NAV_DSC(keep_right), NAV_DSC(uturn_left), NAV_DSC(uturn_right),
    NAV_DSC(offroute), NAV_DSC(roundabout_right), NAV_DSC(roundabout_left),
    NAV_DSC(roundabout_straight), NAV_DSC(roundabout_uturn), NAV_DSC(finish),
};

const lv_image_dsc_t *ui_icon_sun(void)   { return &s_sun; }
const lv_image_dsc_t *ui_icon_watch(void) { return &s_watch; }
const lv_image_dsc_t *ui_icon_clock(void) { return &s_clock; }
const lv_image_dsc_t *ui_icon_timer(void) { return &s_timer; }
const lv_image_dsc_t *ui_icon_torch(void) { return &s_torch; }
const lv_image_dsc_t *ui_icon_hourglass(void) { return &s_hourglass; }
const lv_image_dsc_t *ui_icon_phone(void) { return &s_phone; }
const lv_image_dsc_t *ui_icon_palette(void) { return &s_palette; }
const lv_image_dsc_t *ui_icon_navigate(void) { return &s_navigate; }
const lv_image_dsc_t *ui_icon_navigate_large(void) { return &s_navigate_lg; }
const lv_image_dsc_t *ui_icon_calendar(void) { return &s_calendar; }
const lv_image_dsc_t *ui_icon_calculator(void) { return &s_calculator; }
const lv_image_dsc_t *ui_icon_camera(void) { return &s_camera; }
const lv_image_dsc_t *ui_icon_weather_menu(void) { return &s_wxs[1]; }   // partly cloudy
const lv_image_dsc_t *ui_icon_camera_large(void) { return &s_camera_lg; }

const lv_image_dsc_t *ui_icon_weather(int kind, bool large)
{
    if (kind < 0 || kind >= (int)(sizeof(s_wx) / sizeof(s_wx[0]))) kind = 0;
    return large ? &s_wx[kind] : &s_wxs[kind];
}

const lv_image_dsc_t *ui_icon_nav(const char *action)
{
    for (int i = 0; i < ICON_NAV_COUNT; i++) {
        if (strcmp(action, s_nav_action_names[i]) == 0) return &s_nav[i];
    }
    return NULL;
}

lv_obj_t *ui_icon_create(lv_obj_t *parent, const lv_image_dsc_t *icon, lv_color_t color)
{
    lv_obj_t *im = lv_image_create(parent);
    lv_image_set_src(im, icon);
    lv_obj_set_style_image_recolor(im, color, 0);
    lv_obj_set_style_image_recolor_opa(im, LV_OPA_COVER, 0);
    lv_obj_set_clickable(im, false);
    return im;
}
