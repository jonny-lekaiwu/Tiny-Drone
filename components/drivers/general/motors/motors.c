/**
 *
 * ESP-Drone Firmware
 *
 * Copyright 2019-2020  Espressif Systems (Shanghai)
 * Copyright (C) 2011-2012 Bitcraze AB
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
 * motors.c - Motor driver
 *
 */

#include <stdbool.h>

//FreeRTOS includes
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "stm32_legacy.h"
#include "motors.h"
#include "pm_esplane.h"
#include "log.h"
#define DEBUG_MODULE "MOTORS"
#include "debug_cf.h"



static uint16_t motorsConvBitsTo16(uint16_t bits);
static uint16_t motorsConv16ToBits(uint16_t bits);

uint32_t motor_ratios[] = {0, 0, 0, 0};

void motorsPlayTone(uint16_t frequency, uint16_t duration_msec);
void motorsPlayMelody(uint16_t *notes);
void motorsBeep(int id, bool enable, uint16_t frequency, uint16_t ratio);
static void motorsTestBeep(uint32_t motorId, bool enable, uint16_t frequency);

const MotorPerifDef **motorMap; /* Current map configuration */

const uint32_t MOTORS[] = {MOTOR_M1, MOTOR_M2, MOTOR_M3, MOTOR_M4};

const uint16_t testsound[NBR_OF_MOTORS] = {A4, A5, F5, D5};

// Keep this synchronized with the actual motor PWM timer frequency. Camera
// initialization may later change it to 1000 Hz through pwm_timmer_set_clock().
static uint32_t frequency_now = 15000;

static bool isInit = false;
static bool isTimerInit = false;

#if CONFIG_USING_CAMERA
extern bool camera_init_ok(void);
#endif

ledc_channel_config_t motors_channel[NBR_OF_MOTORS] = {
    {
        .channel = MOT_PWM_CH1,
        .duty = 0,
        .gpio_num = MOTOR1_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_sel = LEDC_TIMER_0
    },
    {
        .channel = MOT_PWM_CH2,
        .duty = 0,
        .gpio_num = MOTOR2_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_sel = LEDC_TIMER_0
    },
    {
        .channel = MOT_PWM_CH3,
        .duty = 0,
        .gpio_num = MOTOR3_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_sel = LEDC_TIMER_0
    },
    {
        .channel = MOT_PWM_CH4,
        .duty = 0,
        .gpio_num = MOTOR4_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_sel = LEDC_TIMER_0
    },
};
/* Private functions */

static uint16_t motorsConvBitsTo16(uint16_t bits)
{
    return ((bits) << (16 - MOTORS_PWM_BITS));
}

static uint16_t motorsConv16ToBits(uint16_t bits)
{
    return ((bits) >> (16 - MOTORS_PWM_BITS) & ((1 << MOTORS_PWM_BITS) - 1));
}

static void motorsTestBeep(uint32_t motorId, bool enable, uint16_t frequency)
{
    const uint16_t ratio = enable ?
        (uint16_t)(((uint32_t)UINT16_MAX * 8U) / 100U) : 0U;

    ASSERT(motorId < NBR_OF_MOTORS);
    ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0,
                  enable ? frequency : frequency_now);
    ledc_set_duty(motors_channel[motorId].speed_mode,
                  motors_channel[motorId].channel,
                  (uint32_t)motorsConv16ToBits(ratio));
    ledc_update_duty(motors_channel[motorId].speed_mode,
                     motors_channel[motorId].channel);
}

bool pwm_timmer_init()
{
    if (isTimerInit) {
        // First to init will configure it
        return TRUE;
    }

    /*
     * Prepare and set configuration of timers
     * that will be used by MOTORS Controller
     */
    ledc_timer_config_t ledc_timer = {
        .duty_resolution = MOTORS_PWM_BITS, // resolution of PWM duty
        .freq_hz = 15000,//15000,//1000,//15000,//1000,//15000,					// frequency of PWM signal
        .speed_mode = LEDC_LOW_SPEED_MODE, // timer mode
        .timer_num = LEDC_TIMER_0,			// timer index
        // .clk_cfg = LEDC_AUTO_CLK,              // Auto select the source clock
    };

    // Set configuration of timer0 for high speed channels
    if (ledc_timer_config(&ledc_timer) == ESP_OK) {
        isTimerInit = TRUE;
        return TRUE;
    } 

    return FALSE;
}

bool pwm_timmer_set_clock(uint16_t hz)
{    
    if (!isInit) 
    {
        motorsInit(motorMapDefaultBrushed);
    }
 
    if (ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, hz) == 0) {
        return FALSE;
    }

    frequency_now = hz;
    return TRUE;
}

/* Public functions */

//Initialization. Will set all motors ratio to 0%
void motorsInit(const MotorPerifDef **motorMapSelect)
{
    int i;

    if (isInit) {
        // First to init will configure it
        return;
    }

    motorMap = motorMapSelect;

    if (pwm_timmer_init() != TRUE) {
        return;
    }

    for (i = 0; i < NBR_OF_MOTORS; i++) {
        ledc_channel_config(&motors_channel[i]);
    }

    isInit = true;
}

void motorsDeInit(const MotorPerifDef **motorMapSelect)
{
    for (int i = 0; i < NBR_OF_MOTORS; i++) {
        ledc_stop(motors_channel[i].speed_mode, motors_channel[i].channel, 0);
    }
}

bool motorsTest(void)
{
    int i;
    const uint32_t runtimeFrequency = frequency_now;
    bool cameraPresent = false;

#if CONFIG_USING_CAMERA
    cameraPresent = camera_init_ok();
#endif

    const uint16_t testFrequency = cameraPresent ? C7 : C5;
    const uint16_t testDurationMs = cameraPresent ? 35U : 100U;

    for (i = 0; i < sizeof(MOTORS) / sizeof(*MOTORS); i++) {
        if (motorMap[i]->drvType == BRUSHED) {
            motorsSetRatio(MOTORS[i], MOTORS_TEST_RATIO);
            vTaskDelay(M2T(MOTORS_TEST_ON_TIME_MS));
            motorsSetRatio(MOTORS[i], 0);
            vTaskDelay(M2T(MOTORS_TEST_DELAY_TIME_MS));

            DEBUG_PRINTI("Motor M%d %s sound: %u Hz, %u ms, duty: 8%%\n",
                         i + 1, cameraPresent ? "camera da" : "no-camera du",
                         (unsigned)testFrequency,
                         (unsigned)testDurationMs);
            motorsTestBeep(MOTORS[i], true, testFrequency);
            vTaskDelay(M2T(testDurationMs));
            motorsTestBeep(MOTORS[i], false, 0);
            vTaskDelay(M2T(MOTORS_TEST_DELAY_TIME_MS));
        }
    }

    /* Camera operation requires 1 kHz. Camera-less boards retain the PWM
     * frequency that was active before the motor test. */
    pwm_timmer_set_clock(cameraPresent ? 1000U : (uint16_t)runtimeFrequency);

    return isInit;
}

// Ithrust is thrust mapped for 65536 <==> 60 grams
void motorsSetRatio(uint32_t id, uint16_t ithrust)
{
    if (isInit) {
        uint16_t ratio;

        ASSERT(id < NBR_OF_MOTORS);

        ratio = ithrust;

#ifdef ENABLE_THRUST_BAT_COMPENSATED

        if (motorMap[id]->drvType == BRUSHED) {
            float thrust = ((float)ithrust / 65536.0f) * 40; //根据实际重量修改
            float volts = -0.0006239f * thrust * thrust + 0.088f * thrust;
            float supply_voltage = pmGetBatteryVoltage();
            float percentage = volts / supply_voltage;
            percentage = percentage > 1.0f ? 1.0f : percentage;
            ratio = percentage * UINT16_MAX;
            motor_ratios[id] = ratio;
        }

#endif
        ledc_set_duty(motors_channel[id].speed_mode, motors_channel[id].channel, (uint32_t)motorsConv16ToBits(ratio));
        ledc_update_duty(motors_channel[id].speed_mode, motors_channel[id].channel);
        motor_ratios[id] = ratio;
#ifdef DEBUG_EP2
        DEBUG_PRINT_LOCAL("motors ID = %d ,ithrust_10bit = %d", id, (uint32_t)motorsConv16ToBits(ratio));
#endif
    }
}

int motorsGetRatio(uint32_t id)
{
    int ratio;
    ASSERT(id < NBR_OF_MOTORS);
    ratio = motorsConvBitsTo16((uint16_t)ledc_get_duty(motors_channel[id].speed_mode, motors_channel[id].channel));
    return ratio;
}

void motorsBeep(int id, bool enable, uint16_t frequency, uint16_t ratio)
{
    uint32_t freq_hz = 15000;
    ASSERT(id < NBR_OF_MOTORS);
    if (ratio != 0) {
        ratio = (uint16_t)(0.05*(1<<16));
    }
    
    if (enable) {
        freq_hz = frequency;
    }
    
    ledc_set_freq(LEDC_LOW_SPEED_MODE,LEDC_TIMER_0,freq_hz);
    ledc_set_duty(motors_channel[id].speed_mode, motors_channel[id].channel, (uint32_t)motorsConv16ToBits(ratio));
    ledc_update_duty(motors_channel[id].speed_mode, motors_channel[id].channel);
}

// Play a tone with a given frequency and a specific duration in milliseconds (ms)
void motorsPlayTone(uint16_t frequency, uint16_t duration_msec)
{
    if (frequency == 0 || duration_msec == 0) {
        return;
    }

    motorsBeep(MOTOR_M1, true, frequency, (uint16_t)(MOTORS_TIM_BEEP_CLK_FREQ / frequency) / 20);
    motorsBeep(MOTOR_M2, true, frequency, (uint16_t)(MOTORS_TIM_BEEP_CLK_FREQ / frequency) / 20);
    motorsBeep(MOTOR_M3, true, frequency, (uint16_t)(MOTORS_TIM_BEEP_CLK_FREQ / frequency) / 20);
    motorsBeep(MOTOR_M4, true, frequency, (uint16_t)(MOTORS_TIM_BEEP_CLK_FREQ / frequency) / 20);
    vTaskDelay(M2T(duration_msec));
    motorsBeep(MOTOR_M1, false, frequency, 0);
    motorsBeep(MOTOR_M2, false, frequency, 0);
    motorsBeep(MOTOR_M3, false, frequency, 0);
    motorsBeep(MOTOR_M4, false, frequency, 0);
}

static uint16_t success_sound[] = {
    C5, 70,
    E5, 70,
    G5, 70,
    C6, 100,
    G5, 50,
    C6, 250,
    0,  0
};

// Fast ascending power-up cue: altitude hold is available.
static uint16_t althold_sound[] = {
    G4, 45,
    C5, 45,
    E5, 45,
    G5, 45,
    C6, 45,
    E6, 45,
    G6, 45,
    C7, 140,
    0,  0
};

// High-register extra-life cue: horizontal position hold is also available.
static uint16_t poshold_sound[] = {
    E6, 70,
    G6, 70,
    E7, 70,
    C7, 70,
    D7, 70,
    G7, 180,
    0,  0
};

// Alternating high/low warning cue: battery is too low for assisted flight.
static uint16_t low_battery_error_sound[] = {
    C6, 90,
    G4, 160,
    C6, 90,
    G4, 240,
    0,  0
};

// Three descending tones: the primary MPU6050 could not be initialized.
static uint16_t mpu6050_error_sound[] = {
    E7, 100,
    C6, 160,
    G4, 360,
    0,  0
};

/* Two sharp high/low pairs: thrust is locked after a tumble detection. */
static uint16_t tumble_error_sound[] = {
    G7, 70,
    D5, 150,
    G7, 70,
    D5, 280,
    0,  0
};

// Plays a melody from a note array
void motorsPlayMelody(uint16_t *notes)
{
    int i = 0;

    while (notes[i] != 0 && notes[i + 1] != 0)
    {
        motorsPlayTone(notes[i], notes[i + 1]);
        i += 2;
    }

    // motorsBeep() temporarily changes the shared LEDC timer. Restore the
    // current requested motor PWM frequency (1000 Hz after camera init).
    if (ledc_get_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0) != frequency_now) {
        ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, frequency_now);
    }
}

void play_succeed(void)
{
    motorsPlayMelody(success_sound);
}

void play_althold_mode(void)
{
    motorsPlayMelody(althold_sound);
}

void play_poshold_mode(void)
{
    motorsPlayMelody(poshold_sound);
}

void play_low_battery_error(void)
{
    motorsPlayMelody(low_battery_error_sound);
}

void play_mpu6050_error(void)
{
    motorsPlayMelody(mpu6050_error_sound);
}

void play_tumble_error(void)
{
    motorsPlayMelody(tumble_error_sound);
}

LOG_GROUP_START(pwm)
LOG_ADD(LOG_UINT32, m1_pwm, &motor_ratios[0])
LOG_ADD(LOG_UINT32, m2_pwm, &motor_ratios[1])
LOG_ADD(LOG_UINT32, m3_pwm, &motor_ratios[2])
LOG_ADD(LOG_UINT32, m4_pwm, &motor_ratios[3])
LOG_GROUP_STOP(pwm)
