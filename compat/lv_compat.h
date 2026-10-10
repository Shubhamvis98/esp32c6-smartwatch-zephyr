/* LVGL 9.6 helpers that Zephyr's LVGL snapshot doesn't have yet. */
#pragma once
#include <lvgl.h>

static inline void lv_obj_set_clickable(lv_obj_t *o, bool v)  { lv_obj_set_flag(o, LV_OBJ_FLAG_CLICKABLE, v); }
static inline void lv_obj_set_scrollable(lv_obj_t *o, bool v) { lv_obj_set_flag(o, LV_OBJ_FLAG_SCROLLABLE, v); }
static inline void lv_obj_set_hidden(lv_obj_t *o, bool v)     { lv_obj_set_flag(o, LV_OBJ_FLAG_HIDDEN, v); }
static inline void lv_obj_set_checkable(lv_obj_t *o, bool v)  { lv_obj_set_flag(o, LV_OBJ_FLAG_CHECKABLE, v); }
static inline void lv_obj_set_floating(lv_obj_t *o, bool v)   { lv_obj_set_flag(o, LV_OBJ_FLAG_FLOATING, v); }
