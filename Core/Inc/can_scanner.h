#ifndef CAN_SCANNER_H
#define CAN_SCANNER_H

#include "can_capture.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CAN_SCANNER_NODE_MIN       1U
#define CAN_SCANNER_NODE_MAX       127U
#define CAN_SCANNER_RESULT_MAX     127U
#define CAN_SCANNER_OBJECT_INDEX   0x1000U
#define CAN_SCANNER_TIMEOUT_MS     100U

typedef enum
{
  CAN_SCANNER_IDLE = 0,
  CAN_SCANNER_PREPARING,
  CAN_SCANNER_SELECTING_BITRATE,
  CAN_SCANNER_ENABLING_TX,
  CAN_SCANNER_SCANNING,
  CAN_SCANNER_COMPLETE,
  CAN_SCANNER_CANCELLED,
  CAN_SCANNER_ERROR
} CanScannerPhase;

typedef enum
{
  CAN_SCANNER_RESPONSE_DEVICE_TYPE = 0,
  CAN_SCANNER_RESPONSE_ABORT,
  CAN_SCANNER_RESPONSE_OTHER
} CanScannerResponseKind;

typedef struct
{
  uint32_t bitrate_kbps;
  uint32_t value;
  uint32_t abort_code;
  uint8_t node_id;
  uint8_t command;
  uint8_t data[8];
  CanScannerResponseKind kind;
} CanScannerResult;

typedef struct
{
  CanScannerPhase phase;
  uint32_t configured_bitrate_kbps;
  uint32_t current_bitrate_kbps;
  uint32_t completed_requests;
  uint32_t total_requests;
  uint32_t result_count;
  uint8_t current_node_id;
  bool running;
  bool auto_bitrate;
} CanScannerStatus;

void CanScanner_Init(void);
bool CanScanner_Start(void);
void CanScanner_Stop(void);
void CanScanner_Update(void);
void CanScanner_ProcessFrame(const CanFrameRecord *frame);
void CanScanner_GetStatus(CanScannerStatus *status);
size_t CanScanner_CopyResults(CanScannerResult *results, size_t capacity);
const char *CanScanner_PhaseName(CanScannerPhase phase);
const char *CanScanner_DeviceHint(uint32_t device_type);

#endif
