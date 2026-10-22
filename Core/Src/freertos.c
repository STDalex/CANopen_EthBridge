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
#include "can_log.h"
#include "can_pipeline.h"
#include "can_tx_manager.h"
#include "canopen_analyzer.h"
#include "api_server.h"
#include "memory_layout.h"
#include "network_service.h"
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
#define ANALYZER_FRAME_QUEUE_LENGTH 128U
static StaticQueue_t guiFrameQueueControlBlock;
static uint8_t guiFrameQueueStorage[GUI_FRAME_QUEUE_LENGTH * sizeof(CanFrameRecord)];
static QueueHandle_t guiFrameQueue;
static StaticQueue_t analyzerFrameQueueControlBlock;
static uint8_t analyzerFrameQueueStorage[ANALYZER_FRAME_QUEUE_LENGTH * sizeof(CanFrameRecord)];
static QueueHandle_t analyzerFrameQueue;
static StaticTask_t storageTaskControlBlock;
static StackType_t storageTaskStack[STORAGE_TASK_STACK_WORDS];
static StaticTask_t canTxTaskControlBlock;
static StackType_t canTxTaskStack[1024U];

/* USER CODE END Variables */
osThreadId canRxTaskHandle;
uint32_t canRxTaskBuffer[ 1024 ];
osStaticThreadDef_t canRxTaskControlBlock;
osThreadId canControlTaskHandle;
uint32_t canControlTaskBuffer[ 1024 ];
osStaticThreadDef_t canControlTaskControlBlock;
osThreadId analyzerTaskHandle;
uint32_t analyzerTaskBuffer[ 1536 ];
osStaticThreadDef_t analyzerTaskControlBlock;
osThreadId guiTaskHandle;
osStaticThreadDef_t guiTaskControlBlock;

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */
static void StartStorageTask(void *argument);
static void StartCanTxTask(void *argument);

/* USER CODE END FunctionPrototypes */

void StartCanRxTask(void const * argument);
void StartCanControlTask(void const * argument);
void StartAnalyzerTask(void const * argument);
void StartGuiTask(void const * argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/* GetIdleTaskMemory prototype (linked to static allocation support) */
void vApplicationGetIdleTaskMemory( StaticTask_t **ppxIdleTaskTCBBuffer, StackType_t **ppxIdleTaskStackBuffer, uint32_t *pulIdleTaskStackSize );

/* Hook prototypes */
void vApplicationStackOverflowHook(xTaskHandle xTask, signed char *pcTaskName);
void vApplicationMallocFailedHook(void);

/* USER CODE BEGIN 4 */
void CanPipeline_ResetStatistics(void)
{
  taskENTER_CRITICAL();
  (void)xQueueReset(guiFrameQueue);
  (void)xQueueReset(analyzerFrameQueue);
  CanControl_ResetStatistics();
  CanopenAnalyzer_Reset();
  CanLogger_ResetCounters();
  CanTx_ResetStatistics();
  taskEXIT_CRITICAL();
}

bool CanPipeline_PublishLocalTx(const CanFrameRecord *frame)
{
  bool analyzer_queued;
  if ((frame == NULL) || ((frame->flags & CAN_FRAME_FLAG_LOCAL_TX) == 0U))
  {
    return false;
  }

  analyzer_queued = xQueueSend(analyzerFrameQueue, frame, 0U) == pdPASS;
  if (!analyzer_queued)
  {
    CanopenAnalyzer_AddQueueOverflow();
  }
  ApiServer_SubmitFrame(frame);
  if (xQueueSend(guiFrameQueue, frame, 0U) != pdPASS)
  {
    CanControl_AddGuiOverflow();
  }
  return analyzer_queued;
}

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
  analyzerFrameQueue = xQueueCreateStatic(ANALYZER_FRAME_QUEUE_LENGTH,
                                          sizeof(CanFrameRecord),
                                          analyzerFrameQueueStorage,
                                          &analyzerFrameQueueControlBlock);
  CanopenAnalyzer_Init();
  CanLogger_Init();
  CanTxManager_Init();
  NetworkService_Start();
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* definition and creation of canRxTask */
  osThreadStaticDef(canRxTask, StartCanRxTask, osPriorityRealtime, 0, 1024, canRxTaskBuffer, &canRxTaskControlBlock);
  canRxTaskHandle = osThreadCreate(osThread(canRxTask), NULL);

  /* definition and creation of canControlTask */
  osThreadStaticDef(canControlTask, StartCanControlTask, osPriorityHigh, 0, 1024, canControlTaskBuffer, &canControlTaskControlBlock);
  canControlTaskHandle = osThreadCreate(osThread(canControlTask), NULL);

  /* definition and creation of analyzerTask */
  osThreadStaticDef(analyzerTask, StartAnalyzerTask, osPriorityAboveNormal, 0, 1536, analyzerTaskBuffer, &analyzerTaskControlBlock);
  analyzerTaskHandle = osThreadCreate(osThread(analyzerTask), NULL);

  /* definition and creation of guiTask */
  osThreadStaticDef(guiTask, StartGuiTask, osPriorityNormal, 0,
                    GUI_TASK_STACK_WORDS, (uint32_t *)GUI_TASK_STACK_BASE,
                    &guiTaskControlBlock);
  guiTaskHandle = osThreadCreate(osThread(guiTask), NULL);

  /* USER CODE BEGIN RTOS_THREADS */
  (void)xTaskCreateStatic(StartStorageTask, "storageTask",
                          STORAGE_TASK_STACK_WORDS,
                          NULL, tskIDLE_PRIORITY + 2U,
                          storageTaskStack,
                          &storageTaskControlBlock);
  (void)xTaskCreateStatic(StartCanTxTask, "canTxTask", 1024U,
                          NULL, tskIDLE_PRIORITY + 4U,
                          canTxTaskStack, &canTxTaskControlBlock);
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
    CanLogger_ProducerPoll();
    while (CanCapture_Pop(&frame))
    {
      CanLogger_SubmitFrame(&frame);
      ApiServer_SubmitFrame(&frame);
      if (xQueueSend(analyzerFrameQueue, &frame, 0U) != pdPASS)
      {
        CanopenAnalyzer_AddQueueOverflow();
      }
      if (xQueueSend(guiFrameQueue, &frame, 0U) != pdPASS)
      {
        CanControl_AddGuiOverflow();
      }
    }
    CanLogger_ProducerPoll();
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

/* USER CODE BEGIN Header_StartAnalyzerTask */
/**
* @brief Function implementing the analyzerTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartAnalyzerTask */
void StartAnalyzerTask(void const * argument)
{
  /* USER CODE BEGIN StartAnalyzerTask */
  CanFrameRecord frame;
  (void)argument;
  for(;;)
  {
    if (xQueueReceive(analyzerFrameQueue, &frame, pdMS_TO_TICKS(20U)) == pdPASS)
    {
      CanopenAnalyzer_ProcessFrame(&frame);
      while (xQueueReceive(analyzerFrameQueue, &frame, 0U) == pdPASS)
      {
        CanopenAnalyzer_ProcessFrame(&frame);
      }
    }
    CanopenAnalyzer_Update(CanCapture_GetTimestampUs());
  }
  /* USER CODE END StartAnalyzerTask */
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
static void StartStorageTask(void *argument)
{
  (void)argument;
  CanLogger_StorageTask();
}

static void StartCanTxTask(void *argument)
{
  (void)argument;
  CanTxManager_Task();
}

/* USER CODE END Application */

