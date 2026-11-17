#ifndef STORAGE_H
#define STORAGE_H

#include <stddef.h>
#include <stdint.h>

#define STORAGE_BLOCK_DATA_SIZE 2046U // - 2 bytes for CRC, 2048 bytes total
#define STORAGE_BLOCK_SIZE (STORAGE_BLOCK_DATA_SIZE + 2U) // + 2 bytes for CRC

/* Call from thread context and serialize all storage operations. */
/* If init finds no valid metadata, explicitly erase/format with storage_erase_data(). */
int storage_init(void);

/* Writes exactly one complete 4096-byte block. */
int storage_write(const uint8_t *buffer, size_t length);

/* Reads committed data at an offset relative to the storage data region. */
int storage_read(uint32_t offset, uint8_t *buffer, size_t length);
/* checking data erea has avaible slot*/
int storage_check_write_space(void);
// get verified area saved data
int storage_get_data_size(uint32_t *size);
/* Erases stored data; power-loss recovery during erase is not guaranteed. */
int storage_erase_all_datas(void);

#endif /* STORAGE_H */
