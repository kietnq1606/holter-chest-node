#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>

#include "../services/storage_service/storage.h"

#define W25N01GV_NODE             DT_NODELABEL(w25n01gv)
#define W25N01GV_PAGE_SIZE        STORAGE_BLOCK_SIZE
#define W25N01GV_ERASE_BLOCK_SIZE (128U * 1024U)
#define TEST_OFFSET               0U

/* Static buffers avoid consuming the main thread stack. */
static uint8_t write_buffer[W25N01GV_PAGE_SIZE];
static uint8_t read_buffer[W25N01GV_PAGE_SIZE];

static void fill_test_pattern(void)
{
	for (size_t i = 0; i < STORAGE_BLOCK_DATA_SIZE; ++i) {
		write_buffer[i] = (uint8_t)((i * 31U + 7U) & 0xFFU);
	}

	/* Match storage_write(): payload CRC16 with a little-endian trailer. */
	uint16_t crc = crc16_ccitt(0xFFFFU, write_buffer, STORAGE_BLOCK_DATA_SIZE);

	sys_put_le16(crc, write_buffer + STORAGE_BLOCK_DATA_SIZE);
}

static bool buffer_is_erased(const uint8_t *buffer, size_t length)
{
	for (size_t i = 0; i < length; ++i) {
		if (buffer[i] != 0xFFU) {
			return false;
		}
	}

	return true;
}

int main(void)
{
	const struct device *flash = DEVICE_DT_GET(W25N01GV_NODE);
	int ret;

	printk("W25N01GV test start\n");
	printk("WARNING: test erases block at offset 0x%08x\n", TEST_OFFSET);
	printk("Storage block: payload=%u CRC=2 total=%u bytes\n",
	       STORAGE_BLOCK_DATA_SIZE, STORAGE_BLOCK_SIZE);

	if (!device_is_ready(flash)) {
		printk("FAIL: flash device is not ready\n");
		return -ENODEV;
	}

	/* NAND must be erased before its page can be programmed. */
	ret = flash_erase(flash, TEST_OFFSET, W25N01GV_ERASE_BLOCK_SIZE);
	if (ret != 0) {
		printk("FAIL: initial erase returned %d\n", ret);
		return ret;
	}

	fill_test_pattern();

	ret = flash_write(flash, TEST_OFFSET, write_buffer,
			  sizeof(write_buffer));
	if (ret != 0) {
		printk("FAIL: write returned %d\n", ret);
		return ret;
	}

	memset(read_buffer, 0, sizeof(read_buffer));
	ret = flash_read(flash, TEST_OFFSET, read_buffer,
			 sizeof(read_buffer));
	if (ret != 0) {
		printk("FAIL: read after write returned %d\n", ret);
		return ret;
	}

	uint16_t stored_crc = sys_get_le16(read_buffer + STORAGE_BLOCK_DATA_SIZE);
	uint16_t calculated_crc = crc16_ccitt(0xFFFFU, read_buffer,
					    STORAGE_BLOCK_DATA_SIZE);

	if (stored_crc != calculated_crc) {
		printk("FAIL: storage CRC16 stored=0x%04x calculated=0x%04x\n",
		       stored_crc, calculated_crc);
		return -EIO;
	}
	printk("PASS: storage CRC16 stored=0x%04x calculated=0x%04x\n",
	       stored_crc, calculated_crc);

	if (memcmp(write_buffer, read_buffer, sizeof(write_buffer)) != 0) {
		printk("FAIL: data read back does not match written data\n");
		return -EIO;
	}
	printk("PASS: write and read verification\n");

	ret = flash_erase(flash, TEST_OFFSET, W25N01GV_ERASE_BLOCK_SIZE);
	if (ret != 0) {
		printk("FAIL: final erase returned %d\n", ret);
		return ret;
	}

	memset(read_buffer, 0, sizeof(read_buffer));
	ret = flash_read(flash, TEST_OFFSET, read_buffer,
			 sizeof(read_buffer));
	if (ret != 0) {
		printk("FAIL: read after erase returned %d\n", ret);
		return ret;
	}

	if (!buffer_is_erased(read_buffer, sizeof(read_buffer))) {
		printk("FAIL: data is not 0xFF after erase\n");
		return -EIO;
	}

	printk("PASS: erase and erased-state verification\n");
	printk("W25N01GV test completed successfully\n");
	return 0;
}
