#ifndef SDMMC_H
#define SDMMC_H

#include "main.h"

#include <stdbool.h>

extern SD_HandleTypeDef hsd1;

bool StorageSd_CardPresent(void);
HAL_StatusTypeDef StorageSd_Init(void);
void StorageSd_DeInit(void);

#endif
