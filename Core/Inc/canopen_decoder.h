#ifndef CANOPEN_DECODER_H
#define CANOPEN_DECODER_H

#include "can_capture.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
  CANOPEN_MESSAGE_UNKNOWN = 0,
  CANOPEN_MESSAGE_NMT,
  CANOPEN_MESSAGE_SYNC,
  CANOPEN_MESSAGE_EMCY,
  CANOPEN_MESSAGE_TIME,
  CANOPEN_MESSAGE_TPDO1,
  CANOPEN_MESSAGE_RPDO1,
  CANOPEN_MESSAGE_TPDO2,
  CANOPEN_MESSAGE_RPDO2,
  CANOPEN_MESSAGE_TPDO3,
  CANOPEN_MESSAGE_RPDO3,
  CANOPEN_MESSAGE_TPDO4,
  CANOPEN_MESSAGE_RPDO4,
  CANOPEN_MESSAGE_SDO_TX,
  CANOPEN_MESSAGE_SDO_RX,
  CANOPEN_MESSAGE_HEARTBEAT_GUARD,
  CANOPEN_MESSAGE_LSS_TX,
  CANOPEN_MESSAGE_LSS_RX
} CanopenMessageType;

typedef struct
{
  CanopenMessageType type;
  uint8_t node_id;
  bool has_node_id;
} CanopenDecodedFrame;

typedef struct
{
  uint16_t index;
  uint8_t subindex;
  uint8_t data_offset;
  bool valid;
} CanopenSdoAddress;

CanopenDecodedFrame CanopenDecoder_Decode(const CanFrameRecord *frame);
CanopenSdoAddress CanopenDecoder_DecodeSdoAddress(const CanFrameRecord *frame,
                                                  CanopenMessageType type);
const char *CanopenDecoder_TypeName(CanopenMessageType type);

#endif
