#pragma once
/**
 * @file hal/audio.h
 * @brief ES8311 codec + NS4150B amp: simple tone/beep playback.
 *
 * The NS4150B CTRL pin is pulled up on the PCB, so the amp is always on;
 * the codec DAC is muted between sounds to avoid idle hiss.
 *
 * The microphone (16 kHz mono) is read by a background task while
 * audio_mic_start() is in effect: it keeps a running level, and can also
 * copy the raw samples into a stream buffer (the voice recorder).
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

#define AUDIO_SAMPLE_RATE      16000
#define AUDIO_WAV_HEADER_SIZE  44

/** @brief Configure I2S (16 kHz, 16-bit) and the ES8311.  Needs i2c_bus_init(). */
esp_err_t audio_init(void);

/** @brief Set output volume, 0–100. */
void audio_set_volume(uint8_t volume);

/**
 * @brief Queue a beep pattern without blocking.
 * @param freq_hz  tone frequency
 * @param on_ms    tone length
 * @param off_ms   silence between repeats
 * @param repeat   number of tones
 */
void audio_beep(uint16_t freq_hz, uint16_t on_ms, uint16_t off_ms, uint8_t repeat);

/** @brief Stop any beep pattern or WAV that is playing or queued. */
void audio_stop(void);

/** @brief Play a 16 kHz 16-bit mono WAV file (stops anything playing). */
void audio_play_wav(const char *path);

/** @brief Pause or resume the WAV that is playing.  A paused WAV still
 *         counts as playing; any new sound request ends it. */
void audio_pause(bool pause);

/** @brief True while a WAV is paused. */
bool audio_is_paused(void);

/** @brief True while a beep or WAV is playing or queued. */
bool audio_is_playing(void);

/* ── Microphone ──────────────────────────────────────────────────────────── */
/**
 * @brief Start the microphone.  If sink is not NULL, 16-bit mono samples are
 *        also written to it (dropped, and counted, if it is full).
 * @return ESP_ERR_INVALID_STATE if the mic is already in use.
 */
esp_err_t audio_mic_start(StreamBufferHandle_t sink);
void      audio_mic_stop(void);
bool      audio_mic_active(void);
/** Level of the last 20 ms block in dB full scale (−96 … 0). */
float     audio_mic_rms_dbfs(void);
float     audio_mic_peak_dbfs(void);
/** Samples dropped because the sink was full, since the last start. */
uint32_t  audio_mic_dropped(void);
