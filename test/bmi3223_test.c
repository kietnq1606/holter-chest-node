#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "bmi323_i2c.h"

#define BMI323_PRINT_INTERVAL 50U

K_SEM_DEFINE(bmi323_data_ready, 0, 1);

static void bmi323_data_ready_callback(const struct device *dev)
{
    ARG_UNUSED(dev);

    /* Defer the I2C read to thread context. */
    k_sem_give(&bmi323_data_ready);
}

int main(void)
{
    const struct device *imu = DEVICE_DT_GET(DT_NODELABEL(bmi323));
    struct bmi323_sample sample;
    uint32_t sample_count = 0U;
    int ret;

    if (!device_is_ready(imu)) {
        printk("BMI323 test: device not ready\n");
        return 0;
    }

    ret = bmi323_set_data_ready_callback(imu, bmi323_data_ready_callback);
    if (ret != 0) {
        printk("BMI323 test: callback setup failed (%d)\n", ret);
        return 0;
    }

    printk("BMI323 test: waiting for INT1 data-ready\n");

    while (true) {
        k_sem_take(&bmi323_data_ready, K_FOREVER);

        ret = bmi323_read_raw_sample(imu, &sample);
        if (ret != 0) {
            printk("BMI323 test: read failed (%d)\n", ret);
            continue;
        }

        sample_count++;
        if ((sample_count % BMI323_PRINT_INTERVAL) == 0U) {
            printk("BMI323 raw: acc=%d,%d,%d gyro=%d,%d,%d samples=%u\n",
                   sample.accel_x,
                   sample.accel_y,
                   sample.accel_z,
                   sample.gyro_x,
                   sample.gyro_y,
                   sample.gyro_z,
                   sample_count);
        }
    }

    return 0;
}
