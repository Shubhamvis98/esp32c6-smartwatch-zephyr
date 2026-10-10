/* ESP-IDF NVS API on Zephyr settings (NVS backend on storage_partition). */
#include <string.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/storage/flash_map.h>
#include "nvs.h"
#include "nvs_flash.h"

#define MAX_NS   12
#define NS_LEN   16

static char s_ns[MAX_NS][NS_LEN];
static K_MUTEX_DEFINE(s_lock);

static void path(nvs_handle_t h, const char *key, char *out, size_t len)
{
    snprintf(out, len, "nvs/%s/%s", s_ns[h], key);
}

esp_err_t nvs_flash_init(void)
{
    /* A partition holding something else (e.g. an older firmware) won't
     * mount; report it like ESP-IDF does so the caller erases and retries. */
    int r = settings_subsys_init();
    if (r != 0) printk("settings init failed: %d\n", r);
    return r == 0 ? ESP_OK : ESP_ERR_NVS_NO_FREE_PAGES;
}

esp_err_t nvs_flash_erase(void)
{
    const struct flash_area *fa;
    if (flash_area_open(FIXED_PARTITION_ID(storage_partition), &fa) != 0) return ESP_FAIL;
    int r = flash_area_erase(fa, 0, fa->fa_size);
    flash_area_close(fa);
    return r == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    k_mutex_lock(&s_lock, K_FOREVER);
    int free_slot = -1;
    for (int i = 0; i < MAX_NS; i++) {
        if (strcmp(s_ns[i], ns) == 0) { *out = i; k_mutex_unlock(&s_lock); return ESP_OK; }
        if (!s_ns[i][0] && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) { k_mutex_unlock(&s_lock); return ESP_ERR_NO_MEM; }
    strncpy(s_ns[free_slot], ns, NS_LEN - 1);
    *out = free_slot;
    k_mutex_unlock(&s_lock);
    return ESP_OK;
}

/* Handles name a namespace and stay valid; nothing to release. */
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }

esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *val, size_t len)
{
    char p[64];
    path(h, key, p, sizeof(p));
    return settings_save_one(p, val, len) == 0 ? ESP_OK : ESP_FAIL;
}

struct read_ctx {
    void   *out;
    size_t  cap;
    ssize_t got;     /* -1 = not found */
    bool    found;
};

static int read_cb(const char *key, size_t len, settings_read_cb read_cb_fn, void *cb_arg, void *param)
{
    struct read_ctx *c = param;
    const char *next;
    if (settings_name_next(key, &next) != 0 || next) return 0;   /* exact match only */
    c->found = true;
    c->got = len;
    if (c->out) {
        if (len > c->cap) { c->got = -2; return 1; }
        ssize_t n = read_cb_fn(cb_arg, c->out, len);
        if (n < 0) c->got = -1;
    }
    return 0;
}

esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    char p[64];
    path(h, key, p, sizeof(p));
    struct read_ctx c = { .out = out, .cap = len ? *len : 0, .got = -1 };
    settings_load_subtree_direct(p, read_cb, &c);
    if (!c.found || c.got == 0) return ESP_ERR_NVS_NOT_FOUND;   /* deleted keys read as 0 bytes */
    if (c.got == -2) { *len = 0; return ESP_ERR_NVS_INVALID_LENGTH; }
    if (c.got < 0) return ESP_FAIL;
    if (len) *len = c.got;
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *val)
{
    return nvs_set_blob(h, key, val, strlen(val) + 1);
}

esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len)
{
    return nvs_get_blob(h, key, out, len);
}

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    char p[64];
    path(h, key, p, sizeof(p));
    return settings_delete(p) == 0 ? ESP_OK : ESP_FAIL;
}

struct erase_ctx { char names[32][40]; int n; };

static int list_cb(const char *key, size_t len, settings_read_cb fn, void *arg, void *param)
{
    struct erase_ctx *c = param;
    if (len && c->n < 32) strncpy(c->names[c->n++], key, 39);
    return 0;
}

esp_err_t nvs_erase_all(nvs_handle_t h)
{
    char p[40];
    snprintf(p, sizeof(p), "nvs/%s", s_ns[h]);
    static struct erase_ctx c;
    c.n = 0;
    settings_load_subtree_direct(p, list_cb, &c);
    for (int i = 0; i < c.n; i++) {
        char full[96];
        snprintf(full, sizeof(full), "%s/%s", p, c.names[i]);
        settings_delete(full);
    }
    return ESP_OK;
}
