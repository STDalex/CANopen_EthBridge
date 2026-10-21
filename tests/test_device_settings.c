#include "device_settings.h"

#include <assert.h>
#include <string.h>

static void TestDefaults(void)
{
  DeviceSettings settings;
  DeviceSettings_SetDefaults(&settings);
  assert(DeviceSettings_IsValid(&settings));
  assert(settings.network.dhcp_enabled);
  assert(settings.network.static_ip.octet[0] == 192U);
  assert(settings.network.static_ip.octet[3] == 10U);
}

static void TestIpv4Parser(void)
{
  Ipv4Address address;
  char text[20];

  assert(DeviceSettings_ParseIpv4("10.20.30.40", &address));
  assert(DeviceSettings_Ipv4ToU32(&address) == 0x0A141E28UL);
  DeviceSettings_FormatIpv4(&address, text, sizeof(text));
  assert(strcmp(text, "10.20.30.40") == 0);

  assert(!DeviceSettings_ParseIpv4("10.20.30", &address));
  assert(!DeviceSettings_ParseIpv4("10.20.30.256", &address));
  assert(!DeviceSettings_ParseIpv4("10..30.40", &address));
  assert(!DeviceSettings_ParseIpv4("10.20.30.40x", &address));
}

static void TestNetworkValidation(void)
{
  DeviceSettings settings;
  DeviceSettings_SetDefaults(&settings);

  settings.network.netmask = (Ipv4Address){{255U, 0U, 255U, 0U}};
  assert(!DeviceSettings_IsValidNetwork(&settings.network));

  DeviceSettings_SetDefaults(&settings);
  settings.network.static_ip = (Ipv4Address){{192U, 168U, 7U, 0U}};
  assert(!DeviceSettings_IsValidNetwork(&settings.network));

  DeviceSettings_SetDefaults(&settings);
  settings.network.gateway = (Ipv4Address){{192U, 168U, 8U, 1U}};
  assert(!DeviceSettings_IsValidNetwork(&settings.network));

  DeviceSettings_SetDefaults(&settings);
  settings.network.gateway = (Ipv4Address){{0U, 0U, 0U, 0U}};
  assert(DeviceSettings_IsValidNetwork(&settings.network));
}

int main(void)
{
  TestDefaults();
  TestIpv4Parser();
  TestNetworkValidation();
  return 0;
}
