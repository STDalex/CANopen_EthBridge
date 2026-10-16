#include "canopen_decoder.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static CanFrameRecord Frame(uint32_t id)
{
  CanFrameRecord frame;
  memset(&frame, 0, sizeof(frame));
  frame.id = id;
  frame.dlc = 8U;
  return frame;
}

static void ExpectNode(uint32_t id, CanopenMessageType type, uint8_t node_id)
{
  CanFrameRecord frame = Frame(id);
  CanopenDecodedFrame decoded = CanopenDecoder_Decode(&frame);
  assert(decoded.type == type);
  assert(decoded.has_node_id);
  assert(decoded.node_id == node_id);
}

int main(void)
{
  CanFrameRecord frame;
  CanopenDecodedFrame decoded;
  CanopenSdoAddress address;

  ExpectNode(0x081U, CANOPEN_MESSAGE_EMCY, 1U);
  ExpectNode(0x0FFU, CANOPEN_MESSAGE_EMCY, 127U);
  ExpectNode(0x181U, CANOPEN_MESSAGE_TPDO1, 1U);
  ExpectNode(0x27FU, CANOPEN_MESSAGE_RPDO1, 127U);
  ExpectNode(0x281U, CANOPEN_MESSAGE_TPDO2, 1U);
  ExpectNode(0x37FU, CANOPEN_MESSAGE_RPDO2, 127U);
  ExpectNode(0x381U, CANOPEN_MESSAGE_TPDO3, 1U);
  ExpectNode(0x47FU, CANOPEN_MESSAGE_RPDO3, 127U);
  ExpectNode(0x481U, CANOPEN_MESSAGE_TPDO4, 1U);
  ExpectNode(0x57FU, CANOPEN_MESSAGE_RPDO4, 127U);
  ExpectNode(0x581U, CANOPEN_MESSAGE_SDO_TX, 1U);
  ExpectNode(0x67FU, CANOPEN_MESSAGE_SDO_RX, 127U);
  ExpectNode(0x701U, CANOPEN_MESSAGE_HEARTBEAT_GUARD, 1U);
  ExpectNode(0x77FU, CANOPEN_MESSAGE_HEARTBEAT_GUARD, 127U);

  frame = Frame(0x000U);
  frame.data[1] = 42U;
  decoded = CanopenDecoder_Decode(&frame);
  assert(decoded.type == CANOPEN_MESSAGE_NMT);
  assert(decoded.has_node_id && (decoded.node_id == 42U));

  frame.data[1] = 0U;
  decoded = CanopenDecoder_Decode(&frame);
  assert(decoded.type == CANOPEN_MESSAGE_NMT);
  assert(!decoded.has_node_id);

  frame = Frame(0x080U);
  decoded = CanopenDecoder_Decode(&frame);
  assert((decoded.type == CANOPEN_MESSAGE_SYNC) && !decoded.has_node_id);

  frame = Frame(0x7E4U);
  decoded = CanopenDecoder_Decode(&frame);
  assert((decoded.type == CANOPEN_MESSAGE_LSS_TX) && !decoded.has_node_id);

  frame = Frame(0x123U);
  decoded = CanopenDecoder_Decode(&frame);
  assert(decoded.type == CANOPEN_MESSAGE_UNKNOWN);

  frame = Frame(0x581U);
  frame.flags = CAN_FRAME_FLAG_EXTENDED;
  decoded = CanopenDecoder_Decode(&frame);
  assert(decoded.type == CANOPEN_MESSAGE_UNKNOWN);

  frame = Frame(0x603U);
  frame.data[0] = 0x23U;
  frame.data[1] = 0x00U;
  frame.data[2] = 0x20U;
  frame.data[3] = 0x02U;
  address = CanopenDecoder_DecodeSdoAddress(&frame, CANOPEN_MESSAGE_SDO_RX);
  assert(address.valid && (address.index == 0x2000U) && (address.subindex == 0x02U));
  assert(address.data_offset == 4U);

  frame.data[0] = 0x00U;
  address = CanopenDecoder_DecodeSdoAddress(&frame, CANOPEN_MESSAGE_SDO_RX);
  assert(!address.valid);

  frame.data[0] = 0xA0U;
  frame.data[1] = 0x06U;
  frame.data[2] = 0x20U;
  frame.data[3] = 0x01U;
  address = CanopenDecoder_DecodeSdoAddress(&frame, CANOPEN_MESSAGE_SDO_RX);
  assert(address.valid && (address.index == 0x2006U) && (address.subindex == 0x01U));

  puts("CANopen decoder tests passed");
  return 0;
}
