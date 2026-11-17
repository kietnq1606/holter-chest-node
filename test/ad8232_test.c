#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include "ad8232.h"

static const struct device *const ad8232_dev = DEVICE_DT_GET(DT_NODELABEL(ad8232));

int main(void)
{

    int16_t ecg;
    bool lo_plus;
    bool lo_minus;

    if (!device_is_ready(ad8232_dev)) {
        return 0;
    }

    while (1) {
        // Read ECG sample
        if (ad8232_read_raw(dev, &ecg) == 0) {
            printk("ECG Sample: %d\n\r", ecg);
        }
        // Read lead-off status
        if (ad8232_get_lo_plus(dev, &lo_plus) == 0) {
            printk("LO+ Status: %d\n\r", lo_plus);
        }
        if (ad8232_get_lo_minus(dev, &lo_minus) == 0) {
            printk("LO- Status: %d\n\r", lo_minus);
        }
        k_msleep(2); // Delay for 5 seconds
    }

    return 0;
}
