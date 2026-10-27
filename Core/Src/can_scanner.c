#include "can_scanner.h"

#include "FreeRTOS.h"
#include "app_settings.h"
#include "can_bridge.h"
#include "can_tx_manager.h"
#include "main.h"
#include "task.h"

#include <string.h>

static const uint32_t scanner_bitrates[] =
{
  1000U, 800U, 500U, 250U, 125U, 100U, 50U, 20U, 10U
};

typedef struct
{
  CanScannerStatus status;
  CanScannerResult results[CAN_SCANNER_RESULT_MAX];
  uint32_t bitrate_index;
  uint32_t tx_sequence;
  uint32_t response_deadline;
  uint32_t last_progress_tick;
  bool request_pending;
  bool request_sent;
  bool response_received;
} CanScannerState;

static CanScannerState scanner;

static uint32_t LoadLe32(const uint8_t *data)
{
  return (uint32_t)data[0] |
         ((uint32_t)data[1] << 8U) |
         ((uint32_t)data[2] << 16U) |
         ((uint32_t)data[3] << 24U);
}

static bool IsScannerRequest(const CanFrameRecord *frame)
{
  return ((frame->flags & CAN_FRAME_FLAG_LOCAL_TX) != 0U) &&
         ((frame->flags & (CAN_FRAME_FLAG_EXTENDED | CAN_FRAME_FLAG_REMOTE)) == 0U) &&
         (frame->id == (0x600U + scanner.status.current_node_id)) &&
         (frame->dlc == 8U) && (frame->data[0] == 0x40U) &&
         (frame->data[1] == 0x00U) && (frame->data[2] == 0x10U) &&
         (frame->data[3] == 0x00U);
}

static bool IsScannerResponse(const CanFrameRecord *frame)
{
  return ((frame->flags & (CAN_FRAME_FLAG_EXTENDED | CAN_FRAME_FLAG_REMOTE |
                           CAN_FRAME_FLAG_LOCAL_TX)) == 0U) &&
         (frame->id == (0x580U + scanner.status.current_node_id)) &&
         (frame->dlc == 8U) &&
         (frame->data[1] == 0x00U) && (frame->data[2] == 0x10U) &&
         (frame->data[3] == 0x00U);
}

static void RestoreConfiguredBitrate(CanScannerPhase final_phase)
{
  DeviceSettings settings;
  CanControl_ForceSilent(CAN_SAFETY_USER);
  (void)CanTx_SetScannerExclusive(false);
  AppSettings_Get(&settings);
  CanControl_RequestBitrate(settings.can_bitrate_kbps);
  taskENTER_CRITICAL();
  scanner.status.phase = final_phase;
  scanner.status.running = false;
  scanner.status.current_node_id = 0U;
  scanner.request_pending = false;
  scanner.request_sent = false;
  scanner.response_received = false;
  taskEXIT_CRITICAL();
}

static void SelectBitrate(uint32_t bitrate_kbps, bool reset_node)
{
  CanControl_ForceSilent(CAN_SAFETY_BITRATE_CHANGE);
  CanControl_RequestBitrate(bitrate_kbps);
  taskENTER_CRITICAL();
  scanner.status.current_bitrate_kbps = bitrate_kbps;
  if (reset_node) scanner.status.current_node_id = CAN_SCANNER_NODE_MIN;
  scanner.status.phase = CAN_SCANNER_SELECTING_BITRATE;
  scanner.request_pending = false;
  scanner.request_sent = false;
  scanner.response_received = false;
  scanner.last_progress_tick = HAL_GetTick();
  taskEXIT_CRITICAL();
}

static void AdvanceRequest(void)
{
  bool finished_rate = false;
  uint32_t next_bitrate = 0U;

  taskENTER_CRITICAL();
  ++scanner.status.completed_requests;
  scanner.request_pending = false;
  scanner.request_sent = false;
  scanner.response_received = false;
  if (scanner.status.current_node_id < CAN_SCANNER_NODE_MAX)
  {
    ++scanner.status.current_node_id;
    scanner.last_progress_tick = HAL_GetTick();
  }
  else
  {
    finished_rate = true;
    if (scanner.status.auto_bitrate &&
        ((scanner.bitrate_index + 1U) <
         (sizeof(scanner_bitrates) / sizeof(scanner_bitrates[0]))))
    {
      ++scanner.bitrate_index;
      next_bitrate = scanner_bitrates[scanner.bitrate_index];
    }
  }
  taskEXIT_CRITICAL();

  if (finished_rate)
  {
    if (next_bitrate != 0U) SelectBitrate(next_bitrate, true);
    else RestoreConfiguredBitrate(CAN_SCANNER_COMPLETE);
  }
}

void CanScanner_Init(void)
{
  memset(&scanner, 0, sizeof(scanner));
  scanner.status.phase = CAN_SCANNER_IDLE;
}

bool CanScanner_Start(void)
{
  DeviceSettings settings;

  taskENTER_CRITICAL();
  if (scanner.status.running)
  {
    taskEXIT_CRITICAL();
    return false;
  }
  taskEXIT_CRITICAL();

  /* Scanning is an exclusive transmitter. Drop the bridge lease first, then
     atomically block normal/bridge submissions and cancel their queued work. */
  CanBridge_Disable();
  if (!CanTx_SetScannerExclusive(true)) return false;
  CanControl_ForceSilent(CAN_SAFETY_USER);
  AppSettings_Get(&settings);

  taskENTER_CRITICAL();
  memset(scanner.results, 0, sizeof(scanner.results));
  scanner.status.phase = CAN_SCANNER_PREPARING;
  scanner.status.configured_bitrate_kbps = settings.can_bitrate_kbps;
  scanner.status.current_bitrate_kbps = settings.can_bitrate_kbps != 0U ?
    settings.can_bitrate_kbps : scanner_bitrates[0];
  scanner.status.completed_requests = 0U;
  scanner.status.total_requests = CAN_SCANNER_NODE_MAX *
    (settings.can_bitrate_kbps == 0U ?
      (uint32_t)(sizeof(scanner_bitrates) / sizeof(scanner_bitrates[0])) : 1U);
  scanner.status.result_count = 0U;
  scanner.status.current_node_id = CAN_SCANNER_NODE_MIN;
  scanner.status.running = true;
  scanner.status.auto_bitrate = settings.can_bitrate_kbps == 0U;
  scanner.bitrate_index = 0U;
  scanner.tx_sequence = 0U;
  scanner.request_pending = false;
  scanner.request_sent = false;
  scanner.response_received = false;
  scanner.last_progress_tick = HAL_GetTick();
  taskEXIT_CRITICAL();
  return true;
}

void CanScanner_Stop(void)
{
  bool running;
  taskENTER_CRITICAL();
  running = scanner.status.running;
  taskEXIT_CRITICAL();
  if (running) RestoreConfiguredBitrate(CAN_SCANNER_CANCELLED);
}

void CanScanner_Update(void)
{
  CanScannerStatus status;
  CanCaptureStatus capture;
  CanTxStatus tx;
  uint32_t now = HAL_GetTick();

  CanScanner_GetStatus(&status);
  if (!status.running) return;
  CanControl_GetStatus(&capture);
  CanTx_GetStatus(&tx);

  if (status.phase == CAN_SCANNER_PREPARING)
  {
    if (!tx.transmitting && (tx.queue_depth == 0U) &&
        (capture.operating_mode == CAN_OPERATING_SILENT))
      SelectBitrate(status.current_bitrate_kbps, true);
    return;
  }

  if (status.phase == CAN_SCANNER_SELECTING_BITRATE)
  {
    if ((capture.bitrate_kbps == status.current_bitrate_kbps) &&
        (capture.state == CAN_SYNC_MANUAL) &&
        (capture.operating_mode == CAN_OPERATING_SILENT))
    {
      if (CanControl_RequestActiveTransient())
      {
        taskENTER_CRITICAL();
        scanner.status.phase = CAN_SCANNER_ENABLING_TX;
        scanner.last_progress_tick = now;
        taskEXIT_CRITICAL();
      }
    }
    else if ((uint32_t)(now - scanner.last_progress_tick) >= 3000U)
    {
      RestoreConfiguredBitrate(CAN_SCANNER_ERROR);
    }
    return;
  }

  if (status.phase == CAN_SCANNER_ENABLING_TX)
  {
    if (capture.operating_mode == CAN_OPERATING_ACTIVE)
    {
      taskENTER_CRITICAL();
      scanner.status.phase = CAN_SCANNER_SCANNING;
      scanner.last_progress_tick = now;
      taskEXIT_CRITICAL();
    }
    else if ((uint32_t)(now - scanner.last_progress_tick) >= 3000U)
    {
      RestoreConfiguredBitrate(CAN_SCANNER_ERROR);
    }
    return;
  }

  if (status.phase != CAN_SCANNER_SCANNING) return;
  if (capture.safety_reason == CAN_SAFETY_BUS_OFF)
  {
    CanScannerStatus advanced;
    uint32_t failed_bitrate = status.current_bitrate_kbps;
    /* Count this node as attempted, reset the controller and continue at the
       next Node ID. Thus even a wrong auto-scan bitrate covers all 1..127. */
    AdvanceRequest();
    CanScanner_GetStatus(&advanced);
    if (advanced.running &&
        (advanced.phase == CAN_SCANNER_SCANNING) &&
        (advanced.current_bitrate_kbps == failed_bitrate))
      SelectBitrate(failed_bitrate, false);
    return;
  }
  if ((capture.bitrate_kbps != status.current_bitrate_kbps) ||
      (capture.operating_mode != CAN_OPERATING_ACTIVE))
  {
    RestoreConfiguredBitrate(CAN_SCANNER_ERROR);
    return;
  }

  taskENTER_CRITICAL();
  status = scanner.status;
  taskEXIT_CRITICAL();
  if (!scanner.request_pending)
  {
    CanTxSdoRequest request;
    CanTxSubmitResult submitted;
    uint32_t sequence = 0U;
    memset(&request, 0, sizeof(request));
    request.node_id = status.current_node_id;
    request.index = CAN_SCANNER_OBJECT_INDEX;
    request.subindex = 0U;
    request.command = CAN_TX_SDO_READ;
    request.timeout_ms = CAN_SCANNER_TIMEOUT_MS;
    /* Arm response matching before waking the higher-priority TX task. */
    taskENTER_CRITICAL();
    scanner.request_pending = true;
    scanner.request_sent = false;
    scanner.response_received = false;
    scanner.last_progress_tick = now;
    taskEXIT_CRITICAL();
    submitted = CanTx_SubmitScannerSdo(&request, &sequence);
    if (submitted == CAN_TX_SUBMIT_OK)
    {
      taskENTER_CRITICAL();
      scanner.tx_sequence = sequence;
      taskEXIT_CRITICAL();
    }
    else
    {
      taskENTER_CRITICAL();
      scanner.request_pending = false;
      taskEXIT_CRITICAL();
    }
    return;
  }

  if (scanner.response_received)
  {
    AdvanceRequest();
    return;
  }
  if (scanner.request_sent &&
      ((int32_t)(now - scanner.response_deadline) >= 0))
  {
    AdvanceRequest();
    return;
  }
  if ((tx.last_sequence == scanner.tx_sequence) &&
      (tx.last_result != CAN_TX_RESULT_SENT) && !tx.transmitting)
  {
    AdvanceRequest();
  }
}

void CanScanner_ProcessFrame(const CanFrameRecord *frame)
{
  CanScannerResult *result;
  uint8_t command;
  uint8_t unused;
  uint8_t value_bytes;
  uint32_t value = 0U;

  if (frame == NULL) return;
  taskENTER_CRITICAL();
  if (!scanner.status.running ||
      (scanner.status.phase != CAN_SCANNER_SCANNING) ||
      !scanner.request_pending)
  {
    taskEXIT_CRITICAL();
    return;
  }
  if (IsScannerRequest(frame))
  {
    scanner.request_sent = true;
    scanner.response_deadline = HAL_GetTick() + CAN_SCANNER_TIMEOUT_MS;
    taskEXIT_CRITICAL();
    return;
  }
  if (!scanner.request_sent || !IsScannerResponse(frame) ||
      scanner.response_received)
  {
    taskEXIT_CRITICAL();
    return;
  }

  if (scanner.status.result_count >= CAN_SCANNER_RESULT_MAX)
  {
    scanner.response_received = true;
    taskEXIT_CRITICAL();
    return;
  }
  result = &scanner.results[scanner.status.result_count++];
  memset(result, 0, sizeof(*result));
  result->bitrate_kbps = scanner.status.current_bitrate_kbps;
  result->node_id = scanner.status.current_node_id;
  result->command = frame->data[0];
  memcpy(result->data, frame->data, sizeof(result->data));
  command = frame->data[0];
  if (command == 0x80U)
  {
    result->kind = CAN_SCANNER_RESPONSE_ABORT;
    result->abort_code = LoadLe32(frame->data + 4U);
  }
  else if (((command >> 5U) == 2U) && ((command & 0x02U) != 0U))
  {
    unused = (command & 0x01U) != 0U ? (uint8_t)((command >> 2U) & 0x03U) : 0U;
    value_bytes = (uint8_t)(4U - unused);
    if (value_bytes >= 1U) value |= (uint32_t)frame->data[4];
    if (value_bytes >= 2U) value |= (uint32_t)frame->data[5] << 8U;
    if (value_bytes >= 3U) value |= (uint32_t)frame->data[6] << 16U;
    if (value_bytes >= 4U) value |= (uint32_t)frame->data[7] << 24U;
    result->kind = CAN_SCANNER_RESPONSE_DEVICE_TYPE;
    result->value = value;
  }
  else
  {
    result->kind = CAN_SCANNER_RESPONSE_OTHER;
  }
  scanner.response_received = true;
  taskEXIT_CRITICAL();
}

void CanScanner_GetStatus(CanScannerStatus *status)
{
  if (status == NULL) return;
  taskENTER_CRITICAL();
  *status = scanner.status;
  taskEXIT_CRITICAL();
}

size_t CanScanner_CopyResults(CanScannerResult *results, size_t capacity)
{
  size_t count;
  if ((results == NULL) || (capacity == 0U)) return 0U;
  taskENTER_CRITICAL();
  count = scanner.status.result_count;
  if (count > capacity) count = capacity;
  memcpy(results, scanner.results, count * sizeof(*results));
  taskEXIT_CRITICAL();
  return count;
}

const char *CanScanner_PhaseName(CanScannerPhase phase)
{
  switch (phase)
  {
    case CAN_SCANNER_PREPARING: return "stopping other TX";
    case CAN_SCANNER_SELECTING_BITRATE: return "selecting bitrate";
    case CAN_SCANNER_ENABLING_TX: return "enabling CAN TX";
    case CAN_SCANNER_SCANNING: return "scanning";
    case CAN_SCANNER_COMPLETE: return "complete";
    case CAN_SCANNER_CANCELLED: return "cancelled";
    case CAN_SCANNER_ERROR: return "CAN error";
    default: return "ready";
  }
}

const char *CanScanner_DeviceHint(uint32_t device_type)
{
  switch ((uint16_t)(device_type & 0xFFFFU))
  {
    case 401U: return "CiA 401 I/O";
    case 402U: return "CiA 402 drive";
    case 403U: return "CiA 403 HMI";
    case 404U: return "CiA 404 measuring";
    case 405U: return "CiA 405 PLC";
    case 406U: return "CiA 406 encoder";
    case 408U: return "CiA 408 fluid power";
    case 410U: return "CiA 410 inclinometer";
    case 412U: return "CiA 412 medical";
    case 413U: return "CiA 413 truck gateway";
    case 414U: return "CiA 414 weaving";
    case 415U: return "CiA 415 sensor";
    case 416U: return "CiA 416 door control";
    case 417U: return "CiA 417 lift";
    case 418U: return "CiA 418 battery";
    case 419U: return "CiA 419 charger";
    case 420U: return "CiA 420 extruder";
    case 422U: return "CiA 422 municipal";
    case 424U: return "CiA 424 rail";
    case 425U: return "CiA 425 medical";
    default: return "";
  }
}
