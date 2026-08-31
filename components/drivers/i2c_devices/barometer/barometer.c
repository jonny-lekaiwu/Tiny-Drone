#include "barometer.h"
#include "FreeRTOS.h"
#include "task.h"
#include "esp_log.h"

#define BAROMETER_REG_PRS_B2    0x00
#define BAROMETER_REG_PRS_CFG   0x06
#define BAROMETER_REG_TMP_CFG   0x07
#define BAROMETER_REG_MEAS_CFG  0x08
#define BAROMETER_REG_CFG       0x09
#define BAROMETER_REG_RESET     0x0C
#define BAROMETER_REG_PRODUCT_ID 0x0D
#define BAROMETER_REG_COEF      0x10
#define BAROMETER_REG_COEF_SRCE 0x28

#define BAROMETER_READY_SENSOR  (1U << 6)
#define BAROMETER_READY_COEF    (1U << 7)
#define BAROMETER_READY_PRESSURE (1U << 4)
#define BAROMETER_READY_TEMP    (1U << 5)

static const char *TAG = "BAROMETER";

static bool shouldLogReadFailure(void)
{
  static TickType_t lastLogTick = 0;
  static bool firstFailure = true;
  const TickType_t now = xTaskGetTickCount();

  if (firstFailure || (now - lastLogTick) >= pdMS_TO_TICKS(1000)) {
    firstFailure = false;
    lastLogTick = now;
    return true;
  }
  return false;
}

static int32_t signExtend(uint32_t value, uint8_t bits)
{
  const uint32_t sign = 1UL << (bits - 1U);
  return (int32_t)((value ^ sign) - sign);
}

static bool waitMask(barometer_t *dev, uint8_t mask, uint32_t timeoutMs)
{
  while (timeoutMs--) {
    uint8_t status = 0;
    if (i2cdevReadByte(dev->bus, dev->address, BAROMETER_REG_MEAS_CFG, &status) &&
        (status & mask) == mask) {
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  return false;
}

static bool initProfile(barometer_t *dev, I2C_Dev *bus, uint8_t address,
                        uint8_t *productId, bool compatibilityProfile)
{
  const char *profileName = compatibilityProfile ? "compatibility" : "native";

  if (!dev || !bus) {
    ESP_LOGE(TAG, "init %s profile: invalid argument dev=%p bus=%p",
             profileName, (void *)dev, (void *)bus);
    return false;
  }
  *dev = (barometer_t){.bus = bus, .address = address};

  uint8_t id = 0;
  if (!i2cdevReadByte(bus, address, BAROMETER_REG_PRODUCT_ID, &id)) {
    ESP_LOGW(TAG, "init %s profile: product ID read failed at address=0x%02X",
             profileName, address);
    return false;
  }
  if (productId) *productId = id;
  if ((id & 0xF0U) != BAROMETER_PRODUCT_ID) {
    ESP_LOGW(TAG, "init %s profile: unexpected product ID=0x%02X",
             profileName, id);
    return false;
  }

  if (!i2cdevWriteByte(bus, address, BAROMETER_REG_RESET, 0x89)) {
    ESP_LOGW(TAG, "init %s profile: soft reset write failed", profileName);
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(15));
  if (!waitMask(dev, BAROMETER_READY_SENSOR | BAROMETER_READY_COEF, 100)) {
    ESP_LOGW(TAG, "init %s profile: sensor/coefficient ready timeout", profileName);
    return false;
  }

  uint8_t coef[18];
  if (!i2cdevReadReg8(bus, address, BAROMETER_REG_COEF, sizeof(coef), coef)) {
    ESP_LOGW(TAG, "init %s profile: calibration coefficient read failed", profileName);
    return false;
  }
  dev->c0  = (int16_t)signExtend(((uint32_t)coef[0] << 4) | (coef[1] >> 4), 12);
  dev->c1  = (int16_t)signExtend(((uint32_t)(coef[1] & 0x0F) << 8) | coef[2], 12);
  dev->c00 = signExtend(((uint32_t)coef[3] << 12) | ((uint32_t)coef[4] << 4) |
                        (coef[5] >> 4), 20);
  dev->c10 = signExtend(((uint32_t)(coef[5] & 0x0F) << 16) |
                        ((uint32_t)coef[6] << 8) | coef[7], 20);
  dev->c01 = (int16_t)(((uint16_t)coef[8] << 8) | coef[9]);
  dev->c11 = (int16_t)(((uint16_t)coef[10] << 8) | coef[11]);
  dev->c20 = (int16_t)(((uint16_t)coef[12] << 8) | coef[13]);
  dev->c21 = (int16_t)(((uint16_t)coef[14] << 8) | coef[15]);
  dev->c30 = (int16_t)(((uint16_t)coef[16] << 8) | coef[17]);

  uint8_t temperatureSource = 0x80U;
  if (!compatibilityProfile) {
    uint8_t source = 0;
    if (!i2cdevReadByte(bus, address, BAROMETER_REG_COEF_SRCE, &source)) {
      ESP_LOGW(TAG, "init %s profile: coefficient source register 0x28 read failed",
               profileName);
      return false;
    }
    temperatureSource = source & 0x80U;
  }

  /* Pressure and temperature at 16 Hz with 16x oversampling. This leaves
   * enough time for both conversions to complete before the next cycle. */
  const uint8_t rate16Osr16 = (4U << 4) | 4U;
  if (!i2cdevWriteByte(bus, address, BAROMETER_REG_PRS_CFG, rate16Osr16)) {
    ESP_LOGW(TAG, "init %s profile: pressure configuration write failed", profileName);
    return false;
  }
  if (!i2cdevWriteByte(bus, address, BAROMETER_REG_TMP_CFG,
                       temperatureSource | rate16Osr16)) {
    ESP_LOGW(TAG, "init %s profile: temperature configuration write failed", profileName);
    return false;
  }
  if (!i2cdevWriteByte(bus, address, BAROMETER_REG_CFG, (1U << 2) | (1U << 3))) {
    ESP_LOGW(TAG, "init %s profile: shift configuration write failed", profileName);
    return false;
  }
  if (!i2cdevWriteByte(bus, address, BAROMETER_REG_MEAS_CFG, 0x07)) {
    ESP_LOGW(TAG, "init %s profile: continuous measurement start failed", profileName);
    return false;
  }
  dev->pressureScale = 253952.0f;
  dev->temperatureScale = 253952.0f;
  if (!waitMask(dev, BAROMETER_READY_PRESSURE | BAROMETER_READY_TEMP, 150)) {
    ESP_LOGW(TAG, "init %s profile: first pressure/temperature ready timeout", profileName);
    return false;
  }

  ESP_LOGI(TAG, "init %s profile configured, ID=0x%02X TMP_EXT=%u",
           profileName, id, temperatureSource != 0U);
  return true;
}

bool barometerInit(barometer_t *dev, I2C_Dev *bus, uint8_t address, uint8_t *productId)
{
#if BAROMETER_ENABLE_SPL06_COMPATIBILITY
  float pressurePa = 0.0f;
  float temperatureC = 0.0f;

  /* Try the native profile and validate an actual compensated sample first.
   * If it cannot produce data, reset and retry the pin-compatible profile.
   * Only the profile which passes this check is retained for later reads. */
  if (initProfile(dev, bus, address, productId, false) &&
      barometerRead(dev, &pressurePa, &temperatureC)) {
    /* The validation read clears the data-ready flags. Wait for the next
     * complete sample so the caller's first read cannot be misclassified as
     * an initialization failure. */
    if (!waitMask(dev, BAROMETER_READY_PRESSURE | BAROMETER_READY_TEMP, 150)) {
      ESP_LOGW(TAG, "init native profile: next sample ready timeout after validation");
      return false;
    }
    ESP_LOGI(TAG, "init succeeded with native profile: P=%.2fPa T=%.2fC",
             (double)pressurePa, (double)temperatureC);
    return true;
  }

  ESP_LOGW(TAG, "native profile did not produce a valid sample; trying compatibility profile");

  if (initProfile(dev, bus, address, productId, true) &&
      barometerRead(dev, &pressurePa, &temperatureC)) {
    if (!waitMask(dev, BAROMETER_READY_PRESSURE | BAROMETER_READY_TEMP, 150)) {
      ESP_LOGW(TAG, "init compatibility profile: next sample ready timeout after validation");
      return false;
    }
    ESP_LOGI(TAG, "init succeeded with compatibility profile: P=%.2fPa T=%.2fC",
             (double)pressurePa, (double)temperatureC);
    return true;
  }

  ESP_LOGE(TAG, "initialization failed: neither profile produced valid data");
  return false;
#else
  /* DPS368-only path: retain the coefficient-selected temperature source and
   * do not switch profiles because of a transient first-sample read failure. */
  return initProfile(dev, bus, address, productId, false);
#endif
}

bool barometerRead(barometer_t *dev, float *pressurePa, float *temperatureC)
{
  if (!dev || !pressurePa || !temperatureC) {
    ESP_LOGE(TAG, "read called with invalid argument: dev=%p pressure=%p temperature=%p",
             (void *)dev, (void *)pressurePa, (void *)temperatureC);
    return false;
  }

  uint8_t status = 0;
  if (!i2cdevReadByte(dev->bus, dev->address, BAROMETER_REG_MEAS_CFG, &status)) {
    if (shouldLogReadFailure()) {
      ESP_LOGW(TAG, "MEAS_CFG read failed: address=0x%02X", dev->address);
    }
    return false;
  }

  const uint8_t readyMask = BAROMETER_READY_PRESSURE | BAROMETER_READY_TEMP;
  if ((status & readyMask) != readyMask) {
    if (shouldLogReadFailure()) {
      ESP_LOGW(TAG,
               "data not ready: MEAS_CFG=0x%02X PRS_RDY=%u TMP_RDY=%u SENSOR_RDY=%u COEF_RDY=%u",
               status,
               (status & BAROMETER_READY_PRESSURE) != 0,
               (status & BAROMETER_READY_TEMP) != 0,
               (status & BAROMETER_READY_SENSOR) != 0,
               (status & BAROMETER_READY_COEF) != 0);
    }
    return false;
  }

  uint8_t raw[6];
  if (!i2cdevReadReg8(dev->bus, dev->address, BAROMETER_REG_PRS_B2, sizeof(raw), raw)) {
    if (shouldLogReadFailure()) {
      ESP_LOGW(TAG, "raw pressure/temperature read failed: address=0x%02X status=0x%02X",
               dev->address, status);
    }
    return false;
  }

  int32_t pRaw = signExtend(((uint32_t)raw[0] << 16) |
                            ((uint32_t)raw[1] << 8) | raw[2], 24);
  int32_t tRaw = signExtend(((uint32_t)raw[3] << 16) |
                            ((uint32_t)raw[4] << 8) | raw[5], 24);
  float p = pRaw / dev->pressureScale;
  float t = tRaw / dev->temperatureScale;
  *temperatureC = dev->c0 * 0.5f + dev->c1 * t;
  *pressurePa = dev->c00 + p * (dev->c10 + p * (dev->c20 + p * dev->c30)) +
                t * dev->c01 + t * p * (dev->c11 + p * dev->c21);

  const bool valid = *pressurePa > 30000.0f && *pressurePa < 120000.0f &&
                     *temperatureC > -50.0f && *temperatureC < 100.0f;
  if (!valid && shouldLogReadFailure()) {
    ESP_LOGW(TAG,
             "compensated value out of range: pRaw=%ld tRaw=%ld P=%.2fPa T=%.2fC status=0x%02X",
             (long)pRaw, (long)tRaw, (double)*pressurePa, (double)*temperatureC, status);
  }
  return valid;
}
