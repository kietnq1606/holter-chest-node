#include "calibration.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

int main(void)
{
    uint32_t coefficient;

    printk("AD8232 calibration: 10 Hz, 500 samples/s, 7 s (skip 2 s)\n");
    int err = ecg_calibrate(&coefficient);
    if (err != 0) {
        printk("Calibration failed: %d\n", err);
        return err;
    }

    printk("uv_per_count_q16=%u\n", (unsigned int)coefficient);
    uint32_t fractional = (uint32_t)(
        (uint64_t)(coefficient & 0xffffU) * 100000U / 65536U);
    printk("Scale=%u.%05u uV/count\n",
           (unsigned int)(coefficient >> 16), (unsigned int)fractional);

    /* Nominal 12-bit ADC: 0.6 V reference and gain 1/6, as in overlay. */
    uint32_t gain_milli = (uint32_t)(
        UINT64_C(3600000) * 65536U * 1000U /
        (UINT64_C(4096) * coefficient));
    printk("Gain at 10 Hz (nominal ADC scale)=%u.%03u V/V\n",
           (unsigned int)(gain_milli / 1000U),
           (unsigned int)(gain_milli % 1000U));
    printk("ECG mode restored. Result is in RAM only.\n");
    return 0;
}
