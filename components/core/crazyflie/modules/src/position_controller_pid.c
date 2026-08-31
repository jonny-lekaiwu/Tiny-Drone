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
 * position_estimator_pid.c: PID-based implementation of the position controller
 */

#include <math.h>
#include "num.h"

#include "commander.h"
#include "log.h"
#include "param.h"
#include "pid.h"
#include "num.h"
#include "position_controller.h"
#include "position_estimator.h"
#define DEBUG_MODULE "POSITION_CONTROLLER"
#include "debug_cf.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct pidInit_s {
  float kp;
  float ki;
  float kd;
};

struct pidAxis_s {
  PidObject pid;

  struct pidInit_s init;
    stab_mode_t previousMode;
  float setpoint;

  float output;
};

struct this_s {
  struct pidAxis_s pidVX;
  struct pidAxis_s pidVY;
  struct pidAxis_s pidVZ;

  struct pidAxis_s pidX;
  struct pidAxis_s pidY;
  struct pidAxis_s pidZ;
  struct pidAxis_s pidZBaro;

  uint16_t thrustBase; // approximate throttle needed when in perfect hover. More weight/older battery can use a higher value
  uint16_t thrustMin;  // Minimum thrust value to output
};

// Maximum roll/pitch angle permited
static float rpLimit  = 20;
static float rpLimitOverhead = 1.10f;
// Velocity maximums
static float xyVelMax = 1.0f;
static float zVelMax  = 1.0f;
static float velMaxOverhead = 1.10f;
static const float thrustScale = 1000.0f;
static bool baroHeightHoldActive = false;
static float baroHeightSetpoint = 0.0f;
static volatile bool verticalResetRequested = false;
static volatile bool verticalLandingProtection = false;
static bool verticalLandingProtectionApplied = false;
static uint16_t verticalTakeoverBlendSteps = 0;
static float verticalTakeoverInitialVelocity = 0.0f;

#define VERTICAL_TAKEOVER_BLEND_STEPS 60U

void positionControllerRequestVerticalReset(uint16_t takeoverThrust)
{
  /* Kept in the API for compatibility. Manual collective is deliberately not
   * inherited by altitude hold; only vertical velocity is blended. */
  (void)takeoverThrust;
  verticalResetRequested = true;
}

void positionControllerSetVerticalLandingProtection(bool enabled)
{
  verticalLandingProtection = enabled;
}

#define DT (float)(1.0f/POSITION_RATE)
#define POSITION_LPF_CUTOFF_FREQ 20.0f
#define POSITION_LPF_ENABLE true

#ifndef UNIT_TEST
static struct this_s this = {
  .pidVX = {
    .init = {
      .kp = 25.0f,
      .ki = 1.0f,
      .kd = 0.0f,
    },
    .pid.dt = DT,
  },

  .pidVY = {
    .init = {
      .kp = 25.0f,
      .ki = 1.0f,
      .kd = 0.0f,
    },
    .pid.dt = DT,
  },

  .pidVZ = {
    .init = {
      .kp = 22,
      .ki = 15,
      .kd = 0,
    },
    .pid.dt = DT,
  },

  .pidX = {
    .init = {
      .kp = 1.9f,
      .ki = 0.1f,
      .kd = 0,
    },
    .pid.dt = DT,
  },

  .pidY = {
    .init = {
      .kp = 1.9f,
      .ki = 0.1f,
      .kd = 0,
    },
    .pid.dt = DT,
  },

  .pidZ = {
    .init = {
      .kp = 1.6f,
      .ki = 0.5,
      .kd = 0,
    },
    .pid.dt = DT,
  },

  .pidZBaro = {
    .init = {
      .kp = 0.8f,
      .ki = 0.05f,
      .kd = 0.0f,
    },
    .pid.dt = DT,
  },

//thrustBase should just lift the drone
#ifdef CONFIG_MOTOR_BRUSHED_715
 // #ifdef CONFIG_TARGET_TINY_DRONE_V1_0
  .thrustBase = 42000,
  .thrustMin  = 8000,
  // #else
  // .thrustBase = 36000,
  // .thrustMin  = 20000,
  // #endif
#else
  .thrustBase = 24000,
  .thrustMin  = 5000,
#endif

};
#endif

void positionControllerInit()
{
  pidInit(&this.pidX.pid, this.pidX.setpoint, this.pidX.init.kp, this.pidX.init.ki, this.pidX.init.kd,
      this.pidX.pid.dt, POSITION_RATE, POSITION_LPF_CUTOFF_FREQ, POSITION_LPF_ENABLE);
  pidInit(&this.pidY.pid, this.pidY.setpoint, this.pidY.init.kp, this.pidY.init.ki, this.pidY.init.kd,
      this.pidY.pid.dt, POSITION_RATE, POSITION_LPF_CUTOFF_FREQ, POSITION_LPF_ENABLE);
  pidInit(&this.pidZ.pid, this.pidZ.setpoint, this.pidZ.init.kp, this.pidZ.init.ki, this.pidZ.init.kd,
      this.pidZ.pid.dt, POSITION_RATE, POSITION_LPF_CUTOFF_FREQ, POSITION_LPF_ENABLE);

  pidInit(&this.pidVX.pid, this.pidVX.setpoint, this.pidVX.init.kp, this.pidVX.init.ki, this.pidVX.init.kd,
      this.pidVX.pid.dt, POSITION_RATE, POSITION_LPF_CUTOFF_FREQ, POSITION_LPF_ENABLE);
  pidInit(&this.pidVY.pid, this.pidVY.setpoint, this.pidVY.init.kp, this.pidVY.init.ki, this.pidVY.init.kd,
      this.pidVY.pid.dt, POSITION_RATE, POSITION_LPF_CUTOFF_FREQ, POSITION_LPF_ENABLE);
  pidInit(&this.pidVZ.pid, this.pidVZ.setpoint, this.pidVZ.init.kp, this.pidVZ.init.ki, this.pidVZ.init.kd,
      this.pidVZ.pid.dt, POSITION_RATE, POSITION_LPF_CUTOFF_FREQ, POSITION_LPF_ENABLE);
  pidInit(&this.pidZBaro.pid, this.pidZBaro.setpoint, this.pidZBaro.init.kp,
      this.pidZBaro.init.ki, this.pidZBaro.init.kd, this.pidZBaro.pid.dt,
      POSITION_RATE, POSITION_LPF_CUTOFF_FREQ, POSITION_LPF_ENABLE);
  pidSetIntegralLimit(&this.pidZBaro.pid, 0.25f);
  DEBUG_PRINTI("thrustBase = %d,thrustMin  = %d",this.thrustBase,this.thrustMin);
}

static float runPid(float input, struct pidAxis_s *axis, float setpoint, float dt) {
  axis->setpoint = setpoint;

  pidSetDesired(&axis->pid, axis->setpoint);
  return pidUpdate(&axis->pid, input, true);
}

void positionController(float* thrust, attitude_t *attitude, setpoint_t *setpoint,
                                                             const state_t *state)
{
  if (verticalLandingProtection && !verticalLandingProtectionApplied) {
    /* Ground contact can make the vertical estimator report a short downward
     * velocity spike. Drop accumulated vertical corrections before they can
     * turn that spike into an upward thrust pulse. */
    pidReset(&this.pidZ.pid);
    pidReset(&this.pidZBaro.pid);
    pidReset(&this.pidVZ.pid);
    baroHeightHoldActive = false;
    verticalTakeoverBlendSteps = 0;
    verticalLandingProtectionApplied = true;
  } else if (!verticalLandingProtection) {
    verticalLandingProtectionApplied = false;
  }

  if (verticalResetRequested) {
    pidReset(&this.pidZ.pid);
    pidReset(&this.pidZBaro.pid);
    pidReset(&this.pidVZ.pid);
    baroHeightHoldActive = false;
    /* A bumpless transfer must preserve both collective thrust and vertical
     * motion. Otherwise a rising manual takeoff is abruptly commanded to
     * zero velocity on the first altitude-hold cycle. Limit the captured
     * estimate so a transient estimator spike cannot become a large command. */
    verticalTakeoverInitialVelocity = constrain(state->velocity.z, -0.30f, 0.50f);
    verticalTakeoverBlendSteps = VERTICAL_TAKEOVER_BLEND_STEPS;
    verticalResetRequested = false;
  }

  const bool barometerPrimary = positionEstimatorAltitudeBarometerIsPrimary();
  this.pidX.pid.outputLimit = xyVelMax * velMaxOverhead;
  this.pidY.pid.outputLimit = xyVelMax * velMaxOverhead;
  // The ROS landing detector will prematurely trip if
  // this value is below 0.5
  this.pidZ.pid.outputLimit = fmaxf(zVelMax, 0.5f)  * velMaxOverhead;

  float cosyaw = cosf(state->attitude.yaw * (float)M_PI / 180.0f);
  float sinyaw = sinf(state->attitude.yaw * (float)M_PI / 180.0f);
  float bodyvx = setpoint->velocity.x;
  float bodyvy = setpoint->velocity.y;

  // X, Y
  if (setpoint->mode.x == modeAbs) {
    setpoint->velocity.x = runPid(state->position.x, &this.pidX, setpoint->position.x, DT);
  } else if (setpoint->velocity_body) {
    setpoint->velocity.x = bodyvx * cosyaw - bodyvy * sinyaw;
  }
  if (setpoint->mode.y == modeAbs) {
    setpoint->velocity.y = runPid(state->position.y, &this.pidY, setpoint->position.y, DT);
  } else if (setpoint->velocity_body) {
    setpoint->velocity.y = bodyvy * cosyaw + bodyvx * sinyaw;
  }

  if (verticalTakeoverBlendSteps > 0U && setpoint->mode.z != modeDisable) {
    const float blend = 1.0f -
        (float)verticalTakeoverBlendSteps /
        (float)VERTICAL_TAKEOVER_BLEND_STEPS;
    setpoint->velocity.z =
        (1.0f - blend) * verticalTakeoverInitialVelocity +
        blend * setpoint->velocity.z;
  }

  if (setpoint->mode.z == modeAbs) {
    setpoint->velocity.z = runPid(state->position.z, &this.pidZ, setpoint->position.z, DT);
    baroHeightHoldActive = false;
  } else if (barometerPrimary && setpoint->mode.z == modeVelocity &&
             fabsf(setpoint->velocity.z) < 0.01f) {
    if (!baroHeightHoldActive) {
      baroHeightSetpoint = state->position.z;
      baroHeightHoldActive = true;
      pidReset(&this.pidZBaro.pid);
    }
    this.pidZBaro.pid.outputLimit = 0.5f;
    setpoint->velocity.z = runPid(state->position.z, &this.pidZBaro,
                                  baroHeightSetpoint, DT);
  } else {
    baroHeightHoldActive = false;
    pidReset(&this.pidZBaro.pid);
  }

  velocityController(thrust, attitude, setpoint, state);

  if (verticalTakeoverBlendSteps > 0U && setpoint->mode.z != modeDisable) {
    /* The velocity target is blended above. Let the reset velocity PID
     * generate hover/braking collective immediately instead of preserving an
     * unknown manual takeoff thrust. */
    verticalTakeoverBlendSteps--;
  } else if (setpoint->mode.z == modeDisable) {
    /* A stop or mode change during takeover must not leak the remaining
     * blend into a later POSHOLD or altitude-control activation. */
    verticalTakeoverBlendSteps = 0;
    verticalTakeoverInitialVelocity = 0.0f;
  }

  if (verticalLandingProtection && *thrust > this.thrustBase) {
    /* Landing must never command more than nominal hover thrust. Normal
     * negative-velocity control remains active below this safety ceiling. */
    *thrust = this.thrustBase;
  }

}

void velocityController(float* thrust, attitude_t *attitude, setpoint_t *setpoint,
                                                             const state_t *state)
{
  this.pidVX.pid.outputLimit = rpLimit * rpLimitOverhead;
  this.pidVY.pid.outputLimit = rpLimit * rpLimitOverhead;
  // Set the output limit to the maximum thrust range
  this.pidVZ.pid.outputLimit = (UINT16_MAX / 2 / thrustScale);
  //this.pidVZ.pid.outputLimit = (this.thrustBase - this.thrustMin) / thrustScale;

  // Roll and Pitch
  float rollRaw  = runPid(state->velocity.x, &this.pidVX, setpoint->velocity.x, DT);
  float pitchRaw = runPid(state->velocity.y, &this.pidVY, setpoint->velocity.y, DT);

  float yawRad = state->attitude.yaw * (float)M_PI / 180;
  attitude->pitch = -(rollRaw  * cosf(yawRad)) - (pitchRaw * sinf(yawRad));
  attitude->roll  = -(pitchRaw * cosf(yawRad)) + (rollRaw  * sinf(yawRad));

  attitude->roll  = constrain(attitude->roll,  -rpLimit, rpLimit);
  attitude->pitch = constrain(attitude->pitch, -rpLimit, rpLimit);

  // Thrust
  float thrustRaw = runPid(state->velocity.z, &this.pidVZ,
                           setpoint->velocity.z, DT);
  // Scale the thrust and add feed forward term
  *thrust = thrustRaw*thrustScale + this.thrustBase;
  // Check for minimum thrust
  if (*thrust < this.thrustMin) {
    *thrust = this.thrustMin;
  }
}

void positionControllerResetAllPID()
{
  pidReset(&this.pidX.pid);
  pidReset(&this.pidY.pid);
  pidReset(&this.pidZ.pid);
  pidReset(&this.pidZBaro.pid);
  pidReset(&this.pidVX.pid);
  pidReset(&this.pidVY.pid);
  pidReset(&this.pidVZ.pid);
  baroHeightHoldActive = false;
  verticalLandingProtectionApplied = false;
}

LOG_GROUP_START(posCtl)

LOG_ADD(LOG_FLOAT, targetVX, &this.pidVX.pid.desired)
LOG_ADD(LOG_FLOAT, targetVY, &this.pidVY.pid.desired)
LOG_ADD(LOG_FLOAT, targetVZ, &this.pidVZ.pid.desired)

LOG_ADD(LOG_FLOAT, targetX, &this.pidX.pid.desired)
LOG_ADD(LOG_FLOAT, targetY, &this.pidY.pid.desired)
LOG_ADD(LOG_FLOAT, targetZ, &this.pidZ.pid.desired)

LOG_ADD(LOG_FLOAT, Xp, &this.pidX.pid.outP)
LOG_ADD(LOG_FLOAT, Xi, &this.pidX.pid.outI)
LOG_ADD(LOG_FLOAT, Xd, &this.pidX.pid.outD)

LOG_ADD(LOG_FLOAT, Yp, &this.pidY.pid.outP)
LOG_ADD(LOG_FLOAT, Yi, &this.pidY.pid.outI)
LOG_ADD(LOG_FLOAT, Yd, &this.pidY.pid.outD)

LOG_ADD(LOG_FLOAT, Zp, &this.pidZ.pid.outP)
LOG_ADD(LOG_FLOAT, Zi, &this.pidZ.pid.outI)
LOG_ADD(LOG_FLOAT, Zd, &this.pidZ.pid.outD)

LOG_ADD(LOG_FLOAT, VXp, &this.pidVX.pid.outP)
LOG_ADD(LOG_FLOAT, VXi, &this.pidVX.pid.outI)
LOG_ADD(LOG_FLOAT, VXd, &this.pidVX.pid.outD)

LOG_ADD(LOG_FLOAT, VZp, &this.pidVZ.pid.outP)
LOG_ADD(LOG_FLOAT, VZi, &this.pidVZ.pid.outI)
LOG_ADD(LOG_FLOAT, VZd, &this.pidVZ.pid.outD)

LOG_GROUP_STOP(posCtl)

PARAM_GROUP_START(velCtlPid)

PARAM_ADD(PARAM_FLOAT, vxKp, &this.pidVX.pid.kp)
PARAM_ADD(PARAM_FLOAT, vxKi, &this.pidVX.pid.ki)
PARAM_ADD(PARAM_FLOAT, vxKd, &this.pidVX.pid.kd)

PARAM_ADD(PARAM_FLOAT, vyKp, &this.pidVY.pid.kp)
PARAM_ADD(PARAM_FLOAT, vyKi, &this.pidVY.pid.ki)
PARAM_ADD(PARAM_FLOAT, vyKd, &this.pidVY.pid.kd)

PARAM_ADD(PARAM_FLOAT, vzKp, &this.pidVZ.pid.kp)
PARAM_ADD(PARAM_FLOAT, vzKi, &this.pidVZ.pid.ki)
PARAM_ADD(PARAM_FLOAT, vzKd, &this.pidVZ.pid.kd)

PARAM_GROUP_STOP(velCtlPid)

PARAM_GROUP_START(posCtlPid)

PARAM_ADD(PARAM_FLOAT, xKp, &this.pidX.pid.kp)
PARAM_ADD(PARAM_FLOAT, xKi, &this.pidX.pid.ki)
PARAM_ADD(PARAM_FLOAT, xKd, &this.pidX.pid.kd)

PARAM_ADD(PARAM_FLOAT, yKp, &this.pidY.pid.kp)
PARAM_ADD(PARAM_FLOAT, yKi, &this.pidY.pid.ki)
PARAM_ADD(PARAM_FLOAT, yKd, &this.pidY.pid.kd)

PARAM_ADD(PARAM_FLOAT, zKp, &this.pidZ.pid.kp)
PARAM_ADD(PARAM_FLOAT, zKi, &this.pidZ.pid.ki)
PARAM_ADD(PARAM_FLOAT, zKd, &this.pidZ.pid.kd)
PARAM_ADD(PARAM_FLOAT, baroZKp, &this.pidZBaro.pid.kp)
PARAM_ADD(PARAM_FLOAT, baroZKi, &this.pidZBaro.pid.ki)
PARAM_ADD(PARAM_FLOAT, baroZKd, &this.pidZBaro.pid.kd)

PARAM_ADD(PARAM_UINT16, thrustBase, &this.thrustBase)
PARAM_ADD(PARAM_UINT16, thrustMin, &this.thrustMin)

PARAM_ADD(PARAM_FLOAT, rpLimit,  &rpLimit)
PARAM_ADD(PARAM_FLOAT, xyVelMax, &xyVelMax)
PARAM_ADD(PARAM_FLOAT, zVelMax,  &zVelMax)

PARAM_GROUP_STOP(posCtlPid)
