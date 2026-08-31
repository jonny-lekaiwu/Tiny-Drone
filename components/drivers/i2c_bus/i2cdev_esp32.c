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
 *
 * i2cdev.c - Functions to write to I2C devices
 */
#define DEBUG_MODULE "I2CDEV"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "stm32_legacy.h"
#include "i2cdev.h"
#include "i2c_drv.h"
#include "nvicconf.h"
#include "debug_cf.h"

#define SW_PRINT_I2CDEV 1

#if SW_PRINT_I2CDEV 
    #include "esp_log.h"
    static const char * TAG = "i2c_dev";
    #define PRINT_I2CDEV(fmt, args...)  do{printf("%s: ", TAG); printf(fmt, ##args); printf("\r\n");} while(0)
#else
    #define PRINT_I2CDEV(...)
#endif



static portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;

#if CONFIG_USING_GPIO_REG 

#include "hal/gpio_ll.h"
#include "soc/gpio_periph.h"
#include <driver/gpio.h> 
#include <rom/ets_sys.h> 
 
#define i2c_hw_t I2cDef  

static inline void gpio_enable(uint8_t pin)
{
    if(pin<32)
        REG_WRITE(GPIO_ENABLE_W1TS_REG, BIT(pin)); 
    #if CONFIG_IDF_TARGET_ESP32S3
    else
        REG_WRITE(GPIO_ENABLE1_W1TS_REG, BIT(pin-32));
    #endif
}

static inline void gpio_disable(uint8_t pin)
{
    if(pin<32)
        REG_WRITE(GPIO_ENABLE_W1TC_REG, BIT(pin)); 
    #if CONFIG_IDF_TARGET_ESP32S3
    else
        REG_WRITE(GPIO_ENABLE1_W1TC_REG, BIT(pin-32));
    #endif
}

static inline void gpio_low(uint8_t pin)
{
    if(pin<32)
        REG_WRITE(GPIO_OUT_W1TC_REG, BIT(pin)); 
    #if CONFIG_IDF_TARGET_ESP32S3
    else
        REG_WRITE(GPIO_OUT1_W1TC_REG, BIT(pin-32));
    #endif
}

#define SDA_OUT(cfg) do{}while(0)
#define SDA_IN(cfg) do{gpio_set_direction(cfg->sda_pin, GPIO_MODE_INPUT);}while(0)   
#define READ_SDA(cfg) (gpio_get_level(cfg->sda_pin)) 

#define SDA_H(cfg) gpio_disable(cfg->sda_pin)
#define SDA_L(cfg) do{gpio_low(cfg->sda_pin);gpio_enable(cfg->sda_pin);}while(0)

#define SCL_H(cfg) gpio_set_level(cfg->scl_pin, 1)
#define SCL_L(cfg) gpio_set_level(cfg->scl_pin, 0)

#define I2C_BIT_DELAY_US        1//2
#define I2C_START_STOP_DELAY_US 2//4

static inline void sw_i2c_master_start(i2c_hw_t *cfg)
{
    SDA_OUT(cfg);
    SDA_H(cfg);
    SCL_H(cfg);
    ets_delay_us(I2C_START_STOP_DELAY_US);
    SDA_L(cfg);
    ets_delay_us(I2C_START_STOP_DELAY_US);
    SCL_L(cfg);
}	

static inline void sw_i2c_master_stop(i2c_hw_t *cfg)
{ 
    SDA_OUT(cfg);
    SCL_L(cfg);
    SDA_L(cfg);
    ets_delay_us(I2C_START_STOP_DELAY_US);
    SCL_H(cfg);
    ets_delay_us(I2C_START_STOP_DELAY_US);
    SDA_H(cfg);
} 

static inline bool sw_i2c_master_wait_ack(i2c_hw_t *cfg)
{
    uint8_t timeout = 0;

    SCL_L(cfg);
    SDA_IN(cfg);
    SDA_H(cfg);
    ets_delay_us(1);
    SCL_H(cfg);
    ets_delay_us(1);

    while (READ_SDA(cfg))
    {
        if (++timeout > 250)
        {
            sw_i2c_master_stop(cfg);
            return false;
        } 
    }

    SCL_L(cfg);

    return true;
}

static inline esp_err_t sw_i2c_master_write_byte(i2c_hw_t *cfg, uint8_t txd)
{     
    SDA_OUT(cfg);
    SCL_L(cfg);

    for (uint8_t i = 0; i < 8; i++) {
        if (txd & 0x80) {
            SDA_H(cfg);
        } else {
            SDA_L(cfg);
        }

        txd <<= 1;
        ets_delay_us(I2C_BIT_DELAY_US);
        SCL_H(cfg);
        ets_delay_us(I2C_BIT_DELAY_US);
        SCL_L(cfg);
        ets_delay_us(I2C_BIT_DELAY_US);
    }

    if (!sw_i2c_master_wait_ack(cfg))
    {
        return 1;
    }

    return 0;
} 	 

static inline esp_err_t sw_i2c_master_write(i2c_hw_t *cfg, uint8_t *buf, uint16_t size)
{     
    for (uint16_t i=0; i<size; i++)
    {     
        if (sw_i2c_master_write_byte(cfg, buf[i]))
        {  
            return 1;
        } 
    }    

    return 0;
} 	 

static inline uint8_t sw_i2c_master_read(i2c_hw_t *cfg, unsigned char ack)
{
    uint8_t receive = 0;

    SDA_IN(cfg);

    for (uint8_t i = 0; i < 8; i++) {
        SCL_L(cfg);
        ets_delay_us(I2C_BIT_DELAY_US);
        SCL_H(cfg);
        receive <<= 1;
        if (READ_SDA(cfg)) {
            receive++;
        }
        ets_delay_us(1);
    }

    SCL_L(cfg);
    SDA_OUT(cfg);
    if (ack) {
        SDA_L(cfg);
    } else {
        SDA_H(cfg);
    }
    ets_delay_us(I2C_BIT_DELAY_US);
    SCL_H(cfg);
    ets_delay_us(I2C_BIT_DELAY_US);
    SCL_L(cfg);
    ets_delay_us(I2C_BIT_DELAY_US);

    return receive;
}   
#endif

int i2cdevInit(I2C_Dev *dev)
{
    i2cdrvInit(dev);
    return true;
}

bool i2cdevRead(I2C_Dev *dev, uint8_t devAddress, uint16_t len, uint8_t *data)
{
    if (!dev->init_ok)
    {
        return false;
    }

    return i2cdevReadReg8(dev, devAddress, I2C_NO_MEM_ADDR, len, data);
}

bool i2cdevReadByte(I2C_Dev *dev, uint8_t devAddress, uint8_t memAddress,
                    uint8_t *data)
{
    if (!dev->init_ok)
    {
        return false;
    }

    return i2cdevReadReg8(dev, devAddress, memAddress, 1, data);
}

bool i2cdevReadBit(I2C_Dev *dev, uint8_t devAddress, uint8_t memAddress,
                   uint8_t bitNum, uint8_t *data)
{
    if (!dev->init_ok)
    {
        return false;
    }

    uint8_t byte;
    bool status;

    status = i2cdevReadReg8(dev, devAddress, memAddress, 1, &byte);
    *data = byte & (1 << bitNum);

    return status;
}

bool i2cdevReadBits(I2C_Dev *dev, uint8_t devAddress, uint8_t memAddress,
                    uint8_t bitStart, uint8_t length, uint8_t *data)
{
    if (!dev->init_ok)
    {
        return false;
    }

    bool status;
    uint8_t byte;

    if ((status = i2cdevReadByte(dev, devAddress, memAddress, &byte)) == true) {
        uint8_t mask = ((1 << length) - 1) << (bitStart - length + 1);
        byte &= mask;
        byte >>= (bitStart - length + 1);
        *data = byte;
    }

    return status;
}

bool i2cdevReadReg8(I2C_Dev *dev, uint8_t devAddress, uint8_t memAddress,
                    uint16_t len, uint8_t *data)
{
    esp_err_t err = -1;

    if (!dev->init_ok)
    {
        return false;
    }

    if (xSemaphoreTake(dev->isBusFreeMutex, M2T(I2C_TIMEOUT)) == pdFALSE) {
        return false;
    }

    if (!dev->is_sw_i2c)
    {
        i2c_master_dev_handle_t handle =
        i2cdrvGetDevice(dev, devAddress);

        if (!handle)
        {
            goto end_read;
        }
        else
        {
            if (memAddress != I2C_NO_MEM_ADDR)
            {
                err = i2c_master_transmit_receive(
                        handle,
                        &memAddress,
                        1,
                        data,
                        len,
                        15);
            }
            else
            {
                err = i2c_master_receive(
                        handle,
                        data,
                        len,
                        15);
            }
        }
    }
    else
    {
        //portENTER_CRITICAL(&g_lock); 
        if (memAddress != I2C_NO_MEM_ADDR) 
        {    
            sw_i2c_master_start(dev->cfg); 
            err = sw_i2c_master_write_byte(dev->cfg, (devAddress << 1) | 0);  
            if (err)
            {  
                PRINT_I2CDEV("1 sw_i2c_master_write_byte failed:%d", devAddress);

                goto end_read;
            } 

            err = sw_i2c_master_write(dev->cfg, &memAddress, 1);  
            if (err)
            {  
                PRINT_I2CDEV("sw_i2c_master_write failed");

                goto end_read;
            } 
        }

        sw_i2c_master_start(dev->cfg); 
        err = sw_i2c_master_write_byte(dev->cfg, (devAddress << 1) | 1);  
        if (err)
        {  
            PRINT_I2CDEV("2 sw_i2c_master_write_byte failed");

            goto end_read;
        } 
        
        for (uint16_t i=0; i<len; i++)
        {    
            data[i]=sw_i2c_master_read(dev->cfg, i==(len-1)?0:1);  
        }
    } 

    end_read:
    xSemaphoreGive(dev->isBusFreeMutex);  

    if (dev->is_sw_i2c)
    {
        sw_i2c_master_stop(dev->cfg);
        //portEXIT_CRITICAL(&g_lock);
    }

    if (err == ESP_OK) {
        return TRUE;
    } else {
        return false;
    } 
}

bool i2cdevReadReg16(I2C_Dev *dev, uint8_t devAddress, uint16_t memAddress,
                     uint16_t len, uint8_t *data)
{
    esp_err_t err = -1;

    if (!dev->init_ok)
    {
        return false;
    }

    if (xSemaphoreTake(dev->isBusFreeMutex, M2T(I2C_TIMEOUT)) == pdFALSE) {
        return false;
    }

    uint8_t memAddress8[2];
    memAddress8[0] = (uint8_t)((memAddress >> 8) & 0x00FF);
    memAddress8[1] = (uint8_t)(memAddress & 0x00FF);

    if (!dev->is_sw_i2c)
    {
        i2c_master_dev_handle_t handle =
            i2cdrvGetDevice(dev, devAddress);

        if (!handle)
        {
            err = ESP_FAIL;
        }
        else
        {
            if (memAddress != I2C_NO_MEM_ADDR)
            {
                err = i2c_master_transmit_receive(
                        handle,
                        memAddress8,
                        2,
                        data,
                        len,
                        15);
            }
            else
            {
                err = i2c_master_receive(
                        handle,
                        data,
                        len,
                        15);
            }
        }
    }
    else
    {
        //portENTER_CRITICAL(&g_lock); 
        if (memAddress != I2C_NO_MEM_ADDR) 
        {   
            sw_i2c_master_start(dev->cfg); 
            err = sw_i2c_master_write_byte(dev->cfg, (devAddress << 1) | I2C_MASTER_WRITE);  
            if (err)
            {
                PRINT_I2CDEV("0 16 sw_i2c_master_write_byte failed");

                goto end_read;
            } 

            err = sw_i2c_master_write(dev->cfg, memAddress8, 2);  
            if (err)
            {
                PRINT_I2CDEV("16 sw_i2c_master_write memAddress8 failed");

                goto end_read;
            } 
        }

        sw_i2c_master_start(dev->cfg); 
        err = sw_i2c_master_write_byte(dev->cfg, (devAddress << 1) | I2C_MASTER_READ);  
        if (err)
        {
            PRINT_I2CDEV("1 16 sw_i2c_master_write_byte failed");

            goto end_read;
        } 
        
        for (uint16_t i=0; i<len; i++)
        {    
            data[i]=sw_i2c_master_read(dev->cfg, i==(len-1)?0:1);  
        }      
    } 
 
    end_read:
    xSemaphoreGive(dev->isBusFreeMutex);  

    if (dev->is_sw_i2c)
    {
        sw_i2c_master_stop(dev->cfg);
        //portEXIT_CRITICAL(&g_lock);
    }
    
    if (err == ESP_OK) {
        return TRUE;
    } else {
        return false;
    }

}

bool i2cdevWriteByte(I2C_Dev *dev, uint8_t devAddress, uint8_t memAddress,
                     uint8_t data)
{
    if (!dev->init_ok)
    {
        return false;
    }

    return i2cdevWriteReg8(dev, devAddress, memAddress, 1, &data);
}

bool i2cdevWrite(I2C_Dev *dev, uint8_t devAddress, uint16_t len, uint8_t *data)
{
    if (!dev->init_ok)
    {
        return false;
    }

    return i2cdevWriteReg8(dev, devAddress, I2C_NO_MEM_ADDR, len, data);
}

bool i2cdevWriteBit(I2C_Dev *dev, uint8_t devAddress, uint8_t memAddress,
                    uint8_t bitNum, uint8_t data)
{
    if (!dev->init_ok)
    {
        return false;
    }

    uint8_t byte;
    i2cdevReadByte(dev, devAddress, memAddress, &byte);
    byte = (data != 0) ? (byte | (1 << bitNum)) : (byte & ~(1 << bitNum));
    return i2cdevWriteByte(dev, devAddress, memAddress, byte);
}

bool i2cdevWriteBits(I2C_Dev *dev, uint8_t devAddress, uint8_t memAddress,
                     uint8_t bitStart, uint8_t length, uint8_t data)
{
    if (!dev->init_ok)
    {
        return false;
    }

    bool status;
    uint8_t byte;

    if ((status = i2cdevReadByte(dev, devAddress, memAddress, &byte)) == true) {
        uint8_t mask = ((1 << length) - 1) << (bitStart - length + 1);
        data <<= (bitStart - length + 1); // shift data into correct position
        data &= mask;                     // zero all non-important bits in data
        byte &= ~(mask);                  // zero all important bits in existing byte
        byte |= data;                     // combine data with existing byte
        status = i2cdevWriteByte(dev, devAddress, memAddress, byte);
    }

    return status;
}


bool i2cdevWriteReg8(I2C_Dev *dev, uint8_t devAddress, uint8_t memAddress,
                     uint16_t len, uint8_t *data)
{
    esp_err_t err = -1;

    if (!dev->init_ok)
    {
        return false;
    }

    if (xSemaphoreTake(dev->isBusFreeMutex, M2T(I2C_TIMEOUT)) == pdFALSE) {
        return false;
    }

    if (!dev->is_sw_i2c)
    {
        i2c_master_dev_handle_t handle = i2cdrvGetDevice(dev, devAddress);

        if (!handle)
        {
            goto end_read; 
        }
        else
        {
            uint8_t tx[len + 1];

            tx[0] = memAddress;

            memcpy(&tx[1], data, len);

            err = i2c_master_transmit(
                    handle,
                    tx,
                    len + 1,
                    15); 
        }
    }
    else
    {
        //portENTER_CRITICAL(&g_lock); 
        sw_i2c_master_start(dev->cfg);
        err = sw_i2c_master_write_byte(dev->cfg, (devAddress << 1) | I2C_MASTER_WRITE);
        if (err)
        {   
            goto end_read; 
        }

        if (memAddress != I2C_NO_MEM_ADDR) 
        {   
            err = sw_i2c_master_write_byte(dev->cfg, memAddress);
            if (err)
            {   
                goto end_read; 
            } 
        }
  
        err = sw_i2c_master_write(dev->cfg, data, len); 
        if (err)
        {   
            goto end_read; 
        } 
    }
 
    end_read:
    xSemaphoreGive(dev->isBusFreeMutex);  

    if (dev->is_sw_i2c)
    {
        sw_i2c_master_stop(dev->cfg);
        //portEXIT_CRITICAL(&g_lock);
    } 

    if (err == ESP_OK) {
        return TRUE;
    } else {
        return false;
    }
}

bool i2cdevWriteReg16(I2C_Dev *dev, uint8_t devAddress, uint16_t memAddress,
                      uint16_t len, uint8_t *data)
{
    esp_err_t err = -1;
    
    if (!dev->init_ok)
    {
        return false;
    }

    if (xSemaphoreTake(dev->isBusFreeMutex, M2T(I2C_TIMEOUT)) == pdFALSE)
    { 
        return false;
    }

    uint8_t memAddress8[2];
    memAddress8[0] = (uint8_t)((memAddress >> 8) & 0x00FF);
    memAddress8[1] = (uint8_t)(memAddress & 0x00FF);
    
    if (!dev->is_sw_i2c)
    { 
        i2c_master_dev_handle_t handle =
            i2cdrvGetDevice(dev, devAddress);

        if (!handle)
        {
            err = ESP_FAIL;
        }
        else
        {
            uint8_t tx[len + 2];

            memcpy(tx, memAddress8, 2);

            memcpy(&tx[2], data, len);

            err = i2c_master_transmit(
                    handle,
                    tx,
                    len + 2,
                    15);
        }
    }
    else
    {
        //portENTER_CRITICAL(&g_lock); 
        sw_i2c_master_start(dev->cfg);
        err = sw_i2c_master_write_byte(dev->cfg, (devAddress << 1) | I2C_MASTER_WRITE); 
        if (err)
        {  
            goto end_read; 
        }

        if (memAddress != I2C_NO_MEM_ADDR) 
        {  
            err = sw_i2c_master_write(dev->cfg, memAddress8, 2);
            if (err)
            {  
                goto end_read; 
            }
        } 

        err = sw_i2c_master_write(dev->cfg, data, len); 
        if (err)
        {   
            goto end_read; 
        } 
    }
  
    end_read:
    xSemaphoreGive(dev->isBusFreeMutex);  

    if (dev->is_sw_i2c)
    {
        sw_i2c_master_stop(dev->cfg);
        //portEXIT_CRITICAL(&g_lock);
    }

    if (err == ESP_OK) {
        return TRUE;
    } else {
        return false;
    }
}
