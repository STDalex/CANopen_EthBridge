#ifndef QSPI_STORE_H
#define QSPI_STORE_H

#include <stdbool.h>
#include <stdint.h>

#define QSPI_STORE_FLASH_BYTES   0x01000000UL
#define QSPI_STORE_BLOCK_BYTES   0x00001000UL

bool QspiStore_Init(void);
bool QspiStore_Read(uint32_t address, void *data, uint32_t size);
bool QspiStore_Write(uint32_t address, const void *data, uint32_t size);
bool QspiStore_EraseBlock(uint32_t address);

#endif
