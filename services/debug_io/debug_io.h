#ifndef DEBUG_IO_H_
#define DEBUG_IO_H_

#include <stdbool.h>

enum debug_led_id {
    DEBUG_LED1 = 0,
    DEBUG_LED2,
    DEBUG_LED3,
    DEBUG_LED_COUNT,
};

enum debug_sw_id {
    DEBUG_SW1 = 0,
    DEBUG_SW2,
    DEBUG_SW3,
    DEBUG_SW4,
    DEBUG_SW_COUNT,
};

/* Call before starting threads; LEDs start off. Returns 0 or negative errno. */
int debug_io_init(void);
int debug_led_on(enum debug_led_id id);
int debug_led_off(enum debug_led_id id);

/* Instantaneous logical state, without debounce. Output unchanged on error. */
int debug_sw_is_on(enum debug_sw_id id, bool *is_on);
int debug_sw_is_off(enum debug_sw_id id, bool *is_off);

#endif /* DEBUG_IO_H_ */
