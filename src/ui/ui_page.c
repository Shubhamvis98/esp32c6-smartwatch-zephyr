/**
 * @file ui/ui_page.c
 * @brief Full-screen sub-pages (used by the settings menu).
 *
 * A page sits on lv_layer_top() above the tileview, has a back button and
 * title, and a vertically scrolling content column.  Swiping right also goes
 * back (detected in ui_manager.c).  An optional update callback runs every second while it is open.
 */

#include "ui/ui_internal.h"

#define PAGE_UPDATE_MS   1000

typedef struct {
    lv_timer_t     *timer;
    ui_page_update_cb_t update;
} page_ctx_t;

static lv_obj_t *s_page = NULL;      // only one page is open at a time
static ui_page_back_cb_t s_back = NULL;

static void page_timer_cb(lv_timer_t *t)
{
    page_ctx_t *ctx = lv_timer_get_user_data(t);
    if (ctx->update) ctx->update();
}

static void page_delete_cb(lv_event_t *e)
{
    page_ctx_t *ctx = lv_event_get_user_data(e);
    if (ctx->timer) lv_timer_delete(ctx->timer);
    lv_free(ctx);
}

static void back_cb(lv_event_t *e)
{
    ui_page_back();
}

void ui_page_set_back(ui_page_back_cb_t cb)
{
    s_back = cb;
}

void ui_page_back(void)
{
    ui_page_back_cb_t cb = s_back;
    if (cb) cb();              // e.g. reopen the parent page
    else    ui_page_close();
}

bool ui_page_is_open(void)
{
    return s_page != NULL;
}

void ui_page_close(void)
{
    s_back = NULL;
    if (!s_page) return;
    lv_obj_delete(s_page);
    s_page = NULL;
}

#define TITLE_W  160      // room right of the back button

lv_obj_t *ui_page_open(const char *title, ui_page_update_cb_t update)
{
    ui_page_close();

    page_ctx_t *ctx = lv_malloc_zeroed(sizeof(page_ctx_t));
    ctx->update = update;

    s_page = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_page);
    lv_obj_set_size(s_page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_page, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_page, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(s_page, false);
    lv_obj_set_clickable(s_page, true);              // block touches to the tiles
    lv_obj_add_event_cb(s_page, page_delete_cb, LV_EVENT_DELETE, ctx);

    lv_obj_t *back = lv_button_create(s_page);
    lv_obj_set_size(back, 34, 34);
    lv_obj_set_pos(back, 16, 10);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    ui_fill(back, UI_COLOR_SURFACE);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *arrow = lv_label_create(back);
    lv_label_set_text(arrow, LV_SYMBOL_LEFT);
    lv_obj_center(arrow);

    /* Title on one line: a long one drops to a smaller font, and if it
     * still doesn't fit it ends in "…" */
    lv_obj_t *lbl = lv_label_create(s_page);
    lv_label_set_text(lbl, title);
    const lv_font_t *font = &lv_font_montserrat_20;
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_update_layout(lbl);                       // measure at its natural width
    if (lv_obj_get_width(lbl) > TITLE_W) {
        font = &lv_font_montserrat_16;
        lv_obj_set_style_text_font(lbl, font, 0);
    }
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(lbl, TITLE_W, lv_font_get_line_height(font));
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 60, 16 + (lv_font_get_line_height(&lv_font_montserrat_20) -
                                                   lv_font_get_line_height(font)) / 2);

    lv_obj_t *content = lv_obj_create(s_page);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, LV_PCT(100), 280 - 52);
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_pad_hor(content, 14, 0);
    lv_obj_set_style_pad_top(content, 2, 0);
    lv_obj_set_style_pad_bottom(content, UI_SAFE_MARGIN + 6, 0);
    lv_obj_set_style_pad_row(content, 10, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_ACTIVE);

    if (update) {
        ctx->timer = lv_timer_create(page_timer_cb, PAGE_UPDATE_MS, ctx);
    }
    return content;
}

lv_obj_t *ui_card(lv_obj_t *parent, const char *title)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(box, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_pad_all(box, 10, 0);
    lv_obj_set_style_pad_row(box, 8, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(box, false);

    if (title) {
        lv_obj_t *lbl = lv_label_create(box);
        lv_label_set_text(lbl, title);
        lv_obj_set_style_text_color(lbl, UI_COLOR_DIM, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    }
    return box;
}

lv_obj_t *ui_tile_layout(lv_obj_t *tile, const char *title)
{
    lv_obj_set_style_bg_color(tile, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);

    /* Same header as ui_page_open; the back button is ui_manager's */
    lv_obj_t *lbl = lv_label_create(tile);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(lbl, 160);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 60, 16);

    lv_obj_t *col = lv_obj_create(tile);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_PCT(100), 280 - 52);
    lv_obj_align(col, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_pad_hor(col, 14, 0);
    lv_obj_set_style_pad_top(col, 2, 0);
    lv_obj_set_style_pad_bottom(col, UI_SAFE_MARGIN + 6, 0);
    lv_obj_set_style_pad_row(col, 8, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_ACTIVE);
    return col;
}
