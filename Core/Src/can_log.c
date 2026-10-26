#include "can_log.h"

#include "app_version.h"
#include "canopen_decoder.h"
#include "cmsis_os.h"
#include "ff.h"
#include "memory_layout.h"
#include "queue.h"
#include "semphr.h"
#include "sdmmc.h"
#include "task.h"

#include <stdio.h>
#include <string.h>

#define CAN_LOG_FORMAT_MAJOR          1U
#define CAN_LOG_FORMAT_MINOR          0U
#define CAN_LOG_FILE_HEADER_SIZE      128U
#define CAN_LOG_BLOCK_HEADER_SIZE     32U
#define CAN_LOG_FOOTER_SIZE           64U
#define CAN_LOG_BLOCK_PAYLOAD_SIZE    (64U * 1024U)
#define CAN_LOG_BLOCK_COUNT           32U
#define CAN_LOG_ROTATE_BYTES          (32UL * 1024UL * 1024UL)
#define CAN_LOG_FREE_RESERVE_BYTES    (128UL * 1024UL)
#define CAN_LOG_MIN_FILE_BYTES        (1UL * 1024UL * 1024UL)
#define CAN_LOG_PARTIAL_FLUSH_MS      1000U
#define CAN_LOG_SYNC_INTERVAL_MS      2000U
#define CAN_LOG_SPACE_UPDATE_MS       5000U
#define CAN_LOG_MOUNT_RETRY_MS        250U
#define CAN_LOG_QUEUE_LENGTH          (CAN_LOG_BLOCK_COUNT + 8U)
#define CAN_LOG_INVALID_BLOCK         0xFFU
#define CAN_LOG_ROOT                  "0:/CANSNIFF"
#define CAN_LOG_DIRECTORY             "0:/CANSNIFF/LOGS"

#define CAN_LOG_ID_EXTENDED           (1UL << 29)
#define CAN_LOG_ID_REMOTE             (1UL << 30)

typedef enum
{
  CAN_LOG_ITEM_BLOCK = 0,
  CAN_LOG_ITEM_CONFIGURATION
} CanLogItemType;

typedef enum
{
  CAN_LOG_REQUEST_LIST = 0,
  CAN_LOG_REQUEST_DOWNLOAD_OPEN,
  CAN_LOG_REQUEST_DOWNLOAD_SEEK,
  CAN_LOG_REQUEST_DOWNLOAD_READ,
  CAN_LOG_REQUEST_DOWNLOAD_CLOSE
} CanLogRequestType;

typedef enum
{
  CAN_LOG_CLOSE_STOPPED = 0,
  CAN_LOG_CLOSE_ROTATED = 1,
  CAN_LOG_CLOSE_RECOVERED = 2,
  CAN_LOG_CLOSE_CONFIGURATION = 3
} CanLogCloseReason;

typedef struct
{
  uint8_t type;
  uint8_t block_index;
  uint8_t mode;
  uint8_t reserved;
  uint32_t generation;
  uint8_t node_filter[16];
} CanLogQueueItem;

typedef struct
{
  uint32_t payload_length;
  uint32_t frame_count;
  uint32_t generation;
  uint32_t started_tick;
  uint64_t first_timestamp_us;
} CanLogBlockMeta;

typedef struct
{
  uint32_t sequence;
  uint8_t type;
  uint8_t reserved[3];
  uint64_t offset;
  uint32_t length;
  char path[72];
} CanLogRequest;

typedef struct
{
  CanLogMode mode;
  uint32_t generation;
  uint8_t node_filter[16];
} CanLogConfiguration;

typedef struct
{
  uint32_t bitrate_kbps;
  CanLogMode mode;
  uint8_t node_filter[16];
  char firmware[16];
  uint32_t file_count;
  uint64_t frames;
  uint64_t dropped;
  uint32_t write_errors;
  bool clean_close;
  bool recovered;
} CanLogSessionMetadata;

static StaticQueue_t free_queue_control;
static QueueHandle_t free_queue;
static StaticQueue_t ready_queue_control;
static QueueHandle_t ready_queue;
static StaticQueue_t request_queue_control;
static QueueHandle_t request_queue;
static uint8_t request_queue_storage[sizeof(CanLogRequest)];
static StaticSemaphore_t request_mutex_control;
static SemaphoreHandle_t request_mutex;
static StaticSemaphore_t request_done_control;
static SemaphoreHandle_t request_done;
static TaskHandle_t storage_task_handle;
static CanLogFileInfo *const request_files =
  (CanLogFileInfo *)CAN_LOG_LIST_BUFFER_BASE;
static volatile uint32_t request_sequence;
static volatile uint32_t response_sequence;
static volatile uint32_t response_count;
static volatile uint32_t response_total;
static volatile bool response_has_more;
static volatile uint64_t response_size;
static volatile int32_t response_error;
static volatile bool response_success;
static uint8_t *const download_buffer =
  (uint8_t *)CAN_LOG_DOWNLOAD_BUFFER_BASE;

static CanLogBlockMeta block_meta[CAN_LOG_BLOCK_COUNT];
static volatile CanLogMode requested_mode;
static volatile uint32_t requested_generation;
static uint8_t requested_node_filter[16];
static CanLogStatus public_status;

static uint8_t producer_block;
static CanLogMode producer_mode;
static uint32_t producer_generation;
static uint8_t producer_node_filter[16];
static uint64_t producer_session_origin;
static bool producer_session_origin_valid;

static FATFS storage_fs;
static FIL storage_file;
static FIL storage_download_file;
static bool storage_mounted;
static bool storage_file_open;
static bool storage_download_open;
static CanLogConfiguration storage_configuration;
static char storage_session_path[48];
static char storage_temp_path[72];
static char storage_final_path[72];
static uint32_t storage_next_session;
static uint32_t storage_session_id;
static uint32_t storage_session_bitrate_kbps;
static uint32_t storage_file_index;
static uint32_t storage_block_sequence;
static uint32_t storage_blocks_in_file;
static uint64_t storage_frames_in_file;
static uint64_t storage_file_position;
static uint64_t storage_file_limit;
static uint64_t storage_session_frames;
static uint64_t storage_session_dropped_base;
static uint32_t storage_session_write_errors_base;
static uint32_t storage_last_sync_tick;
static uint32_t storage_last_space_tick;
static uint32_t storage_last_mount_tick;
static bool storage_pending_block_valid;
static CanLogQueueItem storage_pending_block;

static bool CaptureReadyForLogging(const CanCaptureStatus *status)
{
  return (status != NULL) && (status->bitrate_kbps != 0U) &&
         ((status->state == CAN_SYNC_LOCKED) ||
          (status->state == CAN_SYNC_MANUAL));
}

static FRESULT RewriteRecoveredSessionJson(const char *session_path,
                                           bool force_recovered);

#define STORAGE_IO_BUFFER_SIZE        512U
#define STORAGE_JSON_BUFFER_SIZE      1024U
static uint8_t *const storage_io_buffer = (uint8_t *)CAN_LOG_IO_BUFFER_BASE;
static char *const storage_json_buffer = (char *)CAN_LOG_JSON_BUFFER_BASE;

static uint8_t *BlockPayload(uint8_t index)
{
  return (uint8_t *)(CAN_LOG_SDRAM_BLOCK_POOL_BASE +
                     ((uint32_t)index * CAN_LOG_BLOCK_PAYLOAD_SIZE));
}

static void StoreU16(uint8_t *destination, uint16_t value)
{
  destination[0] = (uint8_t)value;
  destination[1] = (uint8_t)(value >> 8);
}

static void StoreU32(uint8_t *destination, uint32_t value)
{
  destination[0] = (uint8_t)value;
  destination[1] = (uint8_t)(value >> 8);
  destination[2] = (uint8_t)(value >> 16);
  destination[3] = (uint8_t)(value >> 24);
}

static void StoreU64(uint8_t *destination, uint64_t value)
{
  StoreU32(destination, (uint32_t)value);
  StoreU32(destination + 4, (uint32_t)(value >> 32));
}

static uint32_t LoadU32(const uint8_t *source)
{
  return (uint32_t)source[0] |
         ((uint32_t)source[1] << 8) |
         ((uint32_t)source[2] << 16) |
         ((uint32_t)source[3] << 24);
}

static uint64_t LoadU64(const uint8_t *source)
{
  return (uint64_t)LoadU32(source) |
         ((uint64_t)LoadU32(source + 4) << 32);
}

static uint32_t Crc32Update(uint32_t crc, const uint8_t *data, uint32_t length)
{
  uint32_t i;
  while (length-- != 0U)
  {
    crc ^= *data++;
    for (i = 0U; i < 8U; ++i)
    {
      crc = (crc >> 1) ^ ((crc & 1U) != 0U ? 0xEDB88320UL : 0U);
    }
  }
  return crc;
}

static uint32_t Crc32(const uint8_t *data, uint32_t length)
{
  return Crc32Update(0xFFFFFFFFUL, data, length) ^ 0xFFFFFFFFUL;
}

static bool FilterContains(const uint8_t filter[16], uint8_t node_id)
{
  if ((node_id == 0U) || (node_id > 127U))
  {
    return false;
  }
  return (filter[node_id >> 3] & (uint8_t)(1U << (node_id & 7U))) != 0U;
}

static uint32_t CountSelectedNodes(const uint8_t filter[16])
{
  uint32_t count = 0U;
  uint32_t node;
  for (node = 1U; node <= 127U; ++node)
  {
    if (FilterContains(filter, (uint8_t)node))
    {
      ++count;
    }
  }
  return count;
}

static void StatusAddDropped(uint64_t count)
{
  taskENTER_CRITICAL();
  public_status.dropped_frames += count;
  taskEXIT_CRITICAL();
}

static void StatusAddWriteError(int32_t error, uint64_t lost_frames)
{
  taskENTER_CRITICAL();
  ++public_status.write_errors;
  public_status.last_error = error;
  public_status.dropped_frames += lost_frames;
  taskEXIT_CRITICAL();
}

static void SnapshotConfiguration(CanLogConfiguration *configuration)
{
  taskENTER_CRITICAL();
  configuration->mode = requested_mode;
  configuration->generation = requested_generation;
  memcpy(configuration->node_filter, requested_node_filter,
         sizeof(configuration->node_filter));
  taskEXIT_CRITICAL();
}

static bool QueueCurrentProducerBlock(void)
{
  CanLogQueueItem item = {0};
  CanLogBlockMeta *meta;
  uint8_t index;

  if (producer_block == CAN_LOG_INVALID_BLOCK)
  {
    return true;
  }

  index = producer_block;
  producer_block = CAN_LOG_INVALID_BLOCK;
  meta = &block_meta[index];
  if (meta->frame_count == 0U)
  {
    (void)xQueueSend(free_queue, &index, 0U);
    return true;
  }

  item.type = CAN_LOG_ITEM_BLOCK;
  item.block_index = index;
  item.generation = meta->generation;
  if (xQueueSend(ready_queue, &item, 0U) != pdPASS)
  {
    StatusAddDropped(meta->frame_count);
    (void)xQueueSend(free_queue, &index, 0U);
    return false;
  }
  if (storage_task_handle != NULL)
  {
    xTaskNotifyGive(storage_task_handle);
  }
  return true;
}

static bool QueueConfiguration(const CanLogConfiguration *configuration)
{
  CanLogQueueItem item = {0};
  item.type = CAN_LOG_ITEM_CONFIGURATION;
  item.mode = (uint8_t)configuration->mode;
  item.generation = configuration->generation;
  memcpy(item.node_filter, configuration->node_filter, sizeof(item.node_filter));
  if (xQueueSend(ready_queue, &item, 0U) != pdPASS)
  {
    return false;
  }
  if (storage_task_handle != NULL)
  {
    xTaskNotifyGive(storage_task_handle);
  }
  return true;
}

static bool AllocateProducerBlock(uint64_t timestamp_us)
{
  uint8_t index;
  CanLogBlockMeta *meta;

  if (xQueueReceive(free_queue, &index, 0U) != pdPASS)
  {
    return false;
  }
  meta = &block_meta[index];
  memset(meta, 0, sizeof(*meta));
  meta->generation = producer_generation;
  meta->first_timestamp_us = timestamp_us;
  meta->started_tick = HAL_GetTick();
  producer_block = index;
  return true;
}

void CanLogger_Init(void)
{
  uint8_t index;

  free_queue = xQueueCreateStatic(CAN_LOG_BLOCK_COUNT, sizeof(uint8_t),
                                  (uint8_t *)CAN_LOG_FREE_QUEUE_STORAGE_BASE,
                                  &free_queue_control);
  ready_queue = xQueueCreateStatic(CAN_LOG_QUEUE_LENGTH, sizeof(CanLogQueueItem),
                                   (uint8_t *)CAN_LOG_READY_QUEUE_STORAGE_BASE,
                                   &ready_queue_control);
  request_queue = xQueueCreateStatic(1U, sizeof(CanLogRequest), request_queue_storage,
                                     &request_queue_control);
  request_mutex = xSemaphoreCreateMutexStatic(&request_mutex_control);
  request_done = xSemaphoreCreateBinaryStatic(&request_done_control);
  for (index = 0U; index < CAN_LOG_BLOCK_COUNT; ++index)
  {
    (void)xQueueSend(free_queue, &index, 0U);
  }

  requested_mode = CAN_LOG_MODE_OFF;
  requested_generation = 1U;
  memset(requested_node_filter, 0, sizeof(requested_node_filter));
  memset(&public_status, 0, sizeof(public_status));
  public_status.mode = CAN_LOG_MODE_OFF;
  public_status.card_state = CAN_LOG_CARD_ABSENT;
  producer_block = CAN_LOG_INVALID_BLOCK;
  producer_mode = CAN_LOG_MODE_OFF;
  producer_generation = 0U;
  producer_session_origin_valid = false;
  request_sequence = 0U;
  response_sequence = 0U;
  response_count = 0U;
  response_total = 0U;
  response_has_more = false;
  response_size = 0U;
  response_error = 0;
  response_success = false;
  storage_task_handle = NULL;
}

static bool ExecuteStorageRequest(CanLogRequest *request, uint32_t timeout_ms)
{
  TickType_t started = xTaskGetTickCount();
  TickType_t timeout = pdMS_TO_TICKS(timeout_ms);

  while (xSemaphoreTake(request_done, 0U) == pdTRUE)
  {
  }
  taskENTER_CRITICAL();
  request->sequence = ++request_sequence;
  taskEXIT_CRITICAL();
  if (xQueueSend(request_queue, request, 0U) != pdPASS)
  {
    return false;
  }
  if (storage_task_handle != NULL)
  {
    xTaskNotifyGive(storage_task_handle);
  }
  for (;;)
  {
    TickType_t elapsed = xTaskGetTickCount() - started;
    TickType_t remaining = elapsed < timeout ? timeout - elapsed : 0U;
    if (xSemaphoreTake(request_done, remaining) != pdTRUE)
    {
      return false;
    }
    if (response_sequence == request->sequence)
    {
      return true;
    }
  }
}

bool CanLogger_GetLogListPage(CanLogFileInfo *files, uint32_t capacity,
                              uint32_t before_session_id,
                              uint32_t before_file_index,
                              uint32_t *count, uint32_t *total,
                              bool *has_more, uint32_t timeout_ms)
{
  CanLogRequest request = {0};
  bool success = false;

  if ((files == NULL) || (count == NULL) || (total == NULL) ||
      (has_more == NULL) || (capacity == 0U))
  {
    return false;
  }
  *count = 0U;
  *total = 0U;
  *has_more = false;
  if (capacity > CAN_LOG_LIST_MAX)
  {
    capacity = CAN_LOG_LIST_MAX;
  }
  if (xSemaphoreTake(request_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
  {
    return false;
  }
  request.type = CAN_LOG_REQUEST_LIST;
  request.length = capacity;
  request.offset = ((uint64_t)before_session_id << 32U) |
                   (uint64_t)before_file_index;
  if (ExecuteStorageRequest(&request, timeout_ms))
  {
    uint32_t available = response_count;
    if (available > capacity)
    {
      available = capacity;
    }
    memcpy(files, request_files, available * sizeof(files[0]));
    *count = available;
    *total = response_total;
    *has_more = response_has_more;
    success = response_success;
  }
  (void)xSemaphoreGive(request_mutex);
  return success;
}

CanLogDownloadResult CanLogger_DownloadBegin(CanLogDownload *download,
                                             const char *path,
                                             uint32_t timeout_ms)
{
  CanLogRequest request = {0};
  CanLogDownloadResult result = CAN_LOG_DOWNLOAD_UNAVAILABLE;
  size_t path_length;
  bool responded;

  if ((download == NULL) || (path == NULL))
  {
    return CAN_LOG_DOWNLOAD_INVALID_PATH;
  }
  memset(download, 0, sizeof(*download));
  path_length = strlen(path);
  if ((path_length == 0U) || (path_length >= sizeof(request.path)))
  {
    return CAN_LOG_DOWNLOAD_INVALID_PATH;
  }
  if (xSemaphoreTake(request_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
  {
    return CAN_LOG_DOWNLOAD_UNAVAILABLE;
  }
  request.type = CAN_LOG_REQUEST_DOWNLOAD_OPEN;
  memcpy(request.path, path, path_length + 1U);
  responded = ExecuteStorageRequest(&request, timeout_ms);
  if (responded && response_success)
  {
    download->size_bytes = response_size;
    download->active = true;
    return CAN_LOG_DOWNLOAD_OK;
  }
  if (!responded)
  {
    result = CAN_LOG_DOWNLOAD_UNAVAILABLE;
  }
  else if (response_error == (int32_t)FR_INVALID_NAME)
  {
    result = CAN_LOG_DOWNLOAD_INVALID_PATH;
  }
  else if ((response_error == (int32_t)FR_NO_FILE) ||
           (response_error == (int32_t)FR_NO_PATH))
  {
    result = CAN_LOG_DOWNLOAD_NOT_FOUND;
  }
  else if (response_error != (int32_t)FR_NOT_READY)
  {
    result = CAN_LOG_DOWNLOAD_IO_ERROR;
  }
  (void)xSemaphoreGive(request_mutex);
  return result;
}

bool CanLogger_DownloadSeek(CanLogDownload *download, uint64_t offset,
                            uint32_t timeout_ms)
{
  CanLogRequest request = {0};
  if ((download == NULL) || !download->active ||
      (offset > download->size_bytes))
  {
    return false;
  }
  request.type = CAN_LOG_REQUEST_DOWNLOAD_SEEK;
  request.offset = offset;
  return ExecuteStorageRequest(&request, timeout_ms) && response_success;
}

bool CanLogger_DownloadRead(CanLogDownload *download, uint32_t requested,
                            const uint8_t **data, uint32_t *length,
                            uint32_t timeout_ms)
{
  CanLogRequest request = {0};
  if ((download == NULL) || !download->active || (data == NULL) ||
      (length == NULL) || (requested == 0U))
  {
    return false;
  }
  if (requested > CAN_LOG_DOWNLOAD_BUFFER_BYTES)
  {
    requested = CAN_LOG_DOWNLOAD_BUFFER_BYTES;
  }
  request.type = CAN_LOG_REQUEST_DOWNLOAD_READ;
  request.length = requested;
  if (!ExecuteStorageRequest(&request, timeout_ms) || !response_success)
  {
    return false;
  }
  *data = download_buffer;
  *length = response_count;
  return true;
}

void CanLogger_DownloadEnd(CanLogDownload *download, uint32_t timeout_ms)
{
  CanLogRequest request = {0};
  if ((download == NULL) || !download->active)
  {
    return;
  }
  request.type = CAN_LOG_REQUEST_DOWNLOAD_CLOSE;
  (void)ExecuteStorageRequest(&request, timeout_ms);
  download->active = false;
  download->size_bytes = 0U;
  (void)xSemaphoreGive(request_mutex);
}

void CanLogger_ProducerPoll(void)
{
  CanLogConfiguration requested;

  SnapshotConfiguration(&requested);
  if (requested.generation != producer_generation)
  {
    (void)QueueCurrentProducerBlock();
    if (QueueConfiguration(&requested))
    {
      producer_mode = requested.mode;
      producer_generation = requested.generation;
      memcpy(producer_node_filter, requested.node_filter,
             sizeof(producer_node_filter));
      producer_session_origin_valid = false;
    }
  }

  if (producer_block != CAN_LOG_INVALID_BLOCK)
  {
    CanLogBlockMeta *meta = &block_meta[producer_block];
    if ((uint32_t)(HAL_GetTick() - meta->started_tick) >= CAN_LOG_PARTIAL_FLUSH_MS)
    {
      (void)QueueCurrentProducerBlock();
    }
  }
}

void CanLogger_SubmitFrame(const CanFrameRecord *frame)
{
  CanopenDecodedFrame decoded;
  CanCaptureStatus capture_status;
  CanLogBlockMeta *meta;
  uint8_t *record;
  uint8_t data_length;
  uint32_t record_length;
  uint32_t id_flags;
  uint64_t capture_time;
  uint64_t timestamp;

  if (frame == NULL)
  {
    return;
  }
  capture_time = ((uint64_t)frame->capture_time_high << 32) |
                 frame->capture_time_us;
  CanLogger_ProducerPoll();

  if (producer_mode == CAN_LOG_MODE_OFF)
  {
    return;
  }
  CanControl_GetStatus(&capture_status);
  if (!CaptureReadyForLogging(&capture_status))
  {
    return;
  }
  if (producer_mode == CAN_LOG_MODE_SELECTED)
  {
    decoded = CanopenDecoder_Decode(frame);
    if (!decoded.has_node_id ||
        !FilterContains(producer_node_filter, decoded.node_id))
    {
      return;
    }
  }

  if (!producer_session_origin_valid)
  {
    producer_session_origin = capture_time;
    producer_session_origin_valid = true;
  }
  timestamp = capture_time - producer_session_origin;
  data_length = ((frame->flags & CAN_FRAME_FLAG_REMOTE) != 0U) ? 0U : frame->dlc;
  if (data_length > 8U)
  {
    data_length = 8U;
  }
  record_length = 9U + data_length;

  if (producer_block == CAN_LOG_INVALID_BLOCK)
  {
    if (!AllocateProducerBlock(timestamp))
    {
      StatusAddDropped(1U);
      return;
    }
  }
  meta = &block_meta[producer_block];
  if ((meta->payload_length + record_length) > CAN_LOG_BLOCK_PAYLOAD_SIZE)
  {
    (void)QueueCurrentProducerBlock();
    if (!AllocateProducerBlock(timestamp))
    {
      StatusAddDropped(1U);
      return;
    }
    meta = &block_meta[producer_block];
  }

  record = BlockPayload(producer_block) + meta->payload_length;
  StoreU32(record, (uint32_t)(timestamp - meta->first_timestamp_us));
  id_flags = frame->id & 0x1FFFFFFFUL;
  if ((frame->flags & CAN_FRAME_FLAG_EXTENDED) != 0U)
  {
    id_flags |= CAN_LOG_ID_EXTENDED;
  }
  if ((frame->flags & CAN_FRAME_FLAG_REMOTE) != 0U)
  {
    id_flags |= CAN_LOG_ID_REMOTE;
  }
  StoreU32(record + 4, id_flags);
  record[8] = frame->dlc & 0x0FU;
  if (data_length != 0U)
  {
    memcpy(record + 9, frame->data, data_length);
  }
  meta->payload_length += record_length;
  ++meta->frame_count;
}

void CanLogger_SetMode(CanLogMode mode)
{
  if (mode > CAN_LOG_MODE_SELECTED)
  {
    return;
  }
  taskENTER_CRITICAL();
  if (requested_mode != mode)
  {
    requested_mode = mode;
    ++requested_generation;
    public_status.mode = mode;
  }
  taskEXIT_CRITICAL();
}

CanLogMode CanLogger_GetMode(void)
{
  CanLogMode mode;
  taskENTER_CRITICAL();
  mode = requested_mode;
  taskEXIT_CRITICAL();
  return mode;
}

void CanLogger_SetNodeSelected(uint8_t node_id, bool selected)
{
  uint8_t mask;
  uint8_t *value;
  if ((node_id == 0U) || (node_id > 127U))
  {
    return;
  }
  mask = (uint8_t)(1U << (node_id & 7U));
  taskENTER_CRITICAL();
  value = &requested_node_filter[node_id >> 3];
  if (selected)
  {
    *value |= mask;
  }
  else
  {
    *value &= (uint8_t)~mask;
  }
  public_status.selected_node_count = CountSelectedNodes(requested_node_filter);
  if (requested_mode == CAN_LOG_MODE_SELECTED)
  {
    ++requested_generation;
  }
  taskEXIT_CRITICAL();
}

bool CanLogger_IsNodeSelected(uint8_t node_id)
{
  bool selected;
  taskENTER_CRITICAL();
  selected = FilterContains(requested_node_filter, node_id);
  taskEXIT_CRITICAL();
  return selected;
}

void CanLogger_ClearSelectedNodes(void)
{
  taskENTER_CRITICAL();
  memset(requested_node_filter, 0, sizeof(requested_node_filter));
  public_status.selected_node_count = 0U;
  if (requested_mode == CAN_LOG_MODE_SELECTED)
  {
    ++requested_generation;
  }
  taskEXIT_CRITICAL();
}

void CanLogger_GetStatus(CanLogStatus *status)
{
  StorageSdDiagnostics diagnostics;
  if (status == NULL)
  {
    return;
  }
  taskENTER_CRITICAL();
  *status = public_status;
  taskEXIT_CRITICAL();
  StorageSd_GetDiagnostics(&diagnostics);
  status->buffered_blocks = CAN_LOG_BLOCK_COUNT -
                            (uint32_t)uxQueueMessagesWaiting(free_queue);
  status->sd_io_errors = diagnostics.error_count;
  status->sd_retries = diagnostics.retry_count;
  status->sd_last_error_tick = diagnostics.last_error_tick;
  status->sd_last_sector = diagnostics.last_sector;
  status->sd_last_count = diagnostics.last_count;
  status->sd_last_hal_error = diagnostics.last_hal_error;
  status->sd_last_status = diagnostics.last_sdmmc_status;
  status->sd_max_read_ms = diagnostics.max_read_ms;
  status->sd_max_write_ms = diagnostics.max_write_ms;
  status->sd_last_operation = diagnostics.last_operation;
  status->sd_last_hal_status = diagnostics.last_hal_status;
  status->sd_last_handle_state = diagnostics.last_handle_state;
}

void CanLogger_ResetCounters(void)
{
  bool reset_sd = false;
  taskENTER_CRITICAL();
  if (requested_mode == CAN_LOG_MODE_OFF)
  {
    public_status.stored_frames = 0U;
    public_status.dropped_frames = 0U;
    public_status.write_errors = 0U;
    public_status.recovery_count = 0U;
    public_status.last_error = 0;
    reset_sd = true;
  }
  taskEXIT_CRITICAL();
  if (reset_sd)
  {
    StorageSd_ResetDiagnostics();
  }
}

const char *CanLogger_ModeName(CanLogMode mode)
{
  switch (mode)
  {
    case CAN_LOG_MODE_FULL: return "Full";
    case CAN_LOG_MODE_SELECTED: return "Selected";
    default: return "Off";
  }
}

const char *CanLogger_CardStateName(CanLogCardState state)
{
  switch (state)
  {
    case CAN_LOG_CARD_MOUNTING: return "Mounting";
    case CAN_LOG_CARD_READY: return "Ready";
    case CAN_LOG_CARD_RECORDING: return "Recording";
    case CAN_LOG_CARD_RECOVERING: return "Recovering";
    case CAN_LOG_CARD_FULL: return "Full";
    case CAN_LOG_CARD_UNSUPPORTED: return "Unsupported";
    case CAN_LOG_CARD_ERROR: return "Error";
    default: return "No card";
  }
}

static void StorageSetState(CanLogCardState state, int32_t error)
{
  taskENTER_CRITICAL();
  public_status.card_state = state;
  /* Keep the last real error visible after a successful remount.  It is
     cleared explicitly together with the counters, not by transient
     Mounting/Ready/Recording state changes. */
  if (error != 0)
  {
    public_status.last_error = error;
  }
  taskEXIT_CRITICAL();
}

static bool StorageStopRequested(void)
{
  bool stopped;
  taskENTER_CRITICAL();
  stopped = requested_mode == CAN_LOG_MODE_OFF;
  taskEXIT_CRITICAL();
  return stopped;
}

static FRESULT StorageWrite(FIL *file, const void *data, UINT length)
{
  UINT written = 0U;
  FRESULT result = f_write(file, data, length, &written);
  if ((result == FR_OK) && (written != length))
  {
    return FR_DISK_ERR;
  }
  return result;
}

static FRESULT StorageRead(FIL *file, void *data, UINT length)
{
  UINT read = 0U;
  FRESULT result = f_read(file, data, length, &read);
  if ((result == FR_OK) && (read != length))
  {
    return FR_INT_ERR;
  }
  return result;
}

static FRESULT StorageUpdateFreeSpace(void)
{
  FATFS *file_system = NULL;
  DWORD free_clusters = 0U;
  FRESULT result = f_getfree("0:", &free_clusters, &file_system);
  if ((result == FR_OK) && (file_system != NULL))
  {
    uint64_t free_bytes = (uint64_t)free_clusters * file_system->csize * 512U;
    taskENTER_CRITICAL();
    public_status.free_bytes = free_bytes;
    taskEXIT_CRITICAL();
  }
  return result;
}

static FRESULT EnsureDirectory(const char *path)
{
  FRESULT result = f_mkdir(path);
  return (result == FR_EXIST) ? FR_OK : result;
}

static bool MetadataCandidateComplete(const char *path)
{
  FIL file;
  FRESULT result;
  FSIZE_t size;
  uint8_t marker[2];
  bool complete = false;

  result = f_open(&file, path, FA_READ);
  if (result != FR_OK)
  {
    return false;
  }
  size = f_size(&file);
  if ((size >= 4U) && (size <= STORAGE_JSON_BUFFER_SIZE) &&
      (StorageRead(&file, marker, 1U) == FR_OK) && (marker[0] == '{') &&
      (f_lseek(&file, size - 2U) == FR_OK) &&
      (StorageRead(&file, marker, 2U) == FR_OK) &&
      (marker[0] == '}') && (marker[1] == '\n'))
  {
    complete = true;
  }
  (void)f_close(&file);
  return complete;
}

static FRESULT RecoverSessionJsonTransaction(const char *session_path)
{
  FILINFO info;
  char final_path[72];
  char pending_path[72];
  FRESULT pending_result;
  FRESULT final_result;

  (void)snprintf(final_path, sizeof(final_path), "%s/SESSION.JSON", session_path);
  (void)snprintf(pending_path, sizeof(pending_path), "%s/SESSION.NEW", session_path);
  pending_result = f_stat(pending_path, &info);
  if (pending_result == FR_NO_FILE)
  {
    return FR_OK;
  }
  if (pending_result != FR_OK)
  {
    return pending_result;
  }

  final_result = f_stat(final_path, &info);
  if (final_result == FR_OK)
  {
    return f_unlink(pending_path);
  }
  if (final_result != FR_NO_FILE)
  {
    return final_result;
  }
  if (!MetadataCandidateComplete(pending_path))
  {
    return f_unlink(pending_path);
  }
  return f_rename(pending_path, final_path);
}

static bool ParseSessionDirectory(const char *name, uint32_t *session_id)
{
  uint32_t value = 0U;
  uint32_t i;
  if ((name == NULL) || (name[0] != 'S') || (strlen(name) != 7U))
  {
    return false;
  }
  for (i = 1U; i < 7U; ++i)
  {
    if ((name[i] < '0') || (name[i] > '9'))
    {
      return false;
    }
    value = (value * 10U) + (uint32_t)(name[i] - '0');
  }
  *session_id = value;
  return true;
}

static FRESULT FindLatestSession(uint32_t *latest_session)
{
  DIR directory;
  FILINFO info;
  FRESULT result;
  uint32_t latest = 0U;

  result = f_opendir(&directory, CAN_LOG_DIRECTORY);
  if (result != FR_OK)
  {
    return result;
  }
  for (;;)
  {
    uint32_t candidate;
    result = f_readdir(&directory, &info);
    if ((result != FR_OK) || (info.fname[0] == '\0'))
    {
      break;
    }
    if (((info.fattrib & AM_DIR) != 0U) &&
        ParseSessionDirectory(info.fname, &candidate) &&
        (candidate > latest))
    {
      latest = candidate;
    }
  }
  (void)f_closedir(&directory);
  if (result == FR_OK)
  {
    *latest_session = latest;
  }
  return result;
}

static void BuildFooter(uint8_t footer[CAN_LOG_FOOTER_SIZE],
                        CanLogCloseReason reason, uint32_t blocks,
                        uint64_t frames, uint64_t dropped,
                        uint32_t write_errors)
{
  memset(footer, 0, CAN_LOG_FOOTER_SIZE);
  memcpy(footer, "END1", 4U);
  StoreU32(footer + 4, (uint32_t)reason);
  StoreU32(footer + 8, blocks);
  StoreU64(footer + 16, frames);
  StoreU64(footer + 24, dropped);
  StoreU32(footer + 32, write_errors);
  StoreU32(footer + 60, Crc32(footer, 60U));
}

static bool HeaderIsValid(const uint8_t header[CAN_LOG_FILE_HEADER_SIZE])
{
  return (memcmp(header, "CSLG", 4U) == 0) &&
         (header[4] == CAN_LOG_FORMAT_MAJOR) &&
         (LoadU32(header + 124) == Crc32(header, 124U));
}

static FRESULT RecoverOneFile(const char *temporary_path, const char *final_path,
                              const char *bad_path, bool *recovered)
{
  FIL file;
  FRESULT result;
  uint64_t valid_end = CAN_LOG_FILE_HEADER_SIZE;
  uint32_t blocks = 0U;
  uint64_t frames = 0U;
  bool footer_found = false;

  if (recovered != NULL)
  {
    *recovered = false;
  }

  result = f_open(&file, temporary_path, FA_READ | FA_WRITE);
  if (result != FR_OK)
  {
    return result;
  }
  result = StorageRead(&file, storage_io_buffer, CAN_LOG_FILE_HEADER_SIZE);
  if ((result != FR_OK) || !HeaderIsValid(storage_io_buffer))
  {
    (void)f_close(&file);
    return f_rename(temporary_path, bad_path);
  }

  for (;;)
  {
    uint64_t block_start = valid_end;
    uint32_t payload_length;
    uint32_t frame_count;
    uint32_t expected_crc;
    uint32_t crc;
    uint32_t remaining;

    if (f_lseek(&file, (FSIZE_t)block_start) != FR_OK)
    {
      break;
    }
    result = StorageRead(&file, storage_io_buffer, 4U);
    if (result != FR_OK)
    {
      result = FR_OK;
      break;
    }
    if (memcmp(storage_io_buffer, "END1", 4U) == 0)
    {
      if ((f_lseek(&file, (FSIZE_t)block_start) == FR_OK) &&
          (StorageRead(&file, storage_io_buffer, CAN_LOG_FOOTER_SIZE) == FR_OK) &&
          (LoadU32(storage_io_buffer + 60) == Crc32(storage_io_buffer, 60U)))
      {
        valid_end += CAN_LOG_FOOTER_SIZE;
        footer_found = true;
      }
      break;
    }
    if (memcmp(storage_io_buffer, "BLK1", 4U) != 0)
    {
      break;
    }
    if ((f_lseek(&file, (FSIZE_t)block_start) != FR_OK) ||
        (StorageRead(&file, storage_io_buffer, CAN_LOG_BLOCK_HEADER_SIZE) != FR_OK) ||
        (LoadU32(storage_io_buffer + 28) != Crc32(storage_io_buffer, 28U)))
    {
      break;
    }
    frame_count = LoadU32(storage_io_buffer + 16);
    payload_length = LoadU32(storage_io_buffer + 20);
    expected_crc = LoadU32(storage_io_buffer + 24);
    if ((payload_length == 0U) || (payload_length > CAN_LOG_BLOCK_PAYLOAD_SIZE))
    {
      break;
    }

    crc = 0xFFFFFFFFUL;
    remaining = payload_length;
    while (remaining != 0U)
    {
      UINT chunk = (remaining > STORAGE_IO_BUFFER_SIZE) ?
                   STORAGE_IO_BUFFER_SIZE : (UINT)remaining;
      if (StorageRead(&file, storage_io_buffer, chunk) != FR_OK)
      {
        remaining = 0xFFFFFFFFUL;
        break;
      }
      crc = Crc32Update(crc, storage_io_buffer, chunk);
      remaining -= chunk;
    }
    if ((remaining != 0U) || ((crc ^ 0xFFFFFFFFUL) != expected_crc))
    {
      break;
    }
    valid_end += CAN_LOG_BLOCK_HEADER_SIZE + payload_length;
    ++blocks;
    frames += frame_count;
  }

  if (!footer_found)
  {
    uint8_t footer[CAN_LOG_FOOTER_SIZE];
    BuildFooter(footer, CAN_LOG_CLOSE_RECOVERED, blocks, frames, 0U, 0U);
    result = f_lseek(&file, (FSIZE_t)valid_end);
    if (result == FR_OK)
    {
      result = StorageWrite(&file, footer, sizeof(footer));
      valid_end += sizeof(footer);
    }
  }
  if (result == FR_OK)
  {
    result = f_lseek(&file, (FSIZE_t)valid_end);
  }
  if (result == FR_OK)
  {
    result = f_truncate(&file);
  }
  if (result == FR_OK)
  {
    result = f_sync(&file);
  }
  if (f_close(&file) != FR_OK && result == FR_OK)
  {
    result = FR_DISK_ERR;
  }
  if (result == FR_OK)
  {
    result = f_rename(temporary_path, final_path);
  }
  if (result == FR_OK)
  {
    taskENTER_CRITICAL();
    ++public_status.recovery_count;
    taskEXIT_CRITICAL();
    if (recovered != NULL)
    {
      *recovered = true;
    }
  }
  return result;
}

static FRESULT BuildAvailableRecoveryPath(const char *session_path,
                                          const char *base_name,
                                          const char *extension,
                                          char *path, size_t capacity)
{
  FILINFO info;
  FRESULT result;
  uint32_t suffix;

  (void)snprintf(path, capacity, "%s/%s.%s",
                 session_path, base_name, extension);
  result = f_stat(path, &info);
  if (result == FR_NO_FILE)
  {
    return FR_OK;
  }
  if (result != FR_OK)
  {
    return result;
  }

  for (suffix = 1U; suffix <= 99U; ++suffix)
  {
    (void)snprintf(path, capacity, "%s/%s_R%02lu.%s",
                   session_path, base_name, (unsigned long)suffix, extension);
    result = f_stat(path, &info);
    if (result == FR_NO_FILE)
    {
      return FR_OK;
    }
    if (result != FR_OK)
    {
      return result;
    }
  }
  return FR_EXIST;
}

static FRESULT RecoverLatestSession(uint32_t latest_session)
{
  DIR directory;
  FILINFO info;
  FRESULT result;
  char session_path[48];
  bool recovered_any = false;

  if (latest_session == 0U)
  {
    return FR_OK;
  }
  (void)snprintf(session_path, sizeof(session_path), "%s/S%06lu",
                 CAN_LOG_DIRECTORY, (unsigned long)latest_session);
  result = RecoverSessionJsonTransaction(session_path);
  if (result != FR_OK)
  {
    return result;
  }
  result = f_opendir(&directory, session_path);
  if (result != FR_OK)
  {
    return result;
  }
  for (;;)
  {
    char temporary_path[72];
    char final_path[72];
    char bad_path[72];
    char *extension;
    bool recovered_file = false;

    result = f_readdir(&directory, &info);
    if ((result != FR_OK) || (info.fname[0] == '\0'))
    {
      break;
    }
    extension = strrchr(info.fname, '.');
    if ((extension == NULL) || (strcmp(extension, ".TMP") != 0))
    {
      continue;
    }
    (void)snprintf(temporary_path, sizeof(temporary_path), "%s/%s",
                   session_path, info.fname);
    *extension = '\0';
    result = BuildAvailableRecoveryPath(session_path, info.fname, "CSL",
                                        final_path, sizeof(final_path));
    if (result == FR_OK)
    {
      result = BuildAvailableRecoveryPath(session_path, info.fname, "BAD",
                                          bad_path, sizeof(bad_path));
    }
    if (result != FR_OK)
    {
      break;
    }
    StorageSetState(CAN_LOG_CARD_RECOVERING, 0);
    result = RecoverOneFile(temporary_path, final_path, bad_path,
                            &recovered_file);
    if (result != FR_OK)
    {
      break;
    }
    recovered_any = recovered_any || recovered_file;
  }
  (void)f_closedir(&directory);
  if (result == FR_OK)
  {
    result = RewriteRecoveredSessionJson(session_path, recovered_any);
  }
  return result;
}

static FRESULT StorageMount(void)
{
  FRESULT result;
  uint32_t latest_session = 0U;

  StorageSetState(CAN_LOG_CARD_MOUNTING, 0);
  result = f_mount(&storage_fs, "0:", 1U);
  if (result != FR_OK)
  {
    StorageSetState(result == FR_NO_FILESYSTEM ? CAN_LOG_CARD_UNSUPPORTED :
                    CAN_LOG_CARD_ERROR, (int32_t)result);
    StorageSd_DeInit();
    return result;
  }
  storage_mounted = true;
  result = EnsureDirectory(CAN_LOG_ROOT);
  if (result == FR_OK)
  {
    result = EnsureDirectory(CAN_LOG_DIRECTORY);
  }
  if (result == FR_OK)
  {
    result = FindLatestSession(&latest_session);
  }
  if (result == FR_OK)
  {
    result = RecoverLatestSession(latest_session);
  }
  if (result == FR_OK)
  {
    storage_next_session = latest_session + 1U;
    if (storage_next_session == 0U || storage_next_session > 999999U)
    {
      storage_next_session = 1U;
    }
    result = StorageUpdateFreeSpace();
  }
  if (result != FR_OK)
  {
    (void)f_mount(NULL, "0:", 0U);
    StorageSd_DeInit();
    storage_mounted = false;
    StorageSetState(CAN_LOG_CARD_ERROR, (int32_t)result);
    return result;
  }
  StorageSetState(CAN_LOG_CARD_READY, 0);
  return FR_OK;
}

static void StorageUnmount(CanLogCardState state, int32_t error)
{
  if (storage_download_open)
  {
    (void)f_close(&storage_download_file);
    storage_download_open = false;
  }
  storage_file_open = false;
  storage_file_position = 0U;
  storage_file_limit = 0U;
  storage_session_id = 0U;
  storage_session_bitrate_kbps = 0U;
  storage_session_path[0] = '\0';
  (void)f_mount(NULL, "0:", 0U);
  StorageSd_DeInit();
  storage_mounted = false;
  taskENTER_CRITICAL();
  public_status.file_name[0] = '\0';
  public_status.file_size_bytes = 0U;
  public_status.free_bytes = 0U;
  public_status.card_state = state;
  public_status.last_error = error;
  taskEXIT_CRITICAL();
}

static void BuildFileHeader(uint8_t header[CAN_LOG_FILE_HEADER_SIZE])
{
  memset(header, 0, CAN_LOG_FILE_HEADER_SIZE);
  memcpy(header, "CSLG", 4U);
  StoreU16(header + 4, CAN_LOG_FORMAT_MAJOR);
  StoreU16(header + 6, CAN_LOG_FORMAT_MINOR);
  StoreU16(header + 8, CAN_LOG_FILE_HEADER_SIZE);
  StoreU16(header + 10, 1U);
  StoreU32(header + 12, storage_session_bitrate_kbps * 1000U);
  header[16] = (uint8_t)storage_configuration.mode;
  header[17] = 1U;
  StoreU32(header + 20, storage_session_id);
  StoreU32(header + 24, storage_file_index);
  StoreU32(header + 28, CAN_LOG_BLOCK_PAYLOAD_SIZE);
  memcpy(header + 32, storage_configuration.node_filter, 16U);
  (void)snprintf((char *)(header + 48), 16U, "%s", CAN_SNIFFER_VERSION);
  StoreU32(header + 124, Crc32(header, 124U));
}

static void BuildSelectedNodesText(const uint8_t node_filter[16],
                                   char *text, size_t capacity)
{
  uint32_t node;
  size_t used = 0U;
  if (capacity == 0U)
  {
    return;
  }
  text[used++] = '[';
  for (node = 1U; node <= 127U; ++node)
  {
    if (FilterContains(node_filter, (uint8_t)node))
    {
      int count = snprintf(text + used, capacity - used, "%s%lu",
                           used > 1U ? "," : "", (unsigned long)node);
      if ((count < 0) || ((size_t)count >= (capacity - used)))
      {
        break;
      }
      used += (size_t)count;
    }
  }
  if (used < (capacity - 1U))
  {
    text[used++] = ']';
  }
  text[used] = '\0';
}

static FRESULT WriteSessionMetadataAtomic(const char *session_path,
                                          const CanLogSessionMetadata *metadata)
{
  FIL metadata_file;
  FILINFO info;
  char final_path[72];
  char pending_path[72];
  char selected_nodes[512];
  FRESULT result;
  FRESULT close_result;
  int length;

  if ((session_path == NULL) || (metadata == NULL))
  {
    return FR_INVALID_PARAMETER;
  }
  BuildSelectedNodesText(metadata->node_filter, selected_nodes,
                         sizeof(selected_nodes));
  length = snprintf(storage_json_buffer, STORAGE_JSON_BUFFER_SIZE,
    "{\n"
    "  \"format\": \"CanSniff Log\",\n"
    "  \"format_version\": \"%u.%u\",\n"
    "  \"firmware\": \"%s\",\n"
    "  \"bitrate\": %lu,\n"
    "  \"mode\": \"%s\",\n"
    "  \"selected_nodes\": %s,\n"
    "  \"files\": %lu,\n"
    "  \"frames_stored\": %llu,\n"
    "  \"frames_dropped\": %llu,\n"
    "  \"write_errors\": %lu,\n"
    "  \"recovered\": %s,\n"
    "  \"clean_close\": %s\n"
    "}\n",
    CAN_LOG_FORMAT_MAJOR, CAN_LOG_FORMAT_MINOR, metadata->firmware,
    (unsigned long)(metadata->bitrate_kbps * 1000U),
    CanLogger_ModeName(metadata->mode), selected_nodes,
    (unsigned long)metadata->file_count,
    (unsigned long long)metadata->frames,
    (unsigned long long)metadata->dropped,
    (unsigned long)metadata->write_errors,
    metadata->recovered ? "true" : "false",
    metadata->clean_close ? "true" : "false");
  if ((length <= 0) || ((size_t)length >= STORAGE_JSON_BUFFER_SIZE))
  {
    return FR_INVALID_PARAMETER;
  }
  (void)snprintf(final_path, sizeof(final_path), "%s/SESSION.JSON", session_path);
  (void)snprintf(pending_path, sizeof(pending_path), "%s/SESSION.NEW", session_path);

  result = f_unlink(pending_path);
  if ((result != FR_OK) && (result != FR_NO_FILE))
  {
    return result;
  }
  result = f_open(&metadata_file, pending_path, FA_CREATE_ALWAYS | FA_WRITE);
  if (result != FR_OK)
  {
    return result;
  }
  result = StorageWrite(&metadata_file, storage_json_buffer, (UINT)length);
  if (result == FR_OK)
  {
    result = f_sync(&metadata_file);
  }
  close_result = f_close(&metadata_file);
  if ((result == FR_OK) && (close_result != FR_OK))
  {
    result = close_result;
  }
  if (result != FR_OK)
  {
    return result;
  }

  result = f_stat(final_path, &info);
  if (result == FR_OK)
  {
    result = f_unlink(final_path);
  }
  else if (result == FR_NO_FILE)
  {
    result = FR_OK;
  }
  if (result == FR_OK)
  {
    result = f_rename(pending_path, final_path);
  }
  return result;
}

static FRESULT WriteSessionJson(bool clean_close)
{
  CanLogStatus status;
  CanLogSessionMetadata metadata = {0};

  if (!storage_mounted || (storage_session_path[0] == '\0'))
  {
    return FR_NOT_READY;
  }
  CanLogger_GetStatus(&status);
  metadata.bitrate_kbps = storage_session_bitrate_kbps;
  metadata.mode = storage_configuration.mode;
  memcpy(metadata.node_filter, storage_configuration.node_filter,
         sizeof(metadata.node_filter));
  (void)snprintf(metadata.firmware, sizeof(metadata.firmware), "%s",
                 CAN_SNIFFER_VERSION);
  metadata.file_count = storage_file_index;
  metadata.frames = storage_session_frames;
  metadata.dropped = status.dropped_frames - storage_session_dropped_base;
  metadata.write_errors = status.write_errors - storage_session_write_errors_base;
  metadata.clean_close = clean_close;
  metadata.recovered = false;
  return WriteSessionMetadataAtomic(storage_session_path, &metadata);
}

static FRESULT RewriteRecoveredSessionJson(const char *session_path,
                                           bool force_recovered)
{
  DIR directory;
  FILINFO info;
  CanLogSessionMetadata metadata = {0};
  FRESULT result;

  metadata.clean_close = true;
  metadata.recovered = force_recovered;
  result = f_opendir(&directory, session_path);
  if (result != FR_OK)
  {
    return result;
  }
  for (;;)
  {
    FIL file;
    FSIZE_t size;
    char file_path[72];
    char *extension;
    uint32_t reason;
    uint64_t dropped;
    uint32_t write_errors;

    result = f_readdir(&directory, &info);
    if ((result != FR_OK) || (info.fname[0] == '\0'))
    {
      break;
    }
    extension = strrchr(info.fname, '.');
    if (((info.fattrib & AM_DIR) != 0U) || (extension == NULL) ||
        (strcmp(extension, ".CSL") != 0))
    {
      continue;
    }
    (void)snprintf(file_path, sizeof(file_path), "%s/%s",
                   session_path, info.fname);
    result = f_open(&file, file_path, FA_READ);
    if (result != FR_OK)
    {
      break;
    }
    size = f_size(&file);
    if ((size < (CAN_LOG_FILE_HEADER_SIZE + CAN_LOG_FOOTER_SIZE)) ||
        (StorageRead(&file, storage_io_buffer, CAN_LOG_FILE_HEADER_SIZE) != FR_OK) ||
        !HeaderIsValid(storage_io_buffer))
    {
      (void)f_close(&file);
      result = FR_INT_ERR;
      break;
    }
    if (metadata.file_count == 0U)
    {
      metadata.bitrate_kbps = LoadU32(storage_io_buffer + 12) / 1000U;
      metadata.mode = (CanLogMode)storage_io_buffer[16];
      memcpy(metadata.node_filter, storage_io_buffer + 32,
             sizeof(metadata.node_filter));
      memcpy(metadata.firmware, storage_io_buffer + 48,
             sizeof(metadata.firmware));
      metadata.firmware[sizeof(metadata.firmware) - 1U] = '\0';
    }
    if ((f_lseek(&file, size - CAN_LOG_FOOTER_SIZE) != FR_OK) ||
        (StorageRead(&file, storage_io_buffer, CAN_LOG_FOOTER_SIZE) != FR_OK) ||
        (memcmp(storage_io_buffer, "END1", 4U) != 0) ||
        (LoadU32(storage_io_buffer + 60) != Crc32(storage_io_buffer, 60U)))
    {
      (void)f_close(&file);
      result = FR_INT_ERR;
      break;
    }
    reason = LoadU32(storage_io_buffer + 4);
    metadata.frames += LoadU64(storage_io_buffer + 16);
    dropped = LoadU64(storage_io_buffer + 24);
    write_errors = LoadU32(storage_io_buffer + 32);
    if (dropped > metadata.dropped)
    {
      metadata.dropped = dropped;
    }
    if (write_errors > metadata.write_errors)
    {
      metadata.write_errors = write_errors;
    }
    if (reason == CAN_LOG_CLOSE_RECOVERED)
    {
      metadata.clean_close = false;
      metadata.recovered = true;
    }
    ++metadata.file_count;
    if (f_close(&file) != FR_OK)
    {
      result = FR_DISK_ERR;
      break;
    }
  }
  (void)f_closedir(&directory);
  if ((result == FR_OK) && (metadata.file_count == 0U))
  {
    return FR_OK;
  }
  if ((result == FR_OK) && metadata.recovered)
  {
    result = WriteSessionMetadataAtomic(session_path, &metadata);
  }
  return result;
}

static FRESULT OpenNewLogFile(void)
{
  uint8_t header[CAN_LOG_FILE_HEADER_SIZE];
  FRESULT result;
  CanLogStatus status;
  uint64_t available;

  result = StorageUpdateFreeSpace();
  if (result != FR_OK)
  {
    return result;
  }
  CanLogger_GetStatus(&status);
  if (status.free_bytes <= (CAN_LOG_FREE_RESERVE_BYTES + CAN_LOG_MIN_FILE_BYTES))
  {
    StorageSetState(CAN_LOG_CARD_FULL, (int32_t)FR_DENIED);
    return FR_DENIED;
  }
  available = status.free_bytes - CAN_LOG_FREE_RESERVE_BYTES;
  storage_file_limit = available < CAN_LOG_ROTATE_BYTES ? available : CAN_LOG_ROTATE_BYTES;
  storage_file_limit &= ~511ULL;

  for (;;)
  {
    char base_name[16];
    (void)snprintf(base_name, sizeof(base_name), "L%06lu",
                   (unsigned long)storage_file_index);
    (void)snprintf(storage_temp_path, sizeof(storage_temp_path), "%s/%s.TMP",
                   storage_session_path, base_name);
    (void)snprintf(storage_final_path, sizeof(storage_final_path), "%s/%s.CSL",
                   storage_session_path, base_name);
    result = f_open(&storage_file, storage_temp_path,
                    FA_CREATE_NEW | FA_READ | FA_WRITE);
    if (result != FR_EXIST)
    {
      break;
    }
    if (++storage_file_index > 999999U)
    {
      return FR_DENIED;
    }
  }
  if (result != FR_OK)
  {
    return result;
  }

  result = f_expand(&storage_file, (FSIZE_t)storage_file_limit, 1U);
  if (result != FR_OK)
  {
    /* Fragmented cards may not have one contiguous 32 MiB extent. Continue
       with ordinary FatFs allocation while retaining the same rotation cap. */
    result = FR_OK;
  }
  if (f_lseek(&storage_file, 0U) != FR_OK)
  {
    result = FR_DISK_ERR;
  }
  if (result == FR_OK)
  {
    BuildFileHeader(header);
    result = StorageWrite(&storage_file, header, sizeof(header));
  }
  if (result != FR_OK)
  {
    (void)f_close(&storage_file);
    return result;
  }

  storage_file_open = true;
  storage_file_position = CAN_LOG_FILE_HEADER_SIZE;
  storage_blocks_in_file = 0U;
  storage_frames_in_file = 0U;
  storage_last_sync_tick = HAL_GetTick();
  taskENTER_CRITICAL();
  (void)snprintf(public_status.file_name, sizeof(public_status.file_name),
                 "L%06lu.TMP", (unsigned long)storage_file_index);
  public_status.file_size_bytes = storage_file_position;
  public_status.card_state = CAN_LOG_CARD_RECORDING;
  taskEXIT_CRITICAL();
  return FR_OK;
}

static FRESULT StartNewSession(void)
{
  FRESULT result;
  CanLogStatus status;
  CanCaptureStatus capture_status;

  if (!storage_mounted || (storage_configuration.mode == CAN_LOG_MODE_OFF))
  {
    return FR_NOT_READY;
  }
  CanControl_GetStatus(&capture_status);
  if (!CaptureReadyForLogging(&capture_status))
  {
    return FR_NOT_READY;
  }
  storage_session_bitrate_kbps = capture_status.bitrate_kbps;
  storage_session_id = storage_next_session++;
  if (storage_next_session > 999999U)
  {
    storage_next_session = 1U;
  }
  (void)snprintf(storage_session_path, sizeof(storage_session_path), "%s/S%06lu",
                 CAN_LOG_DIRECTORY, (unsigned long)storage_session_id);
  result = EnsureDirectory(storage_session_path);
  if (result != FR_OK)
  {
    return result;
  }
  storage_file_index = 1U;
  storage_block_sequence = 0U;
  storage_session_frames = 0U;
  CanLogger_GetStatus(&status);
  storage_session_dropped_base = status.dropped_frames;
  storage_session_write_errors_base = status.write_errors;
  result = WriteSessionJson(false);
  if (result == FR_OK)
  {
    result = OpenNewLogFile();
  }
  return result;
}

static FRESULT CloseLogFile(CanLogCloseReason reason)
{
  uint8_t footer[CAN_LOG_FOOTER_SIZE];
  FRESULT result;
  CanLogStatus status;

  if (!storage_file_open)
  {
    return FR_OK;
  }
  CanLogger_GetStatus(&status);
  BuildFooter(footer, reason, storage_blocks_in_file, storage_frames_in_file,
              status.dropped_frames - storage_session_dropped_base,
              status.write_errors - storage_session_write_errors_base);
  result = StorageWrite(&storage_file, footer, sizeof(footer));
  if (result == FR_OK)
  {
    storage_file_position += sizeof(footer);
    result = f_truncate(&storage_file);
  }
  if (result == FR_OK)
  {
    result = f_sync(&storage_file);
  }
  if ((f_close(&storage_file) != FR_OK) && (result == FR_OK))
  {
    result = FR_DISK_ERR;
  }
  storage_file_open = false;
  if (result == FR_OK)
  {
    result = f_rename(storage_temp_path, storage_final_path);
  }
  taskENTER_CRITICAL();
  public_status.file_name[0] = '\0';
  public_status.file_size_bytes = 0U;
  taskEXIT_CRITICAL();
  return result;
}

static FRESULT RotateLogFile(void)
{
  FRESULT result = CloseLogFile(CAN_LOG_CLOSE_ROTATED);
  if (result == FR_OK)
  {
    ++storage_file_index;
    result = WriteSessionJson(false);
    if (result == FR_OK)
    {
      result = OpenNewLogFile();
    }
  }
  return result;
}

static FRESULT WriteLogBlock(uint8_t block_index)
{
  uint8_t header[CAN_LOG_BLOCK_HEADER_SIZE];
  CanLogBlockMeta *meta = &block_meta[block_index];
  uint8_t *payload = BlockPayload(block_index);
  FRESULT result;
  uint64_t required = CAN_LOG_BLOCK_HEADER_SIZE + meta->payload_length +
                      CAN_LOG_FOOTER_SIZE;

  if ((storage_file_position + required) > storage_file_limit)
  {
    result = RotateLogFile();
    if (result != FR_OK)
    {
      return result;
    }
  }

  memset(header, 0, sizeof(header));
  memcpy(header, "BLK1", 4U);
  StoreU32(header + 4, storage_block_sequence);
  StoreU64(header + 8, meta->first_timestamp_us);
  StoreU32(header + 16, meta->frame_count);
  StoreU32(header + 20, meta->payload_length);
  StoreU32(header + 24, Crc32(payload, meta->payload_length));
  StoreU32(header + 28, Crc32(header, 28U));

  result = StorageWrite(&storage_file, header, sizeof(header));
  if (result == FR_OK)
  {
    result = StorageWrite(&storage_file, payload, meta->payload_length);
  }
  if (result == FR_OK)
  {
    storage_file_position += sizeof(header) + meta->payload_length;
    ++storage_block_sequence;
    ++storage_blocks_in_file;
    storage_frames_in_file += meta->frame_count;
    storage_session_frames += meta->frame_count;
    taskENTER_CRITICAL();
    public_status.stored_frames += meta->frame_count;
    public_status.file_size_bytes = storage_file_position;
    taskEXIT_CRITICAL();
  }
  return result;
}

static void HandleConfigurationItem(const CanLogQueueItem *item)
{
  FRESULT result = FR_OK;
  bool old_session = storage_session_id != 0U;

  if (storage_file_open)
  {
    result = CloseLogFile(item->mode == CAN_LOG_MODE_OFF ?
                          CAN_LOG_CLOSE_STOPPED : CAN_LOG_CLOSE_CONFIGURATION);
  }
  if (old_session && storage_mounted)
  {
    FRESULT metadata_result = WriteSessionJson(result == FR_OK);
    if (result == FR_OK)
    {
      result = metadata_result;
    }
  }
  storage_session_id = 0U;
  storage_session_bitrate_kbps = 0U;
  storage_session_path[0] = '\0';
  storage_configuration.mode = (CanLogMode)item->mode;
  storage_configuration.generation = item->generation;
  memcpy(storage_configuration.node_filter, item->node_filter,
         sizeof(storage_configuration.node_filter));

  if (result != FR_OK)
  {
    StatusAddWriteError((int32_t)result, 0U);
    StorageUnmount(CAN_LOG_CARD_ERROR, (int32_t)result);
  }
  else if (storage_configuration.mode == CAN_LOG_MODE_OFF)
  {
    if (storage_mounted)
    {
      StorageSetState(CAN_LOG_CARD_READY, 0);
      (void)StorageUpdateFreeSpace();
    }
  }
  else if (storage_mounted)
  {
    StorageSetState(CAN_LOG_CARD_READY, 0);
  }
}

static bool HandleBlockItem(uint8_t block_index, uint32_t generation)
{
  CanLogBlockMeta *meta = &block_meta[block_index];
  FRESULT result = FR_OK;
  CanLogCardState card_state;

  taskENTER_CRITICAL();
  card_state = public_status.card_state;
  taskEXIT_CRITICAL();

  if ((storage_configuration.mode == CAN_LOG_MODE_OFF) ||
      (generation != storage_configuration.generation))
  {
    StatusAddDropped(meta->frame_count);
  }
  else if (!storage_mounted || !storage_file_open)
  {
    if ((card_state != CAN_LOG_CARD_FULL) && !StorageStopRequested())
    {
      return false;
    }
    StatusAddDropped(meta->frame_count);
  }
  else
  {
    result = WriteLogBlock(block_index);
    if (result != FR_OK)
    {
      if (result == FR_DENIED)
      {
        StatusAddWriteError((int32_t)result, meta->frame_count);
        StorageSetState(CAN_LOG_CARD_FULL, (int32_t)result);
      }
      else
      {
        /* Keep this block owned by the storage task. It will be written into
           the next recovered session after the card is mounted again. */
        StatusAddWriteError((int32_t)result, 0U);
        StorageUnmount(CAN_LOG_CARD_ERROR, (int32_t)result);
        return false;
      }
    }
  }
  (void)xQueueSend(free_queue, &block_index, portMAX_DELAY);
  return true;
}

static bool ParseCompletedLogFile(const char *name, uint32_t *file_index)
{
  uint32_t value = 0U;
  uint32_t index;

  if ((name == NULL) || (file_index == NULL) || (strlen(name) != 11U) ||
      (name[0] != 'L') || (strcmp(name + 7U, ".CSL") != 0))
  {
    return false;
  }
  for (index = 1U; index <= 6U; ++index)
  {
    if ((name[index] < '0') || (name[index] > '9'))
    {
      return false;
    }
    value = (value * 10U) + (uint32_t)(name[index] - '0');
  }
  *file_index = value;
  return true;
}

static bool IsCompletedLogPath(const char *path)
{
  static const char prefix[] = "/CANSNIFF/LOGS/";
  const char *tail;
  uint32_t index;
  if ((path == NULL) ||
      (strncmp(path, prefix, sizeof(prefix) - 1U) != 0))
  {
    return false;
  }
  tail = path + sizeof(prefix) - 1U;
  if ((strlen(tail) != 19U) || (tail[0] != 'S') || (tail[7] != '/') ||
      (tail[8] != 'L') || (strcmp(tail + 15U, ".CSL") != 0))
  {
    return false;
  }
  for (index = 1U; index <= 6U; ++index)
  {
    if ((tail[index] < '0') || (tail[index] > '9'))
    {
      return false;
    }
  }
  for (index = 9U; index <= 14U; ++index)
  {
    if ((tail[index] < '0') || (tail[index] > '9'))
    {
      return false;
    }
  }
  return true;
}

static FRESULT BuildCompletedLogList(uint32_t capacity,
                                     uint32_t before_session_id,
                                     uint32_t before_file_index,
                                     uint32_t *file_count,
                                     uint32_t *total_count,
                                     bool *has_more)
{
  DIR root;
  FILINFO session_info;
  FRESULT result;
  uint32_t count = 0U;
  uint32_t eligible_count = 0U;
  uint32_t total = 0U;
  bool cursor_present = (before_session_id != 0U) ||
                        (before_file_index != 0U);

  *file_count = 0U;
  *total_count = 0U;
  *has_more = false;
  if (!storage_mounted)
  {
    return FR_NOT_READY;
  }
  result = f_opendir(&root, CAN_LOG_DIRECTORY);
  if (result != FR_OK)
  {
    return result;
  }

  for (;;)
  {
    uint32_t session_id;
    DIR session;
    FILINFO file_info;
    char session_path[48];

    result = f_readdir(&root, &session_info);
    if ((result != FR_OK) || (session_info.fname[0] == '\0'))
    {
      break;
    }
    if (((session_info.fattrib & AM_DIR) == 0U) ||
        !ParseSessionDirectory(session_info.fname, &session_id))
    {
      continue;
    }
    (void)snprintf(session_path, sizeof(session_path), "%s/%s",
                   CAN_LOG_DIRECTORY, session_info.fname);
    result = f_opendir(&session, session_path);
    if (result != FR_OK)
    {
      break;
    }
    for (;;)
    {
      uint32_t file_index;
      uint32_t position;
      uint32_t move;
      CanLogFileInfo *destination;

      result = f_readdir(&session, &file_info);
      if ((result != FR_OK) || (file_info.fname[0] == '\0'))
      {
        break;
      }
      if (((file_info.fattrib & AM_DIR) != 0U) ||
          !ParseCompletedLogFile(file_info.fname, &file_index))
      {
        continue;
      }
      ++total;
      if (cursor_present &&
          ((session_id > before_session_id) ||
           ((session_id == before_session_id) &&
            (file_index >= before_file_index))))
      {
        continue;
      }
      ++eligible_count;

      /* FatFs does not promise directory enumeration order. Keep only the
         newest requested entries, sorted by session and file number. */
      position = 0U;
      while (position < count)
      {
        const CanLogFileInfo *current = &request_files[position];
        if ((current->session_id < session_id) ||
            ((current->session_id == session_id) &&
             (current->file_index < file_index)))
        {
          break;
        }
        ++position;
      }
      if (position >= capacity)
      {
        continue;
      }
      if (count < capacity)
      {
        ++count;
      }
      move = count - 1U;
      while (move > position)
      {
        request_files[move] = request_files[move - 1U];
        --move;
      }
      destination = &request_files[position];
      (void)snprintf(destination->path, sizeof(destination->path),
                     "/CANSNIFF/LOGS/%s/%s", session_info.fname,
                     file_info.fname);
      destination->session_id = session_id;
      destination->file_index = file_index;
      destination->size_bytes = (uint64_t)file_info.fsize;
    }
    (void)f_closedir(&session);
    if (result != FR_OK)
    {
      break;
    }
  }
  (void)f_closedir(&root);
  if (result == FR_OK)
  {
    *file_count = count;
    *total_count = total;
    *has_more = eligible_count > count;
  }
  return result;
}

static bool HandleStorageRequests(void)
{
  CanLogRequest request;

  if (xQueueReceive(request_queue, &request, 0U) == pdPASS)
  {
    FRESULT result = FR_OK;
    uint32_t count = 0U;
    uint32_t total = 0U;
    bool has_more = false;
    uint64_t size = 0U;

    if (request.type == CAN_LOG_REQUEST_LIST)
    {
      result = BuildCompletedLogList(request.length,
                                     (uint32_t)(request.offset >> 32U),
                                     (uint32_t)request.offset,
                                     &count, &total, &has_more);
    }
    else if (request.type == CAN_LOG_REQUEST_DOWNLOAD_OPEN)
    {
      char fatfs_path[76];
      if (storage_download_open)
      {
        (void)f_close(&storage_download_file);
        storage_download_open = false;
      }
      if (!storage_mounted)
      {
        result = FR_NOT_READY;
      }
      else if (!IsCompletedLogPath(request.path))
      {
        result = FR_INVALID_NAME;
      }
      else
      {
        (void)snprintf(fatfs_path, sizeof(fatfs_path), "0:%s", request.path);
        result = f_open(&storage_download_file, fatfs_path, FA_READ);
        if (result == FR_OK)
        {
          storage_download_open = true;
          size = (uint64_t)f_size(&storage_download_file);
        }
      }
    }
    else if (request.type == CAN_LOG_REQUEST_DOWNLOAD_SEEK)
    {
      result = storage_download_open ?
               f_lseek(&storage_download_file, (FSIZE_t)request.offset) :
               FR_NOT_READY;
    }
    else if (request.type == CAN_LOG_REQUEST_DOWNLOAD_READ)
    {
      UINT bytes_read = 0U;
      if (!storage_download_open)
      {
        result = FR_NOT_READY;
      }
      else
      {
        UINT requested = request.length > CAN_LOG_DOWNLOAD_BUFFER_BYTES ?
                         (UINT)CAN_LOG_DOWNLOAD_BUFFER_BYTES :
                         (UINT)request.length;
        result = f_read(&storage_download_file, download_buffer,
                        requested, &bytes_read);
        count = bytes_read;
      }
    }
    else if (request.type == CAN_LOG_REQUEST_DOWNLOAD_CLOSE)
    {
      if (storage_download_open)
      {
        result = f_close(&storage_download_file);
        storage_download_open = false;
      }
    }
    else
    {
      result = FR_INVALID_PARAMETER;
    }

    response_count = count;
    response_total = total;
    response_has_more = has_more;
    response_size = size;
    response_error = (int32_t)result;
    response_success = result == FR_OK;
    response_sequence = request.sequence;
    (void)xSemaphoreGive(request_done);
    return true;
  }
  return false;
}

void CanLogger_StorageTask(void)
{
  CanLogQueueItem item;

  memset(&storage_configuration, 0, sizeof(storage_configuration));
  storage_configuration.mode = CAN_LOG_MODE_OFF;
  storage_mounted = false;
  storage_file_open = false;
  storage_download_open = false;
  storage_pending_block_valid = false;
  storage_task_handle = xTaskGetCurrentTaskHandle();
  storage_last_mount_tick = HAL_GetTick() - CAN_LOG_MOUNT_RETRY_MS;

  for (;;)
  {
    uint32_t now = HAL_GetTick();
    bool present = StorageSd_CardPresent();
    bool handled_work = false;

    if (!present && storage_mounted)
    {
      StorageUnmount(CAN_LOG_CARD_ABSENT, 0);
    }
    else if (!present)
    {
      StorageSetState(CAN_LOG_CARD_ABSENT, 0);
    }
    else if (!storage_mounted &&
             ((uint32_t)(now - storage_last_mount_tick) >= CAN_LOG_MOUNT_RETRY_MS))
    {
      storage_last_mount_tick = now;
      (void)StorageMount();
    }

    /* Open the recovered/new session before removing its oldest block from
       the queue.  A failed write then leaves that block owned by this task
       and it is retried first after the next mount. */
    if (storage_mounted && !storage_file_open &&
        (storage_session_id == 0U) &&
        (storage_configuration.mode != CAN_LOG_MODE_OFF))
    {
      FRESULT result = StartNewSession();
      if ((result != FR_OK) && (result != FR_NOT_READY) &&
          (result != FR_DENIED))
      {
        StatusAddWriteError((int32_t)result, 0U);
        StorageUnmount(CAN_LOG_CARD_ERROR, (int32_t)result);
      }
    }

    if (storage_pending_block_valid)
    {
      if (HandleBlockItem(storage_pending_block.block_index,
                          storage_pending_block.generation))
      {
        storage_pending_block_valid = false;
        handled_work = true;
      }
    }
    else if (xQueuePeek(ready_queue, &item, 0U) == pdPASS)
    {
      if (item.type == CAN_LOG_ITEM_CONFIGURATION)
      {
        if (xQueueReceive(ready_queue, &item, 0U) == pdPASS)
        {
          HandleConfigurationItem(&item);
          handled_work = true;
        }
      }
      else if ((item.block_index < CAN_LOG_BLOCK_COUNT) &&
               (storage_file_open || StorageStopRequested() ||
                (public_status.card_state == CAN_LOG_CARD_FULL)))
      {
        if (xQueueReceive(ready_queue, &item, 0U) == pdPASS)
        {
          if (!HandleBlockItem(item.block_index, item.generation))
          {
            storage_pending_block = item;
            storage_pending_block_valid = true;
          }
          else
          {
            handled_work = true;
          }
        }
      }
    }

    if (!handled_work)
    {
      handled_work = HandleStorageRequests();
    }

    now = HAL_GetTick();
    if (storage_file_open &&
        ((uint32_t)(now - storage_last_sync_tick) >= CAN_LOG_SYNC_INTERVAL_MS))
    {
      FRESULT result = f_sync(&storage_file);
      storage_last_sync_tick = now;
      if (result != FR_OK)
      {
        StatusAddWriteError((int32_t)result, 0U);
        StorageUnmount(CAN_LOG_CARD_ERROR, (int32_t)result);
      }
    }
    if (storage_mounted &&
        ((uint32_t)(now - storage_last_space_tick) >= CAN_LOG_SPACE_UPDATE_MS))
    {
      FRESULT result;
      storage_last_space_tick = now;
      result = StorageUpdateFreeSpace();
      if (result != FR_OK)
      {
        StatusAddWriteError((int32_t)result, 0U);
        StorageUnmount(CAN_LOG_CARD_ERROR, (int32_t)result);
      }
    }
    if (!handled_work)
    {
      (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20U));
    }
  }
}
