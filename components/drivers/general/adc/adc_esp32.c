/**
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
 * adc.c - Analog Digital Conversion
 *
 *
 */

#include "esp_idf_version.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "adc_esp32.h"
#include "config.h"
#include "pm_esplane.h"
#include "stm32_legacy.h"
#define DEBUG_MODULE "ADC"
#include "debug_cf.h"

static bool isInit;

#define CONFIG_EN_ADC 1

/* Both boards route battery voltage to GPIO1, but GPIO1 is connected to a
 * different ADC1 channel on ESP32-S3 and ESP32-C3. Keep the physical pin in
 * the board configuration and select the SoC ADC channel here. */
#if defined(CONFIG_IDF_TARGET_ESP32S3)
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_0
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_1
#else
#error "Battery ADC channel is not defined for this target"
#endif

static esp_adc_cal_characteristics_t *adc_chars;

#if CONFIG_EN_ADC

static const adc_channel_t channel = BATTERY_ADC_CHANNEL;

static const adc_bits_width_t width = ADC_WIDTH_MAX-1;
static const adc_atten_t atten = 3; // we directly set the attenuation to 3(11dB/12dB) to avoid the build warning
static const adc_unit_t unit = ADC_UNIT_1;
#endif

#define DEFAULT_VREF 1100 //Use adc2_vref_to_gpio() to obtain a better estimate
#define NO_OF_SAMPLES   30          //Multisampling

static void print_char_val_type(esp_adc_cal_value_t val_type)
{
    if (val_type == ESP_ADC_CAL_VAL_EFUSE_TP) {
        //printf("Characterized using Two Point Value\n");
    } else if (val_type == ESP_ADC_CAL_VAL_EFUSE_VREF) {
        //printf("Characterized using eFuse Vref\n");
    } else {
        //printf("Characterized using Default Vref\n");
    }
}

float analogReadVoltage(uint32_t pin)
{
    #if CONFIG_EN_ADC
    uint32_t adc_reading = 0;
    for (int i = 0; i < NO_OF_SAMPLES; i++) {
        if (unit == ADC_UNIT_1) {
            adc_reading += adc1_get_raw((adc1_channel_t)channel);
        } else {
            int raw;
            adc2_get_raw((adc2_channel_t)channel, width, &raw);
            adc_reading += raw;
        }
    }
    adc_reading /= NO_OF_SAMPLES;
    //Convert adc_reading to voltage in mV
    uint32_t voltage = esp_adc_cal_raw_to_voltage(adc_reading, adc_chars);
    return voltage / 1000.0; 
    #else
    return 0;
    #endif
}

void adcInit(void)
{
    if (isInit) {
        return;
    }

    #if CONFIG_EN_ADC
    if (unit == ADC_UNIT_1) {
        adc1_config_width(width);
        adc1_config_channel_atten(channel, atten);
    } else {
        adc2_config_channel_atten((adc2_channel_t)channel, atten);
    }

    // //Characterize ADC
    adc_chars = calloc(1, sizeof(esp_adc_cal_characteristics_t));
    esp_adc_cal_value_t val_type = esp_adc_cal_characterize(unit, atten, width, DEFAULT_VREF, adc_chars);
    print_char_val_type(val_type);
    #endif

    isInit = true;
}

bool adcTest(void)
{
    return isInit;
}
