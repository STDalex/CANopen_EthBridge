#include "diskio.h"
#include "FreeRTOS.h"
#include "cmsis_os.h"
#include "sdmmc.h"
#include "task.h"

#define SD_PHYSICAL_DRIVE       0U
#define SD_IO_TIMEOUT_MS        30000U
#define SD_READY_TIMEOUT_MS     30000U

static DSTATUS disk_state = STA_NOINIT;

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

DSTATUS disk_initialize(BYTE pdrv)
{
  if (pdrv != SD_PHYSICAL_DRIVE)
  {
    return STA_NOINIT;
  }
  if (!StorageSd_CardPresent())
  {
    disk_state = STA_NOINIT | STA_NODISK;
    return disk_state;
  }

  StorageSd_DeInit();
  disk_state = STA_NOINIT;
  if ((StorageSd_Init() == HAL_OK) && WaitUntilReady(SD_READY_TIMEOUT_MS))
  {
    disk_state = 0U;
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
  HAL_StatusTypeDef result;
  UINT index;

  if ((pdrv != SD_PHYSICAL_DRIVE) || (buff == NULL) || (count == 0U))
  {
    return RES_PARERR;
  }
  if ((disk_status(pdrv) & STA_NOINIT) != 0U)
  {
    return RES_NOTRDY;
  }
  for (index = 0U; index < count; ++index)
  {
    /* Polling-mode SDMMC has only a small FIFO service window. Keep each
       protected interval to one 512-byte sector so CAN and GUI latency stays
       bounded, while preventing task or ISR pre-emption during the transfer. */
    taskENTER_CRITICAL();
    result = HAL_SD_ReadBlocks(&hsd1, buff, sector, 1U, SD_IO_TIMEOUT_MS);
    taskEXIT_CRITICAL();
    if ((result != HAL_OK) || !WaitUntilReady(SD_READY_TIMEOUT_MS))
    {
      disk_state = STA_NOINIT;
      return RES_ERROR;
    }
    buff += 512U;
    ++sector;
  }
  return RES_OK;
}

#if _USE_WRITE == 1
DRESULT disk_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count)
{
  HAL_StatusTypeDef result;
  UINT index;

  if ((pdrv != SD_PHYSICAL_DRIVE) || (buff == NULL) || (count == 0U))
  {
    return RES_PARERR;
  }
  if ((disk_status(pdrv) & STA_NOINIT) != 0U)
  {
    return RES_NOTRDY;
  }
  for (index = 0U; index < count; ++index)
  {
    taskENTER_CRITICAL();
    result = HAL_SD_WriteBlocks(&hsd1, (uint8_t *)buff, sector, 1U,
                                SD_IO_TIMEOUT_MS);
    taskEXIT_CRITICAL();
    if ((result != HAL_OK) || !WaitUntilReady(SD_READY_TIMEOUT_MS))
    {
      disk_state = STA_NOINIT;
      return RES_ERROR;
    }
    buff += 512U;
    ++sector;
  }
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
      return WaitUntilReady(SD_READY_TIMEOUT_MS) ? RES_OK : RES_ERROR;

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
