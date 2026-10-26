#include "sdmmc.h"

#define SD_DETECT_DEBOUNCE_MS 50U

SD_HandleTypeDef hsd1;
DMA_HandleTypeDef hdma_sdmmc1_rx;
DMA_HandleTypeDef hdma_sdmmc1_tx;

bool StorageSd_CardPresent(void)
{
  static bool initialized;
  static bool stable_present;
  static bool sampled_present;
  static uint32_t changed_tick;
  bool present = HAL_GPIO_ReadPin(SD_DETECT_GPIO_Port,
                                 SD_DETECT_Pin) == GPIO_PIN_RESET;
  uint32_t now = HAL_GetTick();

  if (!initialized)
  {
    initialized = true;
    stable_present = present;
    sampled_present = present;
    changed_tick = now;
  }
  else if (present != sampled_present)
  {
    sampled_present = present;
    changed_tick = now;
  }
  else if ((present != stable_present) &&
           ((uint32_t)(now - changed_tick) >= SD_DETECT_DEBOUNCE_MS))
  {
    stable_present = present;
  }
  return stable_present;
}

HAL_StatusTypeDef StorageSd_Init(void)
{
  if (!StorageSd_CardPresent())
  {
    return HAL_ERROR;
  }

  hsd1.Instance = SDMMC1;
  hsd1.Init.ClockEdge = SDMMC_CLOCK_EDGE_RISING;
  hsd1.Init.ClockBypass = SDMMC_CLOCK_BYPASS_DISABLE;
  hsd1.Init.ClockPowerSave = SDMMC_CLOCK_POWER_SAVE_DISABLE;
  hsd1.Init.BusWide = SDMMC_BUS_WIDE_1B;
  hsd1.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_DISABLE;
  hsd1.Init.ClockDiv = 2U;

  if (HAL_SD_Init(&hsd1) != HAL_OK)
  {
    return HAL_ERROR;
  }
  if (HAL_SD_ConfigWideBusOperation(&hsd1, SDMMC_BUS_WIDE_4B) != HAL_OK)
  {
    (void)HAL_SD_DeInit(&hsd1);
    return HAL_ERROR;
  }
  return HAL_OK;
}

void StorageSd_DeInit(void)
{
  if (hsd1.Instance == SDMMC1)
  {
    (void)HAL_SD_DeInit(&hsd1);
  }
}

void HAL_SD_MspInit(SD_HandleTypeDef *sdHandle)
{
  GPIO_InitTypeDef gpio = {0};

  if ((sdHandle == NULL) || (sdHandle->Instance != SDMMC1))
  {
    return;
  }

  __HAL_RCC_SDMMC1_CLK_ENABLE();
  __HAL_RCC_DMA2_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();

  gpio.Pin = GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12;
  gpio.Mode = GPIO_MODE_AF_PP;
  gpio.Pull = GPIO_PULLUP;
  gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  gpio.Alternate = GPIO_AF12_SDMMC1;
  HAL_GPIO_Init(GPIOC, &gpio);

  gpio.Pin = GPIO_PIN_2;
  HAL_GPIO_Init(GPIOD, &gpio);

  hdma_sdmmc1_rx.Instance = DMA2_Stream3;
  hdma_sdmmc1_rx.Init.Channel = DMA_CHANNEL_4;
  hdma_sdmmc1_rx.Init.Direction = DMA_PERIPH_TO_MEMORY;
  hdma_sdmmc1_rx.Init.PeriphInc = DMA_PINC_DISABLE;
  hdma_sdmmc1_rx.Init.MemInc = DMA_MINC_ENABLE;
  hdma_sdmmc1_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
  hdma_sdmmc1_rx.Init.MemDataAlignment = DMA_MDATAALIGN_WORD;
  hdma_sdmmc1_rx.Init.Mode = DMA_PFCTRL;
  hdma_sdmmc1_rx.Init.Priority = DMA_PRIORITY_VERY_HIGH;
  hdma_sdmmc1_rx.Init.FIFOMode = DMA_FIFOMODE_ENABLE;
  hdma_sdmmc1_rx.Init.FIFOThreshold = DMA_FIFO_THRESHOLD_FULL;
  hdma_sdmmc1_rx.Init.MemBurst = DMA_MBURST_INC4;
  hdma_sdmmc1_rx.Init.PeriphBurst = DMA_PBURST_INC4;
  if (HAL_DMA_Init(&hdma_sdmmc1_rx) != HAL_OK)
  {
    Error_Handler();
  }
  __HAL_LINKDMA(sdHandle, hdmarx, hdma_sdmmc1_rx);

  hdma_sdmmc1_tx.Instance = DMA2_Stream6;
  hdma_sdmmc1_tx.Init.Channel = DMA_CHANNEL_4;
  hdma_sdmmc1_tx.Init.Direction = DMA_MEMORY_TO_PERIPH;
  hdma_sdmmc1_tx.Init.PeriphInc = DMA_PINC_DISABLE;
  hdma_sdmmc1_tx.Init.MemInc = DMA_MINC_ENABLE;
  hdma_sdmmc1_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
  hdma_sdmmc1_tx.Init.MemDataAlignment = DMA_MDATAALIGN_WORD;
  hdma_sdmmc1_tx.Init.Mode = DMA_PFCTRL;
  hdma_sdmmc1_tx.Init.Priority = DMA_PRIORITY_VERY_HIGH;
  hdma_sdmmc1_tx.Init.FIFOMode = DMA_FIFOMODE_ENABLE;
  hdma_sdmmc1_tx.Init.FIFOThreshold = DMA_FIFO_THRESHOLD_FULL;
  hdma_sdmmc1_tx.Init.MemBurst = DMA_MBURST_INC4;
  hdma_sdmmc1_tx.Init.PeriphBurst = DMA_PBURST_INC4;
  if (HAL_DMA_Init(&hdma_sdmmc1_tx) != HAL_OK)
  {
    Error_Handler();
  }
  __HAL_LINKDMA(sdHandle, hdmatx, hdma_sdmmc1_tx);

  /* DMA completion must pre-empt SDMMC DATAEND handling. Both priorities are
     valid for FreeRTOS FromISR APIs used by the disk I/O completion signal. */
  HAL_NVIC_SetPriority(DMA2_Stream3_IRQn, 5U, 0U);
  HAL_NVIC_EnableIRQ(DMA2_Stream3_IRQn);
  HAL_NVIC_SetPriority(DMA2_Stream6_IRQn, 5U, 0U);
  HAL_NVIC_EnableIRQ(DMA2_Stream6_IRQn);
  HAL_NVIC_SetPriority(SDMMC1_IRQn, 6U, 0U);
  HAL_NVIC_EnableIRQ(SDMMC1_IRQn);
}

void HAL_SD_MspDeInit(SD_HandleTypeDef *sdHandle)
{
  if ((sdHandle == NULL) || (sdHandle->Instance != SDMMC1))
  {
    return;
  }

  HAL_NVIC_DisableIRQ(SDMMC1_IRQn);
  HAL_NVIC_DisableIRQ(DMA2_Stream3_IRQn);
  HAL_NVIC_DisableIRQ(DMA2_Stream6_IRQn);
  (void)HAL_DMA_DeInit(&hdma_sdmmc1_rx);
  (void)HAL_DMA_DeInit(&hdma_sdmmc1_tx);
  __HAL_RCC_SDMMC1_CLK_DISABLE();
  HAL_GPIO_DeInit(GPIOC, GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 |
                         GPIO_PIN_11 | GPIO_PIN_12);
  HAL_GPIO_DeInit(GPIOD, GPIO_PIN_2);
}
