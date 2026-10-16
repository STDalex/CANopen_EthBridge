#include "canopen_analyzer.h"

#include "FreeRTOS.h"
#include "task.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define CANOPEN_EVENT_HISTORY_LENGTH  512U
#define CANOPEN_EVENT_HISTORY_MASK    (CANOPEN_EVENT_HISTORY_LENGTH - 1U)
#define CANOPEN_SDO_OBJECT_POOL_LENGTH 256U
#define CANOPEN_SDO_OBJECT_POOL_MASK   (CANOPEN_SDO_OBJECT_POOL_LENGTH - 1U)

typedef struct
{
  bool present;
  CanopenNodeSnapshot snapshot;
} CanopenNodeState;

typedef struct
{
  uint8_t node_id;
  CanopenMessageType type;
  CanFrameRecord frame;
  uint32_t response_time_us;
  bool has_response_time;
} CanopenEventState;

typedef struct
{
  bool used;
  uint8_t node_id;
  uint16_t index;
  uint8_t subindex;
  CanopenMessageType direction;
  uint32_t access_count;
  CanFrameRecord frame;
  uint32_t response_time_us;
  bool has_response_time;
  bool pending_request;
} CanopenSdoObjectState;

static CanopenNodeState node_states[CANOPEN_NODE_COUNT_MAX];
static CanopenEventState event_history[CANOPEN_EVENT_HISTORY_LENGTH];
static CanopenSdoObjectState sdo_objects[CANOPEN_SDO_OBJECT_POOL_LENGTH];
static uint32_t event_write;
static uint32_t event_count;
static uint32_t queue_overflows;
static CanopenGlobalSnapshot global_snapshot;

static bool Update_SdoObject(const CanFrameRecord *frame, const CanopenDecodedFrame *decoded,
                             const CanopenSdoAddress *address, uint32_t *response_time_us)
{
  uint32_t hash;
  uint32_t probe;
  CanopenSdoObjectState *current = NULL;

  if (!address->valid || (response_time_us == NULL))
  {
    return false;
  }

  hash = (((uint32_t)decoded->node_id * 131U) + ((uint32_t)address->index * 17U) +
          address->subindex + ((uint32_t)decoded->type * 7U)) & CANOPEN_SDO_OBJECT_POOL_MASK;
  for (probe = 0U; probe < CANOPEN_SDO_OBJECT_POOL_LENGTH; ++probe)
  {
    CanopenSdoObjectState *object = &sdo_objects[(hash + probe) & CANOPEN_SDO_OBJECT_POOL_MASK];
    if (!object->used)
    {
      object->used = true;
      object->node_id = decoded->node_id;
      object->index = address->index;
      object->subindex = address->subindex;
      object->direction = decoded->type;
      current = object;
      break;
    }
    if ((object->node_id == decoded->node_id) &&
        (object->index == address->index) &&
        (object->subindex == address->subindex) &&
        (object->direction == decoded->type))
    {
      current = object;
      break;
    }
  }

  if (current == NULL)
  {
    return false;
  }

  ++current->access_count;
  current->frame = *frame;
  current->has_response_time = false;
  if (decoded->type == CANOPEN_MESSAGE_SDO_RX)
  {
    current->pending_request = true;
    return false;
  }

  if (decoded->type == CANOPEN_MESSAGE_SDO_TX)
  {
    for (probe = 0U; probe < CANOPEN_SDO_OBJECT_POOL_LENGTH; ++probe)
    {
      CanopenSdoObjectState *request = &sdo_objects[probe];
      if (request->used && request->pending_request &&
          (request->node_id == decoded->node_id) &&
          (request->index == address->index) &&
          (request->subindex == address->subindex) &&
          (request->direction == CANOPEN_MESSAGE_SDO_RX))
      {
        *response_time_us = frame->timestamp_us - request->frame.timestamp_us;
        current->response_time_us = *response_time_us;
        current->has_response_time = true;
        request->pending_request = false;
        return true;
      }
    }
  }
  return false;
}

void CanopenAnalyzer_Init(void)
{
  memset(node_states, 0, sizeof(node_states));
  memset(event_history, 0, sizeof(event_history));
  memset(sdo_objects, 0, sizeof(sdo_objects));
  event_write = 0U;
  event_count = 0U;
  queue_overflows = 0U;
  memset(&global_snapshot, 0, sizeof(global_snapshot));
}

void CanopenAnalyzer_ProcessFrame(const CanFrameRecord *frame)
{
  CanopenDecodedFrame decoded;
  CanopenSdoAddress sdo_address;
  CanopenNodeState *node;
  CanopenEventState *event;
  uint32_t response_time_us = 0U;
  bool has_response_time;

  if (frame == NULL)
  {
    return;
  }

  decoded = CanopenDecoder_Decode(frame);
  sdo_address = CanopenDecoder_DecodeSdoAddress(frame, decoded.type);
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
  if (decoded.type == CANOPEN_MESSAGE_SDO_RX)
  {
    ++node->snapshot.sdo_request_count;
  }
  else if (decoded.type == CANOPEN_MESSAGE_SDO_TX)
  {
    ++node->snapshot.sdo_response_count;
  }
  has_response_time = Update_SdoObject(frame, &decoded, &sdo_address, &response_time_us);

  event = &event_history[event_write & CANOPEN_EVENT_HISTORY_MASK];
  event->node_id = decoded.node_id;
  event->type = decoded.type;
  event->frame = *frame;
  event->response_time_us = response_time_us;
  event->has_response_time = has_response_time;
  ++event_write;
  if (event_count < CANOPEN_EVENT_HISTORY_LENGTH)
  {
    ++event_count;
  }
  taskEXIT_CRITICAL();
}

void CanopenAnalyzer_AddQueueOverflow(void)
{
  taskENTER_CRITICAL();
  ++queue_overflows;
  taskEXIT_CRITICAL();
}

uint32_t CanopenAnalyzer_GetNodesSnapshot(CanopenNodeSnapshot *nodes, uint32_t capacity,
                                          CanopenGlobalSnapshot *global)
{
  uint32_t node_index;
  uint32_t copied = 0U;

  if ((nodes == NULL) || (capacity == 0U))
  {
    return 0U;
  }

  taskENTER_CRITICAL();
  if (global != NULL)
  {
    *global = global_snapshot;
  }
  for (node_index = 0U;
       (node_index < CANOPEN_NODE_COUNT_MAX) && (copied < capacity);
       ++node_index)
  {
    if (node_states[node_index].present)
    {
      nodes[copied] = node_states[node_index].snapshot;
      ++copied;
    }
  }
  taskEXIT_CRITICAL();
  return copied;
}

uint32_t CanopenAnalyzer_GetNodeEventsSnapshot(uint8_t node_id,
                                               CanopenNodeEventSnapshot *events,
                                               uint32_t capacity)
{
  uint32_t age;
  uint32_t copied = 0U;

  if ((node_id < 1U) || (node_id > 127U) || (events == NULL) || (capacity == 0U))
  {
    return 0U;
  }

  taskENTER_CRITICAL();
  for (age = 0U; (age < event_count) && (copied < capacity); ++age)
  {
    const CanopenEventState *event =
      &event_history[(event_write - 1U - age) & CANOPEN_EVENT_HISTORY_MASK];
    if (event->node_id == node_id)
    {
      events[copied].frame = event->frame;
      events[copied].type = event->type;
      events[copied].response_time_us = event->response_time_us;
      events[copied].has_response_time = event->has_response_time;
      ++copied;
    }
  }
  taskEXIT_CRITICAL();
  return copied;
}

uint32_t CanopenAnalyzer_GetNodeObjectsSnapshot(uint8_t node_id,
                                                CanopenNodeEventSnapshot *events,
                                                uint32_t capacity)
{
  uint32_t object_index;
  uint32_t copied = 0U;

  if ((node_id < 1U) || (node_id > 127U) || (events == NULL) || (capacity == 0U))
  {
    return 0U;
  }

  taskENTER_CRITICAL();
  for (object_index = 0U;
       (object_index < CANOPEN_SDO_OBJECT_POOL_LENGTH) && (copied < capacity);
       ++object_index)
  {
    const CanopenSdoObjectState *object = &sdo_objects[object_index];
    if (object->used && (object->node_id == node_id))
    {
      events[copied].frame = object->frame;
      events[copied].type = object->direction;
      events[copied].response_time_us = object->response_time_us;
      events[copied].has_response_time = object->has_response_time;
      ++copied;
    }
  }
  taskEXIT_CRITICAL();
  return copied;
}

void CanopenAnalyzer_GetGlobalSnapshot(CanopenGlobalSnapshot *global)
{
  if (global == NULL)
  {
    return;
  }
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
  memset(node_states, 0, sizeof(node_states));
  memset(event_history, 0, sizeof(event_history));
  memset(sdo_objects, 0, sizeof(sdo_objects));
  event_write = 0U;
  event_count = 0U;
  queue_overflows = 0U;
  memset(&global_snapshot, 0, sizeof(global_snapshot));
  taskEXIT_CRITICAL();
}
