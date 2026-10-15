#ifndef TOUCH_FT5336_H
#define TOUCH_FT5336_H

#include <stdbool.h>
#include <stdint.h>

bool TouchFT5336_Read(uint16_t *x, uint16_t *y, bool *pressed);

#endif
