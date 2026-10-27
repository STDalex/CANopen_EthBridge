#ifndef CANOPEN_ANALYZER_H
#define CANOPEN_ANALYZER_H

#include "can_capture.h"
#include "canopen_decoder.h"

#include <stdint.h>

#define CANOPEN_NODE_COUNT_MAX       127U
#define CANOPEN_EVENT_SNAPSHOT_MAX   128U

typedef enum
{
  CANOPEN_SDO_STATUS_NONE = 0,
  CANOPEN_SDO_STATUS_WAITING,
  CANOPEN_SDO_STATUS_COMPLETED,
  CANOPEN_SDO_STATUS_ABORTED,
  CANOPEN_SDO_STATUS_TIMEOUT,
  CANOPEN_SDO_STATUS_AMBIGUOUS,
  CANOPEN_SDO_STATUS_PROTOCOL_ERROR,
  CANOPEN_SDO_STATUS_ORPHAN
} CanopenSdoStatus;

typedef enum
{
  CANOPEN_SDO_CONFIDENCE_NONE = 0,
  CANOPEN_SDO_CONFIDENCE_EXACT,
  CANOPEN_SDO_CONFIDENCE_INFERRED,
  CANOPEN_SDO_CONFIDENCE_AMBIGUOUS
} CanopenSdoConfidence;

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
  uint32_t sdo_started_count;
  uint32_t sdo_completed_count;
  uint32_t sdo_aborted_count;
  uint32_t sdo_timeout_count;
  uint32_t sdo_conflict_count;
  uint32_t sdo_orphan_count;
  uint32_t sdo_protocol_error_count;
  uint8_t nmt_state;
  bool has_nmt_state;
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
  uint16_t object_index;
  uint8_t object_subindex;
  uint8_t data_offset;
  bool has_sdo_address;
  CanopenSdoStatus sdo_status;
  CanopenSdoConfidence sdo_confidence;
  uint32_t transaction_time_us;
  uint32_t transferred_bytes;
  uint32_t abort_code;
  uint16_t segment_count;
  bool local_transaction;
} CanopenNodeEventSnapshot;

void CanopenAnalyzer_Init(void);
void CanopenAnalyzer_ProcessFrame(const CanFrameRecord *frame);
void CanopenAnalyzer_Update(uint32_t now_us);
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
