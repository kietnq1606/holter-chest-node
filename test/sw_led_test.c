#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "debug_io.h"

#define ALL_LEDS_MASK ((1U << DEBUG_LED_COUNT) - 1U)

int main(void)
{
    k_msleep(2000);

    int err = debug_io_init();

    if (err != 0) {
        printk("[FAIL] LED/switch init: err=%d\n", err);
        return err;
    }

    printk("========== LED/SWITCH TEST ==========\n");
    printk("SW1 -> LED1, SW2 -> LED2, SW3 -> LED3, SW4 -> all LEDs\n");
    printk("1=ON, 0=OFF. LEDs follow switch states.\n");

    uint8_t previous_mask = UINT8_MAX;

    while (true) {
        uint8_t switch_mask = 0U;

        for (unsigned int i = 0U; i < DEBUG_SW_COUNT; i++) {
            bool on;

            err = debug_sw_is_on((enum debug_sw_id)i, &on);
            if (err != 0) {
                printk("[FAIL] Read SW%u: err=%d\n", i + 1U, err);
                return err;
            }
            if (on) {
                switch_mask |= 1U << i;
            }
        }

        if (switch_mask != previous_mask) {
            uint8_t led_mask = switch_mask & ALL_LEDS_MASK;

            if (switch_mask & (1U << DEBUG_SW4)) {
                led_mask = ALL_LEDS_MASK;
            }
            for (unsigned int i = 0U; i < DEBUG_LED_COUNT; i++) {
                enum debug_led_id id = (enum debug_led_id)i;

                err = (led_mask & (1U << i)) ? debug_led_on(id) : debug_led_off(id);
                if (err != 0) {
                    printk("[FAIL] Set LED%u: err=%d\n", i + 1U, err);
                    return err;
                }
            }

            printk("SW1=%u SW2=%u SW3=%u SW4=%u | LED1=%u LED2=%u LED3=%u\n",
                   (switch_mask >> DEBUG_SW1) & 1U,
                   (switch_mask >> DEBUG_SW2) & 1U,
                   (switch_mask >> DEBUG_SW3) & 1U,
                   (switch_mask >> DEBUG_SW4) & 1U,
                   (led_mask >> DEBUG_LED1) & 1U,
                   (led_mask >> DEBUG_LED2) & 1U,
                   (led_mask >> DEBUG_LED3) & 1U);
            previous_mask = switch_mask;
        }

        k_msleep(20);
    }
}
