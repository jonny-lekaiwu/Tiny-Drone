/**
 * Tiny-Drone-Controller Firmware
 *
 * Copyright (C) 2025-2026 Jonny Chan (LEKAIWU)
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of Tiny-Drone-Controller.
 *
 * Tiny-Drone-Controller is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as published
 * by the Free Software Foundation.
 *
 * Tiny-Drone-Controller is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Tiny-Drone-Controller. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef __WEB_OTA_H__
#define __WEB_OTA_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <esp_http_server.h>
#include "esp_partition.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_netif.h" 
 

#define CONFIG_FIRMWARE_VERSION "00.01"
  
#define CONFIG_SERVER_PORT      80
#define WEB_BUFSIZE             4096 

extern httpd_handle_t   start_web_ota_server(void);
extern void stop_web_ota_server(httpd_handle_t hd);   

#define WEB_MIN(a, b)            \
    ({                          \
        __typeof__(a) _a = (a); \
        __typeof__(b) _b = (b); \
        _a < _b ? _a : _b;      \
    })
#define WEB_MAX(a, b)            \
    ({                          \
        __typeof__(a) _a = (a); \
        __typeof__(b) _b = (b); \
        _a > _b ? _a : _b;      \
    })


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif // __WEB_SERVER_H__
