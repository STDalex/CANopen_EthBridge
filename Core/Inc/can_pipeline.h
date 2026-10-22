#ifndef CAN_PIPELINE_H
#define CAN_PIPELINE_H

#include "can_capture.h"

#include <stdbool.h>

void CanPipeline_ResetStatistics(void);
bool CanPipeline_PublishLocalTx(const CanFrameRecord *frame);

#endif
