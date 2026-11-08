#include "signal_mux.h"

#include <errno.h>
#include <stdbool.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>

enum signal_mux_mode {
    SIGNAL_MUX_CAL_ZERO = 0,
    SIGNAL_MUX_CAL_HIGH,
    SIGNAL_MUX_ECG,
};

#define USER_NODE DT_PATH(zephyr_user)

static const struct gpio_dt_spec sel_cal = GPIO_DT_SPEC_GET(USER_NODE, sel_cal_gpios);
static const struct gpio_dt_spec sel_pulse = GPIO_DT_SPEC_GET(USER_NODE, sel_pulse_gpios);

static bool initialized;

int signal_mux_init(void)
{
    int err;

    if (initialized) {
        err = 0;
        goto out;
    }
    if (!gpio_is_ready_dt(&sel_cal) || !gpio_is_ready_dt(&sel_pulse)) {
        err = -ENODEV;
        goto out;
    }

    /* Select electrodes before configuring the calibration reference. */
    err = gpio_pin_configure_dt(&sel_cal, GPIO_OUTPUT_ACTIVE);
    if (err != 0) {
        goto out;
    }
    err = gpio_pin_configure_dt(&sel_pulse, GPIO_OUTPUT_INACTIVE);
    if (err != 0) {
        goto out;
    }
    initialized = true;
out:
    return err;
}

static int signal_mux_set_mode(enum signal_mux_mode mode)
{
    int err;

    if (mode != SIGNAL_MUX_ECG && mode != SIGNAL_MUX_CAL_ZERO &&
        mode != SIGNAL_MUX_CAL_HIGH) {
        return -EINVAL;
    }

    if (!initialized) {
        err = -EACCES;
        goto out;
    }

    if (mode == SIGNAL_MUX_ECG) {
        /* Disconnect the reference before returning its level to zero. */
        err = gpio_pin_set_dt(&sel_cal, 1);
        if (err == 0) {
            err = gpio_pin_set_dt(&sel_pulse, 0);
        }
    } else {
        /* Set the reference level before connecting it to the amplifier. */
        err = gpio_pin_set_dt(&sel_pulse, mode == SIGNAL_MUX_CAL_HIGH);
        if (err == 0) {
            err = gpio_pin_set_dt(&sel_cal, 0);
        }
    }
out:
    /* A GPIO failure may leave the hardware partially switched. */
    return err;
}

int signal_mux_mode0(void)
{
    return signal_mux_set_mode(SIGNAL_MUX_CAL_ZERO);
}

int signal_mux_mode1(void)
{
    return signal_mux_set_mode(SIGNAL_MUX_CAL_HIGH);
}

int signal_mux_mode2(void)
{
    return signal_mux_set_mode(SIGNAL_MUX_ECG);
}
