#include "can_tx_manager.h"

#include "FreeRTOS.h"
#include "app_watchdog.h"
#include "can.h"
#include "can_capture.h"
#include "can_pipeline.h"
#include "queue.h"
#include "task.h"

#include <string.h>

typedef enum
{
  CAN_TX_OWNER_NORMAL = 0,
  CAN_TX_OWNER_BRIDGE,
  CAN_TX_OWNER_SCANNER
} CanTxOwner;

typedef struct
{
  uint32_t sequence;
  CanTxFrame frame;
  CanTxOwner owner;
  bool periodic;
} CanTxQueueEntry;

typedef struct
{
  CanTxFrame frame;
  uint32_t sequence;
  uint32_t period_ms;
  uint32_t next_due_tick;
  bool enabled;
} CanTxPeriodicState;

typedef struct
{
  CanTxSdoRequest request;
  CanFrameRecord response;
  uint32_t sequence;
  uint32_t request_timestamp_us;
  uint32_t response_time_us;
  bool used;
  bool waiting;
  bool has_response;
} CanTxResponseCorrelation;

static StaticQueue_t tx_queue_control;
static uint8_t tx_queue_storage[CAN_TX_QUEUE_CAPACITY * sizeof(CanTxQueueEntry)];
static QueueHandle_t tx_queue;
static CanTxHistoryEntry history[CAN_TX_HISTORY_CAPACITY];
static uint32_t history_write;
static uint32_t history_count;
static uint32_t next_sequence;
static uint32_t pending_sequences[CAN_TX_QUEUE_CAPACITY];
static uint32_t pending_count;
static uint32_t cancelled_sequences[CAN_TX_QUEUE_CAPACITY];
static uint32_t cancelled_count;
static volatile bool cancel_all_requested;
static CanTxStatus public_status;
static CanTxPeriodicState periodic_state;
static CanTxHistoryEntry periodic_history;
static bool periodic_history_valid;
static CanTxResponseCorrelation response_correlations[CAN_TX_QUEUE_CAPACITY];
static uint32_t auto_silent_deadline;
static bool auto_silent_armed;
static CanTxOwner tx_owner;

static bool IsPendingLocked(uint32_t sequence)
{
  uint32_t index;
  for (index = 0U; index < pending_count; ++index)
  {
    if (pending_sequences[index] == sequence) return true;
  }
  return false;
}

static void RemovePendingLocked(uint32_t sequence)
{
  uint32_t index;
  for (index = 0U; index < pending_count; ++index)
  {
    if (pending_sequences[index] == sequence)
    {
      --pending_count;
      pending_sequences[index] = pending_sequences[pending_count];
      return;
    }
  }
}

static bool ConsumeCancellation(uint32_t sequence)
{
  bool cancelled = false;
  uint32_t index;
  taskENTER_CRITICAL();
  for (index = 0U; index < cancelled_count; ++index)
  {
    if (cancelled_sequences[index] == sequence)
    {
      --cancelled_count;
      cancelled_sequences[index] = cancelled_sequences[cancelled_count];
      cancelled = true;
      break;
    }
  }
  taskEXIT_CRITICAL();
  return cancelled;
}

static CanTxHistoryEntry *FindHistoryLocked(uint32_t sequence)
{
  uint32_t age;
  if (periodic_history_valid && (periodic_history.sequence == sequence))
    return &periodic_history;
  for (age = 0U; age < history_count; ++age)
  {
    uint32_t source = (history_write - 1U - age) % CAN_TX_HISTORY_CAPACITY;
    if (history[source].sequence == sequence) return &history[source];
  }
  return NULL;
}

static CanTxResponseCorrelation *FindCorrelationLocked(uint32_t sequence)
{
  uint32_t index;
  for (index = 0U; index < CAN_TX_QUEUE_CAPACITY; ++index)
  {
    if (response_correlations[index].used &&
        (response_correlations[index].sequence == sequence))
      return &response_correlations[index];
  }
  return NULL;
}

static void ApplyResponseLocked(uint32_t sequence,
                                const CanTxResponseCorrelation *correlation)
{
  CanTxHistoryEntry *entry;
  if ((correlation == NULL) || !correlation->has_response) return;
  entry = FindHistoryLocked(sequence);
  if (entry == NULL) return;
  entry->response = correlation->response;
  entry->response_time_us = correlation->response_time_us;
  entry->has_response = true;
}

static void ArmResponseCorrelation(const CanTxQueueEntry *entry,
                                   uint32_t request_timestamp_us)
{
  CanTxSdoRequest request;
  CanTxResponseCorrelation *slot = NULL;
  CanTxResponseCorrelation *oldest = NULL;
  uint32_t index;
  uint32_t oldest_age = 0U;
  if ((entry == NULL) || !CanTx_DecodeSdoRequest(&entry->frame, &request)) return;

  taskENTER_CRITICAL();
  slot = FindCorrelationLocked(entry->sequence);
  for (index = 0U; (slot == NULL) && (index < CAN_TX_QUEUE_CAPACITY); ++index)
  {
    CanTxResponseCorrelation *candidate = &response_correlations[index];
    uint32_t age = request_timestamp_us - candidate->request_timestamp_us;
    if (!candidate->used ||
        (candidate->waiting &&
         (age > ((uint32_t)candidate->request.timeout_ms * 1000U))))
    {
      slot = candidate;
      break;
    }
    if ((oldest == NULL) || (age > oldest_age))
    {
      oldest_age = age;
      oldest = candidate;
    }
  }
  if (slot == NULL) slot = oldest;
  if (slot != NULL)
  {
    memset(slot, 0, sizeof(*slot));
    slot->request = request;
    slot->sequence = entry->sequence;
    slot->request_timestamp_us = request_timestamp_us;
    slot->used = true;
    slot->waiting = true;
  }
  taskEXIT_CRITICAL();
}

static void FinishResponseCorrelationLocked(uint32_t sequence, CanTxResult result)
{
  CanTxResponseCorrelation *correlation = FindCorrelationLocked(sequence);
  CanTxHistoryEntry *entry;
  if (correlation == NULL) return;
  if (result != CAN_TX_RESULT_SENT)
  {
    memset(correlation, 0, sizeof(*correlation));
    return;
  }
  entry = FindHistoryLocked(sequence);
  if (entry != NULL) entry->timestamp_us = correlation->request_timestamp_us;
  ApplyResponseLocked(sequence, correlation);
}

static void RecordResult(const CanTxQueueEntry *entry, CanTxResult result)
{
  CanTxHistoryEntry *stored;
  taskENTER_CRITICAL();
  if (entry->periodic)
  {
    stored = &periodic_history;
    if (!periodic_history_valid)
    {
      memset(stored, 0, sizeof(*stored));
      stored->sequence = entry->sequence;
      stored->frame = entry->frame;
      stored->periodic = true;
      periodic_history_valid = true;
    }
    ++stored->attempt_count;
    public_status.periodic_attempts = stored->attempt_count;
  }
  else
  {
    stored = &history[history_write % CAN_TX_HISTORY_CAPACITY];
    memset(stored, 0, sizeof(*stored));
    stored->sequence = entry->sequence;
    stored->frame = entry->frame;
    stored->attempt_count = 1U;
    ++history_write;
    if (history_count < CAN_TX_HISTORY_CAPACITY) ++history_count;
    RemovePendingLocked(entry->sequence);
  }
  stored->sequence = entry->sequence;
  stored->timestamp_us = CanCapture_GetTimestampUs();
  stored->frame = entry->frame;
  stored->result = result;
  FinishResponseCorrelationLocked(entry->sequence, result);
  public_status.last_sequence = entry->sequence;
  public_status.last_result = result;
  public_status.transmitting = false;
  switch (result)
  {
    case CAN_TX_RESULT_SENT: ++public_status.sent; break;
    case CAN_TX_RESULT_CANCELLED: ++public_status.cancelled; break;
    case CAN_TX_RESULT_TIMEOUT: ++public_status.timeouts; break;
    case CAN_TX_RESULT_ARBITRATION_LOST: ++public_status.arbitration_lost; break;
    case CAN_TX_RESULT_BUS_OFF: ++public_status.bus_off; break;
    default: ++public_status.errors; break;
  }
  taskEXIT_CRITICAL();
}

static uint32_t MailboxShift(uint32_t mailbox)
{
  if (mailbox == CAN_TX_MAILBOX1) return 8U;
  if (mailbox == CAN_TX_MAILBOX2) return 16U;
  return 0U;
}

static CanTxResult Transmit(const CanTxQueueEntry *entry)
{
  CAN_TxHeaderTypeDef header = {0};
  CanFrameRecord local_frame = {0};
  uint32_t mailbox;
  uint32_t shift;
  uint32_t started;

  if (!CanControl_IsActive()) return CAN_TX_RESULT_MODE_DISABLED;
  if (ConsumeCancellation(entry->sequence)) return CAN_TX_RESULT_CANCELLED;

  header.IDE = entry->frame.extended ? CAN_ID_EXT : CAN_ID_STD;
  header.RTR = entry->frame.remote ? CAN_RTR_REMOTE : CAN_RTR_DATA;
  header.DLC = entry->frame.dlc;
  header.TransmitGlobalTime = DISABLE;
  if (entry->frame.extended) header.ExtId = entry->frame.id;
  else header.StdId = entry->frame.id;

  if (HAL_CAN_AddTxMessage(&hcan1, &header, (uint8_t *)entry->frame.data,
                           &mailbox) != HAL_OK)
  {
    return CAN_TX_RESULT_DRIVER_ERROR;
  }

  CanCapture_GetTimestamps(&local_frame.timestamp_us,
                           &local_frame.capture_time_us,
                           &local_frame.capture_time_high);
  local_frame.id = entry->frame.id;
  local_frame.dlc = entry->frame.dlc;
  local_frame.flags = CAN_FRAME_FLAG_LOCAL_TX;
  if (entry->frame.extended) local_frame.flags |= CAN_FRAME_FLAG_EXTENDED;
  if (entry->frame.remote) local_frame.flags |= CAN_FRAME_FLAG_REMOTE;
  memcpy(local_frame.data, entry->frame.data, sizeof(local_frame.data));
  ArmResponseCorrelation(entry, local_frame.timestamp_us);
  (void)CanPipeline_PublishLocalTx(&local_frame);

  shift = MailboxShift(mailbox);
  started = HAL_GetTick();
  taskENTER_CRITICAL();
  public_status.transmitting = true;
  taskEXIT_CRITICAL();

  for (;;)
  {
    uint32_t tsr = CAN1->TSR;
    AppWatchdog_Heartbeat(APP_WATCHDOG_CAN_TX);
    if ((CAN1->ESR & CAN_ESR_BOFF) != 0U)
    {
      (void)HAL_CAN_AbortTxRequest(&hcan1, mailbox);
      CanControl_ForceSilent(CAN_SAFETY_BUS_OFF);
      return CAN_TX_RESULT_BUS_OFF;
    }
    if ((tsr & (CAN_TSR_RQCP0 << shift)) != 0U)
    {
      CanTxResult result = CAN_TX_RESULT_DRIVER_ERROR;
      if ((tsr & (CAN_TSR_TXOK0 << shift)) != 0U)
        result = CAN_TX_RESULT_SENT;
      else if ((tsr & (CAN_TSR_ALST0 << shift)) != 0U)
        result = CAN_TX_RESULT_ARBITRATION_LOST;
      CAN1->TSR = CAN_TSR_RQCP0 << shift;
      return result;
    }
    if (cancel_all_requested || ConsumeCancellation(entry->sequence) ||
        !CanControl_IsActive())
    {
      CanCaptureStatus capture;
      (void)HAL_CAN_AbortTxRequest(&hcan1, mailbox);
      CanControl_GetStatus(&capture);
      if (capture.safety_reason == CAN_SAFETY_BUS_OFF)
      {
        return CAN_TX_RESULT_BUS_OFF;
      }
      return CAN_TX_RESULT_CANCELLED;
    }
    if ((uint32_t)(HAL_GetTick() - started) >= entry->frame.timeout_ms)
    {
      (void)HAL_CAN_AbortTxRequest(&hcan1, mailbox);
      return CAN_TX_RESULT_TIMEOUT;
    }
    vTaskDelay(pdMS_TO_TICKS(1U));
  }
}

void CanTxManager_Init(void)
{
  memset(&public_status, 0, sizeof(public_status));
  memset(history, 0, sizeof(history));
  history_write = 0U;
  history_count = 0U;
  next_sequence = 1U;
  pending_count = 0U;
  cancelled_count = 0U;
  cancel_all_requested = false;
  memset(&periodic_state, 0, sizeof(periodic_state));
  memset(&periodic_history, 0, sizeof(periodic_history));
  periodic_history_valid = false;
  memset(response_correlations, 0, sizeof(response_correlations));
  auto_silent_deadline = 0U;
  auto_silent_armed = false;
  tx_owner = CAN_TX_OWNER_NORMAL;
  public_status.last_result = CAN_TX_RESULT_MODE_DISABLED;
  tx_queue = xQueueCreateStatic(CAN_TX_QUEUE_CAPACITY, sizeof(CanTxQueueEntry),
                                tx_queue_storage, &tx_queue_control);
}

static bool ValidateFrame(const CanTxFrame *frame)
{
  if ((frame == NULL) || (frame->dlc > 8U) ||
      (frame->timeout_ms < 10U) || (frame->timeout_ms > 1000U))
  {
    return false;
  }
  if (frame->extended)
  {
    if (frame->id > 0x1FFFFFFFU) return false;
  }
  else if (frame->id > 0x7FFU)
  {
    return false;
  }
  return true;
}

static CanTxSubmitResult SubmitFrame(const CanTxFrame *frame, uint32_t *sequence,
                                     CanTxOwner owner)
{
  CanTxQueueEntry entry;
  CanCaptureStatus capture;
  if (!ValidateFrame(frame)) return CAN_TX_SUBMIT_INVALID;
  if (cancel_all_requested) return CAN_TX_SUBMIT_MODE_DISABLED;
  CanControl_GetStatus(&capture);
  if ((capture.bitrate_kbps == 0U) || (capture.state == CAN_SYNC_SEARCHING))
    return CAN_TX_SUBMIT_MODE_DISABLED;

  taskENTER_CRITICAL();
  if (tx_owner != owner)
  {
    taskEXIT_CRITICAL();
    return CAN_TX_SUBMIT_MODE_DISABLED;
  }
  if (pending_count >= CAN_TX_QUEUE_CAPACITY)
  {
    taskEXIT_CRITICAL();
    return CAN_TX_SUBMIT_QUEUE_FULL;
  }
  entry.sequence = next_sequence++;
  if (next_sequence == 0U) next_sequence = 1U;
  pending_sequences[pending_count++] = entry.sequence;
  taskEXIT_CRITICAL();
  entry.frame = *frame;
  entry.owner = owner;
  entry.periodic = false;

  if (xQueueSend(tx_queue, &entry, 0U) != pdPASS)
  {
    taskENTER_CRITICAL();
    RemovePendingLocked(entry.sequence);
    taskEXIT_CRITICAL();
    return CAN_TX_SUBMIT_QUEUE_FULL;
  }
  taskENTER_CRITICAL();
  ++public_status.queued;
  taskEXIT_CRITICAL();
  if (sequence != NULL) *sequence = entry.sequence;
  return CAN_TX_SUBMIT_OK;
}

bool CanTx_BuildSdoRequest(const CanTxSdoRequest *request, CanTxFrame *frame)
{
  uint8_t command_specifier;
  uint8_t value_bytes;
  uint32_t index;
  if ((request == NULL) || (frame == NULL) ||
      (request->node_id < 1U) || (request->node_id > 127U) ||
      (request->timeout_ms < 10U) || (request->timeout_ms > 1000U))
  {
    return false;
  }

  switch (request->command)
  {
    case CAN_TX_SDO_READ:
      command_specifier = 0x40U;
      value_bytes = 0U;
      break;
    case CAN_TX_SDO_WRITE_U8:
      command_specifier = 0x2FU;
      value_bytes = 1U;
      if (request->value > 0xFFU) return false;
      break;
    case CAN_TX_SDO_WRITE_U16:
      command_specifier = 0x2BU;
      value_bytes = 2U;
      if (request->value > 0xFFFFU) return false;
      break;
    case CAN_TX_SDO_WRITE_U24:
      command_specifier = 0x27U;
      value_bytes = 3U;
      if (request->value > 0xFFFFFFU) return false;
      break;
    case CAN_TX_SDO_WRITE_U32:
      command_specifier = 0x23U;
      value_bytes = 4U;
      break;
    default:
      return false;
  }

  memset(frame, 0, sizeof(*frame));
  frame->id = 0x600U + request->node_id;
  frame->timeout_ms = request->timeout_ms;
  frame->dlc = 8U;
  frame->data[0] = command_specifier;
  frame->data[1] = (uint8_t)(request->index & 0xFFU);
  frame->data[2] = (uint8_t)(request->index >> 8U);
  frame->data[3] = request->subindex;
  for (index = 0U; index < value_bytes; ++index)
  {
    frame->data[4U + index] = (uint8_t)(request->value >> (index * 8U));
  }
  return true;
}

bool CanTx_DecodeSdoRequest(const CanTxFrame *frame, CanTxSdoRequest *request)
{
  uint32_t value;
  if ((frame == NULL) || (request == NULL) || frame->extended || frame->remote ||
      (frame->dlc != 8U) || (frame->id < 0x601U) || (frame->id > 0x67FU))
  {
    return false;
  }
  switch (frame->data[0])
  {
    case 0x40U: request->command = CAN_TX_SDO_READ; break;
    case 0x2FU: request->command = CAN_TX_SDO_WRITE_U8; break;
    case 0x2BU: request->command = CAN_TX_SDO_WRITE_U16; break;
    case 0x27U: request->command = CAN_TX_SDO_WRITE_U24; break;
    case 0x23U: request->command = CAN_TX_SDO_WRITE_U32; break;
    default: return false;
  }
  request->node_id = (uint8_t)(frame->id - 0x600U);
  request->index = (uint16_t)((uint16_t)frame->data[1] |
                              ((uint16_t)frame->data[2] << 8U));
  request->subindex = frame->data[3];
  request->timeout_ms = frame->timeout_ms;
  value = (uint32_t)frame->data[4] |
          ((uint32_t)frame->data[5] << 8U) |
          ((uint32_t)frame->data[6] << 16U) |
          ((uint32_t)frame->data[7] << 24U);
  request->value = value;
  return true;
}

bool CanTx_BuildNmt(CanTxNmtCommand command, uint8_t node_id,
                    CanTxFrame *frame)
{
  if ((frame == NULL) || (node_id > 127U)) return false;
  switch (command)
  {
    case CAN_TX_NMT_START:
    case CAN_TX_NMT_STOP:
    case CAN_TX_NMT_PRE_OPERATIONAL:
    case CAN_TX_NMT_RESET_NODE:
    case CAN_TX_NMT_RESET_COMMUNICATION:
      break;
    default:
      return false;
  }
  memset(frame, 0, sizeof(*frame));
  frame->id = 0U;
  frame->timeout_ms = CAN_TX_DEFAULT_TIMEOUT_MS;
  frame->dlc = 2U;
  frame->data[0] = (uint8_t)command;
  frame->data[1] = node_id;
  return true;
}

bool CanTx_DecodeNmt(const CanTxFrame *frame, CanTxNmtCommand *command,
                     uint8_t *node_id)
{
  CanTxNmtCommand decoded;
  if ((frame == NULL) || (command == NULL) || (node_id == NULL) ||
      frame->extended || frame->remote || (frame->id != 0U) ||
      (frame->dlc != 2U) || (frame->data[1] > 127U)) return false;
  decoded = (CanTxNmtCommand)frame->data[0];
  switch (decoded)
  {
    case CAN_TX_NMT_START:
    case CAN_TX_NMT_STOP:
    case CAN_TX_NMT_PRE_OPERATIONAL:
    case CAN_TX_NMT_RESET_NODE:
    case CAN_TX_NMT_RESET_COMMUNICATION:
      break;
    default:
      return false;
  }
  *command = decoded;
  *node_id = frame->data[1];
  return true;
}

CanTxSubmitResult CanTx_SubmitNmt(CanTxNmtCommand command, uint8_t node_id,
                                 uint32_t *sequence)
{
  CanTxFrame frame;
  if (!CanTx_BuildNmt(command, node_id, &frame)) return CAN_TX_SUBMIT_INVALID;
  return SubmitFrame(&frame, sequence, CAN_TX_OWNER_NORMAL);
}

CanTxSubmitResult CanTx_SubmitSdo(const CanTxSdoRequest *request,
                                 uint32_t *sequence)
{
  CanTxFrame frame;
  if (!CanTx_BuildSdoRequest(request, &frame)) return CAN_TX_SUBMIT_INVALID;
  return SubmitFrame(&frame, sequence, CAN_TX_OWNER_NORMAL);
}

CanTxSubmitResult CanTx_SubmitBridgeSdo(const CanTxSdoRequest *request,
                                       uint32_t *sequence)
{
  CanTxFrame frame;
  if (!CanTx_BuildSdoRequest(request, &frame)) return CAN_TX_SUBMIT_INVALID;
  return SubmitFrame(&frame, sequence, CAN_TX_OWNER_BRIDGE);
}

bool CanTx_SetBridgeExclusive(bool enabled)
{
  bool accepted = true;
  taskENTER_CRITICAL();
  if (enabled && ((tx_owner != CAN_TX_OWNER_NORMAL) ||
                  (pending_count != 0U) || periodic_state.enabled ||
                  public_status.transmitting || cancel_all_requested))
  {
    accepted = false;
  }
  else if (enabled)
  {
    tx_owner = CAN_TX_OWNER_BRIDGE;
  }
  else if (tx_owner == CAN_TX_OWNER_BRIDGE)
  {
    tx_owner = CAN_TX_OWNER_NORMAL;
  }
  else if (tx_owner != CAN_TX_OWNER_NORMAL)
  {
    accepted = false;
  }
  else
  {
    tx_owner = CAN_TX_OWNER_NORMAL;
  }
  taskEXIT_CRITICAL();
  return accepted;
}

CanTxSubmitResult CanTx_SubmitScannerSdo(const CanTxSdoRequest *request,
                                        uint32_t *sequence)
{
  CanTxFrame frame;
  if (!CanTx_BuildSdoRequest(request, &frame)) return CAN_TX_SUBMIT_INVALID;
  return SubmitFrame(&frame, sequence, CAN_TX_OWNER_SCANNER);
}

bool CanTx_SetScannerExclusive(bool enabled)
{
  bool accepted = true;
  taskENTER_CRITICAL();
  if (enabled)
  {
    if (tx_owner != CAN_TX_OWNER_NORMAL)
    {
      accepted = false;
    }
    else
    {
      /* Ownership changes before cancellation is observed, so no new normal
         or bridge request can enter while the old queue drains. */
      tx_owner = CAN_TX_OWNER_SCANNER;
      periodic_state.enabled = false;
      public_status.periodic_active = false;
      auto_silent_armed = false;
      cancel_all_requested = true;
    }
  }
  else if (tx_owner == CAN_TX_OWNER_SCANNER)
  {
    tx_owner = CAN_TX_OWNER_NORMAL;
  }
  else if (tx_owner != CAN_TX_OWNER_NORMAL)
  {
    accepted = false;
  }
  taskEXIT_CRITICAL();
  return accepted;
}

CanTxSubmitResult CanTx_StartPeriodicSdo(const CanTxSdoRequest *request,
                                         uint32_t period_ms,
                                         uint32_t *sequence)
{
  CanTxFrame frame;
  CanCaptureStatus capture;
  uint32_t assigned_sequence;
  if (!CanTx_BuildSdoRequest(request, &frame) ||
      (period_ms < CAN_TX_PERIOD_MIN_MS) ||
      (period_ms > CAN_TX_PERIOD_MAX_MS) ||
      (period_ms < request->timeout_ms))
  {
    return CAN_TX_SUBMIT_INVALID;
  }
  taskENTER_CRITICAL();
  if (tx_owner != CAN_TX_OWNER_NORMAL)
  {
    taskEXIT_CRITICAL();
    return CAN_TX_SUBMIT_MODE_DISABLED;
  }
  if (periodic_state.enabled)
  {
    taskEXIT_CRITICAL();
    return CAN_TX_SUBMIT_QUEUE_FULL;
  }
  if (cancel_all_requested)
  {
    taskEXIT_CRITICAL();
    return CAN_TX_SUBMIT_MODE_DISABLED;
  }
  taskEXIT_CRITICAL();
  CanControl_GetStatus(&capture);
  if ((capture.bitrate_kbps == 0U) || (capture.state == CAN_SYNC_SEARCHING))
    return CAN_TX_SUBMIT_MODE_DISABLED;

  taskENTER_CRITICAL();
  assigned_sequence = next_sequence++;
  if (next_sequence == 0U) next_sequence = 1U;
  periodic_state.frame = frame;
  periodic_state.sequence = assigned_sequence;
  periodic_state.period_ms = period_ms;
  periodic_state.next_due_tick = HAL_GetTick();
  periodic_state.enabled = true;
  memset(&periodic_history, 0, sizeof(periodic_history));
  periodic_history.sequence = assigned_sequence;
  periodic_history.frame = frame;
  periodic_history.periodic = true;
  periodic_history_valid = false;
  public_status.periodic_active = true;
  public_status.periodic_sequence = assigned_sequence;
  public_status.periodic_period_ms = period_ms;
  public_status.periodic_attempts = 0U;
  auto_silent_armed = false;
  taskEXIT_CRITICAL();
  if (sequence != NULL) *sequence = assigned_sequence;
  return CAN_TX_SUBMIT_OK;
}

bool CanTx_StopPeriodic(void)
{
  bool was_enabled;
  taskENTER_CRITICAL();
  was_enabled = periodic_state.enabled;
  periodic_state.enabled = false;
  public_status.periodic_active = false;
  if (was_enabled && !CanControl_IsActiveLatched())
  {
    auto_silent_deadline = HAL_GetTick() + periodic_state.frame.timeout_ms;
    auto_silent_armed = true;
  }
  taskEXIT_CRITICAL();
  return was_enabled;
}

bool CanTx_Cancel(uint32_t sequence)
{
  bool accepted = false;
  taskENTER_CRITICAL();
  if (IsPendingLocked(sequence) && (cancelled_count < CAN_TX_QUEUE_CAPACITY))
  {
    uint32_t index;
    accepted = true;
    for (index = 0U; index < cancelled_count; ++index)
    {
      if (cancelled_sequences[index] == sequence)
      {
        taskEXIT_CRITICAL();
        return true;
      }
    }
    cancelled_sequences[cancelled_count++] = sequence;
  }
  taskEXIT_CRITICAL();
  return accepted;
}

void CanTx_CancelAll(void)
{
  cancel_all_requested = true;
}

void CanTx_ObserveRxFrame(const CanFrameRecord *frame)
{
  CanTxResponseCorrelation *match = NULL;
  uint32_t node_id;
  uint16_t object_index;
  uint8_t subindex;
  uint32_t index;
  if ((frame == NULL) || ((frame->flags & CAN_FRAME_FLAG_LOCAL_TX) != 0U) ||
      ((frame->flags & (CAN_FRAME_FLAG_EXTENDED | CAN_FRAME_FLAG_REMOTE)) != 0U) ||
      (frame->id < 0x581U) || (frame->id > 0x5FFU) || (frame->dlc < 4U))
    return;

  node_id = frame->id - 0x580U;
  object_index = (uint16_t)frame->data[1] | ((uint16_t)frame->data[2] << 8U);
  subindex = frame->data[3];
  taskENTER_CRITICAL();
  for (index = 0U; index < CAN_TX_QUEUE_CAPACITY; ++index)
  {
    CanTxResponseCorrelation *candidate = &response_correlations[index];
    uint32_t elapsed;
    if (!candidate->used || !candidate->waiting ||
        (candidate->request.node_id != node_id) ||
        (candidate->request.index != object_index) ||
        (candidate->request.subindex != subindex))
      continue;
    elapsed = frame->timestamp_us - candidate->request_timestamp_us;
    if (elapsed > ((uint32_t)candidate->request.timeout_ms * 1000U)) continue;
    if ((match == NULL) ||
        ((int32_t)(candidate->request_timestamp_us -
                   match->request_timestamp_us) > 0))
      match = candidate;
  }
  if (match != NULL)
  {
    match->response = *frame;
    match->response_time_us = frame->timestamp_us - match->request_timestamp_us;
    match->has_response = true;
    match->waiting = false;
    ApplyResponseLocked(match->sequence, match);
  }
  taskEXIT_CRITICAL();
}

void CanTx_GetStatus(CanTxStatus *status)
{
  if (status == NULL) return;
  taskENTER_CRITICAL();
  *status = public_status;
  /* pending_count also includes the entry currently waiting for ACTIVE/rate
     limit or a hardware mailbox. This prevents a bridge session from being
     enabled in the short gap after xQueueReceive() and before transmission. */
  status->queue_depth = pending_count;
  taskEXIT_CRITICAL();
}

void CanTx_ResetStatistics(void)
{
  taskENTER_CRITICAL();
  public_status.queued = 0U;
  public_status.sent = 0U;
  public_status.cancelled = 0U;
  public_status.timeouts = 0U;
  public_status.arbitration_lost = 0U;
  public_status.bus_off = 0U;
  public_status.errors = 0U;
  public_status.last_sequence = 0U;
  public_status.last_result = CAN_TX_RESULT_MODE_DISABLED;
  public_status.periodic_attempts = 0U;
  history_write = 0U;
  history_count = 0U;
  memset(history, 0, sizeof(history));
  memset(&periodic_history, 0, sizeof(periodic_history));
  periodic_history_valid = false;
  memset(response_correlations, 0, sizeof(response_correlations));
  taskEXIT_CRITICAL();
}

size_t CanTx_CopyHistory(CanTxHistoryEntry *entries, size_t capacity)
{
  size_t count;
  size_t index;
  if ((entries == NULL) || (capacity == 0U)) return 0U;
  taskENTER_CRITICAL();
  count = history_count;
  if (periodic_history_valid && (count >= capacity)) count = capacity - 1U;
  else if (count > capacity) count = capacity;
  for (index = 0U; index < count; ++index)
  {
    uint32_t source = (history_write - 1U - (uint32_t)index) %
                      CAN_TX_HISTORY_CAPACITY;
    entries[index] = history[source];
  }
  if (periodic_history_valid)
  {
    size_t position = 0U;
    size_t new_count = count < capacity ? count + 1U : count;
    while ((position < count) &&
           (entries[position].timestamp_us >= periodic_history.timestamp_us))
      ++position;
    for (index = new_count - 1U; index > position; --index)
      entries[index] = entries[index - 1U];
    entries[position] = periodic_history;
    count = new_count;
  }
  taskEXIT_CRITICAL();
  return count;
}

const char *CanTx_ResultName(CanTxResult result)
{
  switch (result)
  {
    case CAN_TX_RESULT_SENT: return "sent";
    case CAN_TX_RESULT_CANCELLED: return "cancelled";
    case CAN_TX_RESULT_TIMEOUT: return "timeout";
    case CAN_TX_RESULT_ARBITRATION_LOST: return "arbitration_lost";
    case CAN_TX_RESULT_BUS_OFF: return "bus_off";
    case CAN_TX_RESULT_MODE_DISABLED: return "mode_disabled";
    default: return "driver_error";
  }
}

const char *CanTx_SdoCommandName(CanTxSdoCommand command)
{
  switch (command)
  {
    case CAN_TX_SDO_READ: return "read";
    case CAN_TX_SDO_WRITE_U8: return "write_u8";
    case CAN_TX_SDO_WRITE_U16: return "write_u16";
    case CAN_TX_SDO_WRITE_U24: return "write_u24";
    case CAN_TX_SDO_WRITE_U32: return "write_u32";
    default: return "invalid";
  }
}

const char *CanTx_NmtCommandName(CanTxNmtCommand command)
{
  switch (command)
  {
    case CAN_TX_NMT_START: return "start";
    case CAN_TX_NMT_STOP: return "stop";
    case CAN_TX_NMT_PRE_OPERATIONAL: return "pre_operational";
    case CAN_TX_NMT_RESET_NODE: return "reset_node";
    case CAN_TX_NMT_RESET_COMMUNICATION: return "reset_communication";
    default: return "invalid";
  }
}

static bool PeriodicStillEnabled(uint32_t sequence)
{
  bool enabled;
  taskENTER_CRITICAL();
  enabled = periodic_state.enabled && (periodic_state.sequence == sequence);
  taskEXIT_CRITICAL();
  return enabled;
}

static bool EnsureActive(const CanTxQueueEntry *entry)
{
  uint32_t started;
  if (CanControl_IsActive()) return true;
  if (!CanControl_RequestActiveTransient())
  {
    CanCaptureStatus capture;
    CanControl_GetStatus(&capture);
    if (capture.operating_mode != CAN_OPERATING_ACTIVE_PENDING) return false;
  }
  started = HAL_GetTick();
  while (!CanControl_IsActive())
  {
    if (cancel_all_requested ||
        (entry->periodic && !PeriodicStillEnabled(entry->sequence)))
      return false;
    if ((uint32_t)(HAL_GetTick() - started) >= 1000U) return false;
    vTaskDelay(pdMS_TO_TICKS(2U));
  }
  return true;
}

static uint32_t ProcessEntry(const CanTxQueueEntry *entry, uint32_t last_attempt)
{
  CanTxResult result;
  if (cancel_all_requested ||
      (entry->periodic && !PeriodicStillEnabled(entry->sequence)))
  {
    RecordResult(entry, CAN_TX_RESULT_CANCELLED);
    return last_attempt;
  }
  if (!EnsureActive(entry))
  {
    RecordResult(entry, CAN_TX_RESULT_MODE_DISABLED);
    if (entry->periodic) (void)CanTx_StopPeriodic();
    return last_attempt;
  }
  while ((entry->owner != CAN_TX_OWNER_BRIDGE) &&
         ((uint32_t)(HAL_GetTick() - last_attempt) < CAN_TX_MIN_INTERVAL_MS))
  {
    if (cancel_all_requested ||
        (entry->periodic && !PeriodicStillEnabled(entry->sequence)))
    {
      RecordResult(entry, CAN_TX_RESULT_CANCELLED);
      return last_attempt;
    }
    vTaskDelay(pdMS_TO_TICKS(1U));
  }
  last_attempt = HAL_GetTick();
  result = Transmit(entry);
  RecordResult(entry, result);
  if (!entry->periodic)
  {
    taskENTER_CRITICAL();
    /* Scanner and bridge sessions own TX for their whole lifetime. Returning
       to silent after every one-shot request would repeatedly reconfigure the
       controller between requests. Normal one-shot requests keep the original
       fail-safe auto-silent behaviour. */
    if ((tx_owner != CAN_TX_OWNER_SCANNER) &&
        (entry->owner != CAN_TX_OWNER_BRIDGE) &&
        !CanControl_IsActiveLatched())
    {
      auto_silent_deadline = HAL_GetTick() + entry->frame.timeout_ms;
      auto_silent_armed = true;
    }
    taskEXIT_CRITICAL();
  }
  return last_attempt;
}

void CanTxManager_Task(void)
{
  CanTxQueueEntry entry;
  uint32_t last_attempt = 0U;
  for (;;)
  {
    AppWatchdog_Heartbeat(APP_WATCHDOG_CAN_TX);
    if (xQueueReceive(tx_queue, &entry, pdMS_TO_TICKS(10U)) == pdPASS)
    {
      last_attempt = ProcessEntry(&entry, last_attempt);
    }

    if (cancel_all_requested)
    {
      while (xQueueReceive(tx_queue, &entry, 0U) == pdPASS)
      {
        RecordResult(&entry, CAN_TX_RESULT_CANCELLED);
      }
      taskENTER_CRITICAL();
      periodic_state.enabled = false;
      public_status.periodic_active = false;
      auto_silent_armed = false;
      cancelled_count = 0U;
      cancel_all_requested = false;
      taskEXIT_CRITICAL();
    }

    taskENTER_CRITICAL();
    if (periodic_state.enabled &&
        ((int32_t)(HAL_GetTick() - periodic_state.next_due_tick) >= 0))
    {
      entry.sequence = periodic_state.sequence;
      entry.frame = periodic_state.frame;
      entry.owner = CAN_TX_OWNER_NORMAL;
      entry.periodic = true;
      periodic_state.next_due_tick = HAL_GetTick() + periodic_state.period_ms;
      ++public_status.queued;
      taskEXIT_CRITICAL();
      last_attempt = ProcessEntry(&entry, last_attempt);
    }
    else
    {
      taskEXIT_CRITICAL();
    }

    taskENTER_CRITICAL();
    if (auto_silent_armed && CanControl_IsActiveLatched())
    {
      auto_silent_armed = false;
      taskEXIT_CRITICAL();
    }
    else if (auto_silent_armed && !periodic_state.enabled &&
        (uxQueueMessagesWaiting(tx_queue) == 0U) &&
        ((int32_t)(HAL_GetTick() - auto_silent_deadline) >= 0))
    {
      auto_silent_armed = false;
      taskEXIT_CRITICAL();
      CanControl_RequestSilent();
    }
    else
    {
      taskEXIT_CRITICAL();
    }
  }
}
