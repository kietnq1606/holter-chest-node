#define DT_DRV_COMPAT adi_ad8232

#include "ad8232.h"
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>

#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>

/* Fixed hardware configuration from Devicetree */
struct ad8232_config {
    struct adc_dt_spec adc;
    struct gpio_dt_spec lo_plus;
    struct gpio_dt_spec lo_minus;
};
// read 1 ecg sample 
static int ad8232_read_raw_impl(const struct device *dev, int16_t *value)
{
    // get info hw configed
    const struct ad8232_config *config = dev->config;
    // init adc sequence for reading data from adc
    struct adc_sequence sequence = {0};
    int ret;
    // set adc sequence parameters
    ret = adc_sequence_init_dt(&config->adc, &sequence);
    if (ret < 0) {
        return ret;
    }
    // set buffer for adc reading
    sequence.buffer = value;
    // set buffer size for adc reading
    sequence.buffer_size = sizeof(*value);
    // read 1 sample from adc
    ret = adc_read_dt(&config->adc, &sequence);
    return ret;
}

// read LO+ food status
static int ad8232_get_lo_minus_impl(const struct device *dev, bool *lead_off)
{
    // get info hw configed
    const struct ad8232_config *config = dev->config;
    // read LO- status from gpio
    int ret = gpio_pin_get_dt(&config->lo_minus);
    if (ret < 0) {
        return ret;
    }
    *lead_off = (ret == 1);
    return 0;
}

// read LO- food status
static int ad8232_get_lo_plus_impl(const struct device *dev, bool *lead_off)
{
    // get info hw configed
    const struct ad8232_config *config = dev->config;
    // read LO+ status from gpio
    int ret = gpio_pin_get_dt(&config->lo_plus);
    if (ret < 0) {
        return ret;
    }
    *lead_off = (ret == 1);
    return 0;
}

// ad8232 initialization function
static int ad8232_init(const struct device *dev)
{
    // get info hw configed
    const struct ad8232_config *config = dev->config;
    // check if adc is ready
    if (!device_is_ready(config->adc.dev)) {
        return -ENODEV;
    }
    // check if LO+ gpio is ready
    if (!device_is_ready(config->lo_plus.port)) {
        return -ENODEV;
    }
    // check if LO- gpio is ready
    if (!device_is_ready(config->lo_minus.port)) {
        return -ENODEV;
    }
    int ret = adc_channel_setup_dt(&config->adc);
    if (ret < 0) {
        return ret;
    }
    // configure LO+ gpio as input
    ret = gpio_pin_configure_dt(&config->lo_plus, GPIO_INPUT);
    if (ret < 0) {
        return ret;
    }
    // configure LO- gpio as input
    ret = gpio_pin_configure_dt(&config->lo_minus, GPIO_INPUT);
    if (ret < 0) {
        return ret;
    }
    printk("AD8232: Ready!\n")
    return 0;
}   


/*API table*/
static DEVICE_API(ad8232, ad8232_api) = {
    .read_raw = ad8232_read_raw_impl,
    .get_lo_plus = ad8232_get_lo_plus_impl,
    .get_lo_minus = ad8232_get_lo_minus_impl
};


/*device creation*/
#define DRIVER_DEFINE(inst)                                      \
                                                                 \
    static const struct ad8232_config ad8232_config_##inst = {   \
        .adc = ADC_DT_SPEC_INST_GET(inst),                       \
        .lo_plus = GPIO_DT_SPEC_INST_GET(inst, lo_plus_gpios),   \
        .lo_minus = GPIO_DT_SPEC_INST_GET(inst, lo_minus_gpios), \
    };                                                           \
                                                                 \
    DEVICE_DT_INST_DEFINE(                                       \
        inst,                                                    \
        ad8232_init,                                             \
        NULL,                                                    \
        NULL,                                                    \
        &ad8232_config_##inst,                                   \
        POST_KERNEL,                                             \
        51,                                                      \
        &ad8232_api                                              \
    );

DT_INST_FOREACH_STATUS_OKAY(DRIVER_DEFINE)
