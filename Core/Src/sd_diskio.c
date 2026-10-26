#include "diskio.h"
#include "FreeRTOS.h"
#include "cmsis_os.h"
#include "sdmmc.h"
#include "semphr.h"
#include "task.h"

#include <string.h>

#define SD_PHYSICAL_DRIVE       0U
#define SD_IO_TIMEOUT_MS        30000U
#define SD_READY_TIMEOUT_MS     30000U
#define SD_IO_RETRY_COUNT       2U
#define SD_IO_RETRY_DELAY_MS    5U
#define SD_BLOCK_SIZE           512U
#define SD_DMA_BUFFER_BLOCKS    64U
#define SD_DMA_BUFFER_SIZE      (SD_DMA_BUFFER_BLOCKS * SD_BLOCK_SIZE)

static DSTATUS disk_state = STA_NOINIT;
static StaticSemaphore_t transfer_done_control;
static SemaphoreHandle_t transfer_done;
static volatile HAL_StatusTypeDef transfer_result;
static StorageSdDiagnostics sd_diagnostics;
static uint8_t dma_scratch[SD_DMA_BUFFER_SIZE]
  __attribute__((section(".bss.SdDmaBuffer"), aligned(32)));

static bool DmaBufferAccessible(const void *buffer, uint32_t length)
{
  uintptr_t start = (uintptr_t)buffer;
  uintptr_t end = start + length;
  bool internal_sram = (start >= 0x20010000UL) && (end <= 0x20050000UL);
  return (end >= start) && ((start & 0x1FU) == 0U) &&
         internal_sram;
}

static void EnsureTransferSignal(void)
{
  if (transfer_done == NULL)
  {
    transfer_done = xSemaphoreCreateBinaryStatic(&transfer_done_control);
  }
}

static void DrainTransferSignal(void)
{
  while ((transfer_done != NULL) &&
         (xSemaphoreTake(transfer_done, 0U) == pdTRUE))
  {
  }
}

static void SignalTransferFromIsr(HAL_StatusTypeDef result)
{
  BaseType_t higher_priority_task_woken = pdFALSE;
  transfer_result = result;
  if (transfer_done != NULL)
  {
    (void)xSemaphoreGiveFromISR(transfer_done, &higher_priority_task_woken);
    portYIELD_FROM_ISR(higher_priority_task_woken);
  }
}

void HAL_SD_TxCpltCallback(SD_HandleTypeDef *handle)
{
  if (handle == &hsd1)
  {
    SignalTransferFromIsr(HAL_OK);
  }
}

void HAL_SD_RxCpltCallback(SD_HandleTypeDef *handle)
{
  if (handle == &hsd1)
  {
    SignalTransferFromIsr(HAL_OK);
  }
}

void HAL_SD_ErrorCallback(SD_HandleTypeDef *handle)
{
  if (handle == &hsd1)
  {
    SignalTransferFromIsr(HAL_ERROR);
  }
}

static void RecordIoError(StorageSdOperation operation,
                          HAL_StatusTypeDef status,
                          DWORD sector, UINT count)
{
  taskENTER_CRITICAL();
  ++sd_diagnostics.error_count;
  sd_diagnostics.last_error_tick = HAL_GetTick();
  sd_diagnostics.last_sector = sector;
  sd_diagnostics.last_count = count;
  sd_diagnostics.last_hal_error = hsd1.ErrorCode;
  sd_diagnostics.last_sdmmc_status = SDMMC1->STA;
  sd_diagnostics.last_operation = (uint8_t)operation;
  sd_diagnostics.last_hal_status = (uint8_t)status;
  sd_diagnostics.last_handle_state = (uint8_t)hsd1.State;
  taskEXIT_CRITICAL();
}

static void RecordRetry(void)
{
  taskENTER_CRITICAL();
  ++sd_diagnostics.retry_count;
  taskEXIT_CRITICAL();
}

static void RecordDuration(StorageSdOperation operation, uint32_t started)
{
  uint32_t elapsed = HAL_GetTick() - started;
  taskENTER_CRITICAL();
  if ((operation == STORAGE_SD_OPERATION_READ) &&
      (elapsed > sd_diagnostics.max_read_ms))
  {
    sd_diagnostics.max_read_ms = elapsed;
  }
  else if ((operation == STORAGE_SD_OPERATION_WRITE) &&
           (elapsed > sd_diagnostics.max_write_ms))
  {
    sd_diagnostics.max_write_ms = elapsed;
  }
  taskEXIT_CRITICAL();
}

void StorageSd_GetDiagnostics(StorageSdDiagnostics *diagnostics)
{
  if (diagnostics == NULL)
  {
    return;
  }
  taskENTER_CRITICAL();
  *diagnostics = sd_diagnostics;
  taskEXIT_CRITICAL();
}

void StorageSd_ResetDiagnostics(void)
{
  taskENTER_CRITICAL();
  memset(&sd_diagnostics, 0, sizeof(sd_diagnostics));
  taskEXIT_CRITICAL();
}

static bool WaitUntilReady(uint32_t timeout_ms)
{
  uint32_t started = HAL_GetTick();

  while (StorageSd_CardPresent())
  {
    HAL_SD_CardStateTypeDef state = HAL_SD_GetCardState(&hsd1);
    if (state == HAL_SD_CARD_TRANSFER)
    {
      return true;
    }
    if ((state == HAL_SD_CARD_ERROR) ||
        ((uint32_t)(HAL_GetTick() - started) >= timeout_ms))
    {
      return false;
    }
    osDelay(1U);
  }
  return false;
}

static HAL_StatusTypeDef WaitForDmaCompletion(void)
{
  if ((transfer_done == NULL) ||
      (xSemaphoreTake(transfer_done,
                      pdMS_TO_TICKS(SD_IO_TIMEOUT_MS)) != pdTRUE))
  {
    (void)HAL_SD_Abort(&hsd1);
    return HAL_TIMEOUT;
  }
  return transfer_result;
}

static HAL_StatusTypeDef ReadBlockPolling(uint8_t *buffer, DWORD sector)
{
  HAL_StatusTypeDef result;

  /* Polling reads are used deliberately. On STM32F746 the SDMMC RX DMA path
     can raise a transfer error on otherwise readable FAT directory sectors.
     Reads are control-plane operations, while the high-volume log path uses
     DMA writes below. Keep the FIFO service window bounded to one sector. */
  taskENTER_CRITICAL();
  result = HAL_SD_ReadBlocks(&hsd1, buffer, sector, 1U, SD_IO_TIMEOUT_MS);
  taskEXIT_CRITICAL();
  if ((result == HAL_OK) && !WaitUntilReady(SD_READY_TIMEOUT_MS))
  {
    result = HAL_ERROR;
  }
  return result;
}

static HAL_StatusTypeDef WriteBlocksDma(const uint8_t *buffer, DWORD sector,
                                        UINT count)
{
  HAL_StatusTypeDef result;
  DrainTransferSignal();
  transfer_result = HAL_BUSY;
  SCB_CleanDCache_by_Addr((uint32_t *)buffer,
                         (int32_t)(count * SD_BLOCK_SIZE));
  result = HAL_SD_WriteBlocks_DMA(&hsd1, (uint8_t *)buffer, sector, count);
  if (result == HAL_OK)
  {
    result = WaitForDmaCompletion();
  }
  if ((result == HAL_OK) && !WaitUntilReady(SD_READY_TIMEOUT_MS))
  {
    result = HAL_ERROR;
  }
  return result;
}

static bool ReadWithRetry(uint8_t *buffer, DWORD sector)
{
  uint32_t attempt;
  for (attempt = 0U; attempt <= SD_IO_RETRY_COUNT; ++attempt)
  {
    HAL_StatusTypeDef result = ReadBlockPolling(buffer, sector);
    if (result == HAL_OK)
    {
      return true;
    }
    RecordIoError(STORAGE_SD_OPERATION_READ, result, sector, 1U);
    if (!StorageSd_CardPresent() || (attempt == SD_IO_RETRY_COUNT))
    {
      break;
    }
    RecordRetry();
    (void)HAL_SD_Abort(&hsd1);
    osDelay(SD_IO_RETRY_DELAY_MS);
  }
  return false;
}

static bool WriteWithRetry(const uint8_t *buffer, DWORD sector, UINT count)
{
  uint32_t attempt;
  for (attempt = 0U; attempt <= SD_IO_RETRY_COUNT; ++attempt)
  {
    HAL_StatusTypeDef result = WriteBlocksDma(buffer, sector, count);
    if (result == HAL_OK)
    {
      return true;
    }
    RecordIoError(STORAGE_SD_OPERATION_WRITE, result, sector, count);
    if (!StorageSd_CardPresent() || (attempt == SD_IO_RETRY_COUNT))
    {
      break;
    }
    RecordRetry();
    (void)HAL_SD_Abort(&hsd1);
    osDelay(SD_IO_RETRY_DELAY_MS);
  }
  return false;
}

DSTATUS disk_initialize(BYTE pdrv)
{
  HAL_StatusTypeDef init_result;
  if (pdrv != SD_PHYSICAL_DRIVE)
  {
    return STA_NOINIT;
  }
  if (!StorageSd_CardPresent())
  {
    disk_state = STA_NOINIT | STA_NODISK;
    return disk_state;
  }

  EnsureTransferSignal();
  DrainTransferSignal();
  StorageSd_DeInit();
  disk_state = STA_NOINIT;
  init_result = StorageSd_Init();
  if ((init_result == HAL_OK) && WaitUntilReady(SD_READY_TIMEOUT_MS))
  {
    disk_state = 0U;
  }
  else
  {
    RecordIoError(STORAGE_SD_OPERATION_INITIALIZE, init_result, 0U, 0U);
  }
  return disk_state;
}

DSTATUS disk_status(BYTE pdrv)
{
  if (pdrv != SD_PHYSICAL_DRIVE)
  {
    return STA_NOINIT;
  }
  if (!StorageSd_CardPresent())
  {
    disk_state = STA_NOINIT | STA_NODISK;
  }
  return disk_state;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count)
{
  uint32_t started;
  UINT index;

  if ((pdrv != SD_PHYSICAL_DRIVE) || (buff == NULL) || (count == 0U))
  {
    return RES_PARERR;
  }
  if ((disk_status(pdrv) & STA_NOINIT) != 0U)
  {
    return RES_NOTRDY;
  }
  started = HAL_GetTick();
  for (index = 0U; index < count; ++index)
  {
    if (!ReadWithRetry(buff, sector))
    {
      disk_state = STA_NOINIT;
      RecordDuration(STORAGE_SD_OPERATION_READ, started);
      return RES_ERROR;
    }
    buff += SD_BLOCK_SIZE;
    ++sector;
  }
  RecordDuration(STORAGE_SD_OPERATION_READ, started);
  return RES_OK;
}

#if _USE_WRITE == 1
DRESULT disk_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count)
{
  uint32_t started;
  UINT remaining;

  if ((pdrv != SD_PHYSICAL_DRIVE) || (buff == NULL) || (count == 0U))
  {
    return RES_PARERR;
  }
  if ((disk_status(pdrv) & STA_NOINIT) != 0U)
  {
    return RES_NOTRDY;
  }
  started = HAL_GetTick();
  if ((count <= SD_DMA_BUFFER_BLOCKS) &&
      DmaBufferAccessible(buff, count * SD_BLOCK_SIZE) &&
      WriteWithRetry(buff, sector, count))
  {
    RecordDuration(STORAGE_SD_OPERATION_WRITE, started);
    return RES_OK;
  }
  if ((count <= SD_DMA_BUFFER_BLOCKS) &&
      DmaBufferAccessible(buff, count * SD_BLOCK_SIZE))
  {
    disk_state = STA_NOINIT;
    RecordDuration(STORAGE_SD_OPERATION_WRITE, started);
    return RES_ERROR;
  }
  remaining = count;
  while (remaining != 0U)
  {
    UINT blocks = remaining > SD_DMA_BUFFER_BLOCKS ?
                  SD_DMA_BUFFER_BLOCKS : remaining;
    uint32_t bytes = blocks * SD_BLOCK_SIZE;
    memcpy(dma_scratch, buff, bytes);
    if (!WriteWithRetry(dma_scratch, sector, blocks))
    {
      disk_state = STA_NOINIT;
      RecordDuration(STORAGE_SD_OPERATION_WRITE, started);
      return RES_ERROR;
    }
    buff += bytes;
    sector += blocks;
    remaining -= blocks;
  }
  RecordDuration(STORAGE_SD_OPERATION_WRITE, started);
  return RES_OK;
}
#endif

#if _USE_IOCTL == 1
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
  HAL_SD_CardInfoTypeDef info;

  if (pdrv != SD_PHYSICAL_DRIVE)
  {
    return RES_PARERR;
  }
  if ((disk_status(pdrv) & STA_NOINIT) != 0U)
  {
    return RES_NOTRDY;
  }

  switch (cmd)
  {
    case CTRL_SYNC:
      if (WaitUntilReady(SD_READY_TIMEOUT_MS))
      {
        return RES_OK;
      }
      RecordIoError(STORAGE_SD_OPERATION_SYNC, HAL_ERROR, 0U, 0U);
      disk_state = STA_NOINIT;
      return RES_ERROR;

    case GET_SECTOR_COUNT:
      if ((buff == NULL) || (HAL_SD_GetCardInfo(&hsd1, &info) != HAL_OK))
      {
        return RES_ERROR;
      }
      *(DWORD *)buff = info.LogBlockNbr;
      return RES_OK;

    case GET_SECTOR_SIZE:
      if (buff == NULL)
      {
        return RES_PARERR;
      }
      *(WORD *)buff = 512U;
      return RES_OK;

    case GET_BLOCK_SIZE:
      if (buff == NULL)
      {
        return RES_PARERR;
      }
      *(DWORD *)buff = 1U;
      return RES_OK;

    default:
      return RES_PARERR;
  }
}
#endif
