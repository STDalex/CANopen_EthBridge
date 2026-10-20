#ifndef CAN_CAPTURE_H
#define CAN_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>

#define CAN_FRAME_FLAG_EXTENDED  (1U << 0)
#define CAN_FRAME_FLAG_REMOTE    (1U << 1)

typedef struct
{
  uint32_t timestamp_us;
  uint32_t capture_time_us;
  uint32_t capture_time_high;
  uint32_t id;
  uint8_t dlc;
  uint8_t flags;
  uint8_t data[8];
} CanFrameRecord;

typedef enum
{
  CAN_SYNC_SEARCHING = 0,
  CAN_SYNC_LOCKED,
  CAN_SYNC_MANUAL
} CanSyncState;

typedef struct
{
  uint32_t bitrate_kbps;
  CanSyncState state;
  uint32_t received;
  uint32_t ring_overflows;
  uint32_t gui_overflows;
  uint32_t errors;
  uint16_t bus_load_permille;
} CanCaptureStatus;

void CanCapture_Init(void);
void CanCapture_SetConsumerTask(void *task_handle);
bool CanCapture_Pop(CanFrameRecord *frame);
void CanCapture_RxIRQHandler(void);
void CanCapture_ErrorIRQHandler(void);
void CanCapture_TimerIRQHandler(void);

void CanControl_Task(void);
void CanControl_RequestBitrate(uint32_t bitrate_kbps);
void CanControl_GetStatus(CanCaptureStatus *status);
void CanControl_AddGuiOverflow(void);
void CanControl_ResetStatistics(void);
uint32_t CanCapture_GetTimestampUs(void);

#endif
