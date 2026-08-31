/**
 * ESP-Drone Firmware
 *
 * Copyright 2019-2020  Espressif Systems (Shanghai)
 * Copyright (c) 2014, Bitcraze AB, All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3.0 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.
 *
 * i2c_drv.c - i2c driver implementation
 *
 * @note
 * For some reason setting CR1 reg in sequence with
 * I2C_AcknowledgeConfig(I2C_SENSORS, ENABLE) and after
 * I2C_GenerateSTART(I2C_SENSORS, ENABLE) sometimes creates an
 * instant start->stop condition (3.9us long) which I found out with an I2C
 * analyzer. This fast start->stop is only possible to generate if both
 * start and stop flag is set in CR1 at the same time. So i tried setting the CR1
 * at once with I2C_SENSORS->CR1 = (I2C_CR1_START | I2C_CR1_ACK | I2C_CR1_PE) and the
 * problem is gone. Go figure...
 */


#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "stm32_legacy.h"
#include "i2c_drv.h"
#include "config.h"
#define DEBUG_MODULE "I2CDRV"
#include "debug_cf.h"

 
// Definitions of sensors I2C bus
#define I2C_DEFAULT_SENSORS_CLOCK_SPEED             400000

// Definition of eeprom and deck I2C buss,use two i2c with 400Khz clock simultaneously could trigger the watchdog
#define I2C_DEFAULT_DECK_CLOCK_SPEED                100000

static bool isinit_i2cPort[I2C_LOGICAL_BUS_COUNT] = {false};
 
static const I2cDef sensorBusDef = {
    .i2cPort            = I2C_NUM_0,
    .i2cClockSpeed      = I2C_DEFAULT_SENSORS_CLOCK_SPEED,
    .scl_pin            = CONFIG_MPU_PIN_SCL,
    .sda_pin            = CONFIG_MPU_PIN_SDA,
    .gpioPullup         = GPIO_PULLUP_DISABLE,
};

I2cDrv sensorsBus = {
    .cfg                = &sensorBusDef, 
};

static const I2cDef deckBusDef = {
    .i2cPort            = I2C_NUM_0+1,
    .i2cClockSpeed      = I2C_DEFAULT_DECK_CLOCK_SPEED,
    .scl_pin            = CONFIG_ZRANGE_PIN_SCL,
    .sda_pin            = CONFIG_ZRANGE_PIN_SDA,
    .gpioPullup         = GPIO_PULLUP_ENABLE,//GPIO_PULLUP_DISABLE,//GPIO_PULLUP_ENABLE,
};

I2cDrv deckBus = {
    .cfg                = &deckBusDef,
};
 
static esp_err_t sw_i2c_init(const I2cDef *cfg)
{   
    if ((-1 == cfg->sda_pin) || (-1 == cfg->scl_pin))
    {
        return ESP_FAIL;
    }

    /* SDA must be released while reading ACK/data, so it remains open-drain. */
    gpio_config_t sda_config = {
        .pin_bit_mask = 1ULL << cfg->sda_pin,
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = cfg->gpioPullup,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    /* SCL is actively driven high and low. This is intentional: the software
     * implementation does not support clock stretching and must not depend on
     * a weak/internal pull-up for its rising edge. */
    gpio_config_t scl_config = {
        .pin_bit_mask = 1ULL << cfg->scl_pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&sda_config);
    if (err != ESP_OK)
    {
        return err;
    }

    err = gpio_config(&scl_config);
    if (err != ESP_OK)
    {
        return err;
    }

    gpio_set_level(cfg->sda_pin, 1);
    gpio_set_level(cfg->scl_pin, 1);

    return ESP_OK;
}  

i2c_master_dev_handle_t i2cdrvGetDevice(I2cDrv *i2c,uint8_t addr)
{
    if(i2c->dev_handle[addr])
    {
        return i2c->dev_handle[addr];
    }

    i2c_device_config_t dev_cfg =
    {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,

        .device_address = addr,

        .scl_speed_hz = i2c->cfg->i2cClockSpeed,
    };

    esp_err_t ret =
        i2c_master_bus_add_device(
            i2c->bus_handle,
            &dev_cfg,
            &i2c->dev_handle[addr]);

    if(ret != ESP_OK)
    {
        return NULL;
    }

    return i2c->dev_handle[addr];
}

static void i2cdrvInitBus(I2cDrv *i2c)
{ 
    esp_err_t err = -1;
    const int logicalPort = (int)i2c->cfg->i2cPort;

    if (logicalPort < 0 || logicalPort >= I2C_LOGICAL_BUS_COUNT)
    {
        DEBUG_PRINTI("invalid logical i2c bus: %d", logicalPort);
        return;
    }

    if (i2c->init_ok || isinit_i2cPort[logicalPort])
    {
        return;
    }

    /* Logical port 2 is only an internal ID for the GPIO software bus. */
    if (!i2c->is_sw_i2c && logicalPort >= I2C_NUM_MAX)
    {
        DEBUG_PRINTI("logical i2c bus %d cannot use hardware I2C", logicalPort);
        return;
    }

    if (!i2c->is_sw_i2c)
    { 
        i2c_master_bus_config_t bus_cfg =
        {
            .clk_source = I2C_CLK_SRC_DEFAULT,

            .i2c_port = i2c->cfg->i2cPort,

            .scl_io_num = i2c->cfg->scl_pin   ,

            .sda_io_num = i2c->cfg->sda_pin   ,

            .glitch_ignore_cnt = 7,

            .flags.enable_internal_pullup =
                i2c->cfg->gpioPullup,
        };

        err = i2c_new_master_bus(
                    &bus_cfg,
                    &i2c->bus_handle); 
    }
    else
    { 
        err = sw_i2c_init(i2c->cfg); 
    } 

    i2c->init_ok = (ESP_OK == err);

    DEBUG_PRINTI("i2c %d new bus res=%d, init_ok:%d", logicalPort, err, i2c->init_ok);

    if (i2c->init_ok)
    {
        i2c->isBusFreeMutex = xSemaphoreCreateMutex();
        isinit_i2cPort[logicalPort] = true;
    }
}


//-----------------------------------------------------------

void i2cdrvInit(I2cDrv *i2c)
{
    i2cdrvInitBus(i2c);
}

void i2cdrvTryToRestartBus(I2cDrv *i2c)
{
    i2cdrvInitBus(i2c);
}

