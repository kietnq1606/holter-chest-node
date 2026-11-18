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
#include "signal_mux.h"

#define AD8232_NODE                DT_NODELABEL(ad8232)
#define AD8232_TEST_START_DELAY_MS 2000U
#define AD8232_TEST_BUF_SAMPLES    50U
#define AD8232_TEST_GET_TIMEOUT    K_MSEC(500)

static char line_buf[384];

int main(void)
{
    const struct device *const dev = DEVICE_DT_GET(AD8232_NODE);
    int16_t local_samples[AD8232_TEST_BUF_SAMPLES];
    uint32_t buf_seq = 0U;
    int err;

    /* Allow time to open the USB console after enumeration. */
    k_msleep(AD8232_TEST_START_DELAY_MS);

    printk("\n========== AD8232 DMA STREAMING TEST ==========\n");

    err = signal_mux_init();
    if (err != 0) {
        printk("[FAIL] Signal mux init failed: err=%d\n", err);
        return err;
    }
    printk("[PASS] Signal mux initialized: Mode 2 (ECG electrodes, SEL_CAL=1, SEL_PULSE=0)\n");

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
    printk("[PASS] AD8232 DMA stream started (500 Hz, 50 samples/buffer = 100 ms/buffer)\n");
    printk("-----------------------------------------------\n\n");

    int64_t stream_start_ms = k_uptime_get();

    while (true) {
        struct ad8232_buffer buf = {0};

        err = ad8232_buffer_get(dev, &buf, AD8232_TEST_GET_TIMEOUT);
        if (err != 0) {
            /* If USB console was not connected yet and caused a temporary
             * backpressure overflow (-ENOBUFS / -EOVERFLOW), restart DMA
             * instead of exiting main() permanently.
             */
            (void)ad8232_stop(dev);
            k_msleep(50);
            (void)ad8232_start(dev);
            continue;
        }

        uint16_t count = MIN(buf.count, (uint16_t)AD8232_TEST_BUF_SAMPLES);

        /* Copy samples out and release the DMA buffer immediately before printing. */
        memcpy(local_samples, buf.samples, count * sizeof(int16_t));

        err = ad8232_buffer_release(dev, &buf);
        if (err != 0) {
            (void)ad8232_stop(dev);
            k_msleep(50);
            (void)ad8232_start(dev);
            continue;
        }

        bool lo_plus = false;
        bool lo_minus = false;

        (void)ad8232_get_lo_plus(dev, &lo_plus);
        (void)ad8232_get_lo_minus(dev, &lo_minus);

        int16_t min_val = local_samples[0];
        int16_t max_val = local_samples[0];
        int32_t sum_val = 0;
        size_t pos = 0U;

        for (uint16_t i = 0U; i < count; i++) {
            int16_t s = local_samples[i];

            if (s < min_val) {
                min_val = s;
            }
            if (s > max_val) {
                max_val = s;
            }
            sum_val += s;

            int written = snprintf(line_buf + pos, sizeof(line_buf) - pos,
                                   "%d%s", s, (i + 1U < count) ? " " : "");
            if (written > 0 && pos + (size_t)written < sizeof(line_buf)) {
                pos += (size_t)written;
            }
        }

        int32_t mean_val = (count > 0U) ? (sum_val / (int32_t)count) : 0;
        bool lead_off = lo_plus || lo_minus;
        uint32_t elapsed_ms = (uint32_t)(k_uptime_get() - stream_start_ms);

        buf_seq++;
        printk("[BUF #%04u | %u ms] LO+=%u LO-=%u (%s) | n=%u | min=%d max=%d mean=%d LSB\n%s\n\n",
               buf_seq,
               elapsed_ms,
               lo_plus ? 1U : 0U,
               lo_minus ? 1U : 0U,
               lead_off ? "LEAD-OFF" : "OK",
               count,
               min_val,
               max_val,
               mean_val,
               line_buf);
    }

    return 0;
}
