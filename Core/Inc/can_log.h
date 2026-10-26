#ifndef CAN_LOG_H
#define CAN_LOG_H

#include "can_capture.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
  CAN_LOG_MODE_OFF = 0,
  CAN_LOG_MODE_FULL,
  CAN_LOG_MODE_SELECTED
} CanLogMode;

typedef enum
{
  CAN_LOG_CARD_ABSENT = 0,
  CAN_LOG_CARD_MOUNTING,
  CAN_LOG_CARD_READY,
  CAN_LOG_CARD_RECORDING,
  CAN_LOG_CARD_RECOVERING,
  CAN_LOG_CARD_FULL,
  CAN_LOG_CARD_UNSUPPORTED,
  CAN_LOG_CARD_ERROR
} CanLogCardState;

typedef struct
{
  CanLogMode mode;
  CanLogCardState card_state;
  char file_name[24];
  uint64_t file_size_bytes;
  uint64_t free_bytes;
  uint64_t stored_frames;
  uint64_t dropped_frames;
  uint32_t write_errors;
  uint32_t recovery_count;
  uint32_t selected_node_count;
  uint32_t buffered_blocks;
  int32_t last_error;
  uint32_t sd_io_errors;
  uint32_t sd_retries;
  uint32_t sd_last_error_tick;
  uint32_t sd_last_sector;
  uint32_t sd_last_count;
  uint32_t sd_last_hal_error;
  uint32_t sd_last_status;
  uint32_t sd_max_read_ms;
  uint32_t sd_max_write_ms;
  uint8_t sd_last_operation;
  uint8_t sd_last_hal_status;
  uint8_t sd_last_handle_state;
  uint8_t reserved;
} CanLogStatus;

#define CAN_LOG_LIST_MAX 32U

typedef struct
{
  char path[72];
  uint32_t session_id;
  uint32_t file_index;
  uint64_t size_bytes;
} CanLogFileInfo;

typedef enum
{
  CAN_LOG_DOWNLOAD_OK = 0,
  CAN_LOG_DOWNLOAD_INVALID_PATH,
  CAN_LOG_DOWNLOAD_NOT_FOUND,
  CAN_LOG_DOWNLOAD_UNAVAILABLE,
  CAN_LOG_DOWNLOAD_IO_ERROR
} CanLogDownloadResult;

typedef struct
{
  uint64_t size_bytes;
  bool active;
} CanLogDownload;

void CanLogger_Init(void);
void CanLogger_StorageTask(void);
void CanLogger_ProducerPoll(void);
void CanLogger_SubmitFrame(const CanFrameRecord *frame);

void CanLogger_SetMode(CanLogMode mode);
CanLogMode CanLogger_GetMode(void);
void CanLogger_SetNodeSelected(uint8_t node_id, bool selected);
bool CanLogger_IsNodeSelected(uint8_t node_id);
void CanLogger_ClearSelectedNodes(void);
void CanLogger_GetStatus(CanLogStatus *status);
bool CanLogger_GetLogListPage(CanLogFileInfo *files, uint32_t capacity,
                              uint32_t before_session_id,
                              uint32_t before_file_index,
                              uint32_t *count, uint32_t *total,
                              bool *has_more, uint32_t timeout_ms);
CanLogDownloadResult CanLogger_DownloadBegin(CanLogDownload *download,
                                             const char *path,
                                             uint32_t timeout_ms);
bool CanLogger_DownloadSeek(CanLogDownload *download, uint64_t offset,
                            uint32_t timeout_ms);
bool CanLogger_DownloadRead(CanLogDownload *download, uint32_t requested,
                            const uint8_t **data, uint32_t *length,
                            uint32_t timeout_ms);
void CanLogger_DownloadEnd(CanLogDownload *download, uint32_t timeout_ms);
void CanLogger_ResetCounters(void);

const char *CanLogger_ModeName(CanLogMode mode);
const char *CanLogger_CardStateName(CanLogCardState state);

#endif
