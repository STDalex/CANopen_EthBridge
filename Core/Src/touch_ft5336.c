#include "touch_ft5336.h"

#include "i2c.h"

#define FT5336_I2C_ADDRESS  0x70U
#define FT5336_TD_STATUS    0x02U
#define FT5336_P1_XH        0x03U

bool TouchFT5336_Read(uint16_t *x, uint16_t *y, bool *pressed)
{
  uint8_t points;
  uint8_t data[4];
  uint16_t raw_x;
  uint16_t raw_y;

  if ((x == NULL) || (y == NULL) || (pressed == NULL))
  {
    return false;
  }

  if (HAL_I2C_Mem_Read(&hi2c3, FT5336_I2C_ADDRESS, FT5336_TD_STATUS,
                       I2C_MEMADD_SIZE_8BIT, &points, 1U, 5U) != HAL_OK)
  {
    *pressed = false;
    return false;
  }

  if ((points & 0x0FU) == 0U)
  {
    *pressed = false;
    return true;
  }

  if (HAL_I2C_Mem_Read(&hi2c3, FT5336_I2C_ADDRESS, FT5336_P1_XH,
                       I2C_MEMADD_SIZE_8BIT, data, sizeof(data), 5U) != HAL_OK)
  {
    *pressed = false;
    return false;
  }

  raw_x = (uint16_t)(((uint16_t)(data[0] & 0x0FU) << 8) | data[1]);
  raw_y = (uint16_t)(((uint16_t)(data[2] & 0x0FU) << 8) | data[3]);
  /* The controller axes are swapped relative to the 480x272 LCD. */
  *x = raw_y;
  *y = raw_x;
  if (*x > 479U)
  {
    *x = 479U;
  }
  if (*y > 271U)
  {
    *y = 271U;
  }
  *pressed = true;
  return true;
}
