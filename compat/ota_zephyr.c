/* ESP-IDF OTA calls (see esp_ota_ops.h) on MCUboot.
 *
 * The app always runs from slot0; an update is written to slot1 and
 * MCUboot swaps it in on the next boot ("test" swap).  If the new firmware
 * doesn't confirm itself (ota_confirm_running) MCUboot swaps back on the
 * following reset, as ESP-IDF's rollback does.  Without MCUboot only the
 * running slot is reported. */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include "esp_ota_ops.h"
#include "esp_log.h"

#if defined(CONFIG_MCUBOOT_IMG_MANAGER)
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#define HAVE_MCUBOOT 1
#endif

static const char *TAG = "ota";

static esp_partition_t s_slot0 = { .type = ESP_PARTITION_TYPE_APP, .label = "ota_0" };
static esp_partition_t s_slot1 = { .type = ESP_PARTITION_TYPE_APP, .label = "ota_1" };

static void init_slots(void)
{
    s_slot0.address = FIXED_PARTITION_OFFSET(slot0_partition);
    s_slot0.size    = FIXED_PARTITION_SIZE(slot0_partition);
    s_slot1.address = FIXED_PARTITION_OFFSET(slot1_partition);
    s_slot1.size    = FIXED_PARTITION_SIZE(slot1_partition);
}

const esp_partition_t *esp_ota_get_running_partition(void)
{
    init_slots();
    return &s_slot0;
}

#if HAVE_MCUBOOT
static struct flash_img_context s_img;
static bool   s_open;
static size_t s_written;
#define IMAGE_MAGIC 0x96f3b83dU

const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *start)
{
    init_slots();
    return &s_slot1;
}

esp_err_t esp_ota_get_state_partition(const esp_partition_t *p, esp_ota_img_states_t *state)
{
    if (p == &s_slot0 && !boot_is_img_confirmed()) *state = ESP_OTA_IMG_PENDING_VERIFY;
    else                                           *state = ESP_OTA_IMG_VALID;
    return ESP_OK;
}

esp_err_t esp_ota_begin(const esp_partition_t *p, size_t size, esp_ota_handle_t *out)
{
    if (s_open) return ESP_ERR_INVALID_STATE;
    /* Erase slot1 up front (also clears the old trailer) */
    int ret = boot_erase_img_bank(FIXED_PARTITION_ID(slot1_partition));
    if (ret) {
        ESP_LOGE(TAG, "erase slot1 failed (%d)", ret);
        return ESP_FAIL;
    }
    ret = flash_img_init_id(&s_img, FIXED_PARTITION_ID(slot1_partition));
    if (ret) return ESP_FAIL;
    s_open = true;
    s_written = 0;
    *out = 1;
    return ESP_OK;
}

esp_err_t esp_ota_write(esp_ota_handle_t h, const void *data, size_t len)
{
    if (!s_open) return ESP_ERR_INVALID_STATE;
    if (s_written == 0 && len >= 4) {
        uint32_t magic;
        memcpy(&magic, data, 4);
        if (magic != IMAGE_MAGIC) return ESP_ERR_OTA_VALIDATE_FAILED;
    }
    if (flash_img_buffered_write(&s_img, data, len, false)) return ESP_FAIL;
    s_written += len;
    return ESP_OK;
}

esp_err_t esp_ota_end(esp_ota_handle_t h)
{
    if (!s_open) return ESP_ERR_INVALID_STATE;
    s_open = false;
    if (flash_img_buffered_write(&s_img, NULL, 0, true)) return ESP_FAIL;

    struct mcuboot_img_header hdr;
    if (boot_read_bank_header(FIXED_PARTITION_ID(slot1_partition), &hdr, sizeof(hdr))) {
        return ESP_ERR_OTA_VALIDATE_FAILED;
    }
    ESP_LOGI(TAG, "New image %u.%u.%u+%u, %u bytes",
             hdr.h.v1.sem_ver.major, hdr.h.v1.sem_ver.minor, hdr.h.v1.sem_ver.revision,
             (unsigned)hdr.h.v1.sem_ver.build_num, (unsigned)hdr.h.v1.image_size);
    return ESP_OK;
}

esp_err_t esp_ota_abort(esp_ota_handle_t h)
{
    s_open = false;
    return ESP_OK;
}

esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p)
{
    if (p != &s_slot1) return ESP_ERR_INVALID_ARG;
    return boot_request_upgrade(BOOT_UPGRADE_TEST) ? ESP_FAIL : ESP_OK;
}

esp_err_t esp_ota_mark_app_valid_cancel_rollback(void)
{
    return boot_write_img_confirmed() ? ESP_FAIL : ESP_OK;
}

#else  /* no MCUboot: report the slot, updates unavailable */

const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *start)
{
    return NULL;
}

esp_err_t esp_ota_get_state_partition(const esp_partition_t *p, esp_ota_img_states_t *state)
{
    *state = ESP_OTA_IMG_VALID;
    return ESP_OK;
}

esp_err_t esp_ota_begin(const esp_partition_t *p, size_t size, esp_ota_handle_t *out) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t esp_ota_write(esp_ota_handle_t h, const void *data, size_t len) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t esp_ota_end(esp_ota_handle_t h) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t esp_ota_abort(esp_ota_handle_t h) { return ESP_OK; }
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void) { return ESP_OK; }
#endif
