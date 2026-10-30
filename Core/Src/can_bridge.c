#include "can_bridge.h"

#include "FreeRTOS.h"
#include "can_capture.h"
#include "main.h"
#include "task.h"

#include <string.h>

#define CAN_BRIDGE_FIRST_NODE_ID 1U
#define CAN_BRIDGE_LAST_NODE_ID 127U
#define CAN_BRIDGE_NODE_COUNT 128U

typedef struct
{
  CanTxSdoRequest request;
  CanTxFrame request_frame;
  uint32_t client_request_id;
  uint32_t tx_sequence;
  uint32_t deadline_tick;
  bool pending;
  bool request_sent;
} CanBridgeNodeState;

typedef struct
{
  CanBridgeStatus status;
  CanBridgeNodeState nodes[CAN_BRIDGE_NODE_COUNT];
  CanBridgeResult results[CAN_BRIDGE_RESULT_CAPACITY];
  uint32_t result_write;
  uint32_t result_count;
  uint32_t lease_deadline_tick;
  uint32_t next_session_id;
} CanBridgeState;

static CanBridgeState bridge;

static uint32_t RemainingMs(uint32_t deadline, uint32_t now)
{
  int32_t remaining = (int32_t)(deadline - now);
  return remaining > 0 ? (uint32_t)remaining : 0U;
}

static bool ResponseMatches(const CanBridgeNodeState *node,
                            const CanFrameRecord *frame)
{
  return ((frame->flags & (CAN_FRAME_FLAG_EXTENDED | CAN_FRAME_FLAG_REMOTE |
                           CAN_FRAME_FLAG_LOCAL_TX)) == 0U) &&
         (frame->id == (0x580U + node->request.node_id)) &&
         (frame->dlc == 8U) &&
         (frame->data[1] == (uint8_t)node->request.index) &&
         (frame->data[2] == (uint8_t)(node->request.index >> 8U)) &&
         (frame->data[3] == node->request.subindex);
}

static bool RequestMatches(const CanBridgeNodeState *node,
                           const CanFrameRecord *frame)
{
  return ((frame->flags & CAN_FRAME_FLAG_LOCAL_TX) != 0U) &&
         (frame->id == node->request_frame.id) &&
         (frame->dlc == node->request_frame.dlc) &&
         (memcmp(frame->data, node->request_frame.data, 8U) == 0);
}

static CanBridgeResult *FindResultLocked(uint32_t client_request_id)
{
  uint32_t offset;
  for (offset = 0U; offset < bridge.result_count; ++offset)
  {
    uint32_t index = (bridge.result_write + CAN_BRIDGE_RESULT_CAPACITY - 1U -
                      offset) % CAN_BRIDGE_RESULT_CAPACITY;
    if (bridge.results[index].client_request_id == client_request_id)
      return &bridge.results[index];
  }
  return NULL;
}

static CanBridgeResult *LatestResultLocked(void)
{
  uint32_t index;
  if (bridge.result_count == 0U) return NULL;
  index = (bridge.result_write + CAN_BRIDGE_RESULT_CAPACITY - 1U) %
          CAN_BRIDGE_RESULT_CAPACITY;
  return &bridge.results[index];
}

static void FinishLocked(CanBridgeNodeState *node,
                         CanBridgeResultCode code,
                         const CanFrameRecord *response,
                         uint32_t abort_code)
{
  CanBridgeResult *result;
  uint32_t capture_low;
  uint32_t capture_high;

  if ((node == NULL) || !node->pending) return;
  node->pending = false;
  node->request_sent = false;
  if (bridge.status.pending_count > 0U) --bridge.status.pending_count;
  bridge.status.request_pending = bridge.status.pending_count != 0U;
  bridge.status.client_request_id = node->client_request_id;
  bridge.status.tx_sequence = node->tx_sequence;
  bridge.status.last_result = code;
  ++bridge.status.result_generation;
  if (bridge.status.result_generation == 0U)
    bridge.status.result_generation = 1U;

  result = &bridge.results[bridge.result_write];
  memset(result, 0, sizeof(*result));
  result->generation = bridge.status.result_generation;
  result->session_id = bridge.status.session_id;
  result->client_request_id = node->client_request_id;
  result->tx_sequence = node->tx_sequence;
  if (response != NULL)
  {
    result->timestamp_us = response->timestamp_us;
    result->capture_time_us = ((uint64_t)response->capture_time_high << 32U) |
                              response->capture_time_us;
  }
  else
  {
    CanCapture_GetTimestamps(&result->timestamp_us,
                             &capture_low, &capture_high);
    result->capture_time_us = ((uint64_t)capture_high << 32U) | capture_low;
  }
  result->abort_code = abort_code;
  result->code = code;
  result->has_response = response != NULL;
  if (response != NULL) result->response = *response;

  bridge.result_write = (bridge.result_write + 1U) %
                        CAN_BRIDGE_RESULT_CAPACITY;
  if (bridge.result_count < CAN_BRIDGE_RESULT_CAPACITY)
    ++bridge.result_count;

  switch (code)
  {
    case CAN_BRIDGE_RESULT_COMPLETED: ++bridge.status.completed; break;
    case CAN_BRIDGE_RESULT_ABORT: ++bridge.status.aborted; break;
    case CAN_BRIDGE_RESULT_TIMEOUT: ++bridge.status.timeouts; break;
    default: break;
  }
}

static void FinishAllLocked(CanBridgeResultCode code)
{
  uint32_t node_id;
  for (node_id = CAN_BRIDGE_FIRST_NODE_ID;
       node_id <= CAN_BRIDGE_LAST_NODE_ID; ++node_id)
  {
    if (bridge.nodes[node_id].pending)
      FinishLocked(&bridge.nodes[node_id], code, NULL, 0U);
  }
}

void CanBridge_Init(void)
{
  memset(&bridge, 0, sizeof(bridge));
  bridge.next_session_id = HAL_GetUIDw0() ^ HAL_GetUIDw1() ^ HAL_GetUIDw2() ^
                           HAL_GetTick() ^ 0x42524731UL;
  if (bridge.next_session_id == 0U) bridge.next_session_id = 1U;
}

bool CanBridge_Enable(uint32_t *session_id)
{
  CanCaptureStatus capture;
  CanTxStatus tx;
  uint32_t assigned;

  taskENTER_CRITICAL();
  if (bridge.status.active)
  {
    assigned = bridge.status.session_id;
    taskEXIT_CRITICAL();
    if (session_id != NULL) *session_id = assigned;
    return true;
  }
  taskEXIT_CRITICAL();

  CanControl_GetStatus(&capture);
  CanTx_GetStatus(&tx);
  if ((capture.bitrate_kbps == 0U) || (capture.state == CAN_SYNC_SEARCHING) ||
      tx.transmitting || (tx.queue_depth != 0U) || tx.periodic_active)
  {
    return false;
  }
  if (!CanTx_SetBridgeExclusive(true)) return false;
  if (!CanControl_RequestActive(true))
  {
    (void)CanTx_SetBridgeExclusive(false);
    return false;
  }

  taskENTER_CRITICAL();
  if (bridge.status.active)
  {
    assigned = bridge.status.session_id;
    taskEXIT_CRITICAL();
    if (session_id != NULL) *session_id = assigned;
    return true;
  }
  assigned = bridge.next_session_id++;
  if (bridge.next_session_id == 0U) bridge.next_session_id = 1U;
  memset(&bridge.status, 0, sizeof(bridge.status));
  memset(bridge.nodes, 0, sizeof(bridge.nodes));
  memset(bridge.results, 0, sizeof(bridge.results));
  bridge.result_write = 0U;
  bridge.result_count = 0U;
  bridge.status.active = true;
  bridge.status.session_id = assigned;
  bridge.status.last_result = CAN_BRIDGE_RESULT_NONE;
  bridge.lease_deadline_tick = HAL_GetTick() + CAN_BRIDGE_LEASE_MS;
  taskEXIT_CRITICAL();
  if (session_id != NULL) *session_id = assigned;
  return true;
}

void CanBridge_Disable(void)
{
  bool was_active;
  taskENTER_CRITICAL();
  was_active = bridge.status.active;
  bridge.status.active = false;
  bridge.status.connected = false;
  FinishAllLocked(CAN_BRIDGE_RESULT_CANCELLED);
  bridge.status.session_id = 0U;
  bridge.lease_deadline_tick = 0U;
  taskEXIT_CRITICAL();
  if (was_active)
  {
    CanTx_CancelAll();
    CanControl_ForceSilent(CAN_SAFETY_USER);
    (void)CanTx_SetBridgeExclusive(false);
  }
}

bool CanBridge_IsActive(void)
{
  bool active;
  taskENTER_CRITICAL();
  active = bridge.status.active;
  taskEXIT_CRITICAL();
  return active;
}

bool CanBridge_Attach(uint32_t session_id)
{
  bool attached = false;
  taskENTER_CRITICAL();
  if (bridge.status.active && !bridge.status.connected &&
      (bridge.status.session_id == session_id) &&
      ((int32_t)(HAL_GetTick() - bridge.lease_deadline_tick) < 0))
  {
    bridge.status.connected = true;
    bridge.lease_deadline_tick = HAL_GetTick() + CAN_BRIDGE_LEASE_MS;
    attached = true;
  }
  taskEXIT_CRITICAL();
  return attached;
}

void CanBridge_Detach(uint32_t session_id)
{
  taskENTER_CRITICAL();
  if (bridge.status.session_id == session_id)
    bridge.status.connected = false;
  taskEXIT_CRITICAL();
}

void CanBridge_Heartbeat(uint32_t session_id)
{
  taskENTER_CRITICAL();
  if (bridge.status.active && (bridge.status.session_id == session_id))
    bridge.lease_deadline_tick = HAL_GetTick() + CAN_BRIDGE_LEASE_MS;
  taskEXIT_CRITICAL();
}

CanBridgeSubmitResult CanBridge_SubmitSdo(uint32_t session_id,
                                         uint32_t client_request_id,
                                         const CanTxSdoRequest *request,
                                         uint32_t *tx_sequence)
{
  CanBridgeNodeState *node;
  CanBridgeResult *retained;
  CanTxFrame frame;
  CanTxSubmitResult submitted;
  uint32_t node_id;
  uint32_t sequence = 0U;

  if ((client_request_id == 0U) ||
      !CanTx_BuildSdoRequest(request, &frame))
    return CAN_BRIDGE_SUBMIT_INVALID;
  node_id = request->node_id;

  taskENTER_CRITICAL();
  if (!bridge.status.active || (bridge.status.session_id != session_id))
  {
    taskEXIT_CRITICAL();
    return CAN_BRIDGE_SUBMIT_DISABLED;
  }
  if (!bridge.status.connected)
  {
    taskEXIT_CRITICAL();
    return CAN_BRIDGE_SUBMIT_NOT_CONNECTED;
  }

  for (uint32_t id = CAN_BRIDGE_FIRST_NODE_ID;
       id <= CAN_BRIDGE_LAST_NODE_ID; ++id)
  {
    if (bridge.nodes[id].pending &&
        (bridge.nodes[id].client_request_id == client_request_id))
    {
      sequence = bridge.nodes[id].tx_sequence;
      taskEXIT_CRITICAL();
      if (tx_sequence != NULL) *tx_sequence = sequence;
      return CAN_BRIDGE_SUBMIT_DUPLICATE;
    }
  }
  retained = FindResultLocked(client_request_id);
  if (retained != NULL)
  {
    sequence = retained->tx_sequence;
    taskEXIT_CRITICAL();
    if (tx_sequence != NULL) *tx_sequence = sequence;
    return CAN_BRIDGE_SUBMIT_DUPLICATE;
  }

  node = &bridge.nodes[node_id];
  if (node->pending)
  {
    ++bridge.status.rejected;
    taskEXIT_CRITICAL();
    return CAN_BRIDGE_SUBMIT_BUSY;
  }
  memset(node, 0, sizeof(*node));
  node->request = *request;
  node->request_frame = frame;
  node->client_request_id = client_request_id;
  node->pending = true;
  ++bridge.status.pending_count;
  bridge.status.request_pending = true;
  bridge.status.client_request_id = client_request_id;
  bridge.status.tx_sequence = 0U;
  bridge.status.last_result = CAN_BRIDGE_RESULT_NONE;
  bridge.lease_deadline_tick = HAL_GetTick() + CAN_BRIDGE_LEASE_MS;
  ++bridge.status.requests;
  taskEXIT_CRITICAL();

  submitted = CanTx_SubmitBridgeSdo(request, &sequence);
  if (submitted != CAN_TX_SUBMIT_OK)
  {
    taskENTER_CRITICAL();
    node = &bridge.nodes[node_id];
    if (node->pending && (node->client_request_id == client_request_id))
    {
      node->pending = false;
      if (bridge.status.pending_count > 0U) --bridge.status.pending_count;
      bridge.status.request_pending = bridge.status.pending_count != 0U;
    }
    ++bridge.status.rejected;
    taskEXIT_CRITICAL();
    return submitted == CAN_TX_SUBMIT_INVALID ? CAN_BRIDGE_SUBMIT_INVALID :
                                                CAN_BRIDGE_SUBMIT_CAN_NOT_READY;
  }

  taskENTER_CRITICAL();
  node = &bridge.nodes[node_id];
  if (node->pending && (node->client_request_id == client_request_id))
  {
    node->tx_sequence = sequence;
    bridge.status.tx_sequence = sequence;
  }
  else
  {
    retained = FindResultLocked(client_request_id);
    if (retained != NULL) retained->tx_sequence = sequence;
  }
  taskEXIT_CRITICAL();
  if (tx_sequence != NULL) *tx_sequence = sequence;
  return CAN_BRIDGE_SUBMIT_OK;
}

void CanBridge_ProcessFrame(const CanFrameRecord *frame,
                            CanFrameRecord *stream_frame)
{
  CanBridgeNodeState *node = NULL;
  uint32_t node_id = 0U;

  if ((frame == NULL) || (stream_frame == NULL)) return;
  *stream_frame = *frame;

  taskENTER_CRITICAL();
  if (!bridge.status.active)
  {
    taskEXIT_CRITICAL();
    return;
  }

  if (((frame->flags & (CAN_FRAME_FLAG_EXTENDED | CAN_FRAME_FLAG_REMOTE)) == 0U) &&
      ((frame->flags & CAN_FRAME_FLAG_LOCAL_TX) != 0U) &&
      (frame->id >= 0x601U) && (frame->id <= 0x67FU))
  {
    node_id = frame->id - 0x600U;
    node = &bridge.nodes[node_id];
    if (node->pending && RequestMatches(node, frame))
    {
      stream_frame->flags |= CAN_FRAME_FLAG_BRIDGE_REQUEST;
      node->request_sent = true;
      node->deadline_tick = HAL_GetTick() + node->request.timeout_ms;
      taskEXIT_CRITICAL();
      return;
    }
  }
  else if (((frame->flags & (CAN_FRAME_FLAG_EXTENDED | CAN_FRAME_FLAG_REMOTE |
                             CAN_FRAME_FLAG_LOCAL_TX)) == 0U) &&
           (frame->id >= 0x581U) && (frame->id <= 0x5FFU))
  {
    node_id = frame->id - 0x580U;
    node = &bridge.nodes[node_id];
    if (node->pending && node->request_sent && ResponseMatches(node, frame))
    {
      uint8_t command = frame->data[0];
      uint32_t abort_code = (uint32_t)frame->data[4] |
                            ((uint32_t)frame->data[5] << 8U) |
                            ((uint32_t)frame->data[6] << 16U) |
                            ((uint32_t)frame->data[7] << 24U);
      CanBridgeResultCode result = CAN_BRIDGE_RESULT_PROTOCOL_ERROR;
      stream_frame->flags |= CAN_FRAME_FLAG_BRIDGE_RESPONSE;
      if (command == 0x80U)
        result = CAN_BRIDGE_RESULT_ABORT;
      else if ((node->request.command == CAN_TX_SDO_READ) &&
               ((command >> 5U) == 2U) && ((command & 0x02U) != 0U))
        result = CAN_BRIDGE_RESULT_COMPLETED;
      else if ((node->request.command != CAN_TX_SDO_READ) &&
               (command == 0x60U))
        result = CAN_BRIDGE_RESULT_COMPLETED;
      FinishLocked(node, result, frame,
                   result == CAN_BRIDGE_RESULT_ABORT ? abort_code : 0U);
      taskEXIT_CRITICAL();
      return;
    }
  }

  stream_frame->flags |= CAN_FRAME_FLAG_BRIDGE_UNSOLICITED;
  ++bridge.status.unsolicited_frames;
  taskEXIT_CRITICAL();
}

void CanBridge_Update(void)
{
  CanBridgeStatus status;
  CanCaptureStatus capture;
  CanTxHistoryEntry history[CAN_TX_HISTORY_CAPACITY];
  size_t history_count = 0U;
  uint32_t node_id;
  uint32_t now = HAL_GetTick();
  bool disable = false;

  taskENTER_CRITICAL();
  if (bridge.status.active &&
      ((int32_t)(now - bridge.lease_deadline_tick) >= 0))
  {
    disable = true;
  }
  else if (bridge.status.active)
  {
    for (node_id = CAN_BRIDGE_FIRST_NODE_ID;
         node_id <= CAN_BRIDGE_LAST_NODE_ID; ++node_id)
    {
      CanBridgeNodeState *node = &bridge.nodes[node_id];
      if (node->pending && node->request_sent &&
          ((int32_t)(now - node->deadline_tick) >= 0))
        FinishLocked(node, CAN_BRIDGE_RESULT_TIMEOUT, NULL, 0U);
    }
  }
  taskEXIT_CRITICAL();

  if (disable)
  {
    CanBridge_Disable();
    return;
  }

  CanBridge_GetStatus(&status);
  if (!status.active || !status.request_pending) return;
  CanControl_GetStatus(&capture);
  if (capture.safety_reason == CAN_SAFETY_BUS_OFF)
  {
    taskENTER_CRITICAL();
    FinishAllLocked(CAN_BRIDGE_RESULT_BUS_OFF);
    taskEXIT_CRITICAL();
    return;
  }

  history_count = CanTx_CopyHistory(history, CAN_TX_HISTORY_CAPACITY);
  taskENTER_CRITICAL();
  for (node_id = CAN_BRIDGE_FIRST_NODE_ID;
       node_id <= CAN_BRIDGE_LAST_NODE_ID; ++node_id)
  {
    CanBridgeNodeState *node = &bridge.nodes[node_id];
    size_t index;
    if (!node->pending || node->request_sent || (node->tx_sequence == 0U))
      continue;
    for (index = 0U; index < history_count; ++index)
    {
      if (history[index].sequence == node->tx_sequence)
      {
        if (history[index].result == CAN_TX_RESULT_SENT)
        {
          node->request_sent = true;
          node->deadline_tick = now + node->request.timeout_ms;
        }
        else
        {
          CanBridgeResultCode result =
            history[index].result == CAN_TX_RESULT_BUS_OFF ?
            CAN_BRIDGE_RESULT_BUS_OFF : CAN_BRIDGE_RESULT_DRIVER_ERROR;
          FinishLocked(node, result, NULL, 0U);
        }
        break;
      }
    }
  }
  taskEXIT_CRITICAL();
}

void CanBridge_GetStatus(CanBridgeStatus *status)
{
  if (status == NULL) return;
  taskENTER_CRITICAL();
  *status = bridge.status;
  status->lease_remaining_ms = bridge.status.active ?
    RemainingMs(bridge.lease_deadline_tick, HAL_GetTick()) : 0U;
  taskEXIT_CRITICAL();
}

void CanBridge_GetResult(CanBridgeResult *result)
{
  CanBridgeResult *latest;
  if (result == NULL) return;
  taskENTER_CRITICAL();
  latest = LatestResultLocked();
  if (latest != NULL) *result = *latest;
  else memset(result, 0, sizeof(*result));
  taskEXIT_CRITICAL();
}

bool CanBridge_FindResult(uint32_t client_request_id,
                          CanBridgeResult *result)
{
  CanBridgeResult *found;
  if ((client_request_id == 0U) || (result == NULL)) return false;
  taskENTER_CRITICAL();
  found = FindResultLocked(client_request_id);
  if (found != NULL) *result = *found;
  taskEXIT_CRITICAL();
  return found != NULL;
}

bool CanBridge_GetNextResult(uint32_t after_generation,
                             CanBridgeResult *result,
                             uint32_t *lost_results)
{
  uint32_t oldest_index;
  uint32_t offset;
  uint32_t lost = 0U;
  bool found = false;

  if (result == NULL) return false;
  taskENTER_CRITICAL();
  if (bridge.result_count != 0U)
  {
    oldest_index = (bridge.result_write + CAN_BRIDGE_RESULT_CAPACITY -
                    bridge.result_count) % CAN_BRIDGE_RESULT_CAPACITY;
    if ((after_generation != 0U) &&
        (bridge.results[oldest_index].generation > after_generation + 1U))
      lost = bridge.results[oldest_index].generation - after_generation - 1U;
    for (offset = 0U; offset < bridge.result_count; ++offset)
    {
      uint32_t index = (oldest_index + offset) % CAN_BRIDGE_RESULT_CAPACITY;
      if ((after_generation == 0U) ||
          (bridge.results[index].generation > after_generation))
      {
        *result = bridge.results[index];
        found = true;
        break;
      }
    }
  }
  taskEXIT_CRITICAL();
  if (lost_results != NULL) *lost_results = lost;
  return found;
}

const char *CanBridge_ResultName(CanBridgeResultCode result)
{
  switch (result)
  {
    case CAN_BRIDGE_RESULT_COMPLETED: return "completed";
    case CAN_BRIDGE_RESULT_ABORT: return "abort";
    case CAN_BRIDGE_RESULT_TIMEOUT: return "timeout";
    case CAN_BRIDGE_RESULT_BUS_OFF: return "bus_off";
    case CAN_BRIDGE_RESULT_DRIVER_ERROR: return "driver_error";
    case CAN_BRIDGE_RESULT_CANCELLED: return "cancelled";
    case CAN_BRIDGE_RESULT_PROTOCOL_ERROR: return "protocol_error";
    default: return "none";
  }
}
