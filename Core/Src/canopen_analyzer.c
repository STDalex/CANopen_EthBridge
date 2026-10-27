#include "canopen_analyzer.h"

#include "FreeRTOS.h"
#include "memory_layout.h"
#include "task.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define CANOPEN_EVENT_HISTORY_LENGTH   512U
#define CANOPEN_EVENT_HISTORY_MASK     (CANOPEN_EVENT_HISTORY_LENGTH - 1U)
#define CANOPEN_SDO_OBJECT_POOL_LENGTH 256U
#define CANOPEN_SDO_OBJECT_POOL_MASK   (CANOPEN_SDO_OBJECT_POOL_LENGTH - 1U)
#define CANOPEN_SDO_TIMEOUT_US         1000000U

typedef enum { SDO_TRANSFER_NONE = 0, SDO_TRANSFER_UPLOAD, SDO_TRANSFER_DOWNLOAD }
  SdoTransferDirection;
typedef enum { SDO_MODE_EXPEDITED = 0, SDO_MODE_SEGMENTED, SDO_MODE_BLOCK }
  SdoTransferMode;
typedef enum
{
  SDO_PHASE_INIT_RESPONSE = 0,
  SDO_PHASE_UPLOAD_REQUEST,
  SDO_PHASE_UPLOAD_RESPONSE,
  SDO_PHASE_DOWNLOAD_SEGMENT,
  SDO_PHASE_DOWNLOAD_ACK,
  SDO_PHASE_BLOCK
} SdoTransferPhase;

typedef struct { bool present; CanopenNodeSnapshot snapshot; } CanopenNodeState;
typedef struct { uint8_t node_id; CanopenNodeEventSnapshot snapshot; } CanopenEventState;

typedef struct
{
  bool used;
  uint8_t node_id;
  uint16_t index;
  uint8_t subindex;
  CanopenMessageType direction;
  uint32_t access_count;
  CanopenNodeEventSnapshot snapshot;
} CanopenSdoObjectState;

typedef struct
{
  bool active;
  bool ambiguous;
  bool last_segment_sent;
  bool local_transaction;
  uint16_t index;
  uint8_t subindex;
  uint8_t expected_toggle;
  SdoTransferDirection direction;
  SdoTransferMode mode;
  SdoTransferPhase phase;
  uint32_t started_us;
  uint32_t last_activity_us;
  uint32_t last_request_us;
  uint32_t transferred_bytes;
  uint16_t segment_count;
} CanopenSdoTransaction;

typedef struct
{
  CanopenSdoStatus status;
  CanopenSdoConfidence confidence;
  uint32_t response_time_us;
  bool has_response_time;
  uint32_t transaction_time_us;
  uint32_t transferred_bytes;
  uint32_t abort_code;
  uint16_t segment_count;
  uint16_t index;
  uint8_t subindex;
  uint8_t data_offset;
  bool has_address;
  bool local_transaction;
} SdoFrameResult;

#define NODE_STATES_BYTES       (CANOPEN_NODE_COUNT_MAX * sizeof(CanopenNodeState))
#define EVENT_HISTORY_BYTES     (CANOPEN_EVENT_HISTORY_LENGTH * sizeof(CanopenEventState))
#define SDO_OBJECTS_BYTES       (CANOPEN_SDO_OBJECT_POOL_LENGTH * sizeof(CanopenSdoObjectState))
#define SDO_TRANSACTIONS_BYTES  (CANOPEN_NODE_COUNT_MAX * sizeof(CanopenSdoTransaction))
#define CANOPEN_ANALYZER_BYTES  (NODE_STATES_BYTES + EVENT_HISTORY_BYTES + \
                                 SDO_OBJECTS_BYTES + SDO_TRANSACTIONS_BYTES)

_Static_assert((CANOPEN_ANALYZER_STATE_BASE + CANOPEN_ANALYZER_BYTES) <= 0xC0800000UL,
               "CANopen analyzer state exceeds external SDRAM");

static CanopenNodeState *const node_states =
  (CanopenNodeState *)CANOPEN_ANALYZER_STATE_BASE;
static CanopenEventState *const event_history =
  (CanopenEventState *)(CANOPEN_ANALYZER_STATE_BASE + NODE_STATES_BYTES);
static CanopenSdoObjectState *const sdo_objects =
  (CanopenSdoObjectState *)(CANOPEN_ANALYZER_STATE_BASE + NODE_STATES_BYTES +
                            EVENT_HISTORY_BYTES);
static CanopenSdoTransaction *const sdo_transactions =
  (CanopenSdoTransaction *)(CANOPEN_ANALYZER_STATE_BASE + NODE_STATES_BYTES +
                            EVENT_HISTORY_BYTES + SDO_OBJECTS_BYTES);
static uint32_t event_write;
static uint32_t event_count;
static uint32_t queue_overflows;
static CanopenGlobalSnapshot global_snapshot;

static CanopenSdoObjectState *Find_Object(uint8_t node_id, uint16_t index,
                                          uint8_t subindex, CanopenMessageType direction,
                                          bool create)
{
  uint32_t hash = (((uint32_t)node_id * 131U) + ((uint32_t)index * 17U) +
                   subindex + ((uint32_t)direction * 7U)) & CANOPEN_SDO_OBJECT_POOL_MASK;
  uint32_t probe;
  for (probe = 0U; probe < CANOPEN_SDO_OBJECT_POOL_LENGTH; ++probe)
  {
    CanopenSdoObjectState *object =
      &sdo_objects[(hash + probe) & CANOPEN_SDO_OBJECT_POOL_MASK];
    if (!object->used)
    {
      if (!create) return NULL;
      memset(object, 0, sizeof(*object));
      object->used = true;
      object->node_id = node_id;
      object->index = index;
      object->subindex = subindex;
      object->direction = direction;
      return object;
    }
    if ((object->node_id == node_id) && (object->index == index) &&
        (object->subindex == subindex) && (object->direction == direction))
    {
      return object;
    }
  }
  return NULL;
}

static void Update_Object(uint8_t node_id, uint16_t index, uint8_t subindex,
                          CanopenMessageType direction, const CanFrameRecord *frame,
                          uint8_t data_offset, const SdoFrameResult *result)
{
  CanopenSdoObjectState *object = Find_Object(node_id, index, subindex, direction, true);
  if ((object == NULL) || (frame == NULL) || (result == NULL)) return;
  ++object->access_count;
  object->snapshot.frame = *frame;
  object->snapshot.type = direction;
  object->snapshot.response_time_us = result->response_time_us;
  object->snapshot.has_response_time = result->has_response_time;
  object->snapshot.object_index = index;
  object->snapshot.object_subindex = subindex;
  object->snapshot.data_offset = data_offset;
  object->snapshot.has_sdo_address = true;
  object->snapshot.sdo_status = result->status;
  object->snapshot.sdo_confidence = result->confidence;
  object->snapshot.transaction_time_us = result->transaction_time_us;
  object->snapshot.transferred_bytes = result->transferred_bytes;
  object->snapshot.abort_code = result->abort_code;
  object->snapshot.segment_count = result->segment_count;
  object->snapshot.local_transaction = result->local_transaction;
}

static void Mark_TransactionObjects(uint8_t node_id, const CanopenSdoTransaction *transaction,
                                    CanopenSdoStatus status, uint32_t finished_us,
                                    uint32_t abort_code)
{
  uint32_t object_index;
  CanopenSdoConfidence confidence = transaction->ambiguous ?
    CANOPEN_SDO_CONFIDENCE_AMBIGUOUS : CANOPEN_SDO_CONFIDENCE_EXACT;
  for (object_index = 0U; object_index < CANOPEN_SDO_OBJECT_POOL_LENGTH; ++object_index)
  {
    CanopenSdoObjectState *object = &sdo_objects[object_index];
    if (object->used && (object->node_id == node_id) &&
        (object->index == transaction->index) &&
        (object->subindex == transaction->subindex))
    {
      object->snapshot.sdo_status = status;
      object->snapshot.sdo_confidence = confidence;
      object->snapshot.transaction_time_us = finished_us - transaction->started_us;
      object->snapshot.transferred_bytes = transaction->transferred_bytes;
      object->snapshot.abort_code = abort_code;
      object->snapshot.segment_count = transaction->segment_count;
      object->snapshot.local_transaction = transaction->local_transaction;
    }
  }
}

static void Finish_Transaction(uint8_t node_id, CanopenSdoStatus status,
                               uint32_t finished_us, uint32_t abort_code,
                               SdoFrameResult *result)
{
  CanopenSdoTransaction *transaction = &sdo_transactions[node_id - 1U];
  CanopenNodeSnapshot *node = &node_states[node_id - 1U].snapshot;
  CanopenSdoConfidence confidence;
  if (!transaction->active) return;
  if (transaction->ambiguous && (status == CANOPEN_SDO_STATUS_COMPLETED))
    status = CANOPEN_SDO_STATUS_AMBIGUOUS;
  confidence = transaction->ambiguous ? CANOPEN_SDO_CONFIDENCE_AMBIGUOUS :
                                        CANOPEN_SDO_CONFIDENCE_EXACT;
  switch (status)
  {
    case CANOPEN_SDO_STATUS_COMPLETED: ++node->sdo_completed_count; break;
    case CANOPEN_SDO_STATUS_ABORTED: ++node->sdo_aborted_count; break;
    case CANOPEN_SDO_STATUS_TIMEOUT: ++node->sdo_timeout_count; break;
    case CANOPEN_SDO_STATUS_PROTOCOL_ERROR: ++node->sdo_protocol_error_count; break;
    case CANOPEN_SDO_STATUS_AMBIGUOUS: ++node->sdo_conflict_count; break;
    default: break;
  }
  Mark_TransactionObjects(node_id, transaction, status, finished_us, abort_code);
  if (result != NULL)
  {
    result->status = status;
    result->confidence = confidence;
    result->transaction_time_us = finished_us - transaction->started_us;
    result->transferred_bytes = transaction->transferred_bytes;
    result->abort_code = abort_code;
    result->segment_count = transaction->segment_count;
    result->index = transaction->index;
    result->subindex = transaction->subindex;
    result->has_address = true;
    result->local_transaction = transaction->local_transaction;
  }
  transaction->active = false;
}

static uint32_t Read_AbortCode(const CanFrameRecord *frame)
{
  if ((frame == NULL) || (frame->dlc < 8U)) return 0U;
  return (uint32_t)frame->data[4] | ((uint32_t)frame->data[5] << 8) |
         ((uint32_t)frame->data[6] << 16) | ((uint32_t)frame->data[7] << 24);
}

static void Set_ResultFromTransaction(SdoFrameResult *result,
                                      const CanopenSdoTransaction *transaction)
{
  result->status = CANOPEN_SDO_STATUS_WAITING;
  result->confidence = transaction->ambiguous ? CANOPEN_SDO_CONFIDENCE_AMBIGUOUS :
                                                CANOPEN_SDO_CONFIDENCE_EXACT;
  result->index = transaction->index;
  result->subindex = transaction->subindex;
  result->has_address = true;
  result->transferred_bytes = transaction->transferred_bytes;
  result->segment_count = transaction->segment_count;
  result->local_transaction = transaction->local_transaction;
}

static void Register_Orphan(uint8_t node_id, const CanopenSdoAddress *address,
                            SdoFrameResult *result)
{
  ++node_states[node_id - 1U].snapshot.sdo_orphan_count;
  result->status = CANOPEN_SDO_STATUS_ORPHAN;
  result->confidence = CANOPEN_SDO_CONFIDENCE_NONE;
  if ((address != NULL) && address->valid)
  {
    result->index = address->index;
    result->subindex = address->subindex;
    result->data_offset = address->data_offset;
    result->has_address = true;
  }
}

static void Start_Transaction(uint8_t node_id, const CanFrameRecord *frame,
                              const CanopenSdoAddress *address, SdoFrameResult *result)
{
  CanopenSdoTransaction *transaction = &sdo_transactions[node_id - 1U];
  uint8_t command = frame->data[0];
  uint8_t ccs = command >> 5;
  SdoTransferDirection requested_direction =
    ((ccs == 2U) || (ccs == 5U)) ? SDO_TRANSFER_UPLOAD : SDO_TRANSFER_DOWNLOAD;
  bool collision = transaction->active;

  if (collision && (transaction->index == address->index) &&
      (transaction->subindex == address->subindex) &&
      (transaction->direction == requested_direction))
  {
    if ((frame->flags & CAN_FRAME_FLAG_LOCAL_TX) != 0U)
      transaction->local_transaction = true;
    if (!transaction->ambiguous)
    {
      transaction->ambiguous = true;
      ++node_states[node_id - 1U].snapshot.sdo_conflict_count;
    }
    transaction->last_activity_us = frame->timestamp_us;
    transaction->last_request_us = frame->timestamp_us;
    Set_ResultFromTransaction(result, transaction);
    result->data_offset = address->data_offset;
    Update_Object(node_id, address->index, address->subindex, CANOPEN_MESSAGE_SDO_RX,
                  frame, address->data_offset, result);
    return;
  }
  if (collision)
    Finish_Transaction(node_id, CANOPEN_SDO_STATUS_AMBIGUOUS, frame->timestamp_us, 0U, NULL);
  memset(transaction, 0, sizeof(*transaction));
  transaction->active = true;
  transaction->ambiguous = collision;
  transaction->index = address->index;
  transaction->subindex = address->subindex;
  transaction->started_us = frame->timestamp_us;
  transaction->last_activity_us = frame->timestamp_us;
  transaction->last_request_us = frame->timestamp_us;
  transaction->local_transaction =
    (frame->flags & CAN_FRAME_FLAG_LOCAL_TX) != 0U;
  transaction->phase = SDO_PHASE_INIT_RESPONSE;
  if (ccs == 2U)
  {
    transaction->direction = requested_direction;
    transaction->mode = SDO_MODE_EXPEDITED;
  }
  else if (ccs == 1U)
  {
    transaction->direction = requested_direction;
    transaction->mode = ((command & 0x02U) != 0U) ? SDO_MODE_EXPEDITED : SDO_MODE_SEGMENTED;
    if ((command & 0x03U) == 0x03U)
      transaction->transferred_bytes = 4U - ((command >> 2) & 0x03U);
  }
  else
  {
    transaction->direction = requested_direction;
    transaction->mode = SDO_MODE_BLOCK;
    transaction->phase = SDO_PHASE_BLOCK;
    transaction->ambiguous = true;
  }
  ++node_states[node_id - 1U].snapshot.sdo_started_count;
  Set_ResultFromTransaction(result, transaction);
  result->data_offset = address->data_offset;
  Update_Object(node_id, address->index, address->subindex, CANOPEN_MESSAGE_SDO_RX,
                frame, address->data_offset, result);
}

static void Process_ClientSdo(uint8_t node_id, const CanFrameRecord *frame,
                              const CanopenSdoAddress *address, SdoFrameResult *result)
{
  CanopenSdoTransaction *transaction = &sdo_transactions[node_id - 1U];
  uint8_t command = frame->data[0];
  uint8_t ccs = command >> 5;
  if (command == 0x80U)
  {
    uint32_t abort_code = Read_AbortCode(frame);
    if (transaction->active && address->valid &&
        (transaction->index == address->index) && (transaction->subindex == address->subindex))
    {
      Finish_Transaction(node_id, CANOPEN_SDO_STATUS_ABORTED,
                         frame->timestamp_us, abort_code, result);
      result->data_offset = address->data_offset;
      Update_Object(node_id, address->index, address->subindex,
                    CANOPEN_MESSAGE_SDO_RX, frame, address->data_offset, result);
    }
    else
    {
      Register_Orphan(node_id, address, result);
      if (address->valid)
        Update_Object(node_id, address->index, address->subindex,
                      CANOPEN_MESSAGE_SDO_RX, frame, address->data_offset, result);
    }
    return;
  }
  if (address->valid && ((ccs == 1U) || (ccs == 2U) || (ccs == 5U) || (ccs == 6U)))
  {
    Start_Transaction(node_id, frame, address, result);
    return;
  }
  if (!transaction->active)
  {
    Register_Orphan(node_id, NULL, result);
    return;
  }
  transaction->last_activity_us = frame->timestamp_us;
  Set_ResultFromTransaction(result, transaction);
  result->data_offset = 1U;
  if (transaction->mode == SDO_MODE_BLOCK)
    Finish_Transaction(node_id, CANOPEN_SDO_STATUS_AMBIGUOUS, frame->timestamp_us, 0U, result);
  else if ((transaction->direction == SDO_TRANSFER_UPLOAD) && (ccs == 3U) &&
           (transaction->phase == SDO_PHASE_UPLOAD_REQUEST))
  {
    if (((command >> 4) & 1U) != transaction->expected_toggle)
      Finish_Transaction(node_id, CANOPEN_SDO_STATUS_PROTOCOL_ERROR,
                         frame->timestamp_us, 0U, result);
    else
    {
      transaction->last_request_us = frame->timestamp_us;
      transaction->phase = SDO_PHASE_UPLOAD_RESPONSE;
    }
  }
  else if ((transaction->direction == SDO_TRANSFER_DOWNLOAD) && (ccs == 0U) &&
           (transaction->phase == SDO_PHASE_DOWNLOAD_SEGMENT))
  {
    uint8_t unused = (command >> 1) & 0x07U;
    if ((((command >> 4) & 1U) != transaction->expected_toggle) || (unused > 7U))
      Finish_Transaction(node_id, CANOPEN_SDO_STATUS_PROTOCOL_ERROR,
                         frame->timestamp_us, 0U, result);
    else
    {
      transaction->transferred_bytes += 7U - unused;
      ++transaction->segment_count;
      transaction->last_segment_sent = (command & 0x01U) != 0U;
      transaction->last_request_us = frame->timestamp_us;
      transaction->phase = SDO_PHASE_DOWNLOAD_ACK;
      result->transferred_bytes = transaction->transferred_bytes;
      result->segment_count = transaction->segment_count;
    }
  }
  else Finish_Transaction(node_id, CANOPEN_SDO_STATUS_PROTOCOL_ERROR,
                          frame->timestamp_us, 0U, result);
  Update_Object(node_id, result->index, result->subindex, CANOPEN_MESSAGE_SDO_RX,
                frame, 1U, result);
}

static void Process_ServerSdo(uint8_t node_id, const CanFrameRecord *frame,
                              const CanopenSdoAddress *address, SdoFrameResult *result)
{
  CanopenSdoTransaction *transaction = &sdo_transactions[node_id - 1U];
  uint8_t command = frame->data[0];
  uint8_t scs = command >> 5;
  if (command == 0x80U)
  {
    uint32_t abort_code = Read_AbortCode(frame);
    if (transaction->active && address->valid &&
        (transaction->index == address->index) && (transaction->subindex == address->subindex))
    {
      result->response_time_us = frame->timestamp_us - transaction->last_request_us;
      result->has_response_time = true;
      Finish_Transaction(node_id, CANOPEN_SDO_STATUS_ABORTED,
                         frame->timestamp_us, abort_code, result);
      result->data_offset = address->data_offset;
      Update_Object(node_id, address->index, address->subindex,
                    CANOPEN_MESSAGE_SDO_TX, frame, address->data_offset, result);
    }
    else
    {
      Register_Orphan(node_id, address, result);
      if (address->valid)
        Update_Object(node_id, address->index, address->subindex,
                      CANOPEN_MESSAGE_SDO_TX, frame, address->data_offset, result);
    }
    return;
  }
  if (!transaction->active)
  {
    Register_Orphan(node_id, address, result);
    if (address->valid)
      Update_Object(node_id, address->index, address->subindex,
                    CANOPEN_MESSAGE_SDO_TX, frame, address->data_offset, result);
    return;
  }
  if (address->valid && ((transaction->index != address->index) ||
                         (transaction->subindex != address->subindex)))
  {
    Finish_Transaction(node_id, CANOPEN_SDO_STATUS_AMBIGUOUS, frame->timestamp_us, 0U, NULL);
    Register_Orphan(node_id, address, result);
    Update_Object(node_id, address->index, address->subindex,
                  CANOPEN_MESSAGE_SDO_TX, frame, address->data_offset, result);
    return;
  }
  transaction->last_activity_us = frame->timestamp_us;
  Set_ResultFromTransaction(result, transaction);
  result->response_time_us = frame->timestamp_us - transaction->last_request_us;
  result->has_response_time = true;
  result->data_offset = address->valid ? address->data_offset : 1U;
  if (transaction->mode == SDO_MODE_BLOCK)
    Finish_Transaction(node_id, CANOPEN_SDO_STATUS_AMBIGUOUS, frame->timestamp_us, 0U, result);
  else if ((transaction->phase == SDO_PHASE_INIT_RESPONSE) && address->valid &&
           (transaction->direction == SDO_TRANSFER_UPLOAD) && (scs == 2U))
  {
    if ((command & 0x02U) != 0U)
    {
      if ((command & 0x01U) != 0U)
        transaction->transferred_bytes = 4U - ((command >> 2) & 0x03U);
      Finish_Transaction(node_id, CANOPEN_SDO_STATUS_COMPLETED,
                         frame->timestamp_us, 0U, result);
    }
    else
    {
      transaction->mode = SDO_MODE_SEGMENTED;
      transaction->phase = SDO_PHASE_UPLOAD_REQUEST;
    }
  }
  else if ((transaction->phase == SDO_PHASE_INIT_RESPONSE) && address->valid &&
           (transaction->direction == SDO_TRANSFER_DOWNLOAD) && (command == 0x60U))
  {
    if (transaction->mode == SDO_MODE_EXPEDITED)
      Finish_Transaction(node_id, CANOPEN_SDO_STATUS_COMPLETED,
                         frame->timestamp_us, 0U, result);
    else transaction->phase = SDO_PHASE_DOWNLOAD_SEGMENT;
  }
  else if ((transaction->direction == SDO_TRANSFER_UPLOAD) && (scs == 0U) &&
           (transaction->phase == SDO_PHASE_UPLOAD_RESPONSE))
  {
    uint8_t unused = (command >> 1) & 0x07U;
    if ((((command >> 4) & 1U) != transaction->expected_toggle) || (unused > 7U))
      Finish_Transaction(node_id, CANOPEN_SDO_STATUS_PROTOCOL_ERROR,
                         frame->timestamp_us, 0U, result);
    else
    {
      transaction->transferred_bytes += 7U - unused;
      ++transaction->segment_count;
      transaction->expected_toggle ^= 1U;
      result->transferred_bytes = transaction->transferred_bytes;
      result->segment_count = transaction->segment_count;
      if ((command & 0x01U) != 0U)
        Finish_Transaction(node_id, CANOPEN_SDO_STATUS_COMPLETED,
                           frame->timestamp_us, 0U, result);
      else transaction->phase = SDO_PHASE_UPLOAD_REQUEST;
    }
  }
  else if ((transaction->direction == SDO_TRANSFER_DOWNLOAD) && (scs == 1U) &&
           (transaction->phase == SDO_PHASE_DOWNLOAD_ACK))
  {
    if (((command >> 4) & 1U) != transaction->expected_toggle)
      Finish_Transaction(node_id, CANOPEN_SDO_STATUS_PROTOCOL_ERROR,
                         frame->timestamp_us, 0U, result);
    else if (transaction->last_segment_sent)
      Finish_Transaction(node_id, CANOPEN_SDO_STATUS_COMPLETED,
                         frame->timestamp_us, 0U, result);
    else
    {
      transaction->expected_toggle ^= 1U;
      transaction->phase = SDO_PHASE_DOWNLOAD_SEGMENT;
    }
  }
  else Finish_Transaction(node_id, CANOPEN_SDO_STATUS_PROTOCOL_ERROR,
                          frame->timestamp_us, 0U, result);
  Update_Object(node_id, result->index, result->subindex, CANOPEN_MESSAGE_SDO_TX,
                frame, result->data_offset, result);
}

static SdoFrameResult Process_Sdo(uint8_t node_id, CanopenMessageType type,
                                  const CanFrameRecord *frame,
                                  const CanopenSdoAddress *address)
{
  SdoFrameResult result;
  memset(&result, 0, sizeof(result));
  if (((frame->flags & CAN_FRAME_FLAG_REMOTE) != 0U) || (frame->dlc == 0U))
    Register_Orphan(node_id, address, &result);
  else if (type == CANOPEN_MESSAGE_SDO_RX)
    Process_ClientSdo(node_id, frame, address, &result);
  else Process_ServerSdo(node_id, frame, address, &result);
  return result;
}

void CanopenAnalyzer_Init(void)
{
  memset(node_states, 0, NODE_STATES_BYTES);
  memset(event_history, 0, EVENT_HISTORY_BYTES);
  memset(sdo_objects, 0, SDO_OBJECTS_BYTES);
  memset(sdo_transactions, 0, SDO_TRANSACTIONS_BYTES);
  event_write = 0U;
  event_count = 0U;
  queue_overflows = 0U;
  memset(&global_snapshot, 0, sizeof(global_snapshot));
}

void CanopenAnalyzer_ProcessFrame(const CanFrameRecord *frame)
{
  CanopenDecodedFrame decoded;
  CanopenSdoAddress address;
  CanopenNodeState *node;
  CanopenEventState *event;
  SdoFrameResult result;
  if (frame == NULL) return;
  decoded = CanopenDecoder_Decode(frame);
  address = CanopenDecoder_DecodeSdoAddress(frame, decoded.type);
  memset(&result, 0, sizeof(result));
  taskENTER_CRITICAL();
  ++global_snapshot.frame_count;
  global_snapshot.last_timestamp_us = frame->timestamp_us;
  if (!decoded.has_node_id)
  {
    taskEXIT_CRITICAL();
    return;
  }
  node = &node_states[decoded.node_id - 1U];
  if (!node->present)
  {
    node->present = true;
    node->snapshot.node_id = decoded.node_id;
    node->snapshot.first_seen_us = frame->timestamp_us;
    ++global_snapshot.unique_node_count;
  }
  node->snapshot.last_type = decoded.type;
  node->snapshot.last_seen_us = frame->timestamp_us;
  ++node->snapshot.frame_count;
  node->snapshot.byte_count += frame->dlc;
  if ((decoded.type == CANOPEN_MESSAGE_HEARTBEAT_GUARD) && (frame->dlc >= 1U))
  {
    node->snapshot.nmt_state = frame->data[0] & 0x7FU;
    node->snapshot.has_nmt_state = true;
  }
  if (decoded.type == CANOPEN_MESSAGE_SDO_RX)
  {
    ++node->snapshot.sdo_request_count;
    result = Process_Sdo(decoded.node_id, decoded.type, frame, &address);
  }
  else if (decoded.type == CANOPEN_MESSAGE_SDO_TX)
  {
    ++node->snapshot.sdo_response_count;
    result = Process_Sdo(decoded.node_id, decoded.type, frame, &address);
  }
  event = &event_history[event_write & CANOPEN_EVENT_HISTORY_MASK];
  memset(event, 0, sizeof(*event));
  event->node_id = decoded.node_id;
  event->snapshot.frame = *frame;
  event->snapshot.type = decoded.type;
  event->snapshot.response_time_us = result.response_time_us;
  event->snapshot.has_response_time = result.has_response_time;
  event->snapshot.object_index = result.index;
  event->snapshot.object_subindex = result.subindex;
  event->snapshot.data_offset = result.data_offset;
  event->snapshot.has_sdo_address = result.has_address;
  event->snapshot.sdo_status = result.status;
  event->snapshot.sdo_confidence = result.confidence;
  event->snapshot.transaction_time_us = result.transaction_time_us;
  event->snapshot.transferred_bytes = result.transferred_bytes;
  event->snapshot.abort_code = result.abort_code;
  event->snapshot.segment_count = result.segment_count;
  event->snapshot.local_transaction = result.local_transaction;
  ++event_write;
  if (event_count < CANOPEN_EVENT_HISTORY_LENGTH) ++event_count;
  taskEXIT_CRITICAL();
}

void CanopenAnalyzer_Update(uint32_t now_us)
{
  uint32_t node_index;
  taskENTER_CRITICAL();
  for (node_index = 0U; node_index < CANOPEN_NODE_COUNT_MAX; ++node_index)
  {
    CanopenSdoTransaction *transaction = &sdo_transactions[node_index];
    if (transaction->active &&
        ((uint32_t)(now_us - transaction->started_us) >= CANOPEN_SDO_TIMEOUT_US))
      Finish_Transaction((uint8_t)(node_index + 1U), CANOPEN_SDO_STATUS_TIMEOUT,
                         now_us, 0U, NULL);
  }
  taskEXIT_CRITICAL();
}

void CanopenAnalyzer_AddQueueOverflow(void)
{
  uint32_t node_index;
  taskENTER_CRITICAL();
  ++queue_overflows;
  for (node_index = 0U; node_index < CANOPEN_NODE_COUNT_MAX; ++node_index)
  {
    if (sdo_transactions[node_index].active)
      Finish_Transaction((uint8_t)(node_index + 1U), CANOPEN_SDO_STATUS_AMBIGUOUS,
                         global_snapshot.last_timestamp_us, 0U, NULL);
  }
  taskEXIT_CRITICAL();
}

uint32_t CanopenAnalyzer_GetNodesSnapshot(CanopenNodeSnapshot *nodes, uint32_t capacity,
                                          CanopenGlobalSnapshot *global)
{
  uint32_t node_index, copied = 0U;
  if ((nodes == NULL) || (capacity == 0U)) return 0U;
  taskENTER_CRITICAL();
  if (global != NULL) *global = global_snapshot;
  for (node_index = 0U; (node_index < CANOPEN_NODE_COUNT_MAX) && (copied < capacity);
       ++node_index)
    if (node_states[node_index].present) nodes[copied++] = node_states[node_index].snapshot;
  taskEXIT_CRITICAL();
  return copied;
}

uint32_t CanopenAnalyzer_GetNodeEventsSnapshot(uint8_t node_id,
                                               CanopenNodeEventSnapshot *events,
                                               uint32_t capacity)
{
  uint32_t age, copied = 0U;
  if ((node_id < 1U) || (node_id > 127U) || (events == NULL) || (capacity == 0U)) return 0U;
  taskENTER_CRITICAL();
  for (age = 0U; (age < event_count) && (copied < capacity); ++age)
  {
    const CanopenEventState *event =
      &event_history[(event_write - 1U - age) & CANOPEN_EVENT_HISTORY_MASK];
    if (event->node_id == node_id) events[copied++] = event->snapshot;
  }
  taskEXIT_CRITICAL();
  return copied;
}

uint32_t CanopenAnalyzer_GetNodeObjectsSnapshot(uint8_t node_id,
                                                CanopenNodeEventSnapshot *events,
                                                uint32_t capacity)
{
  uint32_t object_index, copied = 0U;
  if ((node_id < 1U) || (node_id > 127U) || (events == NULL) || (capacity == 0U)) return 0U;
  taskENTER_CRITICAL();
  for (object_index = 0U;
       (object_index < CANOPEN_SDO_OBJECT_POOL_LENGTH) && (copied < capacity); ++object_index)
  {
    const CanopenSdoObjectState *object = &sdo_objects[object_index];
    if (object->used && (object->node_id == node_id)) events[copied++] = object->snapshot;
  }
  taskEXIT_CRITICAL();
  return copied;
}

void CanopenAnalyzer_GetGlobalSnapshot(CanopenGlobalSnapshot *global)
{
  if (global == NULL) return;
  taskENTER_CRITICAL();
  *global = global_snapshot;
  taskEXIT_CRITICAL();
}

uint32_t CanopenAnalyzer_GetQueueOverflows(void)
{
  uint32_t value;
  taskENTER_CRITICAL();
  value = queue_overflows;
  taskEXIT_CRITICAL();
  return value;
}

void CanopenAnalyzer_Reset(void)
{
  taskENTER_CRITICAL();
  memset(node_states, 0, NODE_STATES_BYTES);
  memset(event_history, 0, EVENT_HISTORY_BYTES);
  memset(sdo_objects, 0, SDO_OBJECTS_BYTES);
  memset(sdo_transactions, 0, SDO_TRANSACTIONS_BYTES);
  event_write = 0U;
  event_count = 0U;
  queue_overflows = 0U;
  memset(&global_snapshot, 0, sizeof(global_snapshot));
  taskEXIT_CRITICAL();
}
