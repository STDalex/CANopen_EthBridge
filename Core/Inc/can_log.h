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
  int32_t last_error;
} CanLogStatus;

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
void CanLogger_ResetCounters(void);

const char *CanLogger_ModeName(CanLogMode mode);
const char *CanLogger_CardStateName(CanLogCardState state);

#endif
