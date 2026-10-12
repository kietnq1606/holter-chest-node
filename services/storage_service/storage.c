#include "storage.h"
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <errno.h>
#include <string.h>

#define W25N01GV_NODE             DT_NODELABEL(w25n01gv)


#define FLASH_SIZE DT_PROP            (W25N01GV_NODE, size_bytes)
#define FLASH_ERASE_BLOCK_SIZE DT_PROP(W25N01GV_NODE, erase_block_size)
#define FLASH_WRITE_BLOCK_SIZE DT_PROP(W25N01GV_NODE, write_block_size)
#define W25N01GV_PAGE_SIZE             2048U
#define METADATA_AREA_SIZE            (64 * 2048U) // 64 pages for metadata(min block to erase)


static const struct device *flash_dev = DEVICE_DT_GET(W25N01GV_NODE);

// struct for metadata 
typedef struct {
    uint32_t sequence;     // Sequence number of the metadata record.
    uint32_t write_offset; // Flash address of the next data page.
    uint32_t crc;            
} storage_metadata_t;

static storage_metadata_t latest_metadata; 
static bool recovered;   // storage has not been recovered with the used metadata yet
static uint8_t page_buffer[STORAGE_BLOCK_SIZE]; // buffer for writing a page to flash, including CRC

/* Validate the next data address, including the full-storage endpoint. */
static bool offset_is_valid(uint32_t offset);
/* helper function to commit metadata to flash */
// attach crc for the meta record 
static void metadata_encode(uint8_t *record, const storage_metadata_t *metadata);
// decode the metadata record 
static bool metadata_decode(const uint8_t *record, storage_metadata_t *metadata);
static int commit_metadata(uint32_t next_write_offset);
/*
 * Write a zero-filled boot marker and commit its next data offset.
 * Init must recover metadata and ensure the target page is erased.
 */
static int storage_write_marker(void);
/* Read one page into the shared buffer and check its erased state. */
int block_is_erased(uint32_t address, bool *erased);
/* Recover the latest valid metadata and account for used metadata pages. */
static int storage_recover_metadata(void);
/* Skip programmed data pages beyond the recovered write offset. */
static int storage_recover_data(void);
/* checking data erea has avaible slot*/
int storage_check_write_space(void);

int storage_init(void){
    int ret;
    if (k_is_in_isr()) {
        return -EWOULDBLOCK;
    }
    /* Block access until recovery completes. */
    recovered = false;
    printk("Storage: init\n");
    if (!device_is_ready(flash_dev)) {
        printk("Storage: device not ready\n");
        return -ENODEV;
    }
    /* Require the agreed NAND geometry and complete erase blocks. */
    if (FLASH_ERASE_BLOCK_SIZE != METADATA_AREA_SIZE ||
        W25N01GV_PAGE_SIZE != STORAGE_BLOCK_SIZE ||
        FLASH_WRITE_BLOCK_SIZE == 0U ||
        STORAGE_BLOCK_SIZE % FLASH_WRITE_BLOCK_SIZE != 0U ||
        FLASH_SIZE > UINT32_MAX ||
        FLASH_SIZE <= METADATA_AREA_SIZE ||
        FLASH_SIZE % FLASH_ERASE_BLOCK_SIZE != 0U) {
        printk("Storage: invalid geometry\n");
        return -EINVAL;
    }
    /* Select the next metadata slot without decoding old records. */
    ret = storage_recover_metadata();
    if (ret != 0) {
        printk("Storage: metadata recovery failed (%d)\n", ret);
        return ret;
    }
    printk("Storage: consumed metadata pages=%u\n",
           (unsigned int)latest_metadata.sequence);

    /* Scan data from its beginning and commit the recovered address. */
    ret = storage_recover_data();
    if (ret != 0) {
        printk("Storage: data recovery failed (%d)\n", ret);
        return ret;
    }
    printk("Storage: write address=%u, data extent=%u\n",
           (unsigned int)latest_metadata.write_offset,
           (unsigned int)(latest_metadata.write_offset -
                          METADATA_AREA_SIZE));
    /* Keep recovered data readable even when storage is full. */
    recovered = true;
    if (FLASH_SIZE - latest_metadata.write_offset < STORAGE_BLOCK_SIZE) {
        printk("Storage: full\n");
        return -ENOSPC;
    }
    /* The marker uses the normal write and verification path. */
    ret = storage_write_marker();
    if (ret != 0) {
        recovered = false;
        printk("Storage: marker failed (%d)\n", ret);
        return ret;
    }
    printk("Storage: ready, next write address=%u\n",
           (unsigned int)latest_metadata.write_offset);

    return 0;
}

int storage_get_data_size(uint32_t *size)
{
    if (size == NULL) {
        return -EINVAL;
    }

    if (!recovered ||
        !offset_is_valid(latest_metadata.write_offset)) {
        return -EINVAL;
    }

    *size = latest_metadata.write_offset - METADATA_AREA_SIZE;
    return 0;
}

/* Write one payload page and commit its next address. */
int storage_write(const uint8_t *buffer, size_t length)
{
    uint16_t crc;
    int ret;

    if (k_is_in_isr()) {
        return -EWOULDBLOCK;
    }
    if (!recovered || buffer == NULL ||
        length != STORAGE_BLOCK_DATA_SIZE ||
        !offset_is_valid(latest_metadata.write_offset)) {
        return -EINVAL;
    }
    if (FLASH_SIZE - latest_metadata.write_offset < STORAGE_BLOCK_SIZE) {
        return -ENOSPC;
    }

    if (buffer != page_buffer) {
        memcpy(page_buffer, buffer, STORAGE_BLOCK_DATA_SIZE);
    }
    crc = crc16_ccitt(0xFFFFU, page_buffer, STORAGE_BLOCK_DATA_SIZE);
    sys_put_le16(crc, page_buffer + STORAGE_BLOCK_DATA_SIZE);

    ret = flash_write(flash_dev, latest_metadata.write_offset,
                      page_buffer, sizeof(page_buffer));
    if (ret != 0) {
        recovered = false;
        return ret;
    }

    ret = flash_read(flash_dev, latest_metadata.write_offset,
                     page_buffer, sizeof(page_buffer));
    if (ret != 0) {
        recovered = false;
        return ret;
    }
    if (sys_get_le16(page_buffer + STORAGE_BLOCK_DATA_SIZE) != crc ||
        crc16_ccitt(0xFFFFU, page_buffer, STORAGE_BLOCK_DATA_SIZE) != crc) {
        recovered = false;
        return -EIO;
    }

    ret = commit_metadata(latest_metadata.write_offset + STORAGE_BLOCK_SIZE);
    if (ret != 0) {
        recovered = false;
    }
    return ret;
}

/* Read using a caller-relative offset within the data region. */
int storage_read(uint32_t offset, uint8_t *buffer, size_t length)
{
    if (k_is_in_isr()) {
        return -EWOULDBLOCK;
    }
    if (!recovered || buffer == NULL || length == 0U ||
        !offset_is_valid(latest_metadata.write_offset)) {
        return -EINVAL;
    }
    if (offset > latest_metadata.write_offset - METADATA_AREA_SIZE ||
        length > latest_metadata.write_offset - METADATA_AREA_SIZE - offset) {
        return -ERANGE;
    }
    return flash_read(flash_dev, METADATA_AREA_SIZE + offset, buffer, length);
}

/*
 * Erase all data and commit empty metadata.
 * Power-loss recovery during erase is not guaranteed.
 * The caller must stop acquisition and serialize storage operations.
 */
int storage_erase_all_datas(void){
    int ret;
    if (k_is_in_isr()) {
        return -EWOULDBLOCK;
    }
    /* First-use formatting also requires a ready, valid device layout. */
    if (!device_is_ready(flash_dev)) {
        return -ENODEV;
    }
    if (FLASH_ERASE_BLOCK_SIZE != METADATA_AREA_SIZE ||
        FLASH_WRITE_BLOCK_SIZE == 0U ||
        STORAGE_BLOCK_SIZE % FLASH_WRITE_BLOCK_SIZE != 0U ||
        FLASH_SIZE > UINT32_MAX ||
        FLASH_SIZE <= METADATA_AREA_SIZE ||
        FLASH_SIZE % FLASH_ERASE_BLOCK_SIZE != 0U) {
        printk("Storage: invalid erase range\n");
        return -EINVAL;
    }
    /* Prevent access to data during or after an incomplete erase. */
    recovered = false;
    /* Erase the entire data region after the metadata block. */
    ret = flash_erase(flash_dev,
                     METADATA_AREA_SIZE,
                     FLASH_SIZE - METADATA_AREA_SIZE);
    if (ret != 0) {
        printk("Storage: data erase failed (%d)\n", ret);
        return ret;
    }
    /* Sequence zero makes commit start a fresh metadata block. */
    latest_metadata = (storage_metadata_t){
        .sequence = 0U,
        .write_offset = METADATA_AREA_SIZE,
    };
    /* Persist the empty data offset and verify the record. */
    ret = commit_metadata(METADATA_AREA_SIZE);
    if (ret != 0) {
        printk("Storage: metadata reset failed (%d)\n", ret);
        return ret;
    }
    return 0;
}

/* Validate the next data address, including the full-storage endpoint. */
static bool offset_is_valid(uint32_t offset){
    return offset >= METADATA_AREA_SIZE && offset <= FLASH_SIZE &&
           offset % STORAGE_BLOCK_SIZE == 0U;
}

// attach crc for the meta record 
static void metadata_encode(uint8_t *record, const storage_metadata_t *metadata){
    sys_put_le32(metadata->sequence, record);
    sys_put_le32(metadata->write_offset, record + 4U);
    sys_put_le32(crc32_ieee(record, 8U), record + 8U);
}
// decode the metadata record 
static bool metadata_decode(const uint8_t *record, storage_metadata_t *metadata){
    uint32_t crc = sys_get_le32(record + 8U);
    uint32_t offset = sys_get_le32(record + 4U);

    if (crc != crc32_ieee(record, 8U) ||
        !offset_is_valid(offset)) {
        return false;
    }
    metadata->sequence = sys_get_le32(record);
    metadata->write_offset = offset;
    metadata->crc = crc;
    return true;
}

/*
 * Append metadata to the page selected by sequence.
 * Erase the metadata block when starting a new page cycle.
 * Update RAM metadata only after readback verification succeeds.
 */
static int commit_metadata(uint32_t next_write_offset)
{
    /* Hold the record being encoded, then the verified readback. */
    storage_metadata_t verified;
    /* Advance sequence; unsigned overflow wraps naturally. */
    uint32_t next_sequence = latest_metadata.sequence + 1U;

    /* Map sequence to a page index, then convert it to a byte offset. */
    uint32_t slot = (latest_metadata.sequence %
         (METADATA_AREA_SIZE / W25N01GV_PAGE_SIZE)) * W25N01GV_PAGE_SIZE;

    /* Store the Flash API result. */
    int ret;

    /* Flash programming and erasing must run in thread context. */
    if (k_is_in_isr()) {
        /* Report the invalid calling context. */
        printk("Metadata: ISR call rejected\n");
        /* Reject the operation without accessing Flash. */
        return -EWOULDBLOCK;
    }

    /* Require an in-range, block-aligned data offset. */
    if (!offset_is_valid(next_write_offset)) {
        /* Report the invalid offset. */
        printk("Metadata: invalid data offset\n");
        return -EINVAL;
    }
    /* Page zero starts a new cycle of metadata records. */
    if (slot == 0U) {
        /* Erase the entire 128 KiB metadata block. */
        ret = flash_erase(flash_dev, 0, METADATA_AREA_SIZE);
        if (ret != 0) {
            /* Report the driver error. */
            printk("Metadata: erase failed (%d)\n", ret);
            return ret;
        }
    }

    /* Set the sequence for the new record. */
    verified.sequence = next_sequence;

    /* Store the next data address. */
    verified.write_offset = next_write_offset;

    /* Fill unused page bytes with the erased value. */
    memset(page_buffer, 0xFF, sizeof(page_buffer));

    /* Encode both fields and calculate their CRC32. */
    metadata_encode(page_buffer, &verified);

    /* Program the complete 2048-byte metadata page. */
    ret = flash_write(flash_dev, slot, page_buffer, sizeof(page_buffer));
    if (ret != 0) {
        /* Report the driver error. */
        printk("Metadata: write failed (%d)\n", ret);
        return ret;
    }

    /* Reuse the page buffer to read the 12-byte record. */
    ret = flash_read(flash_dev, slot, page_buffer, 12U);
    if (ret != 0) {
        /* Report the driver error. */
        printk("Metadata: readback failed (%d)\n", ret);
        return ret;
    }

    /* Decode only a record with valid CRC and data offset. */
    if (!metadata_decode(page_buffer, &verified)) {
        /* Report an invalid record read from Flash. */
        printk("Metadata: invalid readback record\n");
        return -EIO;
    }

    /* Require the readback fields to match the intended commit. */
    if (verified.sequence != next_sequence ||
        verified.write_offset != next_write_offset) {
        /* Report a valid but unexpected record. */
        printk("Metadata: readback mismatch\n");
        return -EIO;
    }

    /* Update RAM only after Flash readback is verified. */
    latest_metadata = verified;
    return 0;
}

/*
 * Write a zero-filled boot marker and commit its next data offset.
 */
static int storage_write_marker(void)
{
    memset(page_buffer, 0, sizeof(page_buffer));
    return storage_write(page_buffer, STORAGE_BLOCK_DATA_SIZE);
}
/* Read one page into the shared buffer and check its erased state. */
static int block_is_erased(uint32_t address, bool *erased)
{
    int ret;

    ret = flash_read(flash_dev, address, page_buffer, sizeof(page_buffer));
    if (ret != 0) {
        return ret;
    }

    *erased = true;
    for (size_t i = 0; i < sizeof(page_buffer); ++i) {
        if (page_buffer[i] != 0xFFU) {
            *erased = false;
            break;
        }
    }

    return 0;
}
/*
 * Find the first erased metadata page.
 * Sequence represents consumed pages so commit selects the next free slot.
 */
static int storage_recover_metadata(void)
{
    uint32_t page = 0U;
    bool erased;
    int ret;

    while (page < METADATA_AREA_SIZE / W25N01GV_PAGE_SIZE) {
        /* Read the full page to detect partially programmed pages. */
        ret = block_is_erased(page * W25N01GV_PAGE_SIZE, &erased);
        if (ret != 0) {
            return ret;
        }

        /* Sequential writes leave all following pages erased. */
        if (erased) {
            break;
        }

        /* A partially programmed page also consumes a slot. */
        ++page;
    }

    /*
     * A count of 64 makes the next commit erase the metadata block.
     * Data recovery determines the actual write address separately.
     */
    latest_metadata = (storage_metadata_t){
        .sequence = page,
        .write_offset = METADATA_AREA_SIZE,
    };

    return 0;
}

/*
 * Scan sequential data pages to find the next writable address.
 * Persist that address before allowing another data write.
 */
static int storage_recover_data(void)
{
    uint32_t offset = METADATA_AREA_SIZE;
    bool erased;
    int ret;

    while (offset < FLASH_SIZE) {
        ret = block_is_erased(offset, &erased);
        if (ret != 0) {
            return ret;
        }

        /* The first fully erased page is available for writing. */
        if (erased) {
            break;
        }

        /* Skip programmed pages even if their CRC is invalid. */
        offset += STORAGE_BLOCK_SIZE;
    }

    /* FLASH_SIZE is a valid address value indicating full storage. */
    return commit_metadata(offset);
}

/* Check space for one page*/
int storage_check_write_space(void)
{
    /* Reject calls from interrupt context. */
    if (k_is_in_isr()) {
        return -EWOULDBLOCK;
    }

    /* Require recovered metadata and an in-range, aligned offset. */
    if (!recovered || !offset_is_valid(latest_metadata.write_offset)) {
        return -EINVAL;
    }

    /* A page needs 2048 bytes: 2046 bytes of data plus 2 bytes of CRC. */
    if (FLASH_SIZE - latest_metadata.write_offset
        < STORAGE_BLOCK_SIZE) {
        return -ENOSPC;
    }

    /* There is enough space to write one complete page. */
    return 0;
}