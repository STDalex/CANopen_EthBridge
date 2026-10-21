#include "app_settings.h"

#include "FreeRTOS.h"
#include "qspi_store.h"
#include "semphr.h"

#include <stddef.h>
#include <string.h>

#define SETTINGS_RECORD_MAGIC       0x43534E46UL /* CSNF */
#define SETTINGS_RECORD_VERSION     1U
#define SETTINGS_SLOT_COUNT         2U
#define SETTINGS_SLOT_SIZE          QSPI_STORE_BLOCK_BYTES
#define SETTINGS_FLASH_BYTES        QSPI_STORE_FLASH_BYTES
#define SETTINGS_SLOT0_ADDRESS      (SETTINGS_FLASH_BYTES - (2U * SETTINGS_SLOT_SIZE))
#define SETTINGS_SLOT1_ADDRESS      (SETTINGS_FLASH_BYTES - SETTINGS_SLOT_SIZE)

typedef struct
{
  uint32_t magic;
  uint16_t record_version;
  uint16_t payload_size;
  uint32_t generation;
  uint32_t payload_crc32;
  uint32_t header_crc32;
} SettingsRecordHeader;

typedef struct
{
  SettingsRecordHeader header;
  DeviceSettings payload;
} SettingsRecord;

static DeviceSettings current_settings;
static AppSettingsStorageState storage_state = APP_SETTINGS_STORAGE_UNINITIALIZED;
static uint32_t current_generation;
static uint8_t current_slot;
static StaticSemaphore_t settings_mutex_control;
static SemaphoreHandle_t settings_mutex;

static uint32_t Crc32(const void *data, uint32_t size)
{
  const uint8_t *bytes = (const uint8_t *)data;
  uint32_t crc = 0xFFFFFFFFUL;
  uint32_t index;

  while (size-- != 0U)
  {
    crc ^= *bytes++;
    for (index = 0U; index < 8U; ++index)
    {
      crc = (crc >> 1) ^ ((crc & 1U) != 0U ? 0xEDB88320UL : 0U);
    }
  }
  return ~crc;
}

static uint32_t SlotAddress(uint8_t slot)
{
  return slot == 0U ? SETTINGS_SLOT0_ADDRESS : SETTINGS_SLOT1_ADDRESS;
}

static bool ReadRecord(uint8_t slot, SettingsRecord *record)
{
  uint32_t header_crc;

  memset(record, 0, sizeof(*record));
  if (!QspiStore_Read(SlotAddress(slot), record, sizeof(*record)))
  {
    return false;
  }
  if ((record->header.magic != SETTINGS_RECORD_MAGIC) ||
      (record->header.record_version != SETTINGS_RECORD_VERSION) ||
      (record->header.payload_size != sizeof(DeviceSettings)))
  {
    return false;
  }

  header_crc = record->header.header_crc32;
  record->header.header_crc32 = 0U;
  if ((Crc32(&record->header, sizeof(record->header)) != header_crc) ||
      (Crc32(&record->payload, sizeof(record->payload)) != record->header.payload_crc32) ||
      !DeviceSettings_IsValid(&record->payload))
  {
    return false;
  }
  record->header.header_crc32 = header_crc;
  return true;
}

static bool IsGenerationNewer(uint32_t left, uint32_t right)
{
  return (int32_t)(left - right) > 0;
}

static bool WriteRecord(const DeviceSettings *settings)
{
  SettingsRecord record;
  SettingsRecord verify;
  uint8_t target_slot = current_slot ^ 1U;

  memset(&record, 0, sizeof(record));
  record.header.magic = SETTINGS_RECORD_MAGIC;
  record.header.record_version = SETTINGS_RECORD_VERSION;
  record.header.payload_size = sizeof(DeviceSettings);
  record.header.generation = current_generation + 1U;
  record.header.payload_crc32 = Crc32(settings, sizeof(*settings));
  record.header.header_crc32 = 0U;
  record.header.header_crc32 = Crc32(&record.header, sizeof(record.header));
  record.payload = *settings;

  if (!QspiStore_EraseBlock(SlotAddress(target_slot)) ||
      !QspiStore_Write(SlotAddress(target_slot), &record, sizeof(record)) ||
      !ReadRecord(target_slot, &verify) ||
      (verify.header.generation != record.header.generation) ||
      (memcmp(&verify.payload, settings, sizeof(*settings)) != 0))
  {
    return false;
  }

  current_generation = record.header.generation;
  current_slot = target_slot;
  return true;
}

void AppSettings_Init(void)
{
  SettingsRecord first;
  SettingsRecord second;
  bool first_valid;
  bool second_valid;

  DeviceSettings_SetDefaults(&current_settings);
  settings_mutex = xSemaphoreCreateMutexStatic(&settings_mutex_control);
  current_generation = 0U;
  current_slot = 1U;

  if (!QspiStore_Init())
  {
    storage_state = APP_SETTINGS_STORAGE_ERROR;
    return;
  }

  first_valid = ReadRecord(0U, &first);
  second_valid = ReadRecord(1U, &second);
  if (first_valid || second_valid)
  {
    const SettingsRecord *selected;
    if (first_valid && second_valid)
    {
      selected = IsGenerationNewer(second.header.generation, first.header.generation) ?
                 &second : &first;
      current_slot = selected == &second ? 1U : 0U;
    }
    else
    {
      selected = first_valid ? &first : &second;
      current_slot = first_valid ? 0U : 1U;
    }
    current_settings = selected->payload;
    current_generation = selected->header.generation;
    storage_state = APP_SETTINGS_STORAGE_READY;
  }
  else
  {
    storage_state = WriteRecord(&current_settings) ?
                    APP_SETTINGS_STORAGE_DEFAULTS : APP_SETTINGS_STORAGE_ERROR;
  }
}

void AppSettings_Get(DeviceSettings *settings)
{
  if (settings == NULL)
  {
    return;
  }
  if (settings_mutex != NULL)
  {
    (void)xSemaphoreTake(settings_mutex, portMAX_DELAY);
  }
  *settings = current_settings;
  if (settings_mutex != NULL)
  {
    (void)xSemaphoreGive(settings_mutex);
  }
}

bool AppSettings_Update(const DeviceSettings *settings, bool persist)
{
  bool success = false;

  if (!DeviceSettings_IsValid(settings))
  {
    return false;
  }
  if (settings_mutex != NULL)
  {
    (void)xSemaphoreTake(settings_mutex, portMAX_DELAY);
  }
  if (!persist || WriteRecord(settings))
  {
    current_settings = *settings;
    storage_state = APP_SETTINGS_STORAGE_READY;
    success = true;
  }
  else
  {
    storage_state = APP_SETTINGS_STORAGE_ERROR;
  }
  if (settings_mutex != NULL)
  {
    (void)xSemaphoreGive(settings_mutex);
  }
  return success;
}

bool AppSettings_UpdateNetwork(const NetworkSettings *network, bool persist)
{
  DeviceSettings settings;

  if (!DeviceSettings_IsValidNetwork(network))
  {
    return false;
  }
  AppSettings_Get(&settings);
  settings.network = *network;
  return AppSettings_Update(&settings, persist);
}

bool AppSettings_UpdateCan(uint32_t bitrate_kbps, bool deep_sdo_enabled, bool persist)
{
  DeviceSettings settings;

  AppSettings_Get(&settings);
  settings.can_bitrate_kbps = bitrate_kbps;
  settings.deep_sdo_enabled = deep_sdo_enabled;
  return AppSettings_Update(&settings, persist);
}

AppSettingsStorageState AppSettings_GetStorageState(void)
{
  return storage_state;
}

uint32_t AppSettings_GetGeneration(void)
{
  return current_generation;
}
