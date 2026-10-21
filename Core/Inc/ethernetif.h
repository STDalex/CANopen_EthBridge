#ifndef __ETHERNETIF_H__
#define __ETHERNETIF_H__


#include "stm32f7xx_hal.h"
#include "lwip/err.h"
#include "lwip/netif.h"
#include "cmsis_os.h"

extern ETH_HandleTypeDef EthHandle;

/* Exported types ------------------------------------------------------------*/
/* Exported functions ------------------------------------------------------- */
err_t ethernetif_init(struct netif *netif);
void ethernet_link_thread( void const * argument );
#endif
