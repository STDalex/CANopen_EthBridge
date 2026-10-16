#ifndef CANOPEN_ANALYZER_H
#define CANOPEN_ANALYZER_H

#include "can_capture.h"
#include "canopen_decoder.h"

#include <stdint.h>

#define CANOPEN_NODE_COUNT_MAX       127U
#define CANOPEN_EVENT_SNAPSHOT_MAX   128U

typedef struct
{
  uint8_t node_id;
  CanopenMessageType last_type;
  uint32_t first_seen_us;
  uint32_t last_seen_us;
  uint32_t frame_count;
  uint32_t byte_count;
  uint32_t sdo_request_count;
  uint32_t sdo_response_count;
} CanopenNodeSnapshot;

typedef struct
{
  uint32_t frame_count;
  uint32_t last_timestamp_us;
  uint32_t unique_node_count;
} CanopenGlobalSnapshot;

typedef struct
{
  CanFrameRecord frame;
  CanopenMessageType type;
  uint32_t response_time_us;
  bool has_response_time;
} CanopenNodeEventSnapshot;

void CanopenAnalyzer_Init(void);
void CanopenAnalyzer_ProcessFrame(const CanFrameRecord *frame);
void CanopenAnalyzer_AddQueueOverflow(void);
uint32_t CanopenAnalyzer_GetNodesSnapshot(CanopenNodeSnapshot *nodes, uint32_t capacity,
                                          CanopenGlobalSnapshot *global);
uint32_t CanopenAnalyzer_GetNodeEventsSnapshot(uint8_t node_id,
                                               CanopenNodeEventSnapshot *events,
                                               uint32_t capacity);
uint32_t CanopenAnalyzer_GetNodeObjectsSnapshot(uint8_t node_id,
                                                CanopenNodeEventSnapshot *events,
                                                uint32_t capacity);
void CanopenAnalyzer_GetGlobalSnapshot(CanopenGlobalSnapshot *global);
uint32_t CanopenAnalyzer_GetQueueOverflows(void);
void CanopenAnalyzer_Reset(void);

#endif
