/**
 * @file hal/button.c
 * @brief Press / double / hold detection for the PWR and BOOT buttons.
 *        One polling task for both, so no ISR or extra component needed.
 */

#include "hal/button.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define POLL_MS        10
#define DEBOUNCE_MS    30
#define HOLD_MS        1000
#define LONG_HOLD_MS   5000
#define DOUBLE_GAP_MS  300     // a second press must start within this

typedef struct {
    bool          active;
    gpio_num_t    pin;
    volatile bool double_on;
    bool          stable, raw_last, ignore;   // ignore: held since start-up
    int64_t       raw_since, press_start, release_at;
    uint8_t       held;                       // 0, 1 = hold fired, 2 = long hold fired
    int           clicks;                     // short presses not yet reported
} button_t;

static button_t     s_btn[BUTTON_COUNT];
static button_cb_t  s_cb;
static TaskHandle_t s_task;

static inline int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void emit(button_id_t id, button_event_t ev)
{
    if (s_cb) s_cb(id, ev);
}

static void poll(button_id_t id, button_t *b, int64_t t)
{
    bool raw = gpio_get_level(b->pin) == 0;
    if (raw != b->raw_last) {
        b->raw_last = raw;
        b->raw_since = t;
    }

    if (raw != b->stable && t - b->raw_since >= DEBOUNCE_MS) {
        b->stable = raw;
        if (b->stable) {
            b->press_start = t;
            b->held = 0;
        } else if (b->ignore) {
            b->ignore = false;                 // the press that powered us on
        } else if (!b->held) {
            b->release_at = t;
            if (!b->double_on) {
                emit(id, BUTTON_CLICK);
            } else if (++b->clicks == 2) {
                b->clicks = 0;
                emit(id, BUTTON_DOUBLE);
            }
        }
    }

    if (b->stable && !b->ignore) {
        if (b->held == 0 && t - b->press_start >= HOLD_MS) {
            b->held = 1;
            b->clicks = 0;                     // a hold cancels a pending click
            emit(id, BUTTON_HOLD);
        } else if (b->held == 1 && t - b->press_start >= LONG_HOLD_MS) {
            b->held = 2;
            emit(id, BUTTON_LONG_HOLD);
        }
    }

    if (b->clicks == 1 && !b->stable && t - b->release_at >= DOUBLE_GAP_MS) {
        b->clicks = 0;
        emit(id, BUTTON_CLICK);
    }
}

static void button_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        int64_t t = now_ms();
        for (int i = 0; i < BUTTON_COUNT; i++) {
            if (s_btn[i].active) poll((button_id_t)i, &s_btn[i], t);
        }
    }
}

void button_set_double(button_id_t id, bool enabled)
{
    if (id < BUTTON_COUNT) s_btn[id].double_on = enabled;
}

esp_err_t button_start(button_id_t id, gpio_num_t pin, button_cb_t cb)
{
    if (id >= BUTTON_COUNT) return ESP_ERR_INVALID_ARG;
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) return err;

    button_t *b = &s_btn[id];
    bool down = gpio_get_level(pin) == 0;
    b->pin = pin;
    b->stable = b->raw_last = b->ignore = down;
    b->raw_since = now_ms();
    s_cb = cb;
    b->active = true;

    if (!s_task && xTaskCreate(button_task, "buttons", 3072, NULL, 5, &s_task) != pdPASS) {
        b->active = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
