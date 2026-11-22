#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "ad8232.h"
#include "debug_io.h"
#include "signal_mux.h"

#define AD8232_NODE                DT_NODELABEL(ad8232)
#define AD8232_TEST_START_DELAY_MS 2000U
#define AD8232_CHANNEL_COUNT       2U
#define AD8232_FRAME_SAMPLES       50U
#define AD8232_TEST_BUF_SAMPLES    (AD8232_FRAME_SAMPLES * AD8232_CHANNEL_COUNT)
#define AD8232_TEST_GET_TIMEOUT    K_MSEC(500)

static char raw_line_buf[384];
static char filt_line_buf[384];

#define MUX_MODE_COUNT   4U
#define MUX_MODE_DEFAULT 2U /* ECG, used when no switch is on */

/* Index = mux mode = switch index (SW1 -> mode 0 ... SW4 -> mode 3). */
static int (*const mux_mode_set[MUX_MODE_COUNT])(void) = {
    signal_mux_mode0, signal_mux_mode1, signal_mux_mode2, signal_mux_mode3,
};
static const char *const mux_mode_desc[MUX_MODE_COUNT] = {
    "0 mV, SEL_CAL=0 SEL_PULSE=0",
    "+1 mV, SEL_CAL=0 SEL_PULSE=1",
    "ECG electrodes, SEL_CAL=1 SEL_PULSE=0",
    "no function, SEL_CAL=1 SEL_PULSE=1",
};

/* Select the mux mode from switches; SW1 has the highest priority. */
static void mux_apply_switches(unsigned int *cur_mode)
{
    unsigned int mode = MUX_MODE_DEFAULT;
    unsigned int sw = 0U; /* 0: no switch on */

    /* if-else chain: first switch that is on wins (SW1 > SW2 > SW3 > SW4). */
    for (unsigned int i = 0U; i < MUX_MODE_COUNT; i++) {
        bool on;

        if (debug_sw_is_on((enum debug_sw_id)i, &on) != 0) {
            return; /* Keep the current mode on read error. */
        }
        if (on) {
            mode = i;
            sw = i + 1U;
            break;
        }
    }
    if (mode == *cur_mode) {
        return;
    }

    int err = mux_mode_set[mode]();

    if (err != 0) {
        /* Retried on the next buffer. */
        printk("[FAIL] Signal mux mode %u set failed: err=%d\n", mode, err);
        return;
    }
    *cur_mode = mode;
    /* "[INFO]" prefix is skipped by virtualize_MultiEcgSignal.py. */
    if (sw != 0U) {
        printk("[INFO] Mux mode %u (%s) by SW%u\n", mode, mux_mode_desc[mode], sw);
    } else {
        printk("[INFO] Mux mode %u (%s) by default (no SW on)\n", mode, mux_mode_desc[mode]);
    }
}

int main(void)
{
    const struct device *const dev = DEVICE_DT_GET(AD8232_NODE);
    int16_t local_samples[AD8232_TEST_BUF_SAMPLES];
    uint32_t buf_seq = 0U;
    int err;

    /* Allow time to open the USB console after enumeration. */
    k_msleep(AD8232_TEST_START_DELAY_MS);

    printk("\n========== AD8232 RAW vs FILTER DMA STREAMING TEST ==========\n");

    err = signal_mux_init();
    if (err != 0) {
        printk("[FAIL] Signal mux init failed: err=%d\n", err);
        return err;
    }
    printk("[PASS] Signal mux initialized: Mode 2 (ECG electrodes, SEL_CAL=1, SEL_PULSE=0)\n");

    err = debug_io_init();
    if (err != 0) {
        printk("[FAIL] Debug IO init failed: err=%d\n", err);
        return err;
    }
    unsigned int mux_mode = MUX_MODE_DEFAULT;

    /* Apply switch state before streaming so the first buffer uses it. */
    mux_apply_switches(&mux_mode);

    if (!device_is_ready(dev)) {
        printk("[FAIL] AD8232 device not ready\n");
        return -ENODEV;
    }
    printk("[PASS] AD8232 device ready\n");

    err = ad8232_start(dev);
    if (err != 0) {
        printk("[FAIL] AD8232 stream start failed: err=%d\n", err);
        return err;
    }
    printk("[PASS] AD8232 dual-channel DMA started (500 Hz, 50 pairs = 100 samples/buffer)\n");
    printk("-------------------------------------------------------------\n\n");

    int64_t stream_start_ms = k_uptime_get();

    while (true) {
        struct ad8232_buffer buf = {0};

        err = ad8232_buffer_get(dev, &buf, AD8232_TEST_GET_TIMEOUT);
        if (err != 0) {
            /* Recover from temporary USB console backpressure overflow. */
            (void)ad8232_stop(dev);
            k_msleep(50);
            (void)ad8232_start(dev);
            continue;
        }

        uint16_t total_count = MIN(buf.count, (uint16_t)AD8232_TEST_BUF_SAMPLES);
        uint16_t pairs = total_count / AD8232_CHANNEL_COUNT;

        /* Copy interleaved [Raw, Filter] samples out and release DMA buffer immediately. */
        memcpy(local_samples, buf.samples, total_count * sizeof(int16_t));

        err = ad8232_buffer_release(dev, &buf);
        if (err != 0) {
            (void)ad8232_stop(dev);
            k_msleep(50);
            (void)ad8232_start(dev);
            continue;
        }

        if (pairs == 0U) {
            continue;
        }

        bool lo_plus = false;
        bool lo_minus = false;

        (void)ad8232_get_lo_plus(dev, &lo_plus);
        (void)ad8232_get_lo_minus(dev, &lo_minus);

        int16_t raw_min = local_samples[0];
        int16_t raw_max = local_samples[0];
        int32_t raw_sum = 0;

        int16_t filt_min = local_samples[1];
        int16_t filt_max = local_samples[1];
        int32_t filt_sum = 0;

        size_t raw_pos = 0U;
        size_t filt_pos = 0U;

        for (uint16_t i = 0U; i < pairs; i++) {
            int16_t raw_s = local_samples[2U * i];
            int16_t filt_s = local_samples[2U * i + 1U];

            if (raw_s < raw_min) {
                raw_min = raw_s;
            }
            if (raw_s > raw_max) {
                raw_max = raw_s;
            }
            raw_sum += raw_s;

            if (filt_s < filt_min) {
                filt_min = filt_s;
            }
            if (filt_s > filt_max) {
                filt_max = filt_s;
            }
            filt_sum += filt_s;

            int w_raw = snprintf(raw_line_buf + raw_pos,
                                 sizeof(raw_line_buf) - raw_pos,
                                 "%d%s", raw_s, (i + 1U < pairs) ? " " : "");
            if (w_raw > 0 && raw_pos + (size_t)w_raw < sizeof(raw_line_buf)) {
                raw_pos += (size_t)w_raw;
            }

            int w_filt = snprintf(filt_line_buf + filt_pos,
                                  sizeof(filt_line_buf) - filt_pos,
                                  "%d%s", filt_s, (i + 1U < pairs) ? " " : "");
            if (w_filt > 0 && filt_pos + (size_t)w_filt < sizeof(filt_line_buf)) {
                filt_pos += (size_t)w_filt;
            }
        }

        int32_t raw_mean = raw_sum / (int32_t)pairs;
        int32_t filt_mean = filt_sum / (int32_t)pairs;
        bool lead_off = lo_plus || lo_minus;
        uint32_t elapsed_ms = (uint32_t)(k_uptime_get() - stream_start_ms);

        buf_seq++;
        printk("[BUF #%04u | %u ms] LO+=%u LO-=%u (%s) | n=%u | "
               "RAW(min=%d max=%d mean=%d) | FILT(min=%d max=%d mean=%d) LSB\n",
               buf_seq,
               elapsed_ms,
               lo_plus ? 1U : 0U,
               lo_minus ? 1U : 0U,
               lead_off ? "LEAD-OFF" : "OK",
               pairs,
               raw_min, raw_max, raw_mean,
               filt_min, filt_max, filt_mean);
        printk("RAW: %s\n", raw_line_buf);
        printk("FILT: %s\n\n", filt_line_buf);

        /* Poll switches once per buffer (~100 ms, also debounces). */
        mux_apply_switches(&mux_mode);
    }

    return 0;
}
