#define DT_DRV_COMPAT bosch_bmi323

#include "bmi323_i2c.h"
#include "bmi323_reg.h"

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>

#define BMI323_CHIP_ID_VALUE       0x43
#define BMI323_I2C_DUMMY_BYTES     2U
#define BMI323_SAMPLE_WORDS        6U
#define BMI323_RESET_DELAY_MS      2U
#define BMI323_SENSOR_STARTUP_MS   50U

struct bmi323_config {
    /* I2C bus and sensor address from devicetree. */
    struct i2c_dt_spec i2c;
    /* Data-ready interrupt pin. */
    struct gpio_dt_spec int1;
};

struct bmi323_data {
    const struct device *dev;
    /* Application callback invoked on data ready. */
    bmi323_callback_t callback;
    struct gpio_callback gpio_cb;
};

/* Read 16-bit registers after the BMI323 I2C dummy bytes. */
static int bmi323_read_words(const struct bmi323_config *config,
                             uint8_t reg,
                             uint16_t *words,
                             size_t word_count)
{
    uint8_t buf[BMI323_I2C_DUMMY_BYTES + (BMI323_SAMPLE_WORDS * sizeof(uint16_t))];
    size_t read_len;
    int ret;

    if ((words == NULL) || (word_count == 0U) || (word_count > BMI323_SAMPLE_WORDS)) {
        return -EINVAL;
    }

    read_len = BMI323_I2C_DUMMY_BYTES + (word_count * sizeof(uint16_t));
    ret = i2c_burst_read_dt(&config->i2c, reg, buf, read_len);
    if (ret != 0) {
        return ret;
    }

    /* BMI323 register words are little-endian. */
    for (size_t i = 0; i < word_count; ++i) {
        words[i] = sys_get_le16(&buf[BMI323_I2C_DUMMY_BYTES + (i * sizeof(uint16_t))]);
    }

    return 0;
}

/* Write one 16-bit register in little-endian order. */
static int bmi323_write_word(const struct bmi323_config *config, uint8_t reg, uint16_t value)
{
    uint8_t buf[3];

    buf[0] = reg;
    sys_put_le16(value, &buf[1]);

    return i2c_write_dt(&config->i2c, buf, sizeof(buf));
}

static void bmi323_gpio_callback(const struct device *port,
                                 struct gpio_callback *cb,
                                 gpio_port_pins_t pins)
{
    struct bmi323_data *data = CONTAINER_OF(cb, struct bmi323_data, gpio_cb);

    ARG_UNUSED(port);
    ARG_UNUSED(pins);

    /* Forward the interrupt to the application callback. */
    if (data->callback != NULL) {
        data->callback(data->dev);
    }
}

/* Read one contiguous accelerometer and gyroscope sample. */
static int bmi323_read_raw_sample_impl(const struct device *dev, struct bmi323_sample *sample)
{
    const struct bmi323_config *config = dev->config;
    uint16_t words[BMI323_SAMPLE_WORDS];
    int ret;

    if (sample == NULL) {
        return -EINVAL;
    }

    ret = bmi323_read_words(config,
                            IMU_BOSCH_BMI323_REG_ACC_DATA_X,
                            words,
                            ARRAY_SIZE(words));
    if (ret != 0) {
        return ret;
    }

    /* 0x8000 indicates that sensor data is not available. */
    for (size_t i = 0; i < ARRAY_SIZE(words); ++i) {
        if (words[i] == 0x8000U) {
            return -ENODATA;
        }
    }

    sample->accel_x = (int16_t)words[0];
    sample->accel_y = (int16_t)words[1];
    sample->accel_z = (int16_t)words[2];
    sample->gyro_x = (int16_t)words[3];
    sample->gyro_y = (int16_t)words[4];
    sample->gyro_z = (int16_t)words[5];

    return 0;
}

static int bmi323_set_data_ready_callback_impl(const struct device *dev,
                                                bmi323_callback_t callback)
{
    struct bmi323_data *data = dev->data;

    data->callback = callback;
    return 0;
}

static int bmi323_init(const struct device *dev)
{
    const struct bmi323_config *config = dev->config;
    struct bmi323_data *data = dev->data;
    uint16_t value;
    int ret;

    data->dev = dev;
    data->callback = NULL;

    if (!i2c_is_ready_dt(&config->i2c)) {
        return -ENODEV;
    }

    if (!gpio_is_ready_dt(&config->int1)) {
        return -ENODEV;
    }

    ret = gpio_pin_configure_dt(&config->int1, GPIO_INPUT);
    if (ret != 0) {
        return ret;
    }

    gpio_init_callback(&data->gpio_cb,
                       bmi323_gpio_callback,
                       BIT(config->int1.pin));

    ret = gpio_add_callback(config->int1.port, &data->gpio_cb);
    if (ret != 0) {
        return ret;
    }

    k_msleep(10);

    /* Verify the device before changing its configuration. */
    ret = bmi323_read_words(config, IMU_BOSCH_BMI323_REG_CHIP_ID, &value, 1U);
    if (ret != 0) {
        printk("BMI323: CHIP_ID read failed (%d)\n", ret);
        return ret;
    }

    printk("BMI323: CHIP_ID=0x%02x\n", (uint8_t)value);

    if ((value & 0x00FFU) != BMI323_CHIP_ID_VALUE) {
        printk("BMI323: invalid CHIP_ID, expected 0x%02x\n", BMI323_CHIP_ID_VALUE);
        return -ENODEV;
    }

    /* Restore the sensor to a known state. */
    ret = bmi323_write_word(config,
                            IMU_BOSCH_BMI323_REG_CMD,
                            IMU_BOSCH_BMI323_REG_CMD_CMD_VAL_SOFT_RESET);
    if (ret != 0) {
        return ret;
    }

    k_msleep(BMI323_RESET_DELAY_MS);

    /* Configure accelerometer for 50 Hz and +/-4 g. */
    value = IMU_BOSCH_BMI323_REG_VALUE(ACC_CONF, ODR, HZ50) |
            IMU_BOSCH_BMI323_REG_VALUE(ACC_CONF, RANGE, G4) |
            IMU_BOSCH_BMI323_REG_VALUE(ACC_CONF, MODE, HPWR);
    ret = bmi323_write_word(config, IMU_BOSCH_BMI323_REG_ACC_CONF, value);
    if (ret != 0) {
        return ret;
    }

    /* Configure gyroscope for 50 Hz and +/-500 dps. */
    value = IMU_BOSCH_BMI323_REG_VALUE(GYRO_CONF, ODR, HZ50) |
            IMU_BOSCH_BMI323_REG_VALUE(GYRO_CONF, RANGE, DPS500) |
            IMU_BOSCH_BMI323_REG_VALUE(GYRO_CONF, MODE, HPWR);
    ret = bmi323_write_word(config, IMU_BOSCH_BMI323_REG_GYRO_CONF, value);
    if (ret != 0) {
        return ret;
    }

    /* Drive INT1 active-high in push-pull mode. */
    value = IMU_BOSCH_BMI323_REG_VALUE(IO_INT_CTRL, INT1_LVL, ACT_HIGH) |
            IMU_BOSCH_BMI323_REG_VALUE(IO_INT_CTRL, INT1_OD, PUSH_PULL) |
            IMU_BOSCH_BMI323_REG_VALUE(IO_INT_CTRL, INT1_OUTPUT_EN, EN);
    ret = bmi323_write_word(config, IMU_BOSCH_BMI323_REG_IO_INT_CTRL, value);
    if (ret != 0) {
        return ret;
    }

    value = IMU_BOSCH_BMI323_REG_VALUE(INT_CONF, INT_LATCH, NON_LATCHED);
    ret = bmi323_write_word(config, IMU_BOSCH_BMI323_REG_INT_CONF, value);
    if (ret != 0) {
        return ret;
    }

    /* Route accelerometer data-ready to INT1. */
    value = IMU_BOSCH_BMI323_REG_VALUE(INT_MAP2, ACC_DRDY_INT, INT1);
    ret = bmi323_write_word(config, IMU_BOSCH_BMI323_REG_INT_MAP2, value);
    if (ret != 0) {
        return ret;
    }

    ret = gpio_pin_interrupt_configure_dt(&config->int1, GPIO_INT_EDGE_TO_ACTIVE);
    if (ret != 0) {
        return ret;
    }

    k_msleep(BMI323_SENSOR_STARTUP_MS);
    printk("BMI323: ready, accel=4g gyro=500dps ODR=50Hz\n");

    return 0;
}

static DEVICE_API(bmi323, bmi323_api) = {
    .read_raw_sample = bmi323_read_raw_sample_impl,
    .set_data_ready_callback = bmi323_set_data_ready_callback_impl,
};

#define BMI323_DEFINE(inst)                                         \
    static struct bmi323_data bmi323_data_##inst;                   \
                                                                    \
    static const struct bmi323_config bmi323_config_##inst = {      \
        .i2c = I2C_DT_SPEC_INST_GET(inst),                          \
        .int1 = GPIO_DT_SPEC_INST_GET(inst, int_gpios),             \
    };                                                              \
                                                                    \
    DEVICE_DT_INST_DEFINE(inst,                                     \
                          bmi323_init,                              \
                          NULL,                                     \
                          &bmi323_data_##inst,                       \
                          &bmi323_config_##inst,                     \
                          POST_KERNEL,                              \
                          52,                                       \
                          &bmi323_api);

DT_INST_FOREACH_STATUS_OKAY(BMI323_DEFINE)
