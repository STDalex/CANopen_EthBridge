#include "sdmmc.h"

SD_HandleTypeDef hsd1;

bool StorageSd_CardPresent(void)
{
  return HAL_GPIO_ReadPin(SD_DETECT_GPIO_Port, SD_DETECT_Pin) == GPIO_PIN_RESET;
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
}

void HAL_SD_MspDeInit(SD_HandleTypeDef *sdHandle)
{
  if ((sdHandle == NULL) || (sdHandle->Instance != SDMMC1))
  {
    return;
  }

  __HAL_RCC_SDMMC1_CLK_DISABLE();
  HAL_GPIO_DeInit(GPIOC, GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 |
                         GPIO_PIN_11 | GPIO_PIN_12);
  HAL_GPIO_DeInit(GPIOD, GPIO_PIN_2);
}
