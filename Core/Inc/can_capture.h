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

typedef enum
{
  CAN_ERROR_NONE = 0,
  CAN_ERROR_STUFF,
  CAN_ERROR_FORM,
  CAN_ERROR_ACK,
  CAN_ERROR_BIT_RECESSIVE,
  CAN_ERROR_BIT_DOMINANT,
  CAN_ERROR_CRC,
  CAN_ERROR_WARNING,
  CAN_ERROR_PASSIVE,
  CAN_ERROR_BUS_OFF,
  CAN_ERROR_UNCLASSIFIED
} CanErrorKind;

typedef struct
{
  uint32_t interrupts;
  uint32_t warning;
  uint32_t passive;
  uint32_t bus_off;
  uint32_t stuff;
  uint32_t form;
  uint32_t ack;
  uint32_t bit_recessive;
  uint32_t bit_dominant;
  uint32_t crc;
  uint32_t unclassified;
  uint32_t last_timestamp_us;
  uint32_t last_esr;
  uint8_t last_tx_error_counter;
  uint8_t last_rx_error_counter;
  CanErrorKind last_error;
} CanErrorStatistics;

typedef struct
{
  uint32_t bitrate_kbps;
  CanSyncState state;
  uint32_t received;
  uint32_t ring_overflows;
  uint32_t gui_overflows;
  uint32_t errors;
  CanErrorStatistics error_statistics;
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
const char *CanControl_ErrorName(CanErrorKind error);
void CanControl_GetStatus(CanCaptureStatus *status);
void CanControl_AddGuiOverflow(void);
void CanControl_ResetStatistics(void);
uint32_t CanCapture_GetTimestampUs(void);
void CanCapture_GetTimestamps(uint32_t *timestamp_us,
                              uint32_t *capture_time_us,
                              uint32_t *capture_time_high_value);

#endif
