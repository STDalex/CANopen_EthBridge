#include "device_settings.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

_Static_assert((__alignof(Ipv4Address) >= 4U) &&
               ((offsetof(NetworkSettings, static_ip) & 3U) == 0U) &&
               ((offsetof(NetworkSettings, netmask) & 3U) == 0U) &&
               ((offsetof(NetworkSettings, gateway) & 3U) == 0U),
               "IPv4 settings fields must be word aligned");

static bool IsSupportedBitrate(uint32_t bitrate_kbps)
{
  static const uint32_t supported[] =
  {
    0U, 10U, 20U, 50U, 100U, 125U, 250U, 500U, 800U, 1000U
  };
  uint32_t index;

  for (index = 0U; index < (sizeof(supported) / sizeof(supported[0])); ++index)
  {
    if (supported[index] == bitrate_kbps)
    {
      return true;
    }
  }
  return false;
}

static bool IsContiguousNetmask(const Ipv4Address *netmask)
{
  uint32_t value = DeviceSettings_Ipv4ToU32(netmask);
  bool saw_zero = false;
  uint32_t bit;

  if ((value == 0U) || (value == 0xFFFFFFFFUL))
  {
    return false;
  }

  for (bit = 0U; bit < 32U; ++bit)
  {
    bool set = (value & (0x80000000UL >> bit)) != 0U;
    if (!set)
    {
      saw_zero = true;
    }
    else if (saw_zero)
    {
      return false;
    }
  }
  return true;
}

static bool IsUsableHost(const Ipv4Address *address, const Ipv4Address *netmask)
{
  uint32_t ip = DeviceSettings_Ipv4ToU32(address);
  uint32_t mask = DeviceSettings_Ipv4ToU32(netmask);
  uint32_t host = ip & ~mask;

  if ((address->octet[0] == 0U) || (address->octet[0] >= 224U) ||
      (ip == 0xFFFFFFFFUL))
  {
    return false;
  }
  return (host != 0U) && (host != ~mask);
}

void DeviceSettings_SetDefaults(DeviceSettings *settings)
{
  if (settings == NULL)
  {
    return;
  }

  memset(settings, 0, sizeof(*settings));
  settings->schema_version = DEVICE_SETTINGS_SCHEMA_VERSION;
  settings->can_bitrate_kbps = 0U;
  settings->deep_sdo_enabled = false;
  settings->network.dhcp_enabled = true;
  settings->network.static_ip = (Ipv4Address){{192U, 168U, 7U, 10U}};
  settings->network.netmask = (Ipv4Address){{255U, 255U, 255U, 0U}};
  settings->network.gateway = (Ipv4Address){{192U, 168U, 7U, 1U}};
  settings->network.http_port = DEVICE_HTTP_PORT_DEFAULT;
  settings->network.dhcp_timeout_ms = DEVICE_DHCP_TIMEOUT_MS_DEFAULT;
}

bool DeviceSettings_IsValidNetwork(const NetworkSettings *network)
{
  uint32_t ip;
  uint32_t gateway;
  uint32_t mask;

  if ((network == NULL) || !IsContiguousNetmask(&network->netmask) ||
      !IsUsableHost(&network->static_ip, &network->netmask) ||
      (network->http_port == 0U) ||
      (network->dhcp_timeout_ms < 3000U) ||
      (network->dhcp_timeout_ms > 120000U))
  {
    return false;
  }

  ip = DeviceSettings_Ipv4ToU32(&network->static_ip);
  gateway = DeviceSettings_Ipv4ToU32(&network->gateway);
  mask = DeviceSettings_Ipv4ToU32(&network->netmask);
  if (gateway != 0U)
  {
    if (!IsUsableHost(&network->gateway, &network->netmask) ||
        ((gateway & mask) != (ip & mask)) || (gateway == ip))
    {
      return false;
    }
  }
  return true;
}

bool DeviceSettings_IsValid(const DeviceSettings *settings)
{
  return (settings != NULL) &&
         (settings->schema_version == DEVICE_SETTINGS_SCHEMA_VERSION) &&
         IsSupportedBitrate(settings->can_bitrate_kbps) &&
         DeviceSettings_IsValidNetwork(&settings->network);
}

bool DeviceSettings_ParseIpv4(const char *text, Ipv4Address *address)
{
  uint32_t part = 0U;
  uint32_t index = 0U;
  uint32_t digits = 0U;
  Ipv4Address parsed = {{0U, 0U, 0U, 0U}};

  if ((text == NULL) || (address == NULL))
  {
    return false;
  }

  for (;;)
  {
    char character = *text++;
    if ((character >= '0') && (character <= '9'))
    {
      part = (part * 10U) + (uint32_t)(character - '0');
      ++digits;
      if ((part > 255U) || (digits > 3U))
      {
        return false;
      }
    }
    else if ((character == '.') || (character == '\0'))
    {
      if ((digits == 0U) || (index >= 4U))
      {
        return false;
      }
      parsed.octet[index++] = (uint8_t)part;
      if (character == '\0')
      {
        break;
      }
      part = 0U;
      digits = 0U;
    }
    else
    {
      return false;
    }
  }

  if (index != 4U)
  {
    return false;
  }
  *address = parsed;
  return true;
}

void DeviceSettings_FormatIpv4(const Ipv4Address *address, char *text, uint32_t capacity)
{
  if ((address == NULL) || (text == NULL) || (capacity == 0U))
  {
    return;
  }
  (void)snprintf(text, capacity, "%u.%u.%u.%u",
                 (unsigned int)address->octet[0],
                 (unsigned int)address->octet[1],
                 (unsigned int)address->octet[2],
                 (unsigned int)address->octet[3]);
}

uint32_t DeviceSettings_Ipv4ToU32(const Ipv4Address *address)
{
  if (address == NULL)
  {
    return 0U;
  }
  return ((uint32_t)address->octet[0] << 24) |
         ((uint32_t)address->octet[1] << 16) |
         ((uint32_t)address->octet[2] << 8) |
         (uint32_t)address->octet[3];
}
