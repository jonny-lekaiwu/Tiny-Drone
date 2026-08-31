#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "i2cdev.h"

#define BAROMETER_I2C_ADDR_SDO_LOW 0x76
#define BAROMETER_PRODUCT_ID       0x10

/* Keep the proven DPS368-only initialization path by default. Enable this
 * explicitly only for boards that must support pin-compatible SPL06 parts. */
#ifndef BAROMETER_ENABLE_SPL06_COMPATIBILITY
#define BAROMETER_ENABLE_SPL06_COMPATIBILITY 0
#endif

typedef struct {
  I2C_Dev *bus;
  uint8_t address;
  int16_t c0;
  int16_t c1;
  int32_t c00;
  int32_t c10;
  int16_t c01;
  int16_t c11;
  int16_t c20;
  int16_t c21;
  int16_t c30;
  float pressureScale;
  float temperatureScale;
} barometer_t;

bool barometerInit(barometer_t *dev, I2C_Dev *bus, uint8_t address, uint8_t *productId);
bool barometerRead(barometer_t *dev, float *pressurePa, float *temperatureC);
