/**
 * @file hal/storage.c
 * @brief LittleFS on the "data" flash partition, mounted at /data.
 */
#include "hal/storage.h"

#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>
#include "esp_log.h"

static const char *TAG = "storage";

#define DATA_PARTITION  data_partition

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(s_lfs);

static struct fs_mount_t s_mnt = {
    .type        = FS_LITTLEFS,
    .fs_data     = &s_lfs,
    .storage_dev = (void *)FIXED_PARTITION_ID(DATA_PARTITION),
    .mnt_point   = STORAGE_PATH,
};

static bool s_mounted;

esp_err_t storage_init(void)
{
    /* Zephyr's LittleFS formats the partition itself if it can't mount it */
    int r = fs_mount(&s_mnt);
    if (r < 0) {
        ESP_LOGE(TAG, "Mount failed: %d", r);
        return ESP_FAIL;
    }
    s_mounted = true;

    uint64_t free_b = 0, total_b = 0;
    storage_get_space(&free_b, &total_b);
    ESP_LOGI(TAG, "Mounted at %s: %llu KB free of %llu KB", STORAGE_PATH,
             (unsigned long long)(free_b / 1024), (unsigned long long)(total_b / 1024));
    return ESP_OK;
}

esp_err_t storage_get_space(uint64_t *free_bytes, uint64_t *total_bytes)
{
    if (!s_mounted) return ESP_FAIL;
    struct fs_statvfs st;
    if (fs_statvfs(STORAGE_PATH, &st) != 0) return ESP_FAIL;
    if (total_bytes) *total_bytes = (uint64_t)st.f_frsize * st.f_blocks;
    if (free_bytes)  *free_bytes  = (uint64_t)st.f_frsize * st.f_bfree;
    return ESP_OK;
}

esp_err_t storage_format(void)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;
    fs_unmount(&s_mnt);
    s_mounted = false;
    const struct flash_area *fa;
    if (flash_area_open(FIXED_PARTITION_ID(DATA_PARTITION), &fa) == 0) {
        flash_area_flatten(fa, 0, fa->fa_size);
        flash_area_close(fa);
    }
    return storage_init();
}
