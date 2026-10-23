#ifndef CAN_CAPTURE_H
#define CAN_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>

#define CAN_FRAME_FLAG_EXTENDED  (1U << 0)
#define CAN_FRAME_FLAG_REMOTE    (1U << 1)
#define CAN_FRAME_FLAG_LOCAL_TX  (1U << 2)
#define CAN_FRAME_FLAG_BRIDGE_REQUEST     (1U << 3)
#define CAN_FRAME_FLAG_BRIDGE_RESPONSE    (1U << 4)
#define CAN_FRAME_FLAG_BRIDGE_UNSOLICITED (1U << 5)

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

typedef enum
{
  CAN_OPERATING_SILENT = 0,
  CAN_OPERATING_ACTIVE_PENDING,
  CAN_OPERATING_ACTIVE
} CanOperatingMode;

typedef enum
{
  CAN_SAFETY_POWER_ON = 0,
  CAN_SAFETY_USER,
  CAN_SAFETY_BITRATE_CHANGE,
  CAN_SAFETY_BUS_OFF,
  CAN_SAFETY_CONTROLLER_ERROR
} CanSafetyReason;

typedef struct
{
  uint32_t bitrate_kbps;
  CanSyncState state;
  uint32_t received;
  uint32_t ring_overflows;
  uint32_t gui_overflows;
  uint32_t errors;
  uint16_t bus_load_permille;
  CanOperatingMode operating_mode;
  CanSafetyReason safety_reason;
  bool tx_gate_enabled;
} CanCaptureStatus;

void CanCapture_Init(void);
void CanCapture_SetConsumerTask(void *task_handle);
bool CanCapture_Pop(CanFrameRecord *frame);
void CanCapture_RxIRQHandler(void);
void CanCapture_ErrorIRQHandler(void);
void CanCapture_TimerIRQHandler(void);
void CanSafety_EmergencyDisableTx(void);

void CanControl_Task(void);
void CanControl_RequestBitrate(uint32_t bitrate_kbps);
bool CanControl_RequestActive(bool confirmed);
void CanControl_RequestSilent(void);
void CanControl_ForceSilent(CanSafetyReason reason);
bool CanControl_IsActive(void);
const char *CanControl_OperatingModeName(CanOperatingMode mode);
const char *CanControl_SafetyReasonName(CanSafetyReason reason);
void CanControl_GetStatus(CanCaptureStatus *status);
void CanControl_AddGuiOverflow(void);
void CanControl_ResetStatistics(void);
uint32_t CanCapture_GetTimestampUs(void);
void CanCapture_GetTimestamps(uint32_t *timestamp_us,
                              uint32_t *capture_time_us,
                              uint32_t *capture_time_high_value);

#endif
