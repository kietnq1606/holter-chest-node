#include "debug_io.h"
#include "storage.h"
#include <zephyr/kernel.h>

int main(void)
{
	int ret;

	(void)debug_io_init();

	/* Allow USB CDC ACM console to reconnect before printing status. */
	k_msleep(3000);

	debug_led_on(DEBUG_LED1);
	printk("FLASH_ERASE_START\n");
	ret = storage_erase_all_datas();
	printk("FLASH_ERASE_END,ret=%d\n", ret);
	debug_led_off(DEBUG_LED1);

	return ret;
}
