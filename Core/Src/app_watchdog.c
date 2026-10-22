#include "app_watchdog.h"

#include <stdbool.h>

#include "FreeRTOS.h"
#include "cmsis_os.h"
#include "stm32f7xx.h"
#include "task.h"

#define APP_WATCHDOG_REQUIRED_MASK \
  (APP_WATCHDOG_CAN_RX | APP_WATCHDOG_CAN_CONTROL | APP_WATCHDOG_CAN_TX)
#define APP_WATCHDOG_CHECK_PERIOD_MS 1000U

/* LSI is nominally 32 kHz. A /256 prescaler and reload value 624 give a
   nominal 5 second timeout while retaining margin for LSI tolerance. */
#define APP_WATCHDOG_PRESCALER_DIV256 6U
#define APP_WATCHDOG_RELOAD           624U

static volatile uint32_t heartbeat_mask;
static bool hardware_started;

void AppWatchdog_StartHardware(void)
{
  heartbeat_mask = 0U;

  IWDG->KR = 0xCCCCU;
  IWDG->KR = 0x5555U;
  IWDG->PR = APP_WATCHDOG_PRESCALER_DIV256;
  IWDG->RLR = APP_WATCHDOG_RELOAD;
  while (IWDG->SR != 0U)
  {
  }
  IWDG->KR = 0xAAAAU;
  hardware_started = true;
}

void AppWatchdog_Heartbeat(AppWatchdogHeartbeat heartbeat)
{
  taskENTER_CRITICAL();
  heartbeat_mask |= (uint32_t)heartbeat;
  taskEXIT_CRITICAL();
}

void AppWatchdog_Task(void)
{
  bool monitoring_failed = false;

  for (;;)
  {
    uint32_t observed;

    osDelay(APP_WATCHDOG_CHECK_PERIOD_MS);
    taskENTER_CRITICAL();
    observed = heartbeat_mask;
    heartbeat_mask = 0U;
    taskEXIT_CRITICAL();

    if ((observed & APP_WATCHDOG_REQUIRED_MASK) != APP_WATCHDOG_REQUIRED_MASK)
    {
      monitoring_failed = true;
    }
    if (hardware_started && !monitoring_failed)
    {
      IWDG->KR = 0xAAAAU;
    }
  }
}
