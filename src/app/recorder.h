#pragma once
/**
 * @file app/recorder.h
 * @brief Voice recorder: 16 kHz 16-bit mono WAV files in STORAGE_PATH/rec.
 *
 * Recording runs in the background (it continues with the screen off).
 * About 31 KB per second, so the 11 MB partition holds roughly 5½ minutes.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

#define RECORDER_MAX_FILES   32

typedef struct {
    char     name[16];      // e.g. "R0007.WAV"
    uint32_t duration_ms;
    time_t   created;       // 0 if the clock wasn't set when recorded
} rec_info_t;

esp_err_t recorder_init(void);

esp_err_t recorder_start(void);
/** @brief Stop and finalise the file (blocks briefly while flushing). */
void      recorder_stop(void);
bool      recorder_is_recording(void);
uint32_t  recorder_elapsed_ms(void);
/** @brief Remaining recording time at the current free space, in seconds. */
uint32_t  recorder_seconds_left(void);

/** @brief List recordings, newest first.  Returns the count. */
int  recorder_list(rec_info_t *out, int max);
void recorder_delete(const char *name);
void recorder_play(const char *name);
