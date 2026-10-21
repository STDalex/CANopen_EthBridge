#ifndef API_SERVER_H
#define API_SERVER_H

#include "can_capture.h"

#include <stdint.h>

typedef struct
{
  uint32_t accepted_connections;
  uint32_t completed_requests;
  uint32_t bad_requests;
  uint32_t send_errors;
  uint32_t websocket_connections;
  uint32_t websocket_batches;
  uint32_t websocket_frames;
  uint32_t websocket_lost_frames;
  bool websocket_active;
  uint32_t download_requests;
  uint32_t download_completed;
  uint64_t download_bytes;
} ApiServerStatus;

void ApiServer_Run(void);
void ApiServer_SubmitFrame(const CanFrameRecord *frame);
void ApiServer_GetStatus(ApiServerStatus *status);

#endif
