#ifndef SDMMC_H
#define SDMMC_H

#include "main.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
  STORAGE_SD_OPERATION_NONE = 0,
  STORAGE_SD_OPERATION_INITIALIZE,
  STORAGE_SD_OPERATION_READ,
  STORAGE_SD_OPERATION_WRITE,
  STORAGE_SD_OPERATION_SYNC
} StorageSdOperation;

typedef struct
{
  uint32_t error_count;
  uint32_t retry_count;
  uint32_t last_error_tick;
  uint32_t last_sector;
  uint32_t last_count;
  uint32_t last_hal_error;
  uint32_t last_sdmmc_status;
  uint32_t max_read_ms;
  uint32_t max_write_ms;
  uint8_t last_operation;
  uint8_t last_hal_status;
  uint8_t last_handle_state;
  uint8_t reserved;
} StorageSdDiagnostics;

extern SD_HandleTypeDef hsd1;
extern DMA_HandleTypeDef hdma_sdmmc1_rx;
extern DMA_HandleTypeDef hdma_sdmmc1_tx;

bool StorageSd_CardPresent(void);
HAL_StatusTypeDef StorageSd_Init(void);
void StorageSd_DeInit(void);
void StorageSd_GetDiagnostics(StorageSdDiagnostics *diagnostics);
void StorageSd_ResetDiagnostics(void);

#endif
