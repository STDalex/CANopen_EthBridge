#include "platform.h"

#include "fmc.h"
#include "main.h"

#define SDRAM_BASE_ADDRESS  0xC0000000UL
#define SDRAM_SIZE_BYTES    (8UL * 1024UL * 1024UL)
#define SDRAM_REFRESH_COUNT 1480U
#define SDRAM_MODEREG_BURST_LENGTH_1             0x0000U
#define SDRAM_MODEREG_BURST_TYPE_SEQUENTIAL      0x0000U
#define SDRAM_MODEREG_CAS_LATENCY_3               0x0030U
#define SDRAM_MODEREG_OPERATING_MODE_STANDARD     0x0000U
#define SDRAM_MODEREG_WRITEBURST_MODE_SINGLE      0x0200U

static bool SDRAM_Initialize(void)
{
  FMC_SDRAM_CommandTypeDef command = {0};
  uint32_t mode_register;

  command.CommandMode = FMC_SDRAM_CMD_CLK_ENABLE;
  command.CommandTarget = FMC_SDRAM_CMD_TARGET_BANK1;
  command.AutoRefreshNumber = 1;
  command.ModeRegisterDefinition = 0;
  if (HAL_SDRAM_SendCommand(&hsdram1, &command, 100U) != HAL_OK)
  {
    return false;
  }

  HAL_Delay(1U);

  command.CommandMode = FMC_SDRAM_CMD_PALL;
  if (HAL_SDRAM_SendCommand(&hsdram1, &command, 100U) != HAL_OK)
  {
    return false;
  }

  command.CommandMode = FMC_SDRAM_CMD_AUTOREFRESH_MODE;
  command.AutoRefreshNumber = 8;
  if (HAL_SDRAM_SendCommand(&hsdram1, &command, 100U) != HAL_OK)
  {
    return false;
  }

  mode_register = SDRAM_MODEREG_BURST_LENGTH_1 |
                  SDRAM_MODEREG_BURST_TYPE_SEQUENTIAL |
                  SDRAM_MODEREG_CAS_LATENCY_3 |
                  SDRAM_MODEREG_OPERATING_MODE_STANDARD |
                  SDRAM_MODEREG_WRITEBURST_MODE_SINGLE;
  command.CommandMode = FMC_SDRAM_CMD_LOAD_MODE;
  command.AutoRefreshNumber = 1;
  command.ModeRegisterDefinition = mode_register;
  if (HAL_SDRAM_SendCommand(&hsdram1, &command, 100U) != HAL_OK)
  {
    return false;
  }

  return HAL_SDRAM_ProgramRefreshRate(&hsdram1, SDRAM_REFRESH_COUNT) == HAL_OK;
}

static bool SDRAM_QuickTest(void)
{
  volatile uint32_t *memory = (volatile uint32_t *)SDRAM_BASE_ADDRESS;
  static const uint32_t patterns[] = {0x00000000U, 0xFFFFFFFFU, 0xA5A55A5AU, 0x5A5AA5A5U};
  uint32_t i;
  uint32_t p;

  for (p = 0U; p < (sizeof(patterns) / sizeof(patterns[0])); ++p)
  {
    for (i = 0U; i < 1024U; ++i)
    {
      memory[i] = patterns[p] ^ i;
    }
    for (i = 0U; i < 1024U; ++i)
    {
      if (memory[i] != (patterns[p] ^ i))
      {
        return false;
      }
    }
  }

  return true;
}

bool Platform_Init(void)
{
  if (!SDRAM_Initialize() || !SDRAM_QuickTest())
  {
    return false;
  }

  /* Clear the complete external RAM, including both possible frame buffers. */
  {
    volatile uint32_t *memory = (volatile uint32_t *)SDRAM_BASE_ADDRESS;
    uint32_t i;
    for (i = 0U; i < (SDRAM_SIZE_BYTES / sizeof(uint32_t)); ++i)
    {
      memory[i] = 0U;
    }
  }

  return true;
}
