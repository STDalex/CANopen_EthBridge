#ifndef CAN_TX_MANAGER_H
#define CAN_TX_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CAN_TX_QUEUE_CAPACITY 16U
#define CAN_TX_HISTORY_CAPACITY 32U
#define CAN_TX_DEFAULT_TIMEOUT_MS 100U
#define CAN_TX_MIN_INTERVAL_MS 50U
#define CAN_TX_PERIOD_MIN_MS 50U
#define CAN_TX_PERIOD_MAX_MS 3600000U

typedef struct
{
  uint32_t id;
  uint16_t timeout_ms;
  uint8_t dlc;
  bool extended;
  bool remote;
  uint8_t data[8];
} CanTxFrame;

typedef enum
{
  CAN_TX_SDO_READ = 0,
  CAN_TX_SDO_WRITE_U8,
  CAN_TX_SDO_WRITE_U16,
  CAN_TX_SDO_WRITE_U24,
  CAN_TX_SDO_WRITE_U32
} CanTxSdoCommand;

typedef struct
{
  uint32_t value;
  uint16_t index;
  uint16_t timeout_ms;
  uint8_t node_id;
  uint8_t subindex;
  CanTxSdoCommand command;
} CanTxSdoRequest;

typedef enum
{
  CAN_TX_RESULT_SENT = 0,
  CAN_TX_RESULT_CANCELLED,
  CAN_TX_RESULT_TIMEOUT,
  CAN_TX_RESULT_ARBITRATION_LOST,
  CAN_TX_RESULT_BUS_OFF,
  CAN_TX_RESULT_DRIVER_ERROR,
  CAN_TX_RESULT_MODE_DISABLED
} CanTxResult;

typedef enum
{
  CAN_TX_SUBMIT_OK = 0,
  CAN_TX_SUBMIT_INVALID,
  CAN_TX_SUBMIT_MODE_DISABLED,
  CAN_TX_SUBMIT_QUEUE_FULL
} CanTxSubmitResult;

typedef struct
{
  uint32_t sequence;
  uint32_t timestamp_us;
  CanTxFrame frame;
  CanTxResult result;
  uint32_t attempt_count;
  bool periodic;
} CanTxHistoryEntry;

typedef struct
{
  uint32_t queued;
  uint32_t sent;
  uint32_t cancelled;
  uint32_t timeouts;
  uint32_t arbitration_lost;
  uint32_t bus_off;
  uint32_t errors;
  uint32_t last_sequence;
  CanTxResult last_result;
  uint32_t queue_depth;
  uint32_t periodic_sequence;
  uint32_t periodic_period_ms;
  uint32_t periodic_attempts;
  bool transmitting;
  bool periodic_active;
} CanTxStatus;

void CanTxManager_Init(void);
void CanTxManager_Task(void);
bool CanTx_BuildSdoRequest(const CanTxSdoRequest *request, CanTxFrame *frame);
bool CanTx_DecodeSdoRequest(const CanTxFrame *frame, CanTxSdoRequest *request);
CanTxSubmitResult CanTx_SubmitSdo(const CanTxSdoRequest *request,
                                 uint32_t *sequence);
CanTxSubmitResult CanTx_SubmitBridgeSdo(const CanTxSdoRequest *request,
                                       uint32_t *sequence);
bool CanTx_SetBridgeExclusive(bool enabled);
CanTxSubmitResult CanTx_SubmitScannerSdo(const CanTxSdoRequest *request,
                                        uint32_t *sequence);
bool CanTx_SetScannerExclusive(bool enabled);
CanTxSubmitResult CanTx_StartPeriodicSdo(const CanTxSdoRequest *request,
                                         uint32_t period_ms,
                                         uint32_t *sequence);
bool CanTx_StopPeriodic(void);
bool CanTx_Cancel(uint32_t sequence);
void CanTx_CancelAll(void);
void CanTx_GetStatus(CanTxStatus *status);
void CanTx_ResetStatistics(void);
size_t CanTx_CopyHistory(CanTxHistoryEntry *entries, size_t capacity);
const char *CanTx_ResultName(CanTxResult result);
const char *CanTx_SdoCommandName(CanTxSdoCommand command);

#endif
