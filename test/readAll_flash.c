#include "debug_io.h"
#include "storage.h"
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

#define W25N01GV_NODE          DT_NODELABEL(w25n01gv)
#define FLASH_SIZE             ((uint32_t)DT_PROP(W25N01GV_NODE, size_bytes))
#define FLASH_ERASE_BLOCK_SIZE ((uint32_t)DT_PROP(W25N01GV_NODE, erase_block_size))
#define FLASH_TOTAL_PAGES      (FLASH_SIZE / STORAGE_BLOCK_SIZE)
#define METADATA_PAGES         (FLASH_ERASE_BLOCK_SIZE / STORAGE_BLOCK_SIZE)
#define HEX_CHUNK_BYTES        64U

static const struct device *const flash_dev = DEVICE_DT_GET(W25N01GV_NODE);

/* Single reusable page buffer to avoid stack/heap bloat. */
static uint8_t page_buf[STORAGE_BLOCK_SIZE];
static char hex_line[(HEX_CHUNK_BYTES * 2U) + 1U];

static bool buf_is_all(const uint8_t *buf, size_t len, uint8_t val)
{
	for (size_t i = 0; i < len; ++i) {
		if (buf[i] != val) {
			return false;
		}
	}
	return true;
}

static void flush_erased(uint32_t *start, uint32_t *count)
{
	if (*count > 0U) {
		printk("ERASED,%u,%u\n", (unsigned int)*start, (unsigned int)*count);
		*start = 0U;
		*count = 0U;
	}
}

static void emit_page(uint32_t page, uint32_t offset, uint16_t stored_crc, uint16_t calc_crc)
{
	static const char hex[] = "0123456789abcdef";

	printk("PAGE_BEGIN,%u,%u,%04x,%04x,%u\n",
	       (unsigned int)page, (unsigned int)offset,
	       (unsigned int)stored_crc, (unsigned int)calc_crc,
	       stored_crc == calc_crc ? 1U : 0U);

	for (uint32_t pos = 0U; pos < STORAGE_BLOCK_SIZE; pos += HEX_CHUNK_BYTES) {
		size_t n = MIN(HEX_CHUNK_BYTES, STORAGE_BLOCK_SIZE - pos);

		for (size_t i = 0; i < n; ++i) {
			hex_line[i * 2U] = hex[page_buf[pos + i] >> 4];
			hex_line[i * 2U + 1U] = hex[page_buf[pos + i] & 0x0FU];
		}
		hex_line[n * 2U] = '\0';
		printk("DATA,%u,%s\n", (unsigned int)(pos / HEX_CHUNK_BYTES), hex_line);
		/* Throttle slightly to avoid overflowing USB CDC TX buffers. */
		k_msleep(1);
	}
	printk("PAGE_END,%u\n", (unsigned int)page);
}

static void dump_flash(bool full_scan)
{
	uint32_t erased_start = 0U, erased_run = 0U, trailing_erased = 0U;
	uint32_t n_erased = 0U, n_meta = 0U, n_data = 0U, n_marker = 0U, n_err = 0U;
	uint32_t max_write_off = FLASH_ERASE_BLOCK_SIZE, scanned = 0U;

	debug_led_on(DEBUG_LED2);
	printk("FLASH_DUMP_START,mode=%s,size=%u,erase_block=%u,page_size=%u,meta_pages=%u,total_pages=%u\n",
	       full_scan ? "all" : "used", (unsigned int)FLASH_SIZE,
	       (unsigned int)FLASH_ERASE_BLOCK_SIZE, (unsigned int)STORAGE_BLOCK_SIZE,
	       (unsigned int)METADATA_PAGES, (unsigned int)FLASH_TOTAL_PAGES);

	/* Read raw pages directly; storage_init() would write a boot marker. */
	for (uint32_t page = 0U; page < FLASH_TOTAL_PAGES; ++page) {
		uint32_t offset = page * STORAGE_BLOCK_SIZE;
		int ret = flash_read(flash_dev, offset, page_buf, STORAGE_BLOCK_SIZE);

		++scanned;
		if (ret != 0) {
			flush_erased(&erased_start, &erased_run);
			++n_err;
			printk("ERR,%u,%u,%d\n", (unsigned int)page, (unsigned int)offset, ret);
			continue;
		}

		if (buf_is_all(page_buf, STORAGE_BLOCK_SIZE, 0xFFU)) {
			if (erased_run == 0U) {
				erased_start = page;
			}
			++erased_run;
			++n_erased;
			if (page >= METADATA_PAGES && !full_scan &&
			    offset >= max_write_off && ++trailing_erased >= METADATA_PAGES) {
				flush_erased(&erased_start, &erased_run);
				printk("FAST_STOP,%u,%u\n",
				       (unsigned int)(page + 1U),
				       (unsigned int)(FLASH_TOTAL_PAGES - page - 1U));
				break;
			}
			if ((page % 16U) == 0U) {
				printk("PROGRESS,%u,%u\n", (unsigned int)page, (unsigned int)FLASH_TOTAL_PAGES);
				k_yield();
			}
			continue;
		}

		flush_erased(&erased_start, &erased_run);
		trailing_erased = 0U;

		if (page < METADATA_PAGES) {
			uint32_t seq = sys_get_le32(page_buf);
			uint32_t w_off = sys_get_le32(page_buf + 4U);
			uint32_t s_crc = sys_get_le32(page_buf + 8U);
			uint32_t c_crc = crc32_ieee(page_buf, 8U);
			bool ok = (s_crc == c_crc) && (w_off >= FLASH_ERASE_BLOCK_SIZE) &&
				  (w_off <= FLASH_SIZE) && ((w_off % STORAGE_BLOCK_SIZE) == 0U);

			++n_meta;
			if (ok && w_off > max_write_off) {
				max_write_off = w_off;
			}
			printk("META,%u,%u,%u,%u,%08x,%08x,%u,%u\n",
			       (unsigned int)page, (unsigned int)offset,
			       (unsigned int)seq, (unsigned int)w_off,
			       (unsigned int)s_crc, (unsigned int)c_crc,
			       ok ? 1U : 0U,
			       buf_is_all(page_buf + 12U, STORAGE_BLOCK_SIZE - 12U, 0xFFU) ? 1U : 0U);
			k_msleep(1);
			continue;
		}

		uint16_t s_crc = sys_get_le16(page_buf + STORAGE_BLOCK_DATA_SIZE);
		uint16_t c_crc = crc16_ccitt(0xFFFFU, page_buf, STORAGE_BLOCK_DATA_SIZE);

		if (buf_is_all(page_buf, STORAGE_BLOCK_DATA_SIZE, 0x00U)) {
			++n_marker;
			printk("MARKER,%u,%u,%04x,%04x,%u\n",
			       (unsigned int)page, (unsigned int)offset,
			       (unsigned int)s_crc, (unsigned int)c_crc,
			       s_crc == c_crc ? 1U : 0U);
			k_msleep(1);
		} else {
			++n_data;
			emit_page(page, offset, s_crc, c_crc);
		}
	}

	flush_erased(&erased_start, &erased_run);
	printk("FLASH_DUMP_END,scanned=%u,erased=%u,meta=%u,markers=%u,data=%u,errors=%u\n",
	       (unsigned int)scanned, (unsigned int)n_erased, (unsigned int)n_meta,
	       (unsigned int)n_marker, (unsigned int)n_data, (unsigned int)n_err);
	debug_led_off(DEBUG_LED2);
}

int main(void)
{
	if (!device_is_ready(flash_dev)) {
		printk("FLASH_DUMP_FATAL,flash_not_ready\n");
		return -ENODEV;
	}

	(void)debug_io_init();

	/* Allow USB CDC ACM host port to reconnect before streaming. */
	k_msleep(3000);
	dump_flash(false);
	return 0;
}
