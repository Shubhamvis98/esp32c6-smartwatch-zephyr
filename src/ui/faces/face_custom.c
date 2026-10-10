/**
 * @file ui/faces/face_custom.c
 * @brief Data-driven watchfaces: /data/faces/<id>/face.json plus images.
 *
 * Only one face is built at a time, so the element state below is static.
 * Images are LVGL .bin files (tools/face_push.py and the upload page convert
 * PNGs).  Small images are loaded into RAM, which hands need for rotation;
 * larger ones (backgrounds) are streamed from flash by LVGL row by row.
 */

#include "ui/faces/face_custom.h"
#include "ui/ui_internal.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "face_custom";

#define MAX_ELEMENTS      48
#define MAX_JSON_BYTES    (16 * 1024)
#define IMG_RAM_MAX       (48 * 1024)     // per image; larger ones stream from flash
#define IMG_RAM_BUDGET    (96 * 1024)     // all RAM images of one face
#define TEXT_MAX          64
#define SCREEN_W          240
#define SCREEN_H          280

/* Per-face info kept for the list (face_custom_scan) */
typedef struct {
    char name[32];
    char desc[48];
} face_info_t;

typedef enum {
    VAL_NONE, VAL_STEPS, VAL_BATTERY, VAL_SECONDS, VAL_MINUTES, VAL_HOURS,
} value_src_t;

typedef enum { EL_TEXT, EL_ARC, EL_BAR, EL_HAND } el_kind_t;
typedef enum { HAND_HOUR, HAND_MINUTE, HAND_SECOND } hand_t;

typedef struct {
    el_kind_t   kind;
    lv_obj_t   *obj;
    char       *tmpl;                 // EL_TEXT: text with {tokens}
    value_src_t value;                // EL_ARC / EL_BAR
    hand_t      hand;                 // EL_HAND
    bool        hand_is_image;
    int16_t     cx, cy;               // EL_HAND: pivot on screen
    int16_t     length, tail;         // EL_HAND (line)
    lv_point_precise_t pts[2];        // EL_HAND (line)
} element_t;

static element_t s_el[MAX_ELEMENTS];
static int       s_el_count;
static void     *s_img_bufs[MAX_ELEMENTS + 1];  // RAM images (and their descriptors)
static int       s_img_count;
static size_t    s_img_bytes;
static char      s_error[96];

const char *face_custom_last_error(void) { return s_error; }

static void set_error(const char *fmt, const char *arg)
{
    snprintf(s_error, sizeof(s_error), fmt, arg);
    ESP_LOGW(TAG, "%s", s_error);
}

bool face_custom_valid_id(const char *id)
{
    size_t n = strlen(id);
    if (n == 0 || n >= FACE_ID_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!(islower((unsigned char)c) || isdigit((unsigned char)c) || c == '_' || c == '-')) return false;
    }
    return true;
}

/* ── JSON helpers ────────────────────────────────────────────────────────── */
static cJSON *load_json(const char *id)
{
    char path[64];
    snprintf(path, sizeof(path), FACES_DIR "/%s/face.json", id);
    FILE *f = fopen(path, "rb");
    if (!f) {
        set_error("%s: no face.json", id);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > MAX_JSON_BYTES) {
        fclose(f);
        set_error("%s: face.json empty or over 16 KB", id);
        return NULL;
    }
    char *buf = malloc(len + 1);
    if (!buf) {
        fclose(f);
        set_error("%s: out of memory", id);
        return NULL;
    }
    size_t got = fread(buf, 1, len, f);
    fclose(f);
    buf[got] = 0;

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        /* Point at where parsing stopped */
        const char *at = cJSON_GetErrorPtr();
        char near[24] = "";
        if (at) strlcpy(near, at, sizeof(near));
        for (char *p = near; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
        snprintf(s_error, sizeof(s_error), "%s: invalid JSON near \"%s\"", id, near);
        ESP_LOGW(TAG, "%s", s_error);
    }
    free(buf);
    return root;
}

static const char *jstr(const cJSON *o, const char *key, const char *def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : def;
}

static int jint(const cJSON *o, const char *key, int def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? (int)v->valuedouble : def;
}

static bool jbool(const cJSON *o, const char *key, bool def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : def;
}

/* "#RRGGBB" or "#RGB" */
static lv_color_t jcolor(const cJSON *o, const char *key, lv_color_t def)
{
    const char *s = jstr(o, key, NULL);
    if (!s || s[0] != '#') return def;
    char *end;
    unsigned long v = strtoul(s + 1, &end, 16);
    size_t n = end - (s + 1);
    if (*end) return def;
    if (n == 3) {
        return lv_color_make(((v >> 8) & 0xF) * 17, ((v >> 4) & 0xF) * 17, (v & 0xF) * 17);
    }
    return n == 6 ? lv_color_hex(v) : def;
}

static const lv_font_t *jfont(const cJSON *o)
{
    switch (jint(o, "font", 20)) {
    case 14: return &lv_font_montserrat_14;
    case 16: return &lv_font_montserrat_16;
    case 24: return &lv_font_montserrat_24;
    case 36: return &lv_font_montserrat_36;
    case 48: return &lv_font_montserrat_48;
    default: return &lv_font_montserrat_20;
    }
}

static lv_align_t jalign(const cJSON *o, lv_align_t def)
{
    static const struct { const char *name; lv_align_t align; } map[] = {
        { "center", LV_ALIGN_CENTER },         { "top", LV_ALIGN_TOP_MID },
        { "bottom", LV_ALIGN_BOTTOM_MID },     { "left", LV_ALIGN_LEFT_MID },
        { "right", LV_ALIGN_RIGHT_MID },       { "top_left", LV_ALIGN_TOP_LEFT },
        { "top_right", LV_ALIGN_TOP_RIGHT },   { "bottom_left", LV_ALIGN_BOTTOM_LEFT },
        { "bottom_right", LV_ALIGN_BOTTOM_RIGHT },
    };
    const char *s = jstr(o, "align", NULL);
    if (!s) return def;
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(s, map[i].name) == 0) return map[i].align;
    }
    return def;
}

static void place(lv_obj_t *obj, const cJSON *o, lv_align_t def)
{
    lv_obj_align(obj, jalign(o, def), jint(o, "x", 0), jint(o, "y", 0));
}

static value_src_t jvalue(const cJSON *o)
{
    const char *s = jstr(o, "value", "steps");
    if (strcmp(s, "battery") == 0) return VAL_BATTERY;
    if (strcmp(s, "seconds") == 0) return VAL_SECONDS;
    if (strcmp(s, "minutes") == 0) return VAL_MINUTES;
    if (strcmp(s, "hours") == 0)   return VAL_HOURS;
    return VAL_STEPS;
}

/* ── Images ──────────────────────────────────────────────────────────────── */
/* "hand.png" in face.json means the converted "hand.bin" */
static void image_path(char *out, size_t len, const char *id, const char *src)
{
    char name[40];
    strlcpy(name, src, sizeof(name));
    char *dot = strrchr(name, '.');
    if (dot && (strcasecmp(dot, ".png") == 0 || strcasecmp(dot, ".jpg") == 0 ||
                strcasecmp(dot, ".jpeg") == 0)) {
        strcpy(dot, ".bin");
    }
    snprintf(out, len, FACES_DIR "/%s/%s", id, name);
}

/* Returns an lv_image source (lv_image_dsc_t * or "A:/path" string), or NULL.
 * @p need_ram: the image will be transformed, so it must be in RAM. */
static const void *load_image(const char *id, const char *src, bool need_ram)
{
    if (strchr(src, '/') || strstr(src, "..")) {
        set_error("bad image name \"%s\"", src);
        return NULL;
    }
    char path[80];
    image_path(path, sizeof(path), id, src);

    FILE *f = fopen(path, "rb");
    if (!f) {
        set_error("missing image %s", src);
        return NULL;
    }
    lv_image_header_t hdr;
    struct stat st;
    bool ok = fread(&hdr, sizeof(hdr), 1, f) == 1 && hdr.magic == LV_IMAGE_HEADER_MAGIC &&
              fstat(fileno(f), &st) == 0;
    size_t data_len = ok ? st.st_size - sizeof(hdr) : 0;
    size_t expect = (size_t)hdr.stride * hdr.h;
    if (ok && hdr.cf == LV_COLOR_FORMAT_RGB565A8) expect += (size_t)hdr.stride / 2 * hdr.h;
    ok = ok && hdr.w && hdr.h && data_len >= expect &&
         (hdr.cf == LV_COLOR_FORMAT_RGB565 || hdr.cf == LV_COLOR_FORMAT_RGB565A8 ||
          hdr.cf == LV_COLOR_FORMAT_ARGB8888 || hdr.cf == LV_COLOR_FORMAT_A8);
    if (!ok) {
        fclose(f);
        set_error("%s is not an LVGL image (convert PNGs with the upload page)", src);
        return NULL;
    }

    bool fits = expect <= IMG_RAM_MAX && s_img_bytes + expect <= IMG_RAM_BUDGET &&
                s_img_count < (int)(sizeof(s_img_bufs) / sizeof(s_img_bufs[0]));
    if (!fits) {
        fclose(f);
        if (need_ram) {
            set_error("%s is too big to rotate (keep hands under 48 KB)", src);
            return NULL;
        }
        /* Stream from flash; LVGL keeps its own copy of the path string */
        static char lv_path[84];
        snprintf(lv_path, sizeof(lv_path), "%s", path);   /* Zephyr LVGL: drive letter is the root "/" */
        return lv_path;
    }

    lv_image_dsc_t *dsc = malloc(sizeof(*dsc) + expect);
    if (!dsc) {
        fclose(f);
        set_error("out of memory loading %s", src);
        return NULL;
    }
    uint8_t *data = (uint8_t *)(dsc + 1);
    ok = fread(data, 1, expect, f) == expect;
    fclose(f);
    if (!ok) {
        free(dsc);
        set_error("can't read %s", src);
        return NULL;
    }
    memset(dsc, 0, sizeof(*dsc));
    dsc->header = hdr;
    dsc->data_size = expect;
    dsc->data = data;
    s_img_bufs[s_img_count++] = dsc;
    s_img_bytes += expect;
    return dsc;
}

/* ── Element builders ────────────────────────────────────────────────────── */
static element_t *new_element(el_kind_t kind)
{
    if (s_el_count >= MAX_ELEMENTS) return NULL;
    element_t *e = &s_el[s_el_count++];
    memset(e, 0, sizeof(*e));
    e->kind = kind;
    return e;
}

static void build_text(lv_obj_t *root, const cJSON *o)
{
    const char *text = jstr(o, "text", "");
    lv_obj_t *l = face_make_label(root, jfont(o), jcolor(o, "color", UI_COLOR_TEXT));
    const char *ta = jstr(o, "text_align", "center");
    lv_obj_set_style_text_align(l, strcmp(ta, "left") == 0  ? LV_TEXT_ALIGN_LEFT :
                                   strcmp(ta, "right") == 0 ? LV_TEXT_ALIGN_RIGHT :
                                                              LV_TEXT_ALIGN_CENTER, 0);
    int w = jint(o, "width", 0);
    if (w > 0) lv_obj_set_width(l, w);
    lv_obj_set_style_text_letter_space(l, jint(o, "letter_space", 0), 0);
    place(l, o, LV_ALIGN_CENTER);

    if (!strchr(text, '{')) {                       // static text
        lv_label_set_text(l, text);
        return;
    }
    element_t *e = new_element(EL_TEXT);
    if (!e) return;
    e->obj = l;
    e->tmpl = strdup(text);
}

static void build_rect(lv_obj_t *root, const cJSON *o)
{
    lv_obj_t *r = face_make_layer(root);
    lv_obj_set_size(r, jint(o, "w", 40), jint(o, "h", 40));
    lv_obj_set_style_bg_color(r, jcolor(o, "color", UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(r, jint(o, "opacity", 255), 0);
    lv_obj_set_style_radius(r, jint(o, "radius", 0), 0);
    lv_obj_set_style_border_width(r, jint(o, "border_width", 0), 0);
    lv_obj_set_style_border_color(r, jcolor(o, "border_color", UI_COLOR_TEXT), 0);
    place(r, o, LV_ALIGN_CENTER);
}

static void build_image(lv_obj_t *root, const char *id, const cJSON *o)
{
    const void *src = load_image(id, jstr(o, "src", ""), false);
    if (!src) return;
    lv_obj_t *im = lv_image_create(root);
    lv_obj_set_clickable(im, false);
    lv_image_set_src(im, src);
    place(im, o, LV_ALIGN_CENTER);
}

static void build_arc(lv_obj_t *root, const cJSON *o)
{
    element_t *e = new_element(EL_ARC);
    if (!e) return;
    int size = jint(o, "size", 200), width = jint(o, "width", 8);
    lv_obj_t *a = lv_arc_create(root);
    lv_obj_set_clickable(a, false);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_set_size(a, size, size);
    lv_arc_set_bg_angles(a, jint(o, "start_angle", 135), jint(o, "end_angle", 45));
    lv_arc_set_rotation(a, 0);
    lv_arc_set_range(a, 0, 1000);
    lv_arc_set_value(a, 0);
    bool rounded = jbool(o, "rounded", true);
    lv_obj_set_style_arc_width(a, width, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(a, rounded, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(a, rounded, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, jcolor(o, "bg_color", UI_COLOR_RAISED), LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, jcolor(o, "color", UI_COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(a, 0, 0);
    place(a, o, LV_ALIGN_CENTER);
    e->obj = a;
    e->value = jvalue(o);
}

static void build_bar(lv_obj_t *root, const cJSON *o)
{
    element_t *e = new_element(EL_BAR);
    if (!e) return;
    lv_obj_t *b = lv_bar_create(root);
    lv_obj_set_clickable(b, false);
    lv_obj_set_size(b, jint(o, "w", 150), jint(o, "h", 6));
    lv_bar_set_range(b, 0, 1000);
    int radius = jint(o, "radius", LV_RADIUS_CIRCLE);
    lv_obj_set_style_radius(b, radius, LV_PART_MAIN);
    lv_obj_set_style_radius(b, radius, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(b, jcolor(o, "bg_color", UI_COLOR_RAISED), LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, jcolor(o, "color", UI_COLOR_ACCENT), LV_PART_INDICATOR);
    place(b, o, LV_ALIGN_CENTER);
    e->obj = b;
    e->value = jvalue(o);
}

static void build_hand(lv_obj_t *root, const char *id, const cJSON *o)
{
    const char *which = jstr(o, "hand", "minute");
    hand_t hand = strcmp(which, "hour") == 0   ? HAND_HOUR :
                  strcmp(which, "second") == 0 ? HAND_SECOND : HAND_MINUTE;
    int cx = jint(o, "cx", SCREEN_W / 2), cy = jint(o, "cy", SCREEN_H / 2);
    const char *src = jstr(o, "src", NULL);

    lv_obj_t *obj;
    if (src) {
        /* Image drawn pointing at 12 o'clock; pivot_x/y is the rotation point */
        const void *img = load_image(id, src, true);
        if (!img) return;
        const lv_image_dsc_t *dsc = img;
        int px = jint(o, "pivot_x", dsc->header.w / 2);
        int py = jint(o, "pivot_y", dsc->header.h - 1);
        obj = lv_image_create(root);
        lv_obj_set_clickable(obj, false);
        lv_image_set_src(obj, img);
        lv_image_set_pivot(obj, px, py);
        lv_obj_set_pos(obj, cx - px, cy - py);
    } else {
        obj = lv_line_create(root);
        lv_obj_set_clickable(obj, false);
        lv_obj_set_size(obj, SCREEN_W, SCREEN_H);
        lv_obj_set_pos(obj, 0, 0);
        lv_obj_set_style_line_width(obj, jint(o, "width", 4), 0);
        lv_obj_set_style_line_color(obj, jcolor(o, "color", UI_COLOR_TEXT), 0);
        lv_obj_set_style_line_rounded(obj, jbool(o, "rounded", true), 0);
    }

    element_t *e = new_element(EL_HAND);
    if (!e) {
        lv_obj_delete(obj);
        return;
    }
    e->obj = obj;
    e->hand = hand;
    e->hand_is_image = src != NULL;
    e->cx = cx;
    e->cy = cy;
    e->length = jint(o, "length", hand == HAND_HOUR ? 55 : 85);
    e->tail = jint(o, "tail", 0);
}

/* ── Runtime ─────────────────────────────────────────────────────────────── */
static void free_runtime(void)
{
    for (int i = 0; i < s_el_count; i++) free(s_el[i].tmpl);
    s_el_count = 0;
    for (int i = 0; i < s_img_count; i++) free(s_img_bufs[i]);
    s_img_count = 0;
    s_img_bytes = 0;
}

static bool create(const watchface_t *face, lv_obj_t *root)
{
    free_runtime();
    s_error[0] = 0;

    cJSON *json = load_json(face->id);
    if (!json) return false;

    lv_obj_set_style_bg_color(root, jcolor(json, "background", UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    const char *bg = jstr(json, "background_image", NULL);
    if (bg) {
        const void *src = load_image(face->id, bg, false);
        if (src) {
            lv_obj_t *im = lv_image_create(root);
            lv_obj_set_clickable(im, false);
            lv_image_set_src(im, src);
            lv_obj_center(im);
        }
    }

    const cJSON *els = cJSON_GetObjectItemCaseSensitive(json, "elements");
    const cJSON *o;
    cJSON_ArrayForEach(o, els) {
        const char *type = jstr(o, "type", "");
        if      (strcmp(type, "text") == 0)  build_text(root, o);
        else if (strcmp(type, "rect") == 0)  build_rect(root, o);
        else if (strcmp(type, "image") == 0) build_image(root, face->id, o);
        else if (strcmp(type, "arc") == 0)   build_arc(root, o);
        else if (strcmp(type, "bar") == 0)   build_bar(root, o);
        else if (strcmp(type, "hand") == 0)  build_hand(root, face->id, o);
        else set_error("unknown element type \"%s\"", type);
    }
    cJSON_Delete(json);
    ESP_LOGI(TAG, "Built %s: %d live elements, %u B of images in RAM", face->id, s_el_count,
             (unsigned)s_img_bytes);
    return true;
}

static void destroy(const watchface_t *face)
{
    free_runtime();
}

/* Expand {tokens}; time tokens show "--" until the clock is set */
static void expand(char *out, size_t len, const char *tmpl, const face_data_t *d)
{
    static const char *const short_days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    const struct tm *tm = d->tm;
    size_t n = 0;

    while (*tmpl && n + 1 < len) {
        const char *close = *tmpl == '{' ? strchr(tmpl, '}') : NULL;
        if (!close || close - tmpl > 10) {
            out[n++] = *tmpl++;
            continue;
        }
        char key[12];
        size_t kl = close - tmpl - 1;
        memcpy(key, tmpl + 1, kl);
        key[kl] = 0;
        tmpl = close + 1;

        char v[24] = "";
        if (strcmp(key, "steps") == 0)        ui_format_thousands(v, sizeof(v), d->steps);
        else if (strcmp(key, "goal") == 0)    ui_format_thousands(v, sizeof(v), d->step_goal);
        else if (strcmp(key, "battery") == 0) snprintf(v, sizeof(v), "%u", d->battery_pct);
        else if (!tm)                         strcpy(v, "--");
        else if (strcmp(key, "HH") == 0)      snprintf(v, sizeof(v), "%02d", face_display_hour(d));
        else if (strcmp(key, "H") == 0)       snprintf(v, sizeof(v), "%d", face_display_hour(d));
        else if (strcmp(key, "mm") == 0)      snprintf(v, sizeof(v), "%02d", tm->tm_min);
        else if (strcmp(key, "ss") == 0)      snprintf(v, sizeof(v), "%02d", tm->tm_sec);
        else if (strcmp(key, "AMPM") == 0)    strcpy(v, d->use_24h ? "" : tm->tm_hour < 12 ? "AM" : "PM");
        else if (strcmp(key, "day") == 0)     strcpy(v, g_face_days[tm->tm_wday]);
        else if (strcmp(key, "ddd") == 0)     strcpy(v, short_days[tm->tm_wday]);
        else if (strcmp(key, "DDD") == 0)     snprintf(v, sizeof(v), "%.3s", g_face_days[tm->tm_wday]);
        else if (strcmp(key, "D") == 0)       snprintf(v, sizeof(v), "%d", tm->tm_mday);
        else if (strcmp(key, "DD") == 0)      snprintf(v, sizeof(v), "%02d", tm->tm_mday);
        else if (strcmp(key, "month") == 0)   strcpy(v, g_face_months[tm->tm_mon]);
        else if (strcmp(key, "mmm") == 0 || strcmp(key, "MMM") == 0)
                                              snprintf(v, sizeof(v), "%.3s", g_face_months[tm->tm_mon]);
        else if (strcmp(key, "MM") == 0)      snprintf(v, sizeof(v), "%02d", tm->tm_mon + 1);
        else if (strcmp(key, "YYYY") == 0)    snprintf(v, sizeof(v), "%d", tm->tm_year + 1900);
        else                                  snprintf(v, sizeof(v), "{%s}", key);   // unknown: keep as is

        if (strcmp(key, "DDD") == 0 || strcmp(key, "MMM") == 0) {
            for (char *p = v; *p; p++) *p = toupper((unsigned char)*p);
        }
        for (const char *p = v; *p && n + 1 < len; p++) out[n++] = *p;
    }
    out[n] = 0;
}

/* 0…1000 */
static int32_t value_permille(value_src_t v, const face_data_t *d)
{
    const struct tm *tm = d->tm;
    switch (v) {
    case VAL_STEPS:   return d->step_goal ? (int32_t)(d->steps >= d->step_goal ? 1000 : d->steps * 1000 / d->step_goal) : 0;
    case VAL_BATTERY: return d->battery_pct * 10;
    case VAL_SECONDS: return tm ? tm->tm_sec * 1000 / 60 : 0;
    case VAL_MINUTES: return tm ? (tm->tm_min * 60 + tm->tm_sec) * 1000 / 3600 : 0;
    case VAL_HOURS:   return tm ? ((tm->tm_hour % 12) * 60 + tm->tm_min) * 1000 / 720 : 0;
    default:          return 0;
    }
}

/* Hand angle in 0.1° clockwise from 12 o'clock */
static int32_t hand_angle(hand_t h, const struct tm *tm)
{
    switch (h) {
    case HAND_HOUR:   return ((tm->tm_hour % 12) * 60 + tm->tm_min) * 3600 / 720;
    case HAND_MINUTE: return (tm->tm_min * 60 + tm->tm_sec) * 3600 / 3600;
    default:          return tm->tm_sec * 60;
    }
}

static void update(const watchface_t *face, const face_data_t *d)
{
    char buf[TEXT_MAX];
    for (int i = 0; i < s_el_count; i++) {
        element_t *e = &s_el[i];
        switch (e->kind) {
        case EL_TEXT:
            expand(buf, sizeof(buf), e->tmpl, d);
            face_set_text(e->obj, buf);
            break;
        case EL_ARC: {
            int32_t v = value_permille(e->value, d);
            if (lv_arc_get_value(e->obj) != v) lv_arc_set_value(e->obj, v);
            break;
        }
        case EL_BAR: {
            int32_t v = value_permille(e->value, d);
            if (lv_bar_get_value(e->obj) != v) lv_bar_set_value(e->obj, v, LV_ANIM_OFF);
            break;
        }
        case EL_HAND:
            lv_obj_set_hidden(e->obj, d->tm == NULL);
            if (!d->tm) break;
            int32_t a = hand_angle(e->hand, d->tm);
            if (e->hand_is_image) {
                if (lv_image_get_rotation(e->obj) != a) lv_image_set_rotation(e->obj, a);
            } else {
                int32_t s = lv_trigo_sin(a / 10), c = lv_trigo_cos(a / 10);   // ×32767
                e->pts[0].x = e->cx - e->tail * s / 32767;
                e->pts[0].y = e->cy + e->tail * c / 32767;
                e->pts[1].x = e->cx + e->length * s / 32767;
                e->pts[1].y = e->cy - e->length * c / 32767;
                lv_line_set_points(e->obj, e->pts, 2);
            }
            break;
        }
    }
}

/* ── Scan ────────────────────────────────────────────────────────────────── */
static int cmp_face(const void *a, const void *b)
{
    return strcmp(((const watchface_t *)a)->id, ((const watchface_t *)b)->id);
}

int face_custom_scan(watchface_t *out, int max)
{
    DIR *dir = opendir(FACES_DIR);
    if (!dir) return 0;

    int n = 0;
    struct dirent *e;
    while (n < max && (e = readdir(dir))) {
        if (!face_custom_valid_id(e->d_name)) continue;

        face_info_t *info = calloc(1, sizeof(*info));
        if (!info) break;
        cJSON *json = load_json(e->d_name);
        if (!json) {
            if (strstr(s_error, "no face.json")) {   // not a face folder (yet)
                free(info);
                continue;
            }
            strlcpy(info->name, e->d_name, sizeof(info->name));
            strlcpy(info->desc, "Error in face.json", sizeof(info->desc));
        } else {
            strlcpy(info->name, jstr(json, "name", e->d_name), sizeof(info->name));
            strlcpy(info->desc, jstr(json, "desc", "Custom face"), sizeof(info->desc));
        }

        watchface_t *f = &out[n++];
        memset(f, 0, sizeof(*f));
        strlcpy(f->id, e->d_name, sizeof(f->id));
        f->name = info->name;
        f->desc = info->desc;
        if (json) cJSON_Delete(json);
        f->create = create;
        f->update = update;
        f->destroy = destroy;
        f->user = info;
    }
    closedir(dir);
    s_error[0] = 0;

    qsort(out, n, sizeof(out[0]), cmp_face);
    ESP_LOGI(TAG, "%d custom face(s)", n);
    return n;
}

void face_custom_free(watchface_t *faces, int count)
{
    for (int i = 0; i < count; i++) {
        free(faces[i].user);
        faces[i].user = NULL;
    }
}

bool face_custom_remove(const char *id)
{
    if (!face_custom_valid_id(id)) return false;
    char dir[48], path[96];
    snprintf(dir, sizeof(dir), FACES_DIR "/%s", id);
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strlen(e->d_name) > 40) continue;        // can't be ours
        snprintf(path, sizeof(path), "%s/%.40s", dir, e->d_name);
        unlink(path);
    }
    closedir(d);
    if (rmdir(dir) != 0) return false;
    ESP_LOGI(TAG, "Deleted face %s", id);
    return true;
}
