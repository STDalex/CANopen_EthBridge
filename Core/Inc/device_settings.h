#ifndef DEVICE_SETTINGS_H
#define DEVICE_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#define DEVICE_SETTINGS_SCHEMA_VERSION 1U
#define DEVICE_HTTP_PORT_DEFAULT        80U
#define DEVICE_DHCP_TIMEOUT_MS_DEFAULT  12000U

typedef struct __attribute__((aligned(4)))
{
  uint8_t octet[4];
} Ipv4Address;

typedef struct
{
  bool dhcp_enabled;
  Ipv4Address static_ip;
  Ipv4Address netmask;
  Ipv4Address gateway;
  uint16_t http_port;
  uint32_t dhcp_timeout_ms;
} NetworkSettings;

typedef struct
{
  uint32_t schema_version;
  uint32_t can_bitrate_kbps;
  bool deep_sdo_enabled;
  NetworkSettings network;
} DeviceSettings;

void DeviceSettings_SetDefaults(DeviceSettings *settings);
bool DeviceSettings_IsValid(const DeviceSettings *settings);
bool DeviceSettings_IsValidNetwork(const NetworkSettings *network);
bool DeviceSettings_ParseIpv4(const char *text, Ipv4Address *address);
void DeviceSettings_FormatIpv4(const Ipv4Address *address, char *text, uint32_t capacity);
uint32_t DeviceSettings_Ipv4ToU32(const Ipv4Address *address);

#endif
