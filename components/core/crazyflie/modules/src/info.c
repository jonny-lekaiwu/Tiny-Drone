/**
 *    ||          ____  _ __
 * +------+      / __ )(_) /_______________ _____  ___
 * | 0xBC |     / __  / / __/ ___/ ___/ __ `/_  / / _ \
 * +------+    / /_/ / / /_/ /__/ /  / /_/ / / /_/  __/
 *  ||  ||    /_____/_/\__/\___/_/   \__,_/ /___/\___/
 *
 * Crazyflie control firmware
 *
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
 * info.c - Receive information requests and send them back to the client
 */

#include <string.h>
#include <math.h>

/*FreeRtos includes*/
#include "FreeRTOS.h"
#include "task.h"

#include "crtp.h"
#include "info.h"
#include "pm_esplane.h"
#include "stm32_legacy.h"
#include "static_mem.h"

#define DEBUG_MODULE "INFO"
#include "debug_cf.h"

typedef enum {
  infoBatteryNr = 0x01,
  infoWarningNr = 0x03
} InfoNbr;

typedef enum {
  batteryVoltage = 0x00,
  batteryMin = 0x01,
  batteryMax = 0x02
} batteryId;

STATIC_MEM_TASK_ALLOC(infoTask, INFO_TASK_STACKSIZE);
static xQueueHandle infoPacketQueue;
STATIC_MEM_QUEUE_ALLOC(infoPacketQueue, 8, sizeof(CRTPPacket));

static void infoCrtpCallback(CRTPPacket *packet)
{
  /* Port 8 is shared with the modern high-level commander. The legacy
   * ESP-Drone information protocol uses non-zero channels for battery
   * information and warnings, so only copy those packets here. */
  if (packet->channel == infoBatteryNr || packet->channel == infoWarningNr)
  {
    xQueueSend(infoPacketQueue, packet, 0);
  }
}

void infoTask(void *param);

void infoInit()
{
  infoPacketQueue = STATIC_MEM_QUEUE_CREATE(infoPacketQueue);
  ASSERT(infoPacketQueue);
  STATIC_MEM_TASK_CREATE(infoTask, infoTask, INFO_TASK_NAME, NULL, INFO_TASK_PRI);
  crtpRegisterPortCB(CRTP_PORT_SETPOINT_HL, infoCrtpCallback);
}

void infoTask(void *param)
{
  CRTPPacket p;
  static int ctr=0;
  uint32_t nextBatteryReport = xTaskGetTickCount() + M2T(2000);

  while (TRUE)
  {
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    #if 0
    if (xQueueReceive(infoPacketQueue, &p, M2T(1000)) == pdTRUE)
    {
      InfoNbr infoNbr = (InfoNbr)p.channel;

      switch (infoNbr)
      {
        #if 0
        case infoCopterNr:
          if (p.data[0] == infoName)
          {
            p.data[1] = 0x90;
            p.data[2] = 0x00;   //Version 0.9.0 (Crazyflie)
            strcpy((char*)&p.data[3], "CrazyFlie");

            p.size = 3+strlen("CrazyFlie");
            crtpSendPacket(&p);
          } else if (p.data[0] == infoVersion) {
            i=1;

            strncpy((char*)&p.data[i], V_SLOCAL_REVISION, 31-i);
            i += strlen(V_SLOCAL_REVISION);

            if (i<31) p.data[i++] = ',';

            strncpy((char*)&p.data[i], V_SREVISION, 31-i);
            i += strlen(V_SREVISION);

            if (i<31) p.data[i++] = ',';

            strncpy((char*)&p.data[i], V_STAG, 31-i);
            i += strlen(V_STAG);

            if (i<31) p.data[i++] = ',';
            if (i<31) p.data[i++] = V_MODIFIED?'M':'C';

            p.size = (i<31)?i:31;
            crtpSendPacket(&p);
          } else if (p.data[0] == infoCpuId) {
            memcpy((char*)&p.data[1], (char*)CpuId, 12);

            p.size = 13;
            crtpSendPacket(&p);
          }

          break;
        #endif
        case infoBatteryNr:
          if (p.data[0] == batteryVoltage)
          {
            float value = pmGetBatteryVoltage();

            DEBUG_PRINTW("Battery request: voltage=%u mV\n", (unsigned int)(value * 1000.0f));

            memcpy(&p.data[1], (char*)&value, 4);

            p.size = 5;
            crtpSendPacket(&p);
          } else if (p.data[0] == batteryMax) {
            float value = pmGetBatteryVoltageMax();

            DEBUG_PRINTW("Battery request: max=%u mV\n", (unsigned int)(value * 1000.0f));

            memcpy(&p.data[1], (char*)&value, 4);

            p.size = 5;
            crtpSendPacket(&p);
          } else if (p.data[0] == batteryMin) {
            float value = pmGetBatteryVoltageMin();

            DEBUG_PRINTW("Battery request: min=%u mV\n", (unsigned int)(value * 1000.0f));

            memcpy(&p.data[1], (char*)&value, 4);

            p.size = 5;
            crtpSendPacket(&p);
          }
          break;
        default:
          break;
      }
    }
    #endif 
    if ((int32_t)(xTaskGetTickCount() - nextBatteryReport) >= 0)
    {
      float value = pmGetBatteryVoltage();

      p.port = CRTP_PORT_SETPOINT_HL;
      p.channel = infoBatteryNr;
      p.reserved = 0;
      p.data[0] = batteryVoltage;
      memcpy(&p.data[1], &value, sizeof(value));
      p.size = 5;

      //DEBUG_PRINTW("Battery report: voltage=%u mV\n",(unsigned int)(value * 1000.0f));
      crtpSendPacket(&p);

      nextBatteryReport = xTaskGetTickCount() + M2T(2000);
    }

    #if 0
    // Send a warning message if the battery voltage drops under 3.3V
    // This is sent every 5 info transaction or every 5 seconds
    if (ctr++>5) {
      ctr=0;

      if (pmGetBatteryVoltageMin() < INFO_BAT_WARNING)
      {
        float value = pmGetBatteryVoltage();

        p.port = CRTP_PORT_SETPOINT_HL;
        p.channel = infoWarningNr;
        p.reserved = 0;
        p.data[0] = 0;
        memcpy(&p.data[1], (char*)&value, 4);

        p.size = 5;
        crtpSendPacket(&p);
      }
    }

    #endif
  }
}
