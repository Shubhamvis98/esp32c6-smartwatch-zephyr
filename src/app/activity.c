/**
 * @file app/activity.c
 * @brief Software pedometer and raise-to-wake detector.
 *
 * Pedometer: the acceleration magnitude is low-pass filtered and compared
 * against a slow moving average.  Each upward crossing of (mean + threshold)
 * is a step candidate.  Candidates only count once 5 arrive with a natural
 * walking rhythm (0.25–2 s apart), which rejects isolated jolts such as
 * typing or tapping the screen.
 *
 * Raise-to-wake: fires when the display goes from roughly vertical (arm
 * hanging / swinging) to roughly face-up and stays there briefly.
 */

#include "app/activity.h"
#include "hal/imu.h"
#include "hal/rtc.h"

#include <math.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "activity";

#define SAMPLE_PERIOD_MS     20          // 50 Hz

/* Pedometer tuning */
#define STEP_LP_ALPHA        0.30f
#define STEP_MEAN_ALPHA      0.02f
#define STEP_THRESH_G        0.10f
#define STEP_HYST_G          0.05f
#define STEP_MIN_INTERVAL_MS 250
#define STEP_MAX_INTERVAL_MS 2000
#define STEP_RUN_TO_COMMIT   5

/* Built-in raise rule, used until the user calibrates (Settings → Display).
 * On this board the QMI8658 reads z ≈ -1 g with the
 * screen facing up, so face-up is negative z.  If raise-to-wake never
 * triggers (or triggers with the screen facing down), flip the sign. */
#define FACE_UP_Z_SIGN       (-1.0f)
/* Face-up: screen within ~56° of level (z > cos 56°).  Looking at a watch
 * tilts it 40–60° towards you, so stricter limits miss real raises. */
#define RAISE_FACE_UP_Z      0.55f
#define RAISE_FACE_UP_XY     0.85f
#define RAISE_VERTICAL_Z     0.40f
/* Calibrated poses: how close (cosine of the angle) the reading must be */
#define RAISE_CAL_UP_COS     0.87f       // within ~30° of the "looking" pose
#define RAISE_CAL_DOWN_COS   0.77f       // within ~40° of the "arm down" pose
#define RAISE_HOLD_MS        150
#define RAISE_LOOKBACK_MS    1500
#define RAISE_COOLDOWN_MS    2000

#define NVS_NS               "activity"
#define NVS_SAVE_PERIOD_MS   (5 * 60 * 1000)

static volatile uint32_t     s_steps = 0;
static volatile bool         s_raise_enabled = true;
static volatile bool         s_raise_cal = false;
static float                 s_cal_up[3], s_cal_down[3];   // unit vectors
static activity_raise_cb_t   s_raise_cb = NULL;
static uint32_t              s_day = 0;        // local yyyymmdd of s_steps
static bool                  s_dirty = false;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

/* For activity_get_debug(); written by the activity task only */
static volatile imu_accel_t s_dbg_accel;
static volatile bool        s_dbg_face_up;
static volatile int64_t     s_dbg_vertical_at = -1;
static volatile int64_t     s_dbg_raise_at = -1;
static volatile uint32_t    s_dbg_raises;

static uint32_t today_yyyymmdd(void)
{
    if (!rtc_time_valid()) return 0;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}

/* ── Persistence ─────────────────────────────────────────────────────────── */
static void load_steps(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint32_t day = 0, steps = 0;
    nvs_get_u32(h, "day", &day);
    nvs_get_u32(h, "steps", &steps);
    nvs_close(h);

    s_day = today_yyyymmdd();
    if (day != 0 && day == s_day) s_steps = steps;
}

void activity_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, "day", s_day);
    nvs_set_u32(h, "steps", s_steps);
    nvs_commit(h);
    nvs_close(h);
    s_dirty = false;
}

/* ── Pedometer ───────────────────────────────────────────────────────────── */
static void step_update(float mag, int64_t t)
{
    static bool    init = false;
    static float   lp, mean;
    static bool    above = false;
    static int64_t last_step = 0;
    static int     run = 0;

    if (!init) {
        lp = mean = mag;
        init = true;
        return;
    }
    lp   += STEP_LP_ALPHA   * (mag - lp);
    mean += STEP_MEAN_ALPHA * (lp - mean);

    if (!above && lp > mean + STEP_THRESH_G) {
        above = true;
        int64_t dt = t - last_step;
        if (dt < STEP_MIN_INTERVAL_MS) return;

        run = (dt > STEP_MAX_INTERVAL_MS) ? 1 : run + 1;
        last_step = t;

        if (run == STEP_RUN_TO_COMMIT) {
            s_steps += STEP_RUN_TO_COMMIT;   // credit the whole warm-up run
            s_dirty = true;
        } else if (run > STEP_RUN_TO_COMMIT) {
            s_steps++;
            s_dirty = true;
        }
    } else if (above && lp < mean - STEP_HYST_G) {
        above = false;
    }
}

/* ── Raise-to-wake ───────────────────────────────────────────────────────── */
static void raise_update(const imu_accel_t *a, int64_t t)
{
    static int64_t last_vertical = -RAISE_LOOKBACK_MS;
    static int64_t face_up_since = 0;
    static int64_t last_fire = -RAISE_COOLDOWN_MS;

    bool arm_down, face_up;
    if (s_raise_cal) {
        float m = sqrtf(a->x * a->x + a->y * a->y + a->z * a->z);
        if (m < 0.5f) return;                  // free fall / bad sample
        float n[3] = { a->x / m, a->y / m, a->z / m };
        float up   = n[0] * s_cal_up[0]   + n[1] * s_cal_up[1]   + n[2] * s_cal_up[2];
        float down = n[0] * s_cal_down[0] + n[1] * s_cal_down[1] + n[2] * s_cal_down[2];
        arm_down = down > RAISE_CAL_DOWN_COS;
        face_up  = up > RAISE_CAL_UP_COS;
    } else {
        float z  = a->z * FACE_UP_Z_SIGN;
        float xy = sqrtf(a->x * a->x + a->y * a->y);
        arm_down = fabsf(z) < RAISE_VERTICAL_Z;
        face_up  = z > RAISE_FACE_UP_Z && xy < RAISE_FACE_UP_XY;
    }

    if (arm_down) {
        last_vertical = t;
        s_dbg_vertical_at = t;
    }
    s_dbg_face_up = face_up;
    if (!face_up) {
        face_up_since = 0;
        return;
    }
    if (face_up_since == 0) face_up_since = t;

    if (t - face_up_since >= RAISE_HOLD_MS &&
        face_up_since - last_vertical <= RAISE_LOOKBACK_MS &&
        t - last_fire >= RAISE_COOLDOWN_MS) {
        last_fire = t;
        last_vertical = -RAISE_LOOKBACK_MS;   // need a new vertical phase
        s_dbg_raise_at = t;
        s_dbg_raises++;
        if (s_raise_enabled && s_raise_cb) s_raise_cb();
    }
}

/* ── Task ────────────────────────────────────────────────────────────────── */
static void activity_task(void *arg)
{
    TickType_t wake = xTaskGetTickCount();
    int64_t last_save = now_ms();
    int64_t last_day_check = 0;

    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
        int64_t t = now_ms();

        imu_accel_t a;
        if (imu_read_accel(&a) == ESP_OK) {
            float mag = sqrtf(a.x * a.x + a.y * a.y + a.z * a.z);
            step_update(mag, t);
            raise_update(&a, t);
            s_dbg_accel = a;
        }

        if (t - last_day_check >= 1000) {
            last_day_check = t;
            uint32_t day = today_yyyymmdd();
            if (day != 0 && day != s_day) {
                if (s_day != 0) s_steps = 0;   // new day (not first valid time)
                s_day = day;
                s_dirty = true;
            }
        }

        if (s_dirty && t - last_save >= NVS_SAVE_PERIOD_MS) {
            last_save = t;
            activity_save();
        }
    }
}

esp_err_t activity_start(void)
{
    load_steps();
    if (xTaskCreate(activity_task, "activity", 3072, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Activity tracking started (%lu steps today)", (unsigned long)s_steps);
    return ESP_OK;
}

uint32_t activity_get_steps(void)            { return s_steps; }
void activity_set_raise_cb(activity_raise_cb_t cb) { s_raise_cb = cb; }
void activity_set_raise_enabled(bool enabled)      { s_raise_enabled = enabled; }

void activity_get_debug(activity_debug_t *out)
{
    int64_t t = now_ms();
    out->x = s_dbg_accel.x;
    out->y = s_dbg_accel.y;
    out->z = s_dbg_accel.z;
    out->face_up = s_dbg_face_up;
    out->vertical_ago_ms = s_dbg_vertical_at < 0 ? -1 : (int32_t)(t - s_dbg_vertical_at);
    out->raises = s_dbg_raises;
    out->raise_ago_ms = s_dbg_raise_at < 0 ? -1 : (int32_t)(t - s_dbg_raise_at);
}

static bool unit(float out[3], const float v[3])
{
    float m = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (m < 0.5f) return false;
    for (int i = 0; i < 3; i++) out[i] = v[i] / m;
    return true;
}

void activity_set_raise_calibration(const float up[3], const float down[3])
{
    s_raise_cal = false;                       // detector ignores the vectors meanwhile
    if (up && down && unit(s_cal_up, up) && unit(s_cal_down, down)) {
        s_raise_cal = true;
    }
}
