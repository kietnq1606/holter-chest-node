#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>

#include "bmi323_i2c.h"
#include "bmi323_reg.h"

#define BMI_TEST_NODE             DT_NODELABEL(bmi323)
#define BMI_TEST_START_DELAY_MS   2000U
#define BMI_TEST_READ_INTERVAL_MS 100U
#define BMI_TEST_DURATION_MS      60000U
#define BMI_TEST_PRINT_EVERY      10U
#define BMI_TEST_CHIP_ID          0x43U

BUILD_ASSERT(BMI_TEST_READ_INTERVAL_MS > 0U);
BUILD_ASSERT(BMI_TEST_DURATION_MS > 0U);
BUILD_ASSERT(BMI_TEST_PRINT_EVERY > 0U);

static const struct device *const imu = DEVICE_DT_GET(BMI_TEST_NODE);
static const struct i2c_dt_spec imu_bus = I2C_DT_SPEC_GET(BMI_TEST_NODE);
static const struct gpio_dt_spec int1 = GPIO_DT_SPEC_GET(BMI_TEST_NODE, int_gpios);

struct axis_stats {
    int64_t sum;
    int16_t min;
    int16_t max;
};

/* Fixed-size accumulators; no sample history or floating-point formatting. */
static struct axis_stats axes[6];
static uint32_t attempts;
static uint32_t valid_reads;
static uint32_t not_ready;
static uint32_t read_errors;

static int check_register(const char *name, uint8_t reg,
                          uint16_t expected, uint16_t mask)
{
    /* Diagnostic reads include the sensor's two I2C dummy bytes. */
    uint8_t bytes[4];
    int err = i2c_burst_read_dt(&imu_bus, reg, bytes, sizeof(bytes));

    if (err != 0) {
        printk("[FAIL] %s read: reg=0x%02x err=%d\n", name, reg, err);
        return err;
    }

    uint16_t actual = sys_get_le16(bytes + 2U);
    bool match = (actual & mask) == (expected & mask);

    printk("[%s] %s expected=0x%04x actual=0x%04x mask=0x%04x\n",
           match ? "PASS" : "FAIL", name, expected, actual, mask);
    return match ? 0 : -EIO;
}

static int check_configuration(void)
{
    const uint16_t acc =
        IMU_BOSCH_BMI323_REG_VALUE(ACC_CONF, ODR, HZ12P5) |
        IMU_BOSCH_BMI323_REG_VALUE(ACC_CONF, RANGE, G4) |
        IMU_BOSCH_BMI323_REG_VALUE(ACC_CONF, MODE, LPWR);
    const uint16_t gyro =
        IMU_BOSCH_BMI323_REG_VALUE(GYRO_CONF, ODR, HZ12P5) |
        IMU_BOSCH_BMI323_REG_VALUE(GYRO_CONF, RANGE, DPS500) |
        IMU_BOSCH_BMI323_REG_VALUE(GYRO_CONF, MODE, LPWR);
    const uint16_t acc_mask =
        IMU_BOSCH_BMI323_REG_MASK(ACC_CONF, ODR) |
        IMU_BOSCH_BMI323_REG_MASK(ACC_CONF, RANGE) |
        IMU_BOSCH_BMI323_REG_MASK(ACC_CONF, MODE);
    const uint16_t gyro_mask =
        IMU_BOSCH_BMI323_REG_MASK(GYRO_CONF, ODR) |
        IMU_BOSCH_BMI323_REG_MASK(GYRO_CONF, RANGE) |
        IMU_BOSCH_BMI323_REG_MASK(GYRO_CONF, MODE);
    int err;

    err = check_register("ACC_CONF", IMU_BOSCH_BMI323_REG_ACC_CONF,
                         acc, acc_mask);
    if (err != 0) {
        return err;
    }
    err = check_register("GYRO_CONF", IMU_BOSCH_BMI323_REG_GYRO_CONF,
                         gyro, gyro_mask);
    if (err != 0) {
        return err;
    }
    return check_register("INT1_OUTPUT_DISABLED",
                          IMU_BOSCH_BMI323_REG_IO_INT_CTRL, 0U,
                          IMU_BOSCH_BMI323_REG_MASK(IO_INT_CTRL, INT1_OUTPUT_EN));
}

static void add_axis(struct axis_stats *stats, int16_t value)
{
    if (valid_reads == 0U || value < stats->min) {
        stats->min = value;
    }
    if (valid_reads == 0U || value > stats->max) {
        stats->max = value;
    }
    stats->sum += value;
}

static void add_sample(const struct bmi323_sample *sample)
{
    add_axis(&axes[0], sample->accel_x);
    add_axis(&axes[1], sample->accel_y);
    add_axis(&axes[2], sample->accel_z);
    add_axis(&axes[3], sample->gyro_x);
    add_axis(&axes[4], sample->gyro_y);
    add_axis(&axes[5], sample->gyro_z);
    valid_reads++;
}

static void print_summary(uint32_t elapsed_ms)
{
    uint32_t failed_reads = not_ready + read_errors;
    uint32_t failure_pct_x100 = attempts == 0U ? 0U :
        (uint32_t)((uint64_t)failed_reads * 10000U / attempts);

    printk("[STAT] elapsed_ms=%u attempts=%u valid=%u not_ready=%u errors=%u\n",
           elapsed_ms, attempts, valid_reads, not_ready, read_errors);
    printk("[STAT] unsuccessful_read_rate=%u.%02u%%\n",
           failure_pct_x100 / 100U, failure_pct_x100 % 100U);
}

static void print_axis_stats(void)
{
    static const char *const names[] = {"AX", "AY", "AZ", "GX", "GY", "GZ"};

    if (valid_reads == 0U) {
        return;
    }
    for (size_t i = 0U; i < ARRAY_SIZE(axes); i++) {
        int mean = (int)(axes[i].sum / valid_reads);
        int peak_to_peak = (int)axes[i].max - (int)axes[i].min;

        printk("[RAW_STAT] %s mean=%d min=%d max=%d peak_to_peak=%d LSB\n",
               names[i], mean, axes[i].min, axes[i].max, peak_to_peak);
    }
}

int main(void)
{
    /* Allow time to open the board's console after USB enumeration. */
    k_msleep(BMI_TEST_START_DELAY_MS);
    printk("\n========== BMI323 POLLING TEST ==========\n");
    printk("[INFO] bus=%s addr=0x%02x clock=%u Hz\n",
           imu_bus.bus->name, imu_bus.addr,
           (unsigned int)DT_PROP(DT_BUS(BMI_TEST_NODE), clock_frequency));
    printk("[INFO] INT1 controller=%s pin=%u; interrupt test skipped\n",
           int1.port->name, int1.pin);

    if (!i2c_is_ready_dt(&imu_bus)) {
        printk("[FAIL] I2C controller not ready\n");
        return -ENODEV;
    }
    int err = check_register("CHIP_ID", IMU_BOSCH_BMI323_REG_CHIP_ID,
                             BMI_TEST_CHIP_ID, 0x00FFU);
    if (err != 0) {
        return err;
    }
    if (!device_is_ready(imu)) {
        printk("[FAIL] BMI323 driver initialization failed; check boot log\n");
        return -ENODEV;
    }
    printk("[PASS] BMI323 device ready\n");

    err = bmi323_set_data_ready_callback(imu, NULL);
    if (err != 0) {
        printk("[FAIL] Callback reset: err=%d\n", err);
        return err;
    }
    err = check_configuration();
    if (err != 0) {
        return err;
    }

    printk("[INFO] accel=+/-4g gyro=+/-500dps ODR=12.5Hz LOW_POWER\n");
    printk("[INFO] duration_ms=%u interval_ms=%u print_every=%u valid reads\n",
           BMI_TEST_DURATION_MS, BMI_TEST_READ_INTERVAL_MS, BMI_TEST_PRINT_EVERY);
    printk("[MANUAL] Keep still for bias, or move one axis at a time for direction\n");

    int64_t start = k_uptime_get();
    int64_t end = start + BMI_TEST_DURATION_MS;

    while (k_uptime_get() < end) {
        struct bmi323_sample sample;

        attempts++;
        err = bmi323_read_raw_sample(imu, &sample);
        if (err == 0) {
            add_sample(&sample);
            if (valid_reads == 1U || valid_reads % BMI_TEST_PRINT_EVERY == 0U) {
                printk("[DATA] read=%u ACC=(%d,%d,%d) GYR=(%d,%d,%d) LSB\n",
                       valid_reads, sample.accel_x, sample.accel_y, sample.accel_z,
                       sample.gyro_x, sample.gyro_y, sample.gyro_z);
            }
        } else if (err == -ENODATA) {
            not_ready++;
            if (not_ready == 1U) {
                printk("[WAIT] Data unavailable (-ENODATA); counted separately\n");
            }
        } else {
            read_errors++;
            if (read_errors == 1U || read_errors % 10U == 0U) {
                printk("[ERROR] Read failed: err=%d total_errors=%u\n",
                       err, read_errors);
            }
        }

        int64_t remaining = end - k_uptime_get();

        if (remaining > 0) {
            k_sleep(K_MSEC(MIN(remaining, (int64_t)BMI_TEST_READ_INTERVAL_MS)));
        }
    }

    printk("========== BMI323 TEST SUMMARY ==========\n");
    print_summary((uint32_t)(k_uptime_get() - start));
    print_axis_stats();
    printk("[MANUAL] Means describe bias only if board stayed stationary\n");

    if (valid_reads == 0U || read_errors != 0U) {
        printk("[FAIL] No valid data or read errors observed\n");
        return -EIO;
    }
    if (not_ready != 0U) {
        printk("[WARN] Valid data received, but some reads returned -ENODATA\n");
        return -ENODATA;
    }
    printk("[PASS] Configuration matches and polling completed without read errors\n");
    printk("[MANUAL] Accuracy, axis direction and actual ODR require separate checks\n");
    return 0;
}
