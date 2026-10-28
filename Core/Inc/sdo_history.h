#ifndef SDO_HISTORY_H
#define SDO_HISTORY_H

#include "can_tx_manager.h"

#include <stdbool.h>
#include <stddef.h>

#define SDO_HISTORY_CAPACITY 10U

void SdoHistory_Init(void);
bool SdoHistory_Remember(const CanTxSdoRequest *request);
size_t SdoHistory_Copy(CanTxSdoRequest *requests, size_t capacity);

#endif
