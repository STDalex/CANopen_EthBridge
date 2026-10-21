#ifndef NETWORK_SERVICE_H
#define NETWORK_SERVICE_H

#include "device_settings.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
  NETWORK_ADDRESS_NONE = 0,
  NETWORK_ADDRESS_DHCP,
  NETWORK_ADDRESS_STATIC,
  NETWORK_ADDRESS_FALLBACK
} NetworkAddressSource;

typedef struct
{
  bool initialized;
  bool link_up;
  NetworkAddressSource address_source;
  Ipv4Address ip;
  Ipv4Address netmask;
  Ipv4Address gateway;
  uint32_t dhcp_attempts;
  uint32_t reconfigurations;
} NetworkStatus;

void NetworkService_Start(void);
void NetworkService_RequestApply(void);
void NetworkService_GetStatus(NetworkStatus *status);
const char *NetworkService_AddressSourceName(NetworkAddressSource source);

#endif
