/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"
#include "queue.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "can_capture.h"
#include "ui.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */
#define GUI_FRAME_QUEUE_LENGTH 64U
static StaticQueue_t guiFrameQueueControlBlock;
static uint8_t guiFrameQueueStorage[GUI_FRAME_QUEUE_LENGTH * sizeof(CanFrameRecord)];
static QueueHandle_t guiFrameQueue;

/* USER CODE END Variables */
osThreadId canRxTaskHandle;
uint32_t canRxTaskBuffer[ 1024 ];
osStaticThreadDef_t canRxTaskControlBlock;
osThreadId canControlTaskHandle;
uint32_t canControlTaskBuffer[ 1024 ];
osStaticThreadDef_t canControlTaskControlBlock;
osThreadId guiTaskHandle;
uint32_t guiTaskBuffer[ 4096 ];
osStaticThreadDef_t guiTaskControlBlock;

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

/* USER CODE END FunctionPrototypes */

void StartCanRxTask(void const * argument);
void StartCanControlTask(void const * argument);
void StartGuiTask(void const * argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/* GetIdleTaskMemory prototype (linked to static allocation support) */
void vApplicationGetIdleTaskMemory( StaticTask_t **ppxIdleTaskTCBBuffer, StackType_t **ppxIdleTaskStackBuffer, uint32_t *pulIdleTaskStackSize );

/* Hook prototypes */
void vApplicationStackOverflowHook(xTaskHandle xTask, signed char *pcTaskName);
void vApplicationMallocFailedHook(void);

/* USER CODE BEGIN 4 */
__weak void vApplicationStackOverflowHook(xTaskHandle xTask, signed char *pcTaskName)
{
   /* Run time stack overflow checking is performed if
   configCHECK_FOR_STACK_OVERFLOW is defined to 1 or 2. This hook function is
   called if a stack overflow is detected. */
}
/* USER CODE END 4 */

/* USER CODE BEGIN 5 */
__weak void vApplicationMallocFailedHook(void)
{
   /* vApplicationMallocFailedHook() will only be called if
   configUSE_MALLOC_FAILED_HOOK is set to 1 in FreeRTOSConfig.h. It is a hook
   function that will get called if a call to pvPortMalloc() fails.
   pvPortMalloc() is called internally by the kernel whenever a task, queue,
   timer or semaphore is created. It is also called by various parts of the
   demo application. If heap_1.c or heap_2.c are used, then the size of the
   heap available to pvPortMalloc() is defined by configTOTAL_HEAP_SIZE in
   FreeRTOSConfig.h, and the xPortGetFreeHeapSize() API function can be used
   to query the size of free heap space that remains (although it does not
   provide information on how the remaining heap might be fragmented). */
}
/* USER CODE END 5 */

/* USER CODE BEGIN GET_IDLE_TASK_MEMORY */
static StaticTask_t xIdleTaskTCBBuffer;
static StackType_t xIdleStack[configMINIMAL_STACK_SIZE];

void vApplicationGetIdleTaskMemory( StaticTask_t **ppxIdleTaskTCBBuffer, StackType_t **ppxIdleTaskStackBuffer, uint32_t *pulIdleTaskStackSize )
{
  *ppxIdleTaskTCBBuffer = &xIdleTaskTCBBuffer;
  *ppxIdleTaskStackBuffer = &xIdleStack[0];
  *pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
  /* place for user code */
}
/* USER CODE END GET_IDLE_TASK_MEMORY */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  guiFrameQueue = xQueueCreateStatic(GUI_FRAME_QUEUE_LENGTH,
                                     sizeof(CanFrameRecord),
                                     guiFrameQueueStorage,
                                     &guiFrameQueueControlBlock);
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* definition and creation of canRxTask */
  osThreadStaticDef(canRxTask, StartCanRxTask, osPriorityRealtime, 0, 1024, canRxTaskBuffer, &canRxTaskControlBlock);
  canRxTaskHandle = osThreadCreate(osThread(canRxTask), NULL);

  /* definition and creation of canControlTask */
  osThreadStaticDef(canControlTask, StartCanControlTask, osPriorityHigh, 0, 1024, canControlTaskBuffer, &canControlTaskControlBlock);
  canControlTaskHandle = osThreadCreate(osThread(canControlTask), NULL);

  /* definition and creation of guiTask */
  osThreadStaticDef(guiTask, StartGuiTask, osPriorityAboveNormal, 0, 4096, guiTaskBuffer, &guiTaskControlBlock);
  guiTaskHandle = osThreadCreate(osThread(guiTask), NULL);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */

}

/* USER CODE BEGIN Header_StartCanRxTask */
/**
  * @brief  Function implementing the canRxTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartCanRxTask */
void StartCanRxTask(void const * argument)
{
  /* USER CODE BEGIN StartCanRxTask */
  CanFrameRecord frame;
  (void)argument;
  CanCapture_SetConsumerTask(xTaskGetCurrentTaskHandle());
  /* Infinite loop */
  for(;;)
  {
    (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20U));
    while (CanCapture_Pop(&frame))
    {
      if (xQueueSend(guiFrameQueue, &frame, 0U) != pdPASS)
      {
        CanControl_AddGuiOverflow();
      }
    }
  }
  /* USER CODE END StartCanRxTask */
}

/* USER CODE BEGIN Header_StartCanControlTask */
/**
* @brief Function implementing the canControlTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartCanControlTask */
void StartCanControlTask(void const * argument)
{
  /* USER CODE BEGIN StartCanControlTask */
  (void)argument;
  CanControl_Task();
  /* USER CODE END StartCanControlTask */
}

/* USER CODE BEGIN Header_StartGuiTask */
/**
* @brief Function implementing the guiTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartGuiTask */
void StartGuiTask(void const * argument)
{
  /* USER CODE BEGIN StartGuiTask */
  CanFrameRecord frame;
  uint32_t count;
  (void)argument;
  Ui_Init();
  /* Infinite loop */
  for(;;)
  {
    count = 0U;
    while ((count < GUI_FRAME_QUEUE_LENGTH) &&
           (xQueueReceive(guiFrameQueue, &frame, 0U) == pdPASS))
    {
      Ui_ProcessFrame(&frame);
      ++count;
    }
    Ui_TaskLoop();
    osDelay(5U);
  }
  /* USER CODE END StartGuiTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* USER CODE END Application */

