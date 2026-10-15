#include "can_capture.h"

#include "FreeRTOS.h"
#include "can.h"
#include "cmsis_os.h"
#include "main.h"
#include "task.h"

#define CAN_RING_LENGTH       1024U
#define CAN_RING_MASK         (CAN_RING_LENGTH - 1U)
#define AUTOBAUD_DWELL_MS     700U
#define AUTOBAUD_MIN_FRAMES   3U
#define SYNC_ERROR_LIMIT      16U
#define SYNC_LOSS_TIME_MS     2000U

typedef struct
{
  uint32_t bitrate_kbps;
  uint32_t prescaler;
  uint32_t time_seg1;
  uint32_t time_seg2;
} CanTiming;

static const CanTiming can_timings[] =
{
  {1000U,   3U, CAN_BS1_13TQ, CAN_BS2_2TQ},
  { 800U,   4U, CAN_BS1_12TQ, CAN_BS2_2TQ},
  { 500U,   6U, CAN_BS1_13TQ, CAN_BS2_2TQ},
  { 250U,  12U, CAN_BS1_13TQ, CAN_BS2_2TQ},
  { 125U,  24U, CAN_BS1_13TQ, CAN_BS2_2TQ},
  { 100U,  30U, CAN_BS1_13TQ, CAN_BS2_2TQ},
  {  50U,  60U, CAN_BS1_13TQ, CAN_BS2_2TQ},
  {  20U, 150U, CAN_BS1_13TQ, CAN_BS2_2TQ},
  {  10U, 300U, CAN_BS1_13TQ, CAN_BS2_2TQ}
};

static CanFrameRecord frame_ring[CAN_RING_LENGTH];
static volatile uint32_t ring_write;
static volatile uint32_t ring_read;
static volatile uint32_t received_count;
static volatile uint32_t ring_overflow_count;
static volatile uint32_t gui_overflow_count;
static volatile uint32_t error_count;
static volatile uint32_t last_valid_tick;
static volatile uint32_t requested_bitrate;
static volatile uint32_t active_bitrate;
static volatile CanSyncState sync_state;
static TaskHandle_t consumer_task;

static void TimestampTimer_Init(void)
{
  RCC_ClkInitTypeDef clocks;
  uint32_t flash_latency;
  uint32_t timer_clock;

  HAL_RCC_GetClockConfig(&clocks, &flash_latency);
  timer_clock = HAL_RCC_GetPCLK1Freq();
  if (clocks.APB1CLKDivider != RCC_HCLK_DIV1)
  {
    timer_clock *= 2U;
  }

  __HAL_RCC_TIM2_CLK_ENABLE();
  TIM2->CR1 = 0U;
  TIM2->PSC = (timer_clock / 1000000U) - 1U;
  TIM2->ARR = 0xFFFFFFFFU;
  TIM2->EGR = TIM_EGR_UG;
  TIM2->CNT = 0U;
  TIM2->CR1 = TIM_CR1_CEN;
}

static const CanTiming *FindTiming(uint32_t bitrate_kbps)
{
  uint32_t i;
  for (i = 0U; i < (sizeof(can_timings) / sizeof(can_timings[0])); ++i)
  {
    if (can_timings[i].bitrate_kbps == bitrate_kbps)
    {
      return &can_timings[i];
    }
  }
  return NULL;
}

static bool ConfigureReceiver(const CanTiming *timing)
{
  CAN_FilterTypeDef filter = {0};

  HAL_NVIC_DisableIRQ(CAN1_RX0_IRQn);
  HAL_NVIC_DisableIRQ(CAN1_SCE_IRQn);
  (void)HAL_CAN_Stop(&hcan1);

  hcan1.Init.Prescaler = timing->prescaler;
  hcan1.Init.SyncJumpWidth = CAN_SJW_1TQ;
  hcan1.Init.TimeSeg1 = timing->time_seg1;
  hcan1.Init.TimeSeg2 = timing->time_seg2;
  hcan1.Init.Mode = CAN_MODE_SILENT;
  hcan1.Init.AutoBusOff = ENABLE;
  hcan1.Init.AutoWakeUp = DISABLE;
  hcan1.Init.AutoRetransmission = DISABLE;
  hcan1.Init.ReceiveFifoLocked = ENABLE;
  hcan1.Init.TransmitFifoPriority = DISABLE;

  if (HAL_CAN_Init(&hcan1) != HAL_OK)
  {
    return false;
  }

  filter.FilterBank = 0U;
  filter.FilterMode = CAN_FILTERMODE_IDMASK;
  filter.FilterScale = CAN_FILTERSCALE_32BIT;
  filter.FilterIdHigh = 0U;
  filter.FilterIdLow = 0U;
  filter.FilterMaskIdHigh = 0U;
  filter.FilterMaskIdLow = 0U;
  filter.FilterFIFOAssignment = CAN_FILTER_FIFO0;
  filter.FilterActivation = ENABLE;
  filter.SlaveStartFilterBank = 14U;
  if (HAL_CAN_ConfigFilter(&hcan1, &filter) != HAL_OK)
  {
    return false;
  }
  if (HAL_CAN_Start(&hcan1) != HAL_OK)
  {
    return false;
  }

  CAN1->IER = CAN_IER_FMPIE0 | CAN_IER_FOVIE0 | CAN_IER_EWGIE |
              CAN_IER_EPVIE | CAN_IER_BOFIE | CAN_IER_LECIE | CAN_IER_ERRIE;
  active_bitrate = timing->bitrate_kbps;
  HAL_NVIC_EnableIRQ(CAN1_RX0_IRQn);
  HAL_NVIC_EnableIRQ(CAN1_SCE_IRQn);
  return true;
}

void CanCapture_Init(void)
{
  ring_write = 0U;
  ring_read = 0U;
  received_count = 0U;
  ring_overflow_count = 0U;
  gui_overflow_count = 0U;
  error_count = 0U;
  last_valid_tick = HAL_GetTick();
  requested_bitrate = 0U;
  active_bitrate = 0U;
  sync_state = CAN_SYNC_SEARCHING;
  consumer_task = NULL;
  TimestampTimer_Init();
}

void CanCapture_SetConsumerTask(void *task_handle)
{
  taskENTER_CRITICAL();
  consumer_task = (TaskHandle_t)task_handle;
  taskEXIT_CRITICAL();
}

bool CanCapture_Pop(CanFrameRecord *frame)
{
  uint32_t read_index = ring_read;
  if ((frame == NULL) || (read_index == ring_write))
  {
    return false;
  }

  *frame = frame_ring[read_index & CAN_RING_MASK];
  __DMB();
  ring_read = read_index + 1U;
  return true;
}

void CanCapture_RxIRQHandler(void)
{
  BaseType_t higher_priority_task_woken = pdFALSE;

  while ((CAN1->RF0R & CAN_RF0R_FMP0) != 0U)
  {
    uint32_t write_index = ring_write;
    uint32_t rir = CAN1->sFIFOMailBox[0].RIR;
    uint32_t rdtr = CAN1->sFIFOMailBox[0].RDTR;
    uint32_t rdlr = CAN1->sFIFOMailBox[0].RDLR;
    uint32_t rdhr = CAN1->sFIFOMailBox[0].RDHR;

    if ((write_index - ring_read) < CAN_RING_LENGTH)
    {
      CanFrameRecord *frame = &frame_ring[write_index & CAN_RING_MASK];
      frame->timestamp_us = TIM2->CNT;
      frame->flags = 0U;
      if ((rir & CAN_RI0R_IDE) != 0U)
      {
        frame->flags |= CAN_FRAME_FLAG_EXTENDED;
        frame->id = (rir >> CAN_RI0R_EXID_Pos) & 0x1FFFFFFFU;
      }
      else
      {
        frame->id = (rir >> CAN_RI0R_STID_Pos) & 0x7FFU;
      }
      if ((rir & CAN_RI0R_RTR) != 0U)
      {
        frame->flags |= CAN_FRAME_FLAG_REMOTE;
      }
      frame->dlc = (uint8_t)(rdtr & CAN_RDT0R_DLC);
      if (frame->dlc > 8U)
      {
        frame->dlc = 8U;
      }
      frame->data[0] = (uint8_t)rdlr;
      frame->data[1] = (uint8_t)(rdlr >> 8);
      frame->data[2] = (uint8_t)(rdlr >> 16);
      frame->data[3] = (uint8_t)(rdlr >> 24);
      frame->data[4] = (uint8_t)rdhr;
      frame->data[5] = (uint8_t)(rdhr >> 8);
      frame->data[6] = (uint8_t)(rdhr >> 16);
      frame->data[7] = (uint8_t)(rdhr >> 24);
      __DMB();
      ring_write = write_index + 1U;
      ++received_count;
      last_valid_tick = HAL_GetTick();
    }
    else
    {
      ++ring_overflow_count;
    }

    CAN1->RF0R = CAN_RF0R_RFOM0;
  }

  if ((CAN1->RF0R & CAN_RF0R_FOVR0) != 0U)
  {
    CAN1->RF0R = CAN_RF0R_FOVR0;
    ++ring_overflow_count;
  }

  if (consumer_task != NULL)
  {
    vTaskNotifyGiveFromISR(consumer_task, &higher_priority_task_woken);
    portYIELD_FROM_ISR(higher_priority_task_woken);
  }
}

void CanCapture_ErrorIRQHandler(void)
{
  ++error_count;
  if ((CAN1->RF0R & CAN_RF0R_FOVR0) != 0U)
  {
    CAN1->RF0R = CAN_RF0R_FOVR0;
    ++ring_overflow_count;
  }
  CAN1->ESR |= CAN_ESR_LEC;
  CAN1->MSR = CAN_MSR_ERRI;
}

void CanControl_RequestBitrate(uint32_t bitrate_kbps)
{
  if ((bitrate_kbps == 0U) || (FindTiming(bitrate_kbps) != NULL))
  {
    taskENTER_CRITICAL();
    requested_bitrate = bitrate_kbps;
    taskEXIT_CRITICAL();
  }
}

void CanControl_GetStatus(CanCaptureStatus *status)
{
  if (status == NULL)
  {
    return;
  }
  taskENTER_CRITICAL();
  status->bitrate_kbps = active_bitrate;
  status->state = sync_state;
  status->received = received_count;
  status->ring_overflows = ring_overflow_count;
  status->gui_overflows = gui_overflow_count;
  status->errors = error_count;
  taskEXIT_CRITICAL();
}

void CanControl_AddGuiOverflow(void)
{
  taskENTER_CRITICAL();
  ++gui_overflow_count;
  taskEXIT_CRITICAL();
}

void CanControl_Task(void)
{
  uint32_t scan_index = 0U;
  uint32_t observed_request = 0xFFFFFFFFU;
  uint32_t trial_start_frames = 0U;
  uint32_t trial_start_errors = 0U;
  uint32_t trial_started = 0U;
  bool trial_active = false;

  for (;;)
  {
    uint32_t request = requested_bitrate;
    uint32_t now = HAL_GetTick();

    if (request != observed_request)
    {
      observed_request = request;
      trial_active = false;
      scan_index = 0U;
    }

    if (request != 0U)
    {
      if (!trial_active)
      {
        const CanTiming *manual_timing = FindTiming(request);
        if ((manual_timing != NULL) && ConfigureReceiver(manual_timing))
        {
          sync_state = CAN_SYNC_MANUAL;
        }
        trial_active = true;
      }
      osDelay(100U);
      continue;
    }

    if (sync_state == CAN_SYNC_LOCKED)
    {
      if (((error_count - trial_start_errors) >= SYNC_ERROR_LIMIT) &&
          ((now - last_valid_tick) >= SYNC_LOSS_TIME_MS))
      {
        sync_state = CAN_SYNC_SEARCHING;
        trial_active = false;
        scan_index = 0U;
      }
      osDelay(100U);
      continue;
    }

    if (!trial_active)
    {
      trial_start_frames = received_count;
      trial_start_errors = error_count;
      trial_started = now;
      (void)ConfigureReceiver(&can_timings[scan_index]);
      sync_state = CAN_SYNC_SEARCHING;
      trial_active = true;
    }
    else if ((now - trial_started) >= AUTOBAUD_DWELL_MS)
    {
      if ((received_count - trial_start_frames) >= AUTOBAUD_MIN_FRAMES)
      {
        sync_state = CAN_SYNC_LOCKED;
        taskENTER_CRITICAL();
        error_count = 0U;
        taskEXIT_CRITICAL();
        trial_start_errors = 0U;
      }
      else
      {
        scan_index = (scan_index + 1U) % (sizeof(can_timings) / sizeof(can_timings[0]));
        trial_active = false;
      }
    }

    osDelay(20U);
  }
}
