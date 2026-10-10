/**
 * @file app/recorder.c
 * @brief WAV voice recorder.
 *
 *   mic task ──► stream buffer (1 s) ──► writer task ──► LittleFS file
 *
 * The stream buffer absorbs flash erase stalls so no samples are lost.
 * The WAV header is written once with "unknown length" sizes and never
 * patched: on LittleFS, rewriting the start of a file copies the whole file,
 * which stalled the stop for seconds.  Lengths come from the file size.
 * Files are numbered R0001.WAV, R0002.WAV, …
 */

#include "app/recorder.h"
#include "hal/audio.h"
#include "hal/storage.h"
#include "hal/rtc.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

static const char *TAG = "recorder";

#define REC_DIR            STORAGE_PATH "/rec"
#define BYTES_PER_SEC      (AUDIO_SAMPLE_RATE * 2)
#define STREAM_BYTES       (BYTES_PER_SEC / 2)        // 0.5 s of slack (RAM is tight on Zephyr)
#define WRITE_CHUNK        4096
#define MIN_FREE_BYTES     (64 * 1024)
#define MAX_RECORD_MS      (30 * 60 * 1000)
#define PATH_LEN           (sizeof(REC_DIR) + 1 + 256)   // dir + "/" + longest file name

static StreamBufferHandle_t s_sb;
static TaskHandle_t         s_writer;
static SemaphoreHandle_t    s_done;
static FILE                *s_file;
static volatile bool        s_recording;
static volatile bool        s_stop_req;
static uint32_t             s_data_bytes;
static int64_t              s_start_us;
static char                 s_cur_name[16];           // file being recorded

/* ── WAV header ──────────────────────────────────────────────────────────── */
static void put_le32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put_le16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

/* data_bytes 0xFFFFFFFF means "unknown length", as streaming encoders do */
static void wav_header(uint8_t h[AUDIO_WAV_HEADER_SIZE], uint32_t data_bytes)
{
    memcpy(h, "RIFF", 4);
    put_le32(h + 4, data_bytes == UINT32_MAX ? UINT32_MAX : 36 + data_bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_le32(h + 16, 16);                  // fmt chunk size
    put_le16(h + 20, 1);                   // PCM
    put_le16(h + 22, 1);                   // mono
    put_le32(h + 24, AUDIO_SAMPLE_RATE);
    put_le32(h + 28, BYTES_PER_SEC);
    put_le16(h + 32, 2);                   // block align
    put_le16(h + 34, 16);                  // bits per sample
    memcpy(h + 36, "data", 4);
    put_le32(h + 40, data_bytes);
}

/* ── Writer ──────────────────────────────────────────────────────────────── */
static void writer_task(void *arg)
{
    static uint8_t buf[WRITE_CHUNK];

    for (;;) {
        size_t n = xStreamBufferReceive(s_sb, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (n) {
            if (fwrite(buf, 1, n, s_file) != n) {
                ESP_LOGE(TAG, "Write failed (storage full?)");
                s_stop_req = true;
            } else {
                s_data_bytes += n;
            }
        }

        if (!s_stop_req) {
            uint64_t free_b = 0, total_b = 0;
            int64_t ms = (esp_timer_get_time() - s_start_us) / 1000;
            if (ms >= MAX_RECORD_MS ||
                (s_data_bytes % (64 * 1024) < WRITE_CHUNK &&
                 storage_get_space(&free_b, &total_b) == ESP_OK && free_b < MIN_FREE_BYTES)) {
                ESP_LOGW(TAG, "Stopping: %s", ms >= MAX_RECORD_MS ? "time limit" : "storage full");
                s_stop_req = true;
            }
        }

        /* After stop: keep draining what's left, then finish */
        if (s_stop_req && !audio_mic_active() && xStreamBufferIsEmpty(s_sb)) break;
        if (s_stop_req && audio_mic_active()) audio_mic_stop();
    }

    fclose(s_file);
    s_file = NULL;

    ESP_LOGI(TAG, "Saved %lu bytes (%lu ms), %lu samples dropped",
             (unsigned long)s_data_bytes, (unsigned long)(s_data_bytes * 1000ull / BYTES_PER_SEC),
             (unsigned long)audio_mic_dropped());

    vStreamBufferDelete(s_sb);
    s_sb = NULL;
    s_recording = false;
    s_writer = NULL;
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

/* ── Files ───────────────────────────────────────────────────────────────── */
static int next_number(void)
{
    int max = 0;
    DIR *d = opendir(REC_DIR);
    if (!d) return 1;
    struct dirent *e;
    while ((e = readdir(d))) {
        int n;
        if (sscanf(e->d_name, "R%4d.WAV", &n) == 1 && n > max) max = n;
    }
    closedir(d);
    return max + 1;
}

esp_err_t recorder_init(void)
{
    s_done = xSemaphoreCreateBinary();
    if (!s_done) return ESP_ERR_NO_MEM;
    if (mkdir(REC_DIR, 0775) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir %s: errno %d (%s)", REC_DIR, errno, strerror(errno));
    }
    return ESP_OK;
}

esp_err_t recorder_start(void)
{
    if (s_recording) return ESP_ERR_INVALID_STATE;

    uint64_t free_b = 0, total_b = 0;
    if (storage_get_space(&free_b, &total_b) != ESP_OK) return ESP_ERR_INVALID_STATE;
    if (free_b < MIN_FREE_BYTES * 2) return ESP_ERR_NO_MEM;

    audio_stop();                                       // no playback while recording

    int num = next_number();
    if (num < 1) num = 1;
    if (num > 9999) num = 9999;                         // names are R0001 … R9999
    snprintf(s_cur_name, sizeof(s_cur_name), "R%04d.WAV", num);
    char path[PATH_LEN];
    snprintf(path, sizeof(path), REC_DIR "/%s", s_cur_name);
    s_file = fopen(path, "wb");
    if (!s_file) {
        ESP_LOGE(TAG, "Cannot create %s: errno %d (%s)", path, errno, strerror(errno));
        return ESP_FAIL;
    }
    uint8_t h[AUDIO_WAV_HEADER_SIZE];
    wav_header(h, UINT32_MAX);
    fwrite(h, 1, sizeof(h), s_file);

    s_sb = xStreamBufferCreate(STREAM_BYTES, 1);
    if (!s_sb) {
        fclose(s_file);
        unlink(path);
        return ESP_ERR_NO_MEM;
    }

    s_data_bytes = 0;
    s_stop_req = false;
    s_start_us = esp_timer_get_time();
    s_recording = true;

    esp_err_t err = audio_mic_start(s_sb);
    if (err == ESP_OK &&
        xTaskCreate(writer_task, "rec_writer", 4096, NULL, 4, &s_writer) != pdPASS) {
        audio_mic_stop();
        err = ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) {
        s_recording = false;
        vStreamBufferDelete(s_sb);
        s_sb = NULL;
        fclose(s_file);
        s_file = NULL;
        unlink(path);
        return err;
    }
    ESP_LOGI(TAG, "Recording to %s", path);
    return ESP_OK;
}

void recorder_stop(void)
{
    if (!s_recording) return;
    s_stop_req = true;
    xSemaphoreTake(s_done, pdMS_TO_TICKS(5000));
}

bool recorder_is_recording(void)
{
    return s_recording;
}

uint32_t recorder_elapsed_ms(void)
{
    return s_recording ? (uint32_t)((esp_timer_get_time() - s_start_us) / 1000) : 0;
}

uint32_t recorder_seconds_left(void)
{
    uint64_t free_b = 0, total_b = 0;
    if (storage_get_space(&free_b, &total_b) != ESP_OK || free_b < MIN_FREE_BYTES) return 0;
    return (uint32_t)((free_b - MIN_FREE_BYTES) / BYTES_PER_SEC);
}

static int newest_first(const void *a, const void *b)
{
    return strcmp(((const rec_info_t *)b)->name, ((const rec_info_t *)a)->name);
}

int recorder_list(rec_info_t *out, int max)
{
    DIR *d = opendir(REC_DIR);
    if (!d) return 0;

    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        int num;
        if (sscanf(e->d_name, "R%4d.WAV", &num) != 1) continue;
        /* Skip the file being written: it has no final header yet */
        if (s_recording && strcmp(s_cur_name, e->d_name) == 0) continue;

        char path[PATH_LEN];
        snprintf(path, sizeof(path), REC_DIR "/%s", e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;

        rec_info_t *r = &out[n++];
        strlcpy(r->name, e->d_name, sizeof(r->name));
        uint32_t data = st.st_size > AUDIO_WAV_HEADER_SIZE ? st.st_size - AUDIO_WAV_HEADER_SIZE : 0;
        r->duration_ms = (uint32_t)(data * 1000ull / BYTES_PER_SEC);
        /* Files written before the clock was set have a 1970-ish time */
        r->created = st.st_mtime > 1600000000 ? st.st_mtime : 0;
    }
    closedir(d);

    qsort(out, n, sizeof(out[0]), newest_first);
    return n;
}

void recorder_delete(const char *name)
{
    char path[PATH_LEN];
    snprintf(path, sizeof(path), REC_DIR "/%s", name);
    audio_stop();
    unlink(path);
}

void recorder_play(const char *name)
{
    if (s_recording) return;
    char path[PATH_LEN];
    snprintf(path, sizeof(path), REC_DIR "/%s", name);
    audio_play_wav(path);
}
