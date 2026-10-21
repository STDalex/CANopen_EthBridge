#ifndef APP_SETTINGS_H
#define APP_SETTINGS_H

#include "device_settings.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
  APP_SETTINGS_STORAGE_UNINITIALIZED = 0,
  APP_SETTINGS_STORAGE_READY,
  APP_SETTINGS_STORAGE_DEFAULTS,
  APP_SETTINGS_STORAGE_ERROR
} AppSettingsStorageState;

void AppSettings_Init(void);
void AppSettings_Get(DeviceSettings *settings);
bool AppSettings_Update(const DeviceSettings *settings, bool persist);
bool AppSettings_UpdateNetwork(const NetworkSettings *network, bool persist);
bool AppSettings_UpdateCan(uint32_t bitrate_kbps, bool deep_sdo_enabled, bool persist);
AppSettingsStorageState AppSettings_GetStorageState(void);
uint32_t AppSettings_GetGeneration(void);

#endif
