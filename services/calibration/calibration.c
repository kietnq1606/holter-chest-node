#include "calibration.h"
#include "ad8232.h"
#include "signal_mux.h"

#include <errno.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>

#define CAL_PERIOD_MS       2U
#define CAL_TOTAL_SAMPLES   3500U
#define CAL_SKIP_SAMPLES    1000U
#define CAL_USED_SAMPLES    (CAL_TOTAL_SAMPLES - CAL_SKIP_SAMPLES)
#define CAL_REFERENCE_UV    1000U /* Nominal reference step. */
#define CAL_ADC_MAX         4095
#define CAL_ADC_MARGIN      8
#define CAL_MIN_AMP_COUNTS  10U


/* Half-cycle coefficients scaled by 1000, at 50 samples per cycle. */
static const int16_t cal_sin[25] = {
       0, 125, 249, 368, 482, 588, 685, 771, 844, 905,
     951, 982, 998, 998, 982, 951, 905, 844, 771, 685,
     588, 482, 368, 249, 125
};
static const int16_t cal_cos[25] = {
    1000, 992, 969, 930, 876, 809, 729, 637, 536, 426,
     309, 187, 63, -63, -187, -309, -426, -536, -637, -729,
    -809, -876, -930, -969, -992
};

static uint32_t cal_isqrt(uint64_t value)
{
    uint64_t root = 0;
    uint64_t bit = UINT64_C(1) << 62;

    while (bit > value) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)root;
}

/* Run before workers; the caller exclusively owns ADC and mux. */
int ecg_calibrate(uint32_t *coefficient)
{
    const struct device *ecg = DEVICE_DT_GET(DT_NODELABEL(ad8232));

    if (coefficient == NULL) {
        return -EINVAL;
    }
    int64_t sum_sin = 0;
    int64_t sum_cos = 0;
    uint32_t computed = 0;
    int err;

    if (!device_is_ready(ecg)) {
        return -ENODEV;
    }
    err = signal_mux_init();
    if (err != 0) {
        return err;
    }
    err = signal_mux_mode0();
    if (err != 0) {
        goto restore;
    }

    const int64_t start = k_uptime_get();

    for (uint16_t n = 0; n < CAL_TOTAL_SAMPLES; ++n) {
        const int64_t target = start + (int64_t)n * CAL_PERIOD_MS;
        int64_t remaining = target - k_uptime_get();

        if (remaining > 0) {
            k_sleep(K_MSEC(remaining));
        }
        /* Reject missed slots instead of collecting delayed sample bursts. */
        if (k_uptime_get() >= target + CAL_PERIOD_MS) {
            err = -ETIMEDOUT;
            goto restore;
        }

        const uint8_t phase = n % 50U;
        if (phase == 0U && n != 0U) {
            err = signal_mux_mode0();
        } else if (phase == 25U) {
            err = signal_mux_mode1();
        }
        if (err != 0) {
            goto restore;
        }

        int16_t raw;
        err = ad8232_read_raw(ecg, &raw);
        if (err != 0) {
            goto restore;
        }
        if (k_uptime_get() >= target + CAL_PERIOD_MS) {
            err = -ETIMEDOUT;
            goto restore;
        }
        if (n < CAL_SKIP_SAMPLES) {
            continue;
        }
        if (raw <= CAL_ADC_MARGIN || raw >= CAL_ADC_MAX - CAL_ADC_MARGIN) {
            err = -ERANGE;
            goto restore;
        }

        const uint8_t index = phase % 25U;
        const int32_t sign = phase < 25U ? 1 : -1;
        sum_sin += (int64_t)raw * cal_sin[index] * sign;
        sum_cos += (int64_t)raw * cal_cos[index] * sign;
    }

    /* Finish the final excitation interval for a nominal seven-second run. */
    {
        int64_t remaining = start + (int64_t)CAL_TOTAL_SAMPLES *
                            CAL_PERIOD_MS - k_uptime_get();
        if (remaining > 0) {
            k_sleep(K_MSEC(remaining));
        }
    }

    /* Normalize before squaring; amplitude uses ADC counts scaled by 1000. */
    const int64_t s = 2 * sum_sin / CAL_USED_SAMPLES;
    const int64_t c = 2 * sum_cos / CAL_USED_SAMPLES;
    const uint32_t amplitude = cal_isqrt(
        (uint64_t)(s * s) + (uint64_t)(c * c));

    if (amplitude < CAL_MIN_AMP_COUNTS * 1000U) {
        err = -ENODATA;
        goto restore;
    }

    /* Input fundamental = reference step * 2/pi; 2/pi ~= 0.636620. */
    computed = (uint32_t)(
        ((uint64_t)CAL_REFERENCE_UV * 636620U * 65536U) /
        ((uint64_t)amplitude * 1000U));
    err = computed != 0U ? 0 : -ERANGE;

restore:
    {
        int restore_err = signal_mux_mode2();
        if (restore_err != 0) {
            return restore_err;
        }
    }
    if (err == 0) {
        *coefficient = computed;
    }
    return err;
}
