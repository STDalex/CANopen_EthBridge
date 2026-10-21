#include "network_service.h"

#include "FreeRTOS.h"
#include "api_server.h"
#include "app_settings.h"
#include "ethernetif.h"
#include "lwip/dhcp.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/netifapi.h"
#include "lwip/tcpip.h"
#include "memory_layout.h"
#include "task.h"

#include <string.h>

static struct netif ethernet_netif;
static NetworkStatus public_status;
static volatile bool apply_requested;
static StaticTask_t network_task_control;
static StaticTask_t api_task_control;
static StackType_t *const network_task_stack = (StackType_t *)NETWORK_TASK_STACK_BASE;
static StackType_t *const api_task_stack = (StackType_t *)API_TASK_STACK_BASE;

static void TcpipReady(void *argument)
{
  TaskHandle_t task = (TaskHandle_t)argument;
  xTaskNotifyGive(task);
}

static void ToLwipAddress(const Ipv4Address *source, ip4_addr_t *destination)
{
  IP4_ADDR(destination, source->octet[0], source->octet[1],
           source->octet[2], source->octet[3]);
}

static void FromLwipAddress(const ip4_addr_t *source, Ipv4Address *destination)
{
  uint32_t value = lwip_ntohl(ip4_addr_get_u32(source));
  destination->octet[0] = (uint8_t)(value >> 24);
  destination->octet[1] = (uint8_t)(value >> 16);
  destination->octet[2] = (uint8_t)(value >> 8);
  destination->octet[3] = (uint8_t)value;
}

static void RefreshPublicStatus(NetworkAddressSource source)
{
  taskENTER_CRITICAL();
  public_status.initialized = true;
  public_status.link_up = netif_is_link_up(&ethernet_netif) != 0;
  public_status.address_source = source;
  FromLwipAddress(netif_ip4_addr(&ethernet_netif), &public_status.ip);
  FromLwipAddress(netif_ip4_netmask(&ethernet_netif), &public_status.netmask);
  FromLwipAddress(netif_ip4_gw(&ethernet_netif), &public_status.gateway);
  taskEXIT_CRITICAL();
}

static void SetAddress(const NetworkSettings *settings, NetworkAddressSource source)
{
  ip4_addr_t ip;
  ip4_addr_t mask;
  ip4_addr_t gateway;

  ToLwipAddress(&settings->static_ip, &ip);
  ToLwipAddress(&settings->netmask, &mask);
  ToLwipAddress(&settings->gateway, &gateway);
  (void)netifapi_netif_set_addr(&ethernet_netif, &ip, &mask, &gateway);
  RefreshPublicStatus(source);
}

static void ClearAddress(void)
{
  ip4_addr_t zero;
  ip4_addr_set_zero(&zero);
  (void)netifapi_netif_set_addr(&ethernet_netif, &zero, &zero, &zero);
  RefreshPublicStatus(NETWORK_ADDRESS_NONE);
}

static void ApiTask(void *argument)
{
  (void)argument;
  ApiServer_Run();
}

static void NetworkTask(void *argument)
{
  DeviceSettings settings;
  ip4_addr_t zero;
  uint32_t dhcp_started_at = 0U;
  bool was_link_up = false;
  bool dhcp_running = false;
  NetworkAddressSource source = NETWORK_ADDRESS_NONE;
  (void)argument;

  memset(&ethernet_netif, 0, sizeof(ethernet_netif));
  memset(&public_status, 0, sizeof(public_status));
  ip4_addr_set_zero(&zero);

  tcpip_init(TcpipReady, xTaskGetCurrentTaskHandle());
  (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000U));

  if (netif_add(&ethernet_netif, &zero, &zero, &zero, NULL,
                ethernetif_init, tcpip_input) == NULL)
  {
    vTaskDelete(NULL);
  }
  ethernet_netif.hostname = "cansniff";
  netif_set_default(&ethernet_netif);
  RefreshPublicStatus(NETWORK_ADDRESS_NONE);

  {
    osThreadDef(EthLink, ethernet_link_thread, osPriorityAboveNormal, 0,
                configMINIMAL_STACK_SIZE * 2U);
    (void)osThreadCreate(osThread(EthLink), &ethernet_netif);
  }

  xTaskCreateStatic(ApiTask, "apiTask", API_TASK_STACK_WORDS, NULL,
                    tskIDLE_PRIORITY + 1U, api_task_stack, &api_task_control);

  for (;;)
  {
    bool link_up = netif_is_link_up(&ethernet_netif) != 0;
    bool restart = apply_requested || (link_up && !was_link_up);

    if (apply_requested)
    {
      apply_requested = false;
      taskENTER_CRITICAL();
      ++public_status.reconfigurations;
      taskEXIT_CRITICAL();
    }

    if (!link_up && was_link_up)
    {
      if (dhcp_running)
      {
        (void)netifapi_dhcp_stop(&ethernet_netif);
        dhcp_running = false;
      }
      ClearAddress();
      source = NETWORK_ADDRESS_NONE;
    }

    if (restart)
    {
      if (dhcp_running)
      {
        (void)netifapi_dhcp_stop(&ethernet_netif);
        dhcp_running = false;
      }
      AppSettings_Get(&settings);
      if (settings.network.dhcp_enabled)
      {
        ClearAddress();
        if (netifapi_dhcp_start(&ethernet_netif) == ERR_OK)
        {
          dhcp_running = true;
          dhcp_started_at = HAL_GetTick();
          source = NETWORK_ADDRESS_NONE;
          taskENTER_CRITICAL();
          ++public_status.dhcp_attempts;
          taskEXIT_CRITICAL();
        }
        else
        {
          SetAddress(&settings.network, NETWORK_ADDRESS_FALLBACK);
          source = NETWORK_ADDRESS_FALLBACK;
        }
      }
      else
      {
        SetAddress(&settings.network, NETWORK_ADDRESS_STATIC);
        source = NETWORK_ADDRESS_STATIC;
      }
    }

    if (link_up && dhcp_running)
    {
      AppSettings_Get(&settings);
      if (dhcp_supplied_address(&ethernet_netif))
      {
        dhcp_running = false;
        source = NETWORK_ADDRESS_DHCP;
        RefreshPublicStatus(source);
      }
      else if ((uint32_t)(HAL_GetTick() - dhcp_started_at) >=
               settings.network.dhcp_timeout_ms)
      {
        (void)netifapi_dhcp_stop(&ethernet_netif);
        dhcp_running = false;
        SetAddress(&settings.network, NETWORK_ADDRESS_FALLBACK);
        source = NETWORK_ADDRESS_FALLBACK;
      }
    }

    if (link_up != was_link_up)
    {
      RefreshPublicStatus(source);
      was_link_up = link_up;
    }
    vTaskDelay(pdMS_TO_TICKS(250U));
  }
}

void NetworkService_Start(void)
{
  apply_requested = false;
  (void)xTaskCreateStatic(NetworkTask, "networkTask", NETWORK_TASK_STACK_WORDS,
                          NULL, tskIDLE_PRIORITY + 2U, network_task_stack,
                          &network_task_control);
}

void NetworkService_RequestApply(void)
{
  apply_requested = true;
}

void NetworkService_GetStatus(NetworkStatus *status)
{
  if (status == NULL)
  {
    return;
  }
  taskENTER_CRITICAL();
  *status = public_status;
  taskEXIT_CRITICAL();
}

const char *NetworkService_AddressSourceName(NetworkAddressSource source)
{
  switch (source)
  {
    case NETWORK_ADDRESS_DHCP: return "dhcp";
    case NETWORK_ADDRESS_STATIC: return "static";
    case NETWORK_ADDRESS_FALLBACK: return "fallback";
    default: return "none";
  }
}
