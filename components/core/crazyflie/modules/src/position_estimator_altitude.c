/**
 *    ||          ____  _ __
 * +------+      / __ )(_) /_______________ _____  ___
 * | 0xBC |     / __  / / __/ ___/ ___/ __ `/_  / / _ \
 * +------+    / /_/ / / /_/ /__/ /  / /_/ / / /_/  __/
 *  ||  ||    /_____/_/\__/\___/_/   \__,_/ /___/\___/
 *
 * Crazyflie Firmware
 *
 * Copyright (C) 2016 Bitcraze AB
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, in version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * position_estimator_altitude.c: Altitude-only position estimator
 */

#define DEBUG_MODULE "ALT_EST"

#include "stm32_legacy.h"
#include "FreeRTOS.h"
#include "task.h"

#include "debug_cf.h"
#include "log.h"
#include "param.h"
#include "num.h"
#include "position_estimator.h"
#include <math.h>

#define G 9.81f;
#define BARO_REFERENCE_SAMPLES 32U
#define BARO_REFERENCE_MAX_VARIANCE 0.04f
#define DEG_TO_RAD 0.017453292519943295f
#define TOF_PRIORITY_HEIGHT 2.0f
#define TOF_CONFIDENCE_STDDEV_SCALE 0.10f
#define TOF_HIGH_CONFIDENCE_THRESHOLD 0.70f
#define KALMAN_ACCEL_NOISE 1.5f
#define KALMAN_ACCEL_BIAS_NOISE 0.05f
#define KALMAN_BARO_MIN_VARIANCE 0.0225f
#define KALMAN_TOF_MIN_VARIANCE 0.0004f
#define KALMAN_INNOVATION_GATE_SQ 25.0f

enum {
  FUSION_MODE_BARO_ONLY = 0,
  FUSION_MODE_TOF_PRIORITY = 1,
  FUSION_MODE_BARO_PRIORITY = 2,
};

struct selfState_s {
  float estimatedZ; // The current Z estimate, has same offset as asl
  float velocityZ; // Vertical speed (world frame) integrated from vertical acceleration (m/s)
  float estAlphaZrange;
  float estAlphaAsl;
  float velocityFactor;
  float vAccDeadband; // Vertical acceleration deadband
  float velZAlpha;   // Blending factor to avoid vertical speed to accumulate error
  float estimatedVZ;
};

static struct selfState_s state = {
  .estimatedZ = 0.0f,
  .velocityZ = 0.0f,
  .estAlphaZrange = 0.90f,
  .estAlphaAsl = 0.997f,
  .velocityFactor = 1.0f,
  .vAccDeadband = 0.04f,
  .velZAlpha = 0.995f,
  .estimatedVZ = 0.0f,
};

/* Acceleration predicts height/velocity at the IMU rate. ToF and barometer
 * measurements correct drift at their own rates. */
typedef struct {
  bool enabled;
  bool initialized;
  bool baroReferenceValid;
  bool baroRelativeValid;
  bool tofSeen;
  float z;
  float vz;
  float accBias;
  float lastAcc;
  float baroReference;
  float lastBaroAsl;
  float baroReferenceMean;
  float baroReferenceM2;
  float baroReferenceVariance;
  float filteredBaroRelative;
  float correctedTof;
  float tofConfidence;
  float covariance[3][3];
  float baroInnovation;
  float tofInnovation;
  uint8_t fusionMode;
  uint32_t baroReferenceSamples;
  uint32_t lastTofTimestamp;
  uint32_t lastBaroCorrectionTick;
  uint32_t lastTofCorrectionTick;
  uint32_t lastBaroPrintTick;
  uint32_t rejectedMeasurements;
} robustAltitudeState_t;

static robustAltitudeState_t robust;
static volatile bool relativeHeightResetRequested;

void positionEstimatorAltitudeEnableRobustFusion(bool enable)
{
  robust = (robustAltitudeState_t){.enabled = enable};
}

bool positionEstimatorAltitudeBarometerIsPrimary(void)
{
  return robust.enabled && robust.baroReferenceValid &&
         robust.fusionMode != FUSION_MODE_TOF_PRIORITY;
}

bool positionEstimatorAltitudeGetRelativeHeight(float *heightMeters)
{
  if (heightMeters == NULL || !robust.enabled || !robust.initialized ||
      !robust.baroReferenceValid || !isfinite(robust.z)) {
    return false;
  }

  *heightMeters = robust.z;
  return true;
}

bool positionEstimatorAltitudeGetFilteredBaroRelative(float *heightMeters)
{
  if (heightMeters == NULL || !robust.enabled ||
      !robust.baroReferenceValid || !robust.baroRelativeValid ||
      !isfinite(robust.filteredBaroRelative)) {
    return false;
  }

  *heightMeters = robust.filteredBaroRelative;
  return true;
}

void positionEstimatorAltitudeRequestRelativeReset(void)
{
  relativeHeightResetRequested = true;
}

static float clampf(float value, float minValue, float maxValue)
{
  return fminf(maxValue, fmaxf(minValue, value));
}

static void kalmanInitialize(float initialHeight)
{
  robust.z = initialHeight;
  robust.vz = 0.0f;
  robust.accBias = 0.0f;
  robust.covariance[0][0] = 0.25f;
  robust.covariance[1][1] = 1.0f;
  robust.covariance[2][2] = 0.25f;
  robust.initialized = true;
}

static void kalmanPredict(float acceleration, float dt)
{
  const float dt2 = dt * dt;
  const float correctedAcc = acceleration - robust.accBias;
  robust.z += robust.vz * dt + 0.5f * correctedAcc * dt2;
  robust.vz += correctedAcc * dt;

  const float f[3][3] = {
    {1.0f, dt, -0.5f * dt2},
    {0.0f, 1.0f, -dt},
    {0.0f, 0.0f, 1.0f},
  };
  float fp[3][3] = {{0}};
  float predicted[3][3] = {{0}};

  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      for (int k = 0; k < 3; k++) {
        fp[i][j] += f[i][k] * robust.covariance[k][j];
      }
    }
  }
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      for (int k = 0; k < 3; k++) {
        predicted[i][j] += fp[i][k] * f[j][k];
      }
    }
  }

  const float accelVariance = KALMAN_ACCEL_NOISE * KALMAN_ACCEL_NOISE;
  const float g[3] = {0.5f * dt2, dt, 0.0f};
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      predicted[i][j] += g[i] * g[j] * accelVariance;
    }
  }
  predicted[2][2] += KALMAN_ACCEL_BIAS_NOISE *
      KALMAN_ACCEL_BIAS_NOISE * dt;
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      robust.covariance[i][j] = predicted[i][j];
    }
  }
}

static bool kalmanCorrectAltitude(float measurement, float variance,
                                  float *innovationOut)
{
  const float innovation = measurement - robust.z;
  const float innovationVariance = robust.covariance[0][0] + variance;
  *innovationOut = innovation;

  if (!isfinite(innovationVariance) || innovationVariance <= 0.0f ||
      innovation * innovation > KALMAN_INNOVATION_GATE_SQ * innovationVariance) {
    robust.rejectedMeasurements++;
    return false;
  }

  const float gain[3] = {
    robust.covariance[0][0] / innovationVariance,
    robust.covariance[1][0] / innovationVariance,
    robust.covariance[2][0] / innovationVariance,
  };
  const float firstRow[3] = {
    robust.covariance[0][0],
    robust.covariance[0][1],
    robust.covariance[0][2],
  };

  robust.z += gain[0] * innovation;
  robust.vz += gain[1] * innovation;
  robust.accBias += gain[2] * innovation;
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      robust.covariance[i][j] -= gain[i] * firstRow[j];
    }
  }
  for (int i = 0; i < 3; i++) {
    for (int j = i + 1; j < 3; j++) {
      const float symmetric = 0.5f *
          (robust.covariance[i][j] + robust.covariance[j][i]);
      robust.covariance[i][j] = symmetric;
      robust.covariance[j][i] = symmetric;
    }
  }
  robust.vz = clampf(robust.vz, -5.0f, 5.0f);
  robust.accBias = clampf(robust.accBias, -1.5f, 1.5f);
  return true;
}

static void updateBaroReference(float asl)
{
  robust.baroReferenceSamples++;
  const float delta = asl - robust.baroReferenceMean;
  robust.baroReferenceMean += delta / robust.baroReferenceSamples;
  const float delta2 = asl - robust.baroReferenceMean;
  robust.baroReferenceM2 += delta * delta2;

  if (robust.baroReferenceSamples < BARO_REFERENCE_SAMPLES) {
    return;
  }

  robust.baroReferenceVariance = robust.baroReferenceM2 /
      (robust.baroReferenceSamples - 1U);
  if (robust.baroReferenceVariance <= BARO_REFERENCE_MAX_VARIANCE) {
    robust.baroReference = robust.baroReferenceMean;
    robust.baroReferenceValid = true;
  } else {
    robust.rejectedMeasurements++;
    robust.baroReferenceSamples = 0;
    robust.baroReferenceMean = 0.0f;
    robust.baroReferenceM2 = 0.0f;
  }
}

static void positionEstimateInternal(state_t* estimate, const sensorData_t* sensorData, const tofMeasurement_t* tofMeasurement, float dt, uint32_t tick, struct selfState_s* state);
static void positionUpdateVelocityInternal(float accWZ, float dt, struct selfState_s* state);

void positionEstimate(state_t* estimate, const sensorData_t* sensorData, const tofMeasurement_t* tofMeasurement, float dt, uint32_t tick) {
  positionEstimateInternal(estimate, sensorData, tofMeasurement, dt, tick, &state);
}

void positionUpdateVelocity(float accWZ, float dt) {
  if (robust.enabled) {
    const float acceleration = accWZ * G;
    robust.lastAcc = acceleration;
    if (robust.initialized) {
      kalmanPredict(acceleration, dt);
    }
    return;
  }
  positionUpdateVelocityInternal(accWZ, dt, &state);
}

static void positionEstimateInternal(state_t* estimate, const sensorData_t* sensorData, const tofMeasurement_t* tofMeasurement, float dt, uint32_t tick, struct selfState_s* state) {
  if (robust.enabled) {
    const uint32_t now = xTaskGetTickCount();
    const float tiltCosine = cosf(estimate->attitude.roll * DEG_TO_RAD) *
                             cosf(estimate->attitude.pitch * DEG_TO_RAD);
    const bool tofFresh = tofMeasurement->timestamp != 0 &&
                          (now - tofMeasurement->timestamp) <= M2T(100) &&
                          isfinite(tofMeasurement->distance) &&
                          tofMeasurement->distance > 0.03f &&
                          tofMeasurement->distance < 4.0f &&
                          tiltCosine > 0.5f;
    const bool baroValid = isfinite(sensorData->baro.asl) &&
                           sensorData->baro.pressure > 300.0f &&
                           sensorData->baro.pressure < 1200.0f;
    const bool baroNew = baroValid &&
                         sensorData->baro.asl != robust.lastBaroAsl;

    if (baroNew) {
      robust.lastBaroAsl = sensorData->baro.asl;
      if (!robust.baroReferenceValid) {
        updateBaroReference(sensorData->baro.asl);
      } 
    }

    if (relativeHeightResetRequested) {
      relativeHeightResetRequested = false;

      if (baroValid) {
        robust.baroReference = sensorData->baro.asl;
        robust.baroReferenceValid = true;
        robust.baroRelativeValid = true;
        robust.filteredBaroRelative = 0.0f;
      }

      if (robust.initialized) {
        kalmanInitialize(0.0f);
      } else {
        robust.z = 0.0f;
        robust.vz = 0.0f;
      }
    }

    if (tofFresh) {
      robust.tofSeen = true;
      robust.correctedTof = tofMeasurement->distance * tiltCosine;
      const float noiseConfidence = clampf(
          1.0f - tofMeasurement->stdDev / TOF_CONFIDENCE_STDDEV_SCALE,
          0.0f, 1.0f);
      robust.tofConfidence = noiseConfidence * clampf(tiltCosine, 0.0f, 1.0f);
    }

    const bool tofHighConfidence = tofFresh &&
        robust.correctedTof < TOF_PRIORITY_HEIGHT &&
        robust.tofConfidence >= TOF_HIGH_CONFIDENCE_THRESHOLD;
    if (!robust.tofSeen) {
      robust.fusionMode = FUSION_MODE_BARO_ONLY;
    } else if (tofHighConfidence) {
      robust.fusionMode = FUSION_MODE_TOF_PRIORITY;
    } else {
      robust.fusionMode = FUSION_MODE_BARO_PRIORITY;
    }

    if (!robust.initialized) {
      if (tofFresh) {
        kalmanInitialize(robust.correctedTof);
      } else if (robust.baroReferenceValid) {
        kalmanInitialize(0.0f);
      }
    }

    if (robust.initialized) {
      if (tofFresh && tofMeasurement->timestamp != robust.lastTofTimestamp) {
        robust.lastTofCorrectionTick = now;
        robust.lastTofTimestamp = tofMeasurement->timestamp;
        const float tofStdDev = fmaxf(tofMeasurement->stdDev, 0.02f);
        float tofVariance = fmaxf(tofStdDev * tofStdDev,
                                  KALMAN_TOF_MIN_VARIANCE);
        if (!tofHighConfidence) {
          tofVariance *= 16.0f;
        }
        kalmanCorrectAltitude(robust.correctedTof, tofVariance,
                              &robust.tofInnovation);
      }

      if (baroNew && robust.baroReferenceValid) {
        robust.lastBaroCorrectionTick = now;
        const float baroRelative = sensorData->baro.asl - robust.baroReference;
        robust.filteredBaroRelative = baroRelative;
        robust.baroRelativeValid = true;
        #if 0
        if ((now - robust.lastBaroPrintTick) >= M2T(1000)) {
          robust.lastBaroPrintTick = now;
          DEBUG_PRINTW("baro relative:%.3f m, filtered:%.3f m, asl:%.3f m, fused:%.3f m, vz:%.3f m/s\n",
                       (double)baroRelative,
                       (double)robust.filteredBaroRelative,
                       (double)sensorData->baro.asl,
                       (double)robust.z,
                       (double)robust.vz);
        }
        #endif
        float baroVariance = fmaxf(robust.baroReferenceVariance * 4.0f,
                                   KALMAN_BARO_MIN_VARIANCE);
        if (robust.fusionMode == FUSION_MODE_TOF_PRIORITY) {
          baroVariance *= 9.0f;
        }
        kalmanCorrectAltitude(baroRelative, baroVariance,
                              &robust.baroInnovation);
      }
    }

    estimate->position.x = 0.0f;
    estimate->position.y = 0.0f;
    estimate->position.z = robust.z;
    estimate->velocity.z = robust.vz;
    state->estimatedZ = robust.z;
    state->estimatedVZ = robust.vz;
    state->velocityZ = robust.vz;
    return;
  }

  float filteredZ;
  static float prev_estimatedZ = 0;
  static bool surfaceFollowingMode = false;

  const uint32_t MAX_SAMPLE_AGE = M2T(50);

  uint32_t now = xTaskGetTickCount();
  bool isSampleUseful = ((now - tofMeasurement->timestamp) <= MAX_SAMPLE_AGE);

  if (isSampleUseful) {
    surfaceFollowingMode = true;
  }

  if (surfaceFollowingMode) {
    if (isSampleUseful) {
      // IIR filter zrange
      filteredZ = (state->estAlphaZrange       ) * state->estimatedZ +
                  (1.0f - state->estAlphaZrange) * tofMeasurement->distance;
      // Use zrange as base and add velocity changes.
      state->estimatedZ = filteredZ + (state->velocityFactor * state->velocityZ * dt);
    }
  } else {
    // FIXME: A bit of an hack to init IIR filter
    if (state->estimatedZ == 0.0f) {
      filteredZ = sensorData->baro.asl;
    } else {
      // IIR filter asl
      filteredZ = (state->estAlphaAsl       ) * state->estimatedZ +
                  (1.0f - state->estAlphaAsl) * sensorData->baro.asl;
    }
    // Use asl as base and add velocity changes.
    state->estimatedZ = filteredZ + (state->velocityFactor * state->velocityZ * dt);
  }

  estimate->position.x = 0.0f;
  estimate->position.y = 0.0f;
  estimate->position.z = state->estimatedZ;
  estimate->velocity.z = (state->estimatedZ - prev_estimatedZ) / dt;
  state->estimatedVZ = estimate->velocity.z;
  prev_estimatedZ = state->estimatedZ;
}

static void positionUpdateVelocityInternal(float accWZ, float dt, struct selfState_s* state) {
  state->velocityZ += deadband(accWZ, state->vAccDeadband) * dt * G;
  state->velocityZ *= state->velZAlpha;
}

LOG_GROUP_START(posEstAlt)
LOG_ADD(LOG_FLOAT, estimatedZ, &state.estimatedZ)
LOG_ADD(LOG_FLOAT, estVZ, &state.estimatedVZ)
LOG_ADD(LOG_FLOAT, velocityZ, &state.velocityZ)
LOG_ADD(LOG_FLOAT, robustBias, &robust.accBias)
LOG_ADD(LOG_UINT32, robustReject, &robust.rejectedMeasurements)
LOG_ADD(LOG_FLOAT, baroRefVar, &robust.baroReferenceVariance)
LOG_ADD(LOG_UINT32, baroRefCnt, &robust.baroReferenceSamples)
LOG_ADD(LOG_FLOAT, tofVertical, &robust.correctedTof)
LOG_ADD(LOG_FLOAT, tofConf, &robust.tofConfidence)
LOG_ADD(LOG_FLOAT, baroInnov, &robust.baroInnovation)
LOG_ADD(LOG_FLOAT, tofInnov, &robust.tofInnovation)
LOG_ADD(LOG_FLOAT, kalmanPz, &robust.covariance[0][0])
LOG_ADD(LOG_FLOAT, kalmanPvz, &robust.covariance[1][1])
LOG_ADD(LOG_UINT8, fusionMode, &robust.fusionMode)
LOG_GROUP_STOP(posEstAlt)

PARAM_GROUP_START(posEstAlt)
PARAM_ADD(PARAM_FLOAT, estAlphaAsl, &state.estAlphaAsl)
PARAM_ADD(PARAM_FLOAT, estAlphaZr, &state.estAlphaZrange)
PARAM_ADD(PARAM_FLOAT, velFactor, &state.velocityFactor)
PARAM_ADD(PARAM_FLOAT, velZAlpha, &state.velZAlpha)
PARAM_ADD(PARAM_FLOAT, vAccDeadband, &state.vAccDeadband)
PARAM_GROUP_STOP(posEstAlt)
