/**
 *    ||          ____  _ __
 * +------+      / __ )(_) /_______________ _____  ___
 * | 0xBC |     / __  / / __/ ___/ ___/ __ `/_  / / _ \
 * +------+    / /_/ / / /_/ /__/ /  / /_/ / / /_/  __/
 *  ||  ||    /_____/_/\__/\___/_/   \__,_/ /___/\___/
 *
 * ESP-Drone Firmware
 *
 * Copyright 2019-2020  Espressif Systems (Shanghai)
 * Copyright (C) 2011-2017 Bitcraze AB
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
 *
 */
#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "crtp_commander.h"
#include "crtp_commander_high_level.h"
#include "commander.h"
#include "estimator.h"
#include "crtp.h"
#include "param.h"
#include "FreeRTOS.h"
#include "task.h"
#include "num.h"
#include "range.h"
#include "position_estimator.h"
#include "position_controller.h"
#include "pm_esplane.h"
#include "sitaw.h"
#include "config.h"
#include "stm32_legacy.h"
#define DEBUG_MODULE "MODE"
#include "debug_cf.h"

#define MIN_THRUST  1000
#define MAX_THRUST  60000
#define MANUAL_LOW_BATTERY_MAX_THRUST 32767U

// A wide center dead zone makes altitude hold tolerant of joystick noise and
// small centering errors. Outside the dead zone the command is re-scaled so
// the full stick range is still available.
#define ALT_HOLD_THRUST_CENTER    32767U
#define ALT_HOLD_THRUST_DEADZONE  5000U
#define ALT_HOLD_MAX_RISE_SPEED   0.30f
#define ALT_HOLD_MAX_FALL_SPEED   0.10f
#define BAROMETER_MAX_RISE_SPEED     0.30f
#define BAROMETER_MAX_FALL_SPEED     0.05f
#define ALT_HOLD_LANDING_SPEED    0.04f
#define ALT_HOLD_TAKEOFF_SPEED    0.12f
#define ALT_HOLD_LOW_SPEED        0.10f
#define ALT_HOLD_LOW_HEIGHT_MM    250.0f
#define ALT_HOLD_TAKEOFF_RAMP_MS  2500U
#define ALT_HOLD_MIN_HEIGHT_MM    100.0f//100.0f
#define ALT_HOLD_MAX_HEIGHT_MM    2000.0f
#define ALT_HOLD_SLOWDOWN_MM      1700.0f
#define ALT_HOLD_RELEASE_MM       1850.0f
#define THRUST_MIN                400//1600
#define ALT_HOLD_UNLOCK_THRUST          200U
#define ALT_HOLD_ENGAGE_HEIGHT_MM       150.0f
#define ALT_HOLD_ENGAGE_RELEASE_MM       80.0f
#define ALT_HOLD_ENGAGE_CONFIRM_MS      250U
#define ALT_HOLD_TAKEOVER_SETTLE_MS     600U
#define ALT_HOLD_ONE_KEY_TAKEOFF_MS     2500U
#define ALT_HOLD_LANDING_HEIGHT_MM      600.0f
#define ALT_HOLD_LANDING_THRUST_MAX     300U
#define ALT_HOLD_NEGATIVE_STOP_HEIGHT_MM (-150.0f)
#define ALT_HOLD_LANDING_CONFIRM_MS      600U
#define ALT_HOLD_LANDING_SETTLE_MS      3000U
#define ALT_HOLD_RESET_MIN_WAIT_MS       500U
#define ALT_HOLD_RESET_STABLE_MS         300U
#define ALT_HOLD_RESET_MAX_WAIT_MS       1200U
#define ALT_HOLD_RESET_STABLE_RANGE_M    0.10f
#define MANUAL_LOW_BATTERY_LANDING_MS   6000U

/**
 * CRTP commander rpyt packet format
 */
struct CommanderCrtpLegacyValues
{
  float roll;       // deg
  float pitch;      // deg
  float yaw;        // deg
  uint16_t thrust;
} __attribute__((packed));

extern const setpoint_t nullSetpoint;

/**
 * Stabilization modes for Roll, Pitch, Yaw.
 */
typedef enum
{
  RATE    = 0,
  ANGLE   = 1,
} RPYType;

/**
 * Yaw flight Modes
 */
typedef enum
{
  CAREFREE  = 0, // Yaw is locked to world coordinates thus heading stays the same when yaw rotates
  PLUSMODE  = 1, // Plus-mode. Motor M1 is defined as front
  XMODE     = 2, // X-mode. M1 & M4 are defined as front
} YawModeType;

static RPYType stabilizationModeRoll  = ANGLE; // Current stabilization type of roll (rate or angle)
static RPYType stabilizationModePitch = ANGLE; // Current stabilization type of pitch (rate or angle)
static RPYType stabilizationModeYaw   = RATE;  // Current stabilization type of yaw (rate or angle)

static YawModeType yawMode = XMODE;            // Legacy senders use reserved=0
static bool carefreeResetFront;             // Reset what is front in carefree mode
static float carefreeFrontYaw;               // Heading considered front in carefree mode
static bool carefreeFrontValid;
static YawModeType previousYawMode = CAREFREE;

static bool thrustLocked = true;
static bool altHoldMode = false;
static bool posHoldMode = false;
static bool posSetMode = false;
static bool altitudeCeilingActive = false;
static bool altHoldTakeoffReady = false;
static bool altHoldTakeoffActive = false;
static volatile bool altHoldHoverActive = false;
static bool landing = false;
static TickType_t altHoldTakeoffStartTick = 0;
static TickType_t altHoldLandingConditionStartTick = 0;
static TickType_t altHoldLandingCountdownStartTick = 0;
static bool altHoldRelativeResetPending = false;
static TickType_t altHoldRelativeResetStartTick = 0;
static TickType_t altHoldRelativeStableStartTick = 0;
static float altHoldRelativeStableMin = 0.0f;
static float altHoldRelativeStableMax = 0.0f;
static volatile bool lowBatteryAlarmRequested = false;
static volatile bool tumbleAlarmRequested = false;
static bool tumbleAlarmIssuedForStick = false;
#if ENABLE_LOW_BATTERY_FLIGHT_PROTECTION
static bool lowBatteryAlarmIssuedForStick = false;
static bool posSetLowBatteryLanding = false;
static TickType_t posSetLowBatteryLandingStartTick = 0;
#endif

static FlightMode flight_mode = STABILIZE_MODE;

/**
 * Set flight mode deponds on the present sensors
 *
 * @param mode flight mode num
 */
void setCommandermode(FlightMode mode){
#ifdef CONFIG_ENABLE_COMMAND_MODE_SET
  switch (mode) {
  case ALTHOLD_MODE:
    altHoldMode = true;
    posHoldMode = false;
    posSetMode = false;
    altHoldTakeoffReady = false;
    altHoldTakeoffActive = false;
    landing = false;
    altHoldTakeoffStartTick = 0;
    registerRequiredEstimator(complementaryEstimator);
    break;
  case POSHOLD_MODE:
    altHoldMode = true;
    posHoldMode = true;
    posSetMode = false;
    altHoldTakeoffReady = false;
    altHoldTakeoffActive = false;
    landing = false;
    altHoldTakeoffStartTick = 0;
    registerRequiredEstimator(kalmanEstimator); 
    break;
  case POSSET_MODE:
    altHoldMode = false;
    posHoldMode = false;
    posSetMode = true;
    altHoldTakeoffReady = false;
    altHoldTakeoffActive = false;
    landing = false;
    altHoldTakeoffStartTick = 0;
    registerRequiredEstimator(kalmanEstimator); 
    break;
  default:
    altHoldMode = false;
    posHoldMode = false;
    posSetMode = false;
    altHoldTakeoffReady = false;
    altHoldTakeoffActive = false;
    landing = false;
    altHoldTakeoffStartTick = 0;
    registerRequiredEstimator(complementaryEstimator);   
    break;
  }
  DEBUG_PRINTI("FlightMode = %d",mode);
#else
  DEBUG_PRINTI("set FlightMode disable");
#endif
  flight_mode = mode;
  altHoldHoverActive = false;
  altHoldLandingConditionStartTick = 0;
  altHoldLandingCountdownStartTick = 0;
  altHoldRelativeResetPending = false;
  altHoldRelativeResetStartTick = 0;
  altHoldRelativeStableStartTick = 0;
  positionControllerSetVerticalLandingProtection(false);
  positionControllerSetVerticalDescentThrustLimit(false);
}

uint8_t get_flight_mode(void)
{
  return flight_mode;
}

bool crtpCommanderConsumeLowBatteryAlarmRequest(void)
{
  if (!lowBatteryAlarmRequested) {
    return false;
  }

  lowBatteryAlarmRequested = false;
  return true;
}

bool crtpCommanderConsumeTumbleAlarmRequest(void)
{
  if (!tumbleAlarmRequested) {
    return false;
  }

  tumbleAlarmRequested = false;
  return true;
}

FlightMode crtpCommanderRpytGetFlightTelemetry(float *relativeHeightM)
{
  if (relativeHeightM == NULL) {
    return flight_mode;
  }

  *relativeHeightM = 0.0f;

  float relativeHeight = 0.0f;
  /* Barometer telemetry must not depend on takeoff/hover state. As soon as
   * filteredBaroRelative has a valid sample, report it. ToF-only POSHOLD
   * configurations fall back to the fused relative-height estimate. */
  const bool heightValid =
      positionEstimatorAltitudeGetFilteredBaroRelative(&relativeHeight) ||
      positionEstimatorAltitudeGetRelativeHeight(&relativeHeight);
  if (heightValid && isfinite(relativeHeight)) {
    *relativeHeightM = relativeHeight;
  }

  return flight_mode;
}

bool crtpCommanderRpytIsAltitudeHoldActive(void)
{
  return flight_mode == ALTHOLD_MODE && altHoldHoverActive;
}

/**
 * Rotate Yaw so that the Crazyflie will change what is considered front.
 *
 * @param yawRad Amount of radians to rotate yaw.
 */
static void rotateYaw(setpoint_t *setpoint, float yawRad)
{
  float cosy = cosf(yawRad);
  float siny = sinf(yawRad);
  float originalRoll = setpoint->attitude.roll;
  float originalPitch = setpoint->attitude.pitch;

  setpoint->attitude.roll = originalRoll * cosy - originalPitch * siny;
  setpoint->attitude.pitch = originalPitch * cosy + originalRoll * siny;
}

/**
 * Update Yaw according to current setting
 */
void crtpCommanderRpytApplyYawMode(setpoint_t *setpoint, const state_t *state)
{
  // Preserve the legacy RPYT behavior: yaw-mode transforms were only applied
  // outside position-set mode while yaw was controlled as a rate.
  if (posSetMode || stabilizationModeYaw != RATE)
  {
    return;
  }

  if (yawMode != CAREFREE)
  {
    if (yawMode == PLUSMODE)
    {
      rotateYaw(setpoint, 45 * M_PI / 180);
    }

    carefreeFrontValid = false;
    previousYawMode = yawMode;
    return;
  }

  if (!carefreeFrontValid || previousYawMode != CAREFREE || carefreeResetFront)
  {
    carefreeFrontYaw = state->attitude.yaw;
    carefreeFrontValid = true;
    carefreeResetFront = false;
  }
  previousYawMode = yawMode;

  float yawDelta = state->attitude.yaw - carefreeFrontYaw;
  while (yawDelta > 180.0f)
  {
    yawDelta -= 360.0f;
  }
  while (yawDelta < -180.0f)
  {
    yawDelta += 360.0f;
  }

  switch (yawMode)
  {
    case CAREFREE:
      if (setpoint->mode.x == modeDisable && setpoint->mode.y == modeDisable)
      {
        // Convert world-referenced stick input to the current body frame.
        rotateYaw(setpoint, yawDelta * M_PI / 180.0f);
      }
      else if (setpoint->mode.x == modeVelocity && setpoint->mode.y == modeVelocity)
      {
        // Express stick velocity in the world frame fixed when carefree was enabled.
        float frontRad = carefreeFrontYaw * M_PI / 180.0f;
        float cosFront = cosf(frontRad);
        float sinFront = sinf(frontRad);
        float bodyVx = setpoint->velocity.x;
        float bodyVy = setpoint->velocity.y;

        setpoint->velocity.x = bodyVx * cosFront - bodyVy * sinFront;
        setpoint->velocity.y = bodyVy * cosFront + bodyVx * sinFront;
        setpoint->velocity_body = false;
      }
      break;
    case PLUSMODE:
      rotateYaw(setpoint, 45 * M_PI / 180);
      break;
    case XMODE: // Fall through
    default:
      // Default in x-mode. Do nothing
      break;
  }
}

extern bool crtpCommanderHighLevelIsLanding(void);  

void crtpCommanderRpytDecodeSetpoint(setpoint_t *setpoint, CRTPPacket *pk)
{
  struct CommanderCrtpLegacyValues *values = (struct CommanderCrtpLegacyValues*)pk->data;

  /*
   * Keep the legacy RPYT payload and port unchanged. New controllers carry
   * the carefree switch in the CRTP header's reserved field, while legacy
   * apps naturally send zero and therefore retain X-mode behavior.
   */
  yawMode = (pk->reserved == 1U) ? CAREFREE : XMODE;
  // if (CAREFREE==yawMode)
  // {
  //   DEBUG_PRINTW("CAREFREE\n");
  // }

  if (commanderGetActivePriority() == COMMANDER_PRIORITY_DISABLE) {
    if (!thrustLocked) {
      DEBUG_PRINTW("thrust locked: control resumed after shutdown\n");
    }
    thrustLocked = true;
  }
  if (values->thrust == 0) {
    thrustLocked = false;
  }

  // Thrust
  uint16_t rawThrust = values->thrust;

#ifdef SITAW_TU_ENABLED
  /* A tumbled emergency stop is intentionally latched. Report the reason
   * once for each new zero-to-nonzero throttle attempt without continuously
   * replaying the melody while the stick remains raised. */
  if (sitAwTuDetected()) {
    if (rawThrust == 0U) {
      tumbleAlarmIssuedForStick = false;
    } else if (!tumbleAlarmIssuedForStick) {
      tumbleAlarmRequested = true;
      tumbleAlarmIssuedForStick = true;
    }
  } else {
    tumbleAlarmIssuedForStick = false;
  }
#endif

#if ENABLE_LOW_BATTERY_FLIGHT_PROTECTION
  const bool batteryCritical = pmIsBatteryCriticalForFlight();

  if (batteryCritical && altHoldMode) {
    if (altHoldTakeoffActive) {
      /* Only Z is captured below. The normal roll/pitch/yaw and POS_HOLD XY
       * processing later in this function remains available for avoidance. */
      landing = true;
      lowBatteryAlarmRequested = true;
    } else {
      /* Critical voltage on the ground is a hard takeoff lock. */
      thrustLocked = true;
      if (rawThrust == 0U) {
        lowBatteryAlarmIssuedForStick = false;
      } else if (!lowBatteryAlarmIssuedForStick) {
        lowBatteryAlarmRequested = true;
        lowBatteryAlarmIssuedForStick = true;
      }
    }
  }
#endif

  if (thrustLocked || (rawThrust < MIN_THRUST)) {
    setpoint->thrust = 0;
  } else {
    setpoint->thrust = fminf(rawThrust, MAX_THRUST);
#if ENABLE_LOW_BATTERY_FLIGHT_PROTECTION
    if (batteryCritical && flight_mode == STABILIZE_MODE &&
        setpoint->thrust > MANUAL_LOW_BATTERY_MAX_THRUST) {
      setpoint->thrust = MANUAL_LOW_BATTERY_MAX_THRUST;
    }
#endif
  }

  if (altHoldMode) {
    altHoldHoverActive = false;

    setpoint->thrust = 0;
    setpoint->mode.z = modeVelocity;

    const uint16_t thrustLow = ALT_HOLD_THRUST_CENTER - ALT_HOLD_THRUST_DEADZONE;
    const uint16_t thrustHigh = ALT_HOLD_THRUST_CENTER + ALT_HOLD_THRUST_DEADZONE;
    const bool barometerPrimary = positionEstimatorAltitudeBarometerIsPrimary();
    const float maxRiseSpeed = barometerPrimary ?
        BAROMETER_MAX_RISE_SPEED : ALT_HOLD_MAX_RISE_SPEED;
    const float maxFallSpeed = barometerPrimary ?
        BAROMETER_MAX_FALL_SPEED : ALT_HOLD_MAX_FALL_SPEED;

    if (rawThrust > thrustHigh) {
      setpoint->velocity.z = maxRiseSpeed *
                             (float)(rawThrust - thrustHigh) /
                             (float)(UINT16_MAX - thrustHigh);
    } else if (rawThrust < thrustLow) {
      setpoint->velocity.z = -maxFallSpeed *
                              (float)(thrustLow - rawThrust) /
                              (float)thrustLow;
    } else {
      setpoint->velocity.z = 0.0f;
    }

    // Do not let the position controller apply hover thrust merely because an
    // altitude-controlled mode was detected. First observe a non-rising stick,
    // then require a later, explicit upward command to activate takeoff.
    if (thrustLocked) {
      altHoldTakeoffReady = false;
      altHoldTakeoffActive = false;
      landing = false;
      altHoldLandingConditionStartTick = 0;
      altHoldLandingCountdownStartTick = 0;
    }

    if (!altHoldTakeoffActive) {
      if (!thrustLocked && rawThrust <= thrustHigh) {
        altHoldTakeoffReady = true;
      } else if (!thrustLocked && altHoldTakeoffReady) {
        altHoldTakeoffActive = true;
        altHoldTakeoffStartTick = xTaskGetTickCount();
      }

      if (!altHoldTakeoffActive) {
        setpoint->mode.z = modeDisable;
        setpoint->thrust = 0;
        setpoint->velocity.z = 0.0f;
      }
    }

    const TickType_t now = xTaskGetTickCount();
    float heightMm = rangeGet(rangeDown);
    bool landingHeightValid = !barometerPrimary &&
        isfinite(heightMm) && heightMm > 0.0f;
    bool negativeBaroStopHeight = false;
    bool baroRelativeValid = false;
    float baroRelativeHeight = 0.0f;
    if (barometerPrimary) {
      if (positionEstimatorAltitudeGetFilteredBaroRelative(&baroRelativeHeight)) {
        baroRelativeValid = isfinite(baroRelativeHeight);
        heightMm = baroRelativeHeight * 1000.0f;
        if (isfinite(heightMm)) {
          landingHeightValid = heightMm > -500.0f;
          negativeBaroStopHeight =
              heightMm <= ALT_HOLD_NEGATIVE_STOP_HEIGHT_MM;
        }
      }
    }

    /* Reset the barometer ground reference only after landing has stopped the
     * motors and the pressure has had time to settle. This state machine is
     * non-blocking: an explicit new ascent command cancels it immediately, so
     * the pilot never has to wait for the reset before taking off again. */
    if (altHoldRelativeResetPending) {
      const bool newTakeoffIntent = altHoldTakeoffActive || rawThrust > thrustHigh;
      if (!barometerPrimary || newTakeoffIntent) {
        altHoldRelativeResetPending = false;
        altHoldRelativeResetStartTick = 0;
        altHoldRelativeStableStartTick = 0;
      } else if (baroRelativeValid) {
        const uint32_t resetElapsedMs =
            T2M(now - altHoldRelativeResetStartTick);

        if (resetElapsedMs >= ALT_HOLD_RESET_MIN_WAIT_MS) {
          if (altHoldRelativeStableStartTick == 0) {
            altHoldRelativeStableStartTick = now;
            altHoldRelativeStableMin = baroRelativeHeight;
            altHoldRelativeStableMax = baroRelativeHeight;
          } else {
            altHoldRelativeStableMin =
                fminf(altHoldRelativeStableMin, baroRelativeHeight);
            altHoldRelativeStableMax =
                fmaxf(altHoldRelativeStableMax, baroRelativeHeight);

            if (altHoldRelativeStableMax - altHoldRelativeStableMin >
                ALT_HOLD_RESET_STABLE_RANGE_M) {
              altHoldRelativeStableStartTick = now;
              altHoldRelativeStableMin = baroRelativeHeight;
              altHoldRelativeStableMax = baroRelativeHeight;
            }
          }

          const bool pressureStable = altHoldRelativeStableStartTick != 0 &&
              T2M(now - altHoldRelativeStableStartTick) >=
                  ALT_HOLD_RESET_STABLE_MS;
          const bool maximumWaitExpired =
              resetElapsedMs >= ALT_HOLD_RESET_MAX_WAIT_MS;
          if (pressureStable || maximumWaitExpired) {
            positionEstimatorAltitudeRequestRelativeReset();
            altHoldRelativeResetPending = false;
            altHoldRelativeResetStartTick = 0;
            altHoldRelativeStableStartTick = 0;
            DEBUG_PRINTI("Ground height reference reset after landing\n");
          }
        }
      }
    }

    const bool belowLandingTriggerHeight = landingHeightValid &&
        heightMm <= ALT_HOLD_LANDING_HEIGHT_MM;
    bool stopImmediatelyAfterLandingConfirm = false;
    /* Manual landing requires explicit low throttle for 600 ms. A plausible
     * sub-0.6 m height starts the normal settling descent. A debounced barometer
     * reading below -0.15 m is treated as near-ground downwash/reference error
     * and stops immediately instead of allowing a return to height hold. */
    const bool manualLandingCondition = altHoldTakeoffActive && !landing &&
        rawThrust <= ALT_HOLD_LANDING_THRUST_MAX &&
        (belowLandingTriggerHeight || negativeBaroStopHeight);
    if (manualLandingCondition) {
      if (altHoldLandingConditionStartTick == 0) {
        altHoldLandingConditionStartTick = now;
      } else if (T2M(now - altHoldLandingConditionStartTick) >=
                 ALT_HOLD_LANDING_CONFIRM_MS) {
        landing = true;
        altHoldLandingCountdownStartTick = now;
        altHoldLandingConditionStartTick = 0;
        stopImmediatelyAfterLandingConfirm = negativeBaroStopHeight;
        DEBUG_PRINTI("Manual landing latched at %ld mm\n", (long)heightMm);
      }
    } else if (!landing) {
      altHoldLandingConditionStartTick = 0;
    }

    // Only latch an automatic landing while the altitude controller is
    // already flying. A landing command received on the ground must not arm
    // the motors.
    if (altHoldTakeoffActive && crtpCommanderHighLevelIsLanding()) {
      landing = true;
      altHoldLandingConditionStartTick = 0;
    }

    /* A high-level landing may start far above the reliable near-ground
     * barometer region. Start its stop countdown only after reaching 0.6 m. */
    if (landing && altHoldLandingCountdownStartTick == 0 &&
        belowLandingTriggerHeight) {
      altHoldLandingCountdownStartTick = now;
    }

    /* Once landing is latched, reset accumulated vertical corrections and
     * prevent the controller from commanding above nominal hover thrust.
     * This removes the near-ground upward kick while retaining controlled
     * braking and manual roll/pitch/yaw authority. */
    /* Full-low throttle in barometer-only ALTHOLD is an explicit descent
     * request. Preserve PID state, but allow only a small positive braking
     * correction while the pilot is trying to land. Keep this strictly out of
     * manual and POS_HOLD modes. */
    const bool lowThrottleDescentProtection =
        flight_mode == ALTHOLD_MODE && altHoldTakeoffActive &&
        rawThrust <= ALT_HOLD_LANDING_THRUST_MAX;
    positionControllerSetVerticalDescentThrustLimit(
        lowThrottleDescentProtection && !landing);
    positionControllerSetVerticalLandingProtection(landing);

    // Automatic landing owns only the vertical axis. Manual attitude control
    // remains available, while an upward throttle command cannot cancel the
    // descent accidentally.
    if (landing) {
      setpoint->mode.z = modeVelocity;
      setpoint->velocity.z = -ALT_HOLD_LANDING_SPEED;
    }

    altHoldHoverActive = altHoldTakeoffActive && !landing &&
        rawThrust >= thrustLow && rawThrust <= thrustHigh;

    const bool landingCountdownExpired =
        altHoldLandingCountdownStartTick != 0 &&
        T2M(now - altHoldLandingCountdownStartTick) >=
            ALT_HOLD_LANDING_SETTLE_MS;
    /* Height is deliberately not consulted after the 0.6 m entry has
     * started the countdown. Barometer noise near the ground must not stop
     * the motors early; the latched landing always gets its full settle time. */
    const bool landingComplete = altHoldTakeoffActive && landing &&
        (landingCountdownExpired || stopImmediatelyAfterLandingConfirm);

    // Automatic and manual landing share the same motor-stop cleanup.
    if (landingComplete) {
      altitudeCeilingActive = false;
      altHoldTakeoffReady = false;
      altHoldTakeoffActive = false;
      altHoldTakeoffStartTick = 0;
      altHoldLandingConditionStartTick = 0;
      altHoldLandingCountdownStartTick = 0;
      altHoldHoverActive = false;
      landing = false;
      thrustLocked = true;
      positionControllerSetVerticalLandingProtection(false);
      positionControllerSetVerticalDescentThrustLimit(false);

      crtpCommanderHighLevelStop();
      if (barometerPrimary) {
        altHoldRelativeResetPending = true;
        altHoldRelativeResetStartTick = now;
        altHoldRelativeStableStartTick = 0;
      }
      memcpy(setpoint, &nullSetpoint, sizeof(*setpoint));
      return;
    }

    #if 0

    if (heightMm >= ALT_HOLD_MAX_HEIGHT_MM) {
      altitudeCeilingActive = true;
    }

    // Re-arm ascent only after the pilot explicitly commands descent and the
    // craft is safely below the ceiling. This prevents range noise from
    // repeatedly switching ascent on and off near the height limit.
    if (altitudeCeilingActive &&
        setpoint->velocity.z < 0.0f &&
        heightMm > 0.0f &&
        heightMm <= ALT_HOLD_RELEASE_MM) {
      altitudeCeilingActive = false;
    }

    if (setpoint->velocity.z > 0.0f) {
      if (heightMm <= 0.0f || altitudeCeilingActive) {
        setpoint->velocity.z = 0.0f;
      } else if (heightMm > ALT_HOLD_SLOWDOWN_MM) {
        const float ceilingScale =
            (ALT_HOLD_MAX_HEIGHT_MM - heightMm) /
            (ALT_HOLD_MAX_HEIGHT_MM - ALT_HOLD_SLOWDOWN_MM);

        setpoint->velocity.z *= fmaxf(0.0f, fminf(1.0f, ceilingScale));
      }

      if (altHoldTakeoffActive) {
        const uint32_t elapsedMs = T2M(xTaskGetTickCount() - altHoldTakeoffStartTick);
        const float ramp = fminf(1.0f,
                                 (float)elapsedMs / (float)ALT_HOLD_TAKEOFF_RAMP_MS);
        float takeoffSpeedLimit = ALT_HOLD_TAKEOFF_SPEED +
            (ALT_HOLD_MAX_RISE_SPEED - ALT_HOLD_TAKEOFF_SPEED) * ramp;

        // Keep the first 25 cm especially gentle. Once clear of the ground,
        // the time ramp above still prevents a sudden jump to full climb rate.
        if (heightMm < ALT_HOLD_LOW_HEIGHT_MM) {
          const float heightScale = fmaxf(0.0f,
              fminf(1.0f, heightMm / ALT_HOLD_LOW_HEIGHT_MM));
          const float lowHeightLimit = ALT_HOLD_TAKEOFF_SPEED +
              (ALT_HOLD_LOW_SPEED - ALT_HOLD_TAKEOFF_SPEED) * heightScale;
          takeoffSpeedLimit = fminf(takeoffSpeedLimit, lowHeightLimit);
        }

        setpoint->velocity.z = fminf(setpoint->velocity.z, takeoffSpeedLimit);
      }
    }

    #endif 
  } else {
    altitudeCeilingActive = false;
    altHoldTakeoffReady = false;
    altHoldTakeoffActive = false;
    landing = false;
    altHoldTakeoffStartTick = 0;
    altHoldLandingConditionStartTick = 0;
    altHoldLandingCountdownStartTick = 0;
    altHoldHoverActive = false;
    positionControllerSetVerticalLandingProtection(false);
    positionControllerSetVerticalDescentThrustLimit(false);
    setpoint->mode.z = modeDisable;
  }

#if ENABLE_LOW_BATTERY_FLIGHT_PROTECTION
  /* In manual stabilize mode the pilot retains full throttle authority.
   * Open-loop thrust reduction cannot guarantee a controlled descent. */
  if (!batteryCritical) {
    posSetLowBatteryLanding = false;
    posSetLowBatteryLandingStartTick = 0;
  }
#endif

  // roll/pitch
  if (posHoldMode && altHoldTakeoffActive) {
    setpoint->mode.x = modeVelocity;
    setpoint->mode.y = modeVelocity;
    setpoint->mode.roll = modeDisable;
    setpoint->mode.pitch = modeDisable;
    setpoint->velocity_body = true;

    setpoint->velocity.x = -values->pitch/30.0f;
    setpoint->velocity.y = -values->roll/30.0f;
    setpoint->attitude.roll  = 0;
    setpoint->attitude.pitch = 0;
  } else if (posSetMode && values->thrust != 0) {
    setpoint->mode.x = modeAbs;
    setpoint->mode.y = modeAbs;
    setpoint->mode.z = modeAbs;
    setpoint->mode.roll = modeDisable;
    setpoint->mode.pitch = modeDisable;
    setpoint->mode.yaw = modeAbs;

    setpoint->position.x = -values->pitch;
    setpoint->position.y = values->roll;
    setpoint->position.z = values->thrust/1000.0f;

    setpoint->attitude.roll  = 0;
    setpoint->attitude.pitch = 0;
    setpoint->attitude.yaw = values->yaw;
    setpoint->thrust = 0;
  } else {
    setpoint->mode.x = modeDisable;
    setpoint->mode.y = modeDisable;

    if (stabilizationModeRoll == RATE) {
      setpoint->mode.roll = modeVelocity;
      setpoint->attitudeRate.roll = values->roll;
      setpoint->attitude.roll = 0;
    } else {
      setpoint->mode.roll = modeAbs;
      setpoint->attitudeRate.roll = 0;
      setpoint->attitude.roll = values->roll;
    }

    if (stabilizationModePitch == RATE) {
      setpoint->mode.pitch = modeVelocity;
      setpoint->attitudeRate.pitch = values->pitch;
      setpoint->attitude.pitch = 0;
    } else {
      setpoint->mode.pitch = modeAbs;
      setpoint->attitudeRate.pitch = 0;
      setpoint->attitude.pitch = values->pitch;
    }

    setpoint->velocity.x = 0;
    setpoint->velocity.y = 0;
  }

#if ENABLE_LOW_BATTERY_FLIGHT_PROTECTION
  if (batteryCritical && flight_mode == POSSET_MODE) {
    const TickType_t now = xTaskGetTickCount();
    if (!posSetLowBatteryLanding && !thrustLocked &&
        rawThrust >= MIN_THRUST) {
      posSetLowBatteryLanding = true;
      posSetLowBatteryLandingStartTick = now;
      lowBatteryAlarmRequested = true;
      DEBUG_PRINTW("POSSET low-battery landing started\n");
    }

    if (posSetLowBatteryLanding) {
      /* Override only Z. The existing X/Y command remains active so the
       * pilot can still steer away from obstacles during descent. */
      setpoint->mode.z = modeVelocity;
      setpoint->velocity.z = -ALT_HOLD_LANDING_SPEED;
      setpoint->thrust = 0;
      if (T2M(now - posSetLowBatteryLandingStartTick) >=
          MANUAL_LOW_BATTERY_LANDING_MS) {
        posSetLowBatteryLanding = false;
        thrustLocked = true;
        memcpy(setpoint, &nullSetpoint, sizeof(*setpoint));
        lowBatteryAlarmRequested = true;
        DEBUG_PRINTW("POSSET low-battery landing complete\n");
      }
    } else {
      setpoint->mode.z = modeDisable;
      setpoint->thrust = 0;
      thrustLocked = true;
      if (rawThrust == 0U) {
        lowBatteryAlarmIssuedForStick = false;
      } else if (!lowBatteryAlarmIssuedForStick) {
        lowBatteryAlarmRequested = true;
        lowBatteryAlarmIssuedForStick = true;
      }
    }
  }
#endif

  // Yaw
  if (!posSetMode) {
    if (stabilizationModeYaw == RATE) {
      // legacy rate input is inverted
      setpoint->attitudeRate.yaw = -values->yaw;
      setpoint->mode.yaw = modeVelocity;
    } else {
      setpoint->mode.yaw = modeAbs;
      setpoint->attitudeRate.yaw = 0;
      setpoint->attitude.yaw = values->yaw;
    }
  }
}

// Params for flight modes
PARAM_GROUP_START(flightmode)
PARAM_ADD(PARAM_UINT8, althold, &altHoldMode)
PARAM_ADD(PARAM_UINT8, poshold, &posHoldMode)
PARAM_ADD(PARAM_UINT8, posSet, &posSetMode)
PARAM_ADD(PARAM_UINT8, yawMode, &yawMode)
PARAM_ADD(PARAM_UINT8, yawRst, &carefreeResetFront)
PARAM_ADD(PARAM_UINT8, stabModeRoll, &stabilizationModeRoll)
PARAM_ADD(PARAM_UINT8, stabModePitch, &stabilizationModePitch)
PARAM_ADD(PARAM_UINT8, stabModeYaw, &stabilizationModeYaw)
PARAM_GROUP_STOP(flightmode)
