#include "can_capture.h"

#include "FreeRTOS.h"
#include "app_watchdog.h"
#include "can.h"
#include "can_tx_manager.h"
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
static volatile uint32_t received_statistics_base;
static volatile uint32_t ring_overflow_statistics_base;
static volatile uint32_t gui_overflow_statistics_base;
static volatile uint32_t error_statistics_base;
static volatile uint64_t estimated_busy_time_ns;
static volatile uint64_t load_window_busy_base;
static volatile uint32_t load_window_started_tick;
static volatile uint16_t current_bus_load_permille;
static volatile uint32_t timestamp_epoch_us;
static volatile uint32_t capture_time_high;
static volatile uint32_t last_valid_tick;
static volatile uint32_t requested_bitrate;
static volatile uint32_t active_bitrate;
static volatile CanSyncState sync_state;
static volatile CanOperatingMode operating_mode;
static volatile CanSafetyReason safety_reason;
static volatile bool requested_active;
static volatile bool tx_gate_enabled;
static TaskHandle_t consumer_task;

void CanSafety_EmergencyDisableTx(void)
{
  uint32_t shift = 9U * 2U;
  RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
  (void)RCC->AHB1ENR;
  GPIOB->BSRR = CAN1_TX_Pin;
  GPIOB->MODER = (GPIOB->MODER & ~(3UL << shift)) | (1UL << shift);
  GPIOB->OTYPER &= ~CAN1_TX_Pin;
  GPIOB->PUPDR &= ~(3UL << shift);
  tx_gate_enabled = false;
}

static void TxGate_Enable(void)
{
  GPIO_InitTypeDef gpio = {0};
  gpio.Pin = CAN1_TX_Pin;
  gpio.Mode = GPIO_MODE_AF_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  gpio.Alternate = GPIO_AF9_CAN1;
  HAL_GPIO_Init(CAN1_TX_GPIO_Port, &gpio);
  tx_gate_enabled = true;
}

static uint32_t NanosecondsPerBit(uint32_t bitrate_kbps)
{
  switch (bitrate_kbps)
  {
    case 1000U: return 1000U;
    case 800U: return 1250U;
    case 500U: return 2000U;
    case 250U: return 4000U;
    case 125U: return 8000U;
    case 100U: return 10000U;
    case 50U: return 20000U;
    case 20U: return 50000U;
    case 10U: return 100000U;
    default: return 0U;
  }
}

static uint32_t EstimateFrameBits(uint32_t rir, uint32_t rdtr)
{
  bool extended = (rir & CAN_RI0R_IDE) != 0U;
  bool remote = (rir & CAN_RI0R_RTR) != 0U;
  uint32_t dlc = rdtr & CAN_RDT0R_DLC;
  uint32_t data_bits;
  uint32_t stuffable_bits;

  if (dlc > 8U) dlc = 8U;
  data_bits = remote ? 0U : (dlc * 8U);
  stuffable_bits = (extended ? 54U : 34U) + data_bits;
  return (extended ? 67U : 47U) + data_bits + ((stuffable_bits - 1U) / 4U);
}

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
  TIM2->SR = 0U;
  TIM2->DIER = TIM_DIER_UIE;
  TIM2->CNT = 0U;
  HAL_NVIC_SetPriority(TIM2_IRQn, 6U, 0U);
  HAL_NVIC_EnableIRQ(TIM2_IRQn);
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

static bool ConfigureController(const CanTiming *timing, bool active)
{
  CAN_FilterTypeDef filter = {0};

  CanSafety_EmergencyDisableTx();
  HAL_NVIC_DisableIRQ(CAN1_RX0_IRQn);
  HAL_NVIC_DisableIRQ(CAN1_SCE_IRQn);
  (void)HAL_CAN_Stop(&hcan1);

  hcan1.Init.Prescaler = timing->prescaler;
  hcan1.Init.SyncJumpWidth = CAN_SJW_1TQ;
  hcan1.Init.TimeSeg1 = timing->time_seg1;
  hcan1.Init.TimeSeg2 = timing->time_seg2;
  hcan1.Init.Mode = active ? CAN_MODE_NORMAL : CAN_MODE_SILENT;
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
  if (active)
  {
    TxGate_Enable();
  }
  else
  {
    CanSafety_EmergencyDisableTx();
  }
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
  received_statistics_base = 0U;
  ring_overflow_statistics_base = 0U;
  gui_overflow_statistics_base = 0U;
  error_statistics_base = 0U;
  estimated_busy_time_ns = 0U;
  load_window_busy_base = 0U;
  load_window_started_tick = HAL_GetTick();
  current_bus_load_permille = 0U;
  last_valid_tick = HAL_GetTick();
  requested_bitrate = 0U;
  active_bitrate = 0U;
  sync_state = CAN_SYNC_SEARCHING;
  operating_mode = CAN_OPERATING_SILENT;
  safety_reason = CAN_SAFETY_POWER_ON;
  requested_active = false;
  tx_gate_enabled = false;
  consumer_task = NULL;
  CanSafety_EmergencyDisableTx();
  TimestampTimer_Init();
  timestamp_epoch_us = TIM2->CNT;
  capture_time_high = 0U;
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
    uint32_t nanoseconds_per_bit = NanosecondsPerBit(active_bitrate);

    if (nanoseconds_per_bit != 0U)
    {
      estimated_busy_time_ns +=
        (uint64_t)EstimateFrameBits(rir, rdtr) * nanoseconds_per_bit;
    }

    if ((write_index - ring_read) < CAN_RING_LENGTH)
    {
      CanFrameRecord *frame = &frame_ring[write_index & CAN_RING_MASK];
      uint32_t capture_high = capture_time_high;
      uint32_t capture_time_us = TIM2->CNT;
      if ((TIM2->SR & TIM_SR_UIF) != 0U)
      {
        capture_high += 1U;
      }
      frame->timestamp_us = capture_time_us - timestamp_epoch_us;
      frame->capture_time_us = capture_time_us;
      frame->capture_time_high = capture_high;
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
  if ((CAN1->ESR & CAN_ESR_BOFF) != 0U)
  {
    CanSafety_EmergencyDisableTx();
    requested_active = false;
    operating_mode = CAN_OPERATING_ACTIVE_PENDING;
    safety_reason = CAN_SAFETY_BUS_OFF;
  }
  if ((CAN1->RF0R & CAN_RF0R_FOVR0) != 0U)
  {
    CAN1->RF0R = CAN_RF0R_FOVR0;
    ++ring_overflow_count;
  }
  CAN1->ESR |= CAN_ESR_LEC;
  CAN1->MSR = CAN_MSR_ERRI;
}

void CanCapture_TimerIRQHandler(void)
{
  if ((TIM2->SR & TIM_SR_UIF) != 0U)
  {
    TIM2->SR &= ~TIM_SR_UIF;
    ++capture_time_high;
  }
}

void CanControl_RequestBitrate(uint32_t bitrate_kbps)
{
  if ((bitrate_kbps == 0U) || (FindTiming(bitrate_kbps) != NULL))
  {
    if (operating_mode != CAN_OPERATING_SILENT)
    {
      CanControl_ForceSilent(CAN_SAFETY_BITRATE_CHANGE);
    }
    taskENTER_CRITICAL();
    requested_bitrate = bitrate_kbps;
    taskEXIT_CRITICAL();
  }
}

bool CanControl_RequestActive(bool confirmed)
{
  bool accepted = false;
  if (!confirmed) return false;
  taskENTER_CRITICAL();
  if ((active_bitrate != 0U) && (sync_state != CAN_SYNC_SEARCHING) &&
      (operating_mode == CAN_OPERATING_SILENT))
  {
    requested_active = true;
    operating_mode = CAN_OPERATING_ACTIVE_PENDING;
    safety_reason = CAN_SAFETY_USER;
    accepted = true;
  }
  taskEXIT_CRITICAL();
  return accepted;
}

void CanControl_RequestSilent(void)
{
  CanControl_ForceSilent(CAN_SAFETY_USER);
}

void CanControl_ForceSilent(CanSafetyReason reason)
{
  CanSafety_EmergencyDisableTx();
  taskENTER_CRITICAL();
  requested_active = false;
  safety_reason = reason;
  taskEXIT_CRITICAL();
  CanTx_CancelAll();
}

bool CanControl_IsActive(void)
{
  return (operating_mode == CAN_OPERATING_ACTIVE) && tx_gate_enabled &&
         ((CAN1->ESR & CAN_ESR_BOFF) == 0U);
}

const char *CanControl_OperatingModeName(CanOperatingMode mode)
{
  switch (mode)
  {
    case CAN_OPERATING_ACTIVE_PENDING: return "active_pending";
    case CAN_OPERATING_ACTIVE: return "active";
    default: return "silent";
  }
}

const char *CanControl_SafetyReasonName(CanSafetyReason reason)
{
  switch (reason)
  {
    case CAN_SAFETY_USER: return "user";
    case CAN_SAFETY_BITRATE_CHANGE: return "bitrate_change";
    case CAN_SAFETY_BUS_OFF: return "bus_off";
    case CAN_SAFETY_CONTROLLER_ERROR: return "controller_error";
    default: return "power_on";
  }
}

void CanControl_GetStatus(CanCaptureStatus *status)
{
  uint32_t now;

  if (status == NULL)
  {
    return;
  }
  now = HAL_GetTick();
  taskENTER_CRITICAL();
  if ((uint32_t)(now - load_window_started_tick) >= 1000U)
  {
    uint32_t elapsed_ms = now - load_window_started_tick;
    uint64_t busy_time_ns = estimated_busy_time_ns - load_window_busy_base;
    uint64_t load_permille = busy_time_ns / ((uint64_t)elapsed_ms * 1000U);
    if (load_permille > 1000U) load_permille = 1000U;
    current_bus_load_permille = (uint16_t)load_permille;
    load_window_busy_base = estimated_busy_time_ns;
    load_window_started_tick = now;
  }
  status->bitrate_kbps = active_bitrate;
  status->state = sync_state;
  status->received = received_count - received_statistics_base;
  status->ring_overflows = ring_overflow_count - ring_overflow_statistics_base;
  status->gui_overflows = gui_overflow_count - gui_overflow_statistics_base;
  status->errors = error_count - error_statistics_base;
  status->bus_load_permille = current_bus_load_permille;
  status->operating_mode = operating_mode;
  status->safety_reason = safety_reason;
  status->tx_gate_enabled = tx_gate_enabled;
  taskEXIT_CRITICAL();
}

void CanControl_AddGuiOverflow(void)
{
  taskENTER_CRITICAL();
  ++gui_overflow_count;
  taskEXIT_CRITICAL();
}

void CanControl_ResetStatistics(void)
{
  taskENTER_CRITICAL();
  ring_read = ring_write;
  timestamp_epoch_us = TIM2->CNT;
  received_statistics_base = received_count;
  ring_overflow_statistics_base = ring_overflow_count;
  gui_overflow_statistics_base = gui_overflow_count;
  error_statistics_base = error_count;
  load_window_busy_base = estimated_busy_time_ns;
  load_window_started_tick = HAL_GetTick();
  current_bus_load_permille = 0U;
  taskEXIT_CRITICAL();
}

uint32_t CanCapture_GetTimestampUs(void)
{
  return TIM2->CNT - timestamp_epoch_us;
}

void CanCapture_GetTimestamps(uint32_t *timestamp_us,
                              uint32_t *capture_time_us,
                              uint32_t *capture_time_high_value)
{
  uint32_t capture_low;
  uint32_t capture_high;
  uint32_t epoch;

  taskENTER_CRITICAL();
  capture_high = capture_time_high;
  capture_low = TIM2->CNT;
  if ((TIM2->SR & TIM_SR_UIF) != 0U)
  {
    ++capture_high;
  }
  epoch = timestamp_epoch_us;
  taskEXIT_CRITICAL();

  if (timestamp_us != NULL) *timestamp_us = capture_low - epoch;
  if (capture_time_us != NULL) *capture_time_us = capture_low;
  if (capture_time_high_value != NULL) *capture_time_high_value = capture_high;
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

    AppWatchdog_Heartbeat(APP_WATCHDOG_CAN_CONTROL);

    if (requested_active)
    {
      if (operating_mode != CAN_OPERATING_ACTIVE)
      {
        const CanTiming *active_timing = FindTiming(active_bitrate);
        if ((active_timing != NULL) && ConfigureController(active_timing, true))
        {
          operating_mode = CAN_OPERATING_ACTIVE;
        }
        else
        {
          requested_active = false;
          operating_mode = CAN_OPERATING_SILENT;
          safety_reason = CAN_SAFETY_CONTROLLER_ERROR;
          sync_state = CAN_SYNC_SEARCHING;
          active_bitrate = 0U;
          observed_request = 0xFFFFFFFFU;
          trial_active = false;
          CanSafety_EmergencyDisableTx();
        }
      }
      osDelay(10U);
      continue;
    }
    else if (operating_mode != CAN_OPERATING_SILENT)
    {
      const CanTiming *silent_timing = FindTiming(active_bitrate);
      CanTx_CancelAll();
      if ((silent_timing == NULL) || !ConfigureController(silent_timing, false))
      {
        safety_reason = CAN_SAFETY_CONTROLLER_ERROR;
        sync_state = CAN_SYNC_SEARCHING;
        active_bitrate = 0U;
        observed_request = 0xFFFFFFFFU;
        trial_active = false;
        CanSafety_EmergencyDisableTx();
      }
      operating_mode = CAN_OPERATING_SILENT;
      osDelay(10U);
      continue;
    }

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
        if ((manual_timing != NULL) && ConfigureController(manual_timing, false))
        {
          sync_state = CAN_SYNC_MANUAL;
          trial_active = true;
        }
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
      (void)ConfigureController(&can_timings[scan_index], false);
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
