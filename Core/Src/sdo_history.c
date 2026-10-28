#include "sdo_history.h"

#include "FreeRTOS.h"
#include "qspi_store.h"
#include "semphr.h"

#include <stdint.h>
#include <string.h>

#define SDO_HISTORY_RECORD_MAGIC   0x53444F48UL /* SDOH */
#define SDO_HISTORY_RECORD_VERSION 1U
#define SDO_HISTORY_SLOT_SIZE      QSPI_STORE_BLOCK_BYTES
#define SDO_HISTORY_SLOT0_ADDRESS  \
  (QSPI_STORE_FLASH_BYTES - (4U * SDO_HISTORY_SLOT_SIZE))
#define SDO_HISTORY_SLOT1_ADDRESS  \
  (QSPI_STORE_FLASH_BYTES - (3U * SDO_HISTORY_SLOT_SIZE))

typedef struct
{
  uint32_t value;
  uint16_t index;
  uint16_t timeout_ms;
  uint8_t node_id;
  uint8_t subindex;
  uint8_t command;
  uint8_t reserved;
} SdoHistoryStoredRequest;

typedef struct
{
  uint8_t count;
  uint8_t reserved[3];
  SdoHistoryStoredRequest requests[SDO_HISTORY_CAPACITY];
} SdoHistoryPayload;

typedef struct
{
  uint32_t magic;
  uint16_t record_version;
  uint16_t payload_size;
  uint32_t generation;
  uint32_t payload_crc32;
  uint32_t header_crc32;
} SdoHistoryRecordHeader;

typedef struct
{
  SdoHistoryRecordHeader header;
  SdoHistoryPayload payload;
} SdoHistoryRecord;

_Static_assert(sizeof(SdoHistoryRecord) <= SDO_HISTORY_SLOT_SIZE,
               "SDO history record exceeds its QSPI slot");

static SdoHistoryPayload history;
static uint32_t current_generation;
static uint8_t current_slot;
static bool storage_ready;
static StaticSemaphore_t history_mutex_control;
static SemaphoreHandle_t history_mutex;

static uint32_t Crc32(const void *data, uint32_t size)
{
  const uint8_t *bytes = (const uint8_t *)data;
  uint32_t crc = 0xFFFFFFFFUL;
  uint32_t bit;

  while (size-- != 0U)
  {
    crc ^= *bytes++;
    for (bit = 0U; bit < 8U; ++bit)
    {
      crc = (crc >> 1U) ^ ((crc & 1U) != 0U ? 0xEDB88320UL : 0U);
    }
  }
  return ~crc;
}

static uint32_t SlotAddress(uint8_t slot)
{
  return slot == 0U ? SDO_HISTORY_SLOT0_ADDRESS : SDO_HISTORY_SLOT1_ADDRESS;
}

static bool StoredRequestIsValid(const SdoHistoryStoredRequest *stored)
{
  CanTxSdoRequest request;
  CanTxFrame frame;

  memset(&request, 0, sizeof(request));
  request.value = stored->value;
  request.index = stored->index;
  request.timeout_ms = stored->timeout_ms;
  request.node_id = stored->node_id;
  request.subindex = stored->subindex;
  request.command = (CanTxSdoCommand)stored->command;
  return CanTx_BuildSdoRequest(&request, &frame);
}

static bool ReadRecord(uint8_t slot, SdoHistoryRecord *record)
{
  uint32_t header_crc;
  uint32_t index;

  memset(record, 0, sizeof(*record));
  if (!QspiStore_Read(SlotAddress(slot), record, sizeof(*record)) ||
      (record->header.magic != SDO_HISTORY_RECORD_MAGIC) ||
      (record->header.record_version != SDO_HISTORY_RECORD_VERSION) ||
      (record->header.payload_size != sizeof(SdoHistoryPayload)) ||
      (record->payload.count > SDO_HISTORY_CAPACITY))
  {
    return false;
  }
  header_crc = record->header.header_crc32;
  record->header.header_crc32 = 0U;
  if ((Crc32(&record->header, sizeof(record->header)) != header_crc) ||
      (Crc32(&record->payload, sizeof(record->payload)) !=
       record->header.payload_crc32))
  {
    return false;
  }
  for (index = 0U; index < record->payload.count; ++index)
  {
    if (!StoredRequestIsValid(&record->payload.requests[index])) return false;
  }
  record->header.header_crc32 = header_crc;
  return true;
}

static bool IsGenerationNewer(uint32_t left, uint32_t right)
{
  return (int32_t)(left - right) > 0;
}

static bool WriteRecord(void)
{
  SdoHistoryRecord record;
  SdoHistoryRecord verify;
  uint8_t target_slot = current_slot ^ 1U;

  memset(&record, 0, sizeof(record));
  record.header.magic = SDO_HISTORY_RECORD_MAGIC;
  record.header.record_version = SDO_HISTORY_RECORD_VERSION;
  record.header.payload_size = sizeof(SdoHistoryPayload);
  record.header.generation = current_generation + 1U;
  record.header.payload_crc32 = Crc32(&history, sizeof(history));
  record.header.header_crc32 = Crc32(&record.header, sizeof(record.header));
  record.payload = history;

  if (!QspiStore_EraseBlock(SlotAddress(target_slot)) ||
      !QspiStore_Write(SlotAddress(target_slot), &record, sizeof(record)) ||
      !ReadRecord(target_slot, &verify) ||
      (verify.header.generation != record.header.generation) ||
      (memcmp(&verify.payload, &history, sizeof(history)) != 0))
  {
    return false;
  }
  current_generation = record.header.generation;
  current_slot = target_slot;
  return true;
}

static SdoHistoryStoredRequest ToStored(const CanTxSdoRequest *request)
{
  SdoHistoryStoredRequest stored;
  memset(&stored, 0, sizeof(stored));
  stored.value = request->command == CAN_TX_SDO_READ ? 0U : request->value;
  stored.index = request->index;
  stored.timeout_ms = request->timeout_ms;
  stored.node_id = request->node_id;
  stored.subindex = request->subindex;
  stored.command = (uint8_t)request->command;
  return stored;
}

static bool SameRequest(const SdoHistoryStoredRequest *left,
                        const SdoHistoryStoredRequest *right)
{
  return (left->node_id == right->node_id) &&
         (left->command == right->command) &&
         (left->index == right->index) &&
         (left->subindex == right->subindex) &&
         (left->value == right->value);
}

void SdoHistory_Init(void)
{
  SdoHistoryRecord first;
  SdoHistoryRecord second;
  const SdoHistoryRecord *selected = NULL;
  bool first_valid;
  bool second_valid;

  memset(&history, 0, sizeof(history));
  current_generation = 0U;
  current_slot = 1U;
  storage_ready = false;
  history_mutex = xSemaphoreCreateMutexStatic(&history_mutex_control);
  if (!QspiStore_Init()) return;

  first_valid = ReadRecord(0U, &first);
  second_valid = ReadRecord(1U, &second);
  if (first_valid && second_valid)
  {
    selected = IsGenerationNewer(second.header.generation,
                                 first.header.generation) ? &second : &first;
  }
  else if (first_valid)
  {
    selected = &first;
  }
  else if (second_valid)
  {
    selected = &second;
  }

  if (selected != NULL)
  {
    history = selected->payload;
    current_generation = selected->header.generation;
    current_slot = selected == &second ? 1U : 0U;
  }
  storage_ready = true;
}

bool SdoHistory_Remember(const CanTxSdoRequest *request)
{
  CanTxFrame frame;
  SdoHistoryStoredRequest stored;
  uint32_t found;
  bool success;

  if ((request == NULL) || !storage_ready ||
      !CanTx_BuildSdoRequest(request, &frame))
  {
    return false;
  }
  stored = ToStored(request);
  (void)xSemaphoreTake(history_mutex, portMAX_DELAY);
  for (found = 0U; found < history.count; ++found)
  {
    if (SameRequest(&history.requests[found], &stored)) break;
  }
  if (found < history.count)
  {
    memmove(&history.requests[1], &history.requests[0],
            found * sizeof(history.requests[0]));
  }
  else
  {
    uint32_t move_count = history.count < SDO_HISTORY_CAPACITY ?
                          history.count : (SDO_HISTORY_CAPACITY - 1U);
    memmove(&history.requests[1], &history.requests[0],
            move_count * sizeof(history.requests[0]));
    if (history.count < SDO_HISTORY_CAPACITY) ++history.count;
  }
  history.requests[0] = stored;
  success = WriteRecord();
  (void)xSemaphoreGive(history_mutex);
  return success;
}

size_t SdoHistory_Copy(CanTxSdoRequest *requests, size_t capacity)
{
  size_t count;
  size_t index;

  if ((requests == NULL) || (capacity == 0U) || !storage_ready) return 0U;
  (void)xSemaphoreTake(history_mutex, portMAX_DELAY);
  count = history.count < capacity ? history.count : capacity;
  for (index = 0U; index < count; ++index)
  {
    const SdoHistoryStoredRequest *stored = &history.requests[index];
    requests[index].value = stored->value;
    requests[index].index = stored->index;
    requests[index].timeout_ms = stored->timeout_ms;
    requests[index].node_id = stored->node_id;
    requests[index].subindex = stored->subindex;
    requests[index].command = (CanTxSdoCommand)stored->command;
  }
  (void)xSemaphoreGive(history_mutex);
  return count;
}
