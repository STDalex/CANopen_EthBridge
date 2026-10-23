#ifndef CAN_BRIDGE_H
#define CAN_BRIDGE_H

#include "can_capture.h"
#include "can_tx_manager.h"

#include <stdbool.h>
#include <stdint.h>

#define CAN_BRIDGE_LEASE_MS 10000U
#define CAN_BRIDGE_RESULT_CAPACITY 32U

typedef enum
{
  CAN_BRIDGE_RESULT_NONE = 0,
  CAN_BRIDGE_RESULT_COMPLETED,
  CAN_BRIDGE_RESULT_ABORT,
  CAN_BRIDGE_RESULT_TIMEOUT,
  CAN_BRIDGE_RESULT_BUS_OFF,
  CAN_BRIDGE_RESULT_DRIVER_ERROR,
  CAN_BRIDGE_RESULT_CANCELLED,
  CAN_BRIDGE_RESULT_PROTOCOL_ERROR
} CanBridgeResultCode;

typedef enum
{
  CAN_BRIDGE_SUBMIT_OK = 0,
  CAN_BRIDGE_SUBMIT_DUPLICATE,
  CAN_BRIDGE_SUBMIT_DISABLED,
  CAN_BRIDGE_SUBMIT_NOT_CONNECTED,
  CAN_BRIDGE_SUBMIT_BUSY,
  CAN_BRIDGE_SUBMIT_INVALID,
  CAN_BRIDGE_SUBMIT_CAN_NOT_READY
} CanBridgeSubmitResult;

typedef struct
{
  bool active;
  bool connected;
  bool request_pending;
  uint32_t pending_count;
  uint32_t session_id;
  uint32_t lease_remaining_ms;
  uint32_t client_request_id;
  uint32_t tx_sequence;
  uint32_t requests;
  uint32_t completed;
  uint32_t aborted;
  uint32_t timeouts;
  uint32_t rejected;
  uint32_t unsolicited_frames;
  uint32_t result_generation;
  CanBridgeResultCode last_result;
} CanBridgeStatus;

typedef struct
{
  uint32_t generation;
  uint32_t session_id;
  uint32_t client_request_id;
  uint32_t tx_sequence;
  uint32_t timestamp_us;
  uint64_t capture_time_us;
  uint32_t abort_code;
  CanBridgeResultCode code;
  CanFrameRecord response;
  bool has_response;
} CanBridgeResult;

void CanBridge_Init(void);
bool CanBridge_Enable(uint32_t *session_id);
void CanBridge_Disable(void);
bool CanBridge_IsActive(void);
bool CanBridge_Attach(uint32_t session_id);
void CanBridge_Detach(uint32_t session_id);
void CanBridge_Heartbeat(uint32_t session_id);
CanBridgeSubmitResult CanBridge_SubmitSdo(uint32_t session_id,
                                         uint32_t client_request_id,
                                         const CanTxSdoRequest *request,
                                         uint32_t *tx_sequence);
void CanBridge_ProcessFrame(const CanFrameRecord *frame,
                            CanFrameRecord *stream_frame);
void CanBridge_Update(void);
void CanBridge_GetStatus(CanBridgeStatus *status);
void CanBridge_GetResult(CanBridgeResult *result);
bool CanBridge_FindResult(uint32_t client_request_id,
                          CanBridgeResult *result);
bool CanBridge_GetNextResult(uint32_t after_generation,
                             CanBridgeResult *result,
                             uint32_t *lost_results);
const char *CanBridge_ResultName(CanBridgeResultCode result);

#endif
