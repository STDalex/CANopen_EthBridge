#ifndef APP_WATCHDOG_H
#define APP_WATCHDOG_H

#include <stdint.h>

typedef enum
{
  APP_WATCHDOG_CAN_RX = 1U << 0,
  APP_WATCHDOG_CAN_CONTROL = 1U << 1,
  APP_WATCHDOG_CAN_TX = 1U << 2
} AppWatchdogHeartbeat;

void AppWatchdog_StartHardware(void);
void AppWatchdog_Heartbeat(AppWatchdogHeartbeat heartbeat);
void AppWatchdog_Task(void);

#endif
