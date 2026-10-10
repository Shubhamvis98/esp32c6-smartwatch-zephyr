/**
 * @file ui/screen_calc.c
 * @brief Calculator: + - x /, percent, sign change and backspace.
 *
 * Works like a pocket calculator: an operator applies the pending one, so
 * "2 + 3 x 4 =" gives 20.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui/ui_internal.h"

#define ENTRY_MAX  12

static lv_obj_t *s_display, *s_expr;
static char      s_entry[ENTRY_MAX + 2];   // number being typed ("" = none)
static double    s_acc;                    // result so far
static char      s_op;                     // pending operator, 0 = none
static bool      s_error;

static const char *const s_map[] = {
    "C", "+/-", "%", "/", "\n",
    "7", "8", "9", "x", "\n",
    "4", "5", "6", "-", "\n",
    "1", "2", "3", "+", "\n",
    "0", ".", LV_SYMBOL_BACKSPACE, "=", "",
};

static void format(char *out, size_t len, double v)
{
    if (!isfinite(v)) {
        snprintf(out, len, "Error");
        return;
    }
    if (v == 0) v = 0;                       // no "-0"
    snprintf(out, len, "%.10g", v);
    if (strlen(out) > ENTRY_MAX) snprintf(out, len, "%.6g", v);
}

static double entry_value(void)
{
    return s_entry[0] ? strtod(s_entry, NULL) : s_acc;
}

static void show(void)
{
    char buf[32];
    if (s_error) snprintf(buf, sizeof(buf), "Error");
    else if (s_entry[0]) snprintf(buf, sizeof(buf), "%s", s_entry);
    else format(buf, sizeof(buf), s_acc);
    lv_label_set_text(s_display, buf);
    lv_obj_set_style_text_font(s_display, strlen(buf) > 8 ? &lv_font_montserrat_24
                                                          : &lv_font_montserrat_36, 0);

    if (s_op && !s_error) {
        char a[24];
        format(a, sizeof(a), s_acc);
        lv_label_set_text_fmt(s_expr, "%s %c", a, s_op);
    } else {
        lv_label_set_text(s_expr, "");
    }
}

static double apply(double a, char op, double b)
{
    switch (op) {
    case '+': return a + b;
    case '-': return a - b;
    case 'x': return a * b;
    case '/': return b == 0 ? NAN : a / b;
    default:  return b;
    }
}

static void clear(void)
{
    s_entry[0] = '\0';
    s_acc = 0;
    s_op = 0;
    s_error = false;
}

static void key_cb(lv_event_t *e)
{
    lv_obj_t *m = lv_event_get_target_obj(e);
    const char *k = lv_buttonmatrix_get_button_text(m, lv_buttonmatrix_get_selected_button(m));
    if (!k) return;
    if (s_error && strcmp(k, "C") != 0) clear();

    size_t len = strlen(s_entry);
    if (k[0] >= '0' && k[0] <= '9' && !k[1]) {
        if (!s_op && !s_entry[0]) s_acc = 0;           // new sum after "="
        if (strcmp(s_entry, "0") == 0) len = 0;
        if (len < ENTRY_MAX) { s_entry[len] = k[0]; s_entry[len + 1] = '\0'; }
    } else if (strcmp(k, ".") == 0) {
        if (!strchr(s_entry, '.') && len < ENTRY_MAX - 1) strcat(s_entry, len ? "." : "0.");
    } else if (strcmp(k, LV_SYMBOL_BACKSPACE) == 0) {
        if (len) s_entry[len - 1] = '\0';
        if (strcmp(s_entry, "-") == 0) s_entry[0] = '\0';
    } else if (strcmp(k, "C") == 0) {
        clear();
    } else if (strcmp(k, "+/-") == 0) {
        if (s_entry[0]) {
            if (s_entry[0] == '-') memmove(s_entry, s_entry + 1, len);
            else if (len < ENTRY_MAX) { memmove(s_entry + 1, s_entry, len + 1); s_entry[0] = '-'; }
        } else {
            s_acc = -s_acc;
        }
    } else if (strcmp(k, "%") == 0) {
        /* "50 + 10 %" = 50 + 5; on its own, divide by 100 */
        double v = entry_value();
        v = (s_op == '+' || s_op == '-') ? s_acc * v / 100.0 : v / 100.0;
        format(s_entry, sizeof(s_entry), v);
    } else if (strcmp(k, "=") == 0) {
        if (s_op) {
            s_acc = apply(s_acc, s_op, entry_value());
            s_op = 0;
        } else if (s_entry[0]) {
            s_acc = entry_value();
        }
        s_entry[0] = '\0';
        s_error = !isfinite(s_acc);
    } else {                                           // + - x /
        if (s_entry[0]) {
            s_acc = s_op ? apply(s_acc, s_op, entry_value()) : entry_value();
            s_entry[0] = '\0';
        }
        s_op = k[0];
        s_error = !isfinite(s_acc);
    }
    show();
}

void screen_calc_open(void)
{
    lv_obj_t *c = ui_page_open("Calculator", NULL);
    lv_obj_set_style_pad_row(c, 4, 0);
    lv_obj_set_scrollable(c, false);
    clear();

    s_expr = lv_label_create(c);
    lv_obj_set_width(s_expr, LV_PCT(100));
    lv_obj_set_style_text_align(s_expr, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(s_expr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_expr, UI_COLOR_DIM, 0);

    s_display = lv_label_create(c);
    lv_obj_set_width(s_display, LV_PCT(100));
    lv_obj_set_style_text_align(s_display, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_height(s_display, lv_font_get_line_height(&lv_font_montserrat_36));
    lv_obj_set_style_text_color(s_display, UI_COLOR_TEXT, 0);
    lv_label_set_long_mode(s_display, LV_LABEL_LONG_MODE_CLIP);

    lv_obj_t *m = lv_buttonmatrix_create(c);
    lv_buttonmatrix_set_map(m, s_map);
    lv_obj_set_width(m, LV_PCT(100));
    lv_obj_set_flex_grow(m, 1);
    lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_pad_all(m, 0, 0);
    lv_obj_set_style_pad_gap(m, 5, 0);
    lv_obj_set_style_radius(m, 10, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(m, UI_COLOR_SURFACE2, LV_PART_ITEMS);
    lv_obj_set_style_text_color(m, UI_COLOR_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_text_font(m, &lv_font_montserrat_20, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(m, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(m, UI_COLOR_RAISED, LV_PART_ITEMS | LV_STATE_PRESSED);
    /* Operators and "=" in the accent colour */
    static const uint8_t ops[] = { 3, 7, 11, 15, 19 };
    for (size_t i = 0; i < sizeof(ops); i++) {
        lv_buttonmatrix_set_button_ctrl(m, ops[i], LV_BUTTONMATRIX_CTRL_CHECKED);
    }
    lv_obj_set_style_bg_color(m, UI_COLOR_ACCENT, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(m, ui_contrast(UI_COLOR_ACCENT), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_add_event_cb(m, key_cb, LV_EVENT_VALUE_CHANGED, NULL);

    show();
}
