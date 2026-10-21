#include "debug_io.h"

#include <errno.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>

static const struct gpio_dt_spec leds[DEBUG_LED_COUNT] = {
    [DEBUG_LED1] = GPIO_DT_SPEC_GET(DT_NODELABEL(debug_led1), gpios),
    [DEBUG_LED2] = GPIO_DT_SPEC_GET(DT_NODELABEL(debug_led2), gpios),
    [DEBUG_LED3] = GPIO_DT_SPEC_GET(DT_NODELABEL(debug_led3), gpios),
};

static const struct gpio_dt_spec switches[DEBUG_SW_COUNT] = {
    [DEBUG_SW1] = GPIO_DT_SPEC_GET(DT_NODELABEL(debug_sw1), gpios),
    [DEBUG_SW2] = GPIO_DT_SPEC_GET(DT_NODELABEL(debug_sw2), gpios),
    [DEBUG_SW3] = GPIO_DT_SPEC_GET(DT_NODELABEL(debug_sw3), gpios),
    [DEBUG_SW4] = GPIO_DT_SPEC_GET(DT_NODELABEL(debug_sw4), gpios),
};

static bool initialized;

int debug_io_init(void)
{
    if (initialized) {
        return 0;
    }
    for (unsigned int i = 0; i < DEBUG_LED_COUNT; ++i) {
        if (!gpio_is_ready_dt(&leds[i])) {
            return -ENODEV;
        }
    }
    for (unsigned int i = 0; i < DEBUG_SW_COUNT; ++i) {
        if (!gpio_is_ready_dt(&switches[i])) {
            return -ENODEV;
        }
    }
    for (unsigned int i = 0; i < DEBUG_LED_COUNT; ++i) {
        int err = gpio_pin_configure_dt(&leds[i], GPIO_OUTPUT_INACTIVE);
        if (err != 0) {
            return err;
        }
    }
    for (unsigned int i = 0; i < DEBUG_SW_COUNT; ++i) {
        int err = gpio_pin_configure_dt(&switches[i], GPIO_INPUT);
        if (err != 0) {
            return err;
        }
    }
    initialized = true;
    return 0;
}

static int led_set(enum debug_led_id id, int value)
{
    if ((unsigned int)id >= DEBUG_LED_COUNT) {
        return -EINVAL;
    }
    if (!initialized) {
        return -EACCES;
    }
    return gpio_pin_set_dt(&leds[id], value);
}

int debug_led_on(enum debug_led_id id)
{
    return led_set(id, 1);
}

int debug_led_off(enum debug_led_id id)
{
    return led_set(id, 0);
}

static int switch_read(enum debug_sw_id id, bool *state, bool invert)
{
    if ((unsigned int)id >= DEBUG_SW_COUNT || state == NULL) {
        return -EINVAL;
    }
    if (!initialized) {
        return -EACCES;
    }
    int value = gpio_pin_get_dt(&switches[id]);
    if (value < 0) {
        return value;
    }
    *state = invert ? (value == 0) : (value != 0);
    return 0;
}

int debug_sw_is_on(enum debug_sw_id id, bool *is_on)
{
    return switch_read(id, is_on, false);
}

int debug_sw_is_off(enum debug_sw_id id, bool *is_off)
{
    return switch_read(id, is_off, true);
}
