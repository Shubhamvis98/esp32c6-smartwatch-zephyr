/**
 * @file hal/battery.c
 * @brief Battery voltage measurement (ADC1 oneshot + curve-fitting calibration).
 */

#include "hal/battery.h"
#include "pinout.h"

#include "esp_log.h"
#include <zephyr/drivers/adc.h>

static const char *TAG = "battery";

#define BAT_DIVIDER        3               // 1/3 resistor divider on the PCB
#define BAT_SAMPLES        16
#define BAT_CHARGING_MV    4250

/* GPIO0 through the 300k/100k divider on the board's "vbat" node */
static const struct adc_dt_spec s_adc = ADC_DT_SPEC_GET_BY_IDX(DT_NODELABEL(vbat), 0);
static bool s_ready;

/* Open-circuit voltage → state of charge for a typical LiPo cell */
static const struct { int mv; uint8_t pct; } s_curve[] = {
    { 4180, 100 }, { 4100, 92 }, { 4000, 80 }, { 3920, 70 }, { 3850, 60 },
    { 3790,  50 }, { 3750, 40 }, { 3710, 30 }, { 3670, 20 }, { 3610, 10 },
    { 3500,   5 }, { 3300,  0 },
};

esp_err_t battery_init(void)
{
    if (!adc_is_ready_dt(&s_adc)) return ESP_ERR_INVALID_STATE;
    if (adc_channel_setup_dt(&s_adc) != 0) {
        ESP_LOGE(TAG, "ADC channel setup failed");
        return ESP_FAIL;
    }
    s_ready = true;
    return ESP_OK;
}

int battery_read_mv(void)
{
    if (!s_ready) return 0;

    int16_t raw;
    struct adc_sequence seq = { .buffer = &raw, .buffer_size = sizeof(raw) };
    adc_sequence_init_dt(&s_adc, &seq);

    int sum = 0, n = 0;
    for (int i = 0; i < BAT_SAMPLES; i++) {
        if (adc_read_dt(&s_adc, &seq) != 0) continue;
        int32_t mv = raw;
        if (adc_raw_to_millivolts_dt(&s_adc, &mv) != 0) continue;
        sum += mv;
        n++;
    }
    return n ? (sum / n) * BAT_DIVIDER : 0;
}

uint8_t battery_percent_from_mv(int mv)
{
    const int last = sizeof(s_curve) / sizeof(s_curve[0]) - 1;
    if (mv >= s_curve[0].mv) return 100;
    if (mv <= s_curve[last].mv) return 0;

    for (int i = 1; i <= last; i++) {
        if (mv >= s_curve[i].mv) {
            int dv = s_curve[i - 1].mv - s_curve[i].mv;
            int dp = s_curve[i - 1].pct - s_curve[i].pct;
            return s_curve[i].pct + (mv - s_curve[i].mv) * dp / dv;
        }
    }
    return 0;
}

bool battery_mv_is_charging(int mv)
{
    return mv >= BAT_CHARGING_MV;
}
