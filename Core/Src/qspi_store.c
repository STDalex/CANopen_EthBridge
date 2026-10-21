#include "qspi_store.h"

#include "FreeRTOS.h"
#include "semphr.h"
#include "stm32746g_discovery_qspi.h"

static StaticSemaphore_t qspi_mutex_control;
static SemaphoreHandle_t qspi_mutex;
static bool qspi_initialized;

static bool RangeIsValid(uint32_t address, uint32_t size)
{
  return (size <= QSPI_STORE_FLASH_BYTES) &&
         (address <= QSPI_STORE_FLASH_BYTES - size);
}

bool QspiStore_Init(void)
{
  if (qspi_initialized)
  {
    return true;
  }
  if (qspi_mutex == NULL)
  {
    qspi_mutex = xSemaphoreCreateMutexStatic(&qspi_mutex_control);
  }
  if ((qspi_mutex == NULL) || (BSP_QSPI_Init() != QSPI_OK))
  {
    return false;
  }
  qspi_initialized = true;
  return true;
}

bool QspiStore_Read(uint32_t address, void *data, uint32_t size)
{
  bool success;
  if (!qspi_initialized || (data == NULL) || !RangeIsValid(address, size))
  {
    return false;
  }
  (void)xSemaphoreTake(qspi_mutex, portMAX_DELAY);
  success = BSP_QSPI_Read((uint8_t *)data, address, size) == QSPI_OK;
  (void)xSemaphoreGive(qspi_mutex);
  return success;
}

bool QspiStore_Write(uint32_t address, const void *data, uint32_t size)
{
  bool success;
  if (!qspi_initialized || (data == NULL) || !RangeIsValid(address, size))
  {
    return false;
  }
  (void)xSemaphoreTake(qspi_mutex, portMAX_DELAY);
  success = BSP_QSPI_Write((uint8_t *)data, address, size) == QSPI_OK;
  (void)xSemaphoreGive(qspi_mutex);
  return success;
}

bool QspiStore_EraseBlock(uint32_t address)
{
  bool success;
  if (!qspi_initialized || ((address % QSPI_STORE_BLOCK_BYTES) != 0U) ||
      !RangeIsValid(address, QSPI_STORE_BLOCK_BYTES))
  {
    return false;
  }
  (void)xSemaphoreTake(qspi_mutex, portMAX_DELAY);
  success = BSP_QSPI_Erase_Block(address) == QSPI_OK;
  (void)xSemaphoreGive(qspi_mutex);
  return success;
}
