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
 * led.h - LED functions header file
 */
#ifndef __LED_H__
#define __LED_H__

#include <stdbool.h>

//Led polarity configuration constant
#define LED_POL_POS 0
#define LED_POL_NEG 1

#define LED_INVALID    0xFF

#define LED_GPIO_BLUE  CONFIG_LED_PIN_BLUE
#define LED_POL_BLUE   LED_POL_POS
#define LED_GPIO_GREEN LED_INVALID  //different from pcb design
#define LED_POL_GREEN  LED_POL_POS
#define LED_GPIO_RED   LED_INVALID
#define LED_POL_RED    LED_POL_POS

#define LINK_LED         LED_INVALID
#define CHG_LED          LED_INVALID
#define LOWBAT_LED       LED_INVALID
#define LINK_DOWN_LED    LED_BLUE
#define SYS_LED          LED_BLUE
#define ERR_LED1         LED_INVALID
#define ERR_LED2         LED_INVALID

#define LED_NUM 1

typedef enum {LED_BLUE = 0} led_t;

void ledInit();
bool ledTest();

// Clear all configured LEDs
void ledClearAll(void);

// Set all configured LEDs
void ledSetAll(void);

// Procedures to set the status of the LEDs
void ledSet(led_t led, bool value);

void ledTask(void *param);
 

#endif
