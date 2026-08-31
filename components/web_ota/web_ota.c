
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

#include <stdlib.h>
#include <stdbool.h>
#include <time.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_http_server.h>

#include <sys/time.h>
 
#include <rom/ets_sys.h>   
#include "web_ota.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h" 
#include "esp_netif.h"
#include "lwip/netdb.h"
#include "lwip/dns.h" 

 
static char buffer[WEB_BUFSIZE] = {0}; 
 
static esp_err_t get_handler(httpd_req_t *req)
{      
    httpd_resp_set_type(req, "text/html");

    esp_err_t res = -1;
  
    extern const uint8_t index_html_start[] asm("_binary_index_html_start");
    extern const uint8_t index_html_end[] asm("_binary_index_html_end"); 

    const size_t html_size = (index_html_end - index_html_start);

    httpd_resp_send_chunk(req, (const char *)index_html_start, html_size);
    int32_t json_len = 0; 
 
    char script_buf[512];
 
    uint8_t set_lang_id = 1; 
   
    json_len += snprintf(script_buf + json_len,
                         sizeof(script_buf),
                         "<script>sys_info={version:\"%s\"};"
                         "window.addEventListener('load',function(){"
                         "var b=document.createElement('button');"
                         "b.textContent='返回遥控';"
                         "b.style.cssText='position:fixed;top:12px;left:12px;z-index:99999;padding:9px 14px;border:0;border-radius:6px;background:#555;color:#fff';"
                         "b.onclick=function(){location.href='/';};document.body.appendChild(b);});"
                         "</script>",
                         CONFIG_FIRMWARE_VERSION);
    
    httpd_resp_send_chunk(req, script_buf, strlen(script_buf));
 
    res = httpd_resp_send_chunk(req, NULL, 0);  

    return res;
}  
  
static void web_response(httpd_req_t *req, const char *status, const char *remind)
{ 
    if (!req)
    {
        return;
    }

    httpd_resp_set_type(req, HTTPD_TYPE_JSON);
    httpd_resp_set_status(req, status);

    if (remind && remind[0])
    {
        httpd_resp_send(req, remind, strlen(remind));
    } 
}
 
const esp_partition_t *_get_ota_update_partition(void)
{
    const esp_partition_t *update_partition = NULL;
    const esp_partition_t *configured = esp_ota_get_boot_partition();
    const esp_partition_t *running = esp_ota_get_running_partition();
    
    update_partition = esp_ota_get_next_update_partition(NULL);
    

    return update_partition;
}

esp_err_t _ota_end(esp_ota_handle_t handle, const esp_partition_t *partition)
{
    esp_err_t err = esp_ota_end(handle);

    if (err != ESP_OK) 
    {
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(partition);
    if (err != ESP_OK) 
    {
        return ESP_FAIL;
    }

    return err;
}

static void delayed_restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}
 
static esp_err_t ota_data_post_handler(httpd_req_t *req)
{
    char *buf = buffer;
    int total_len = req->content_len;
    int remaining_len = req->content_len;
    int received_len = 0;
    esp_err_t err = ESP_FAIL;
    esp_ota_handle_t update_handle = 0;
 
    const esp_partition_t *update_partition = _get_ota_update_partition();
   
    if (update_partition->size < total_len) 
    {
        goto err_handler;
    }
 
    memset(buf, 0x0, WEB_BUFSIZE);
      
    err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
    if (err != ESP_OK) 
    { 
        goto err_handler;
    }
    
    while (remaining_len > 0) 
    {
        received_len = httpd_req_recv(req, buf, WEB_MIN(remaining_len, WEB_BUFSIZE));
        if (received_len <= 0) 
        {
            if (received_len == HTTPD_SOCK_ERR_TIMEOUT) 
            {
                continue;
            } 
            esp_ota_end(update_handle);
            goto err_handler;
        }
        else 
        { 
            err = esp_ota_write(update_handle, buf, received_len);
            if (err != ESP_OK) 
            { 
                esp_ota_end(update_handle);
                goto err_handler;
            }
            remaining_len -= received_len;
        }
    }

    err = _ota_end(update_handle, update_partition);
    if (err != ESP_OK) {
        goto err_handler;
    }
 
    char response_res[32]={0};

    snprintf(response_res, sizeof(response_res), "{\"result\":%d}", 0);

    web_response(req, HTTPD_200, response_res); 
     
    if (xTaskCreate(delayed_restart_task, "restart", 2048, NULL, 5, NULL) != pdPASS)
    {
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    } 

    return ESP_OK;

err_handler:  
    web_response(req, HTTPD_500, NULL); 
    
    return ESP_FAIL;
}
         
static const httpd_uri_t basic_handlers[] = 
{
    { .uri      = "/update",
      .method   = HTTP_GET,
      .handler  = get_handler,
      .user_ctx = NULL,
    },    
    { .uri      = "/up_fw",
      .method   = HTTP_POST,
      .handler  = ota_data_post_handler,
      .user_ctx = NULL,
    },  
};

static const int basic_handlers_no = sizeof(basic_handlers)/sizeof(httpd_uri_t);
 
static void register_basic_handlers(httpd_handle_t hd)
{
    int i;  

    for (i = 0; i < basic_handlers_no; i++) 
    {
        if (httpd_register_uri_handler(hd, &basic_handlers[i]) != ESP_OK) 
        { 
            return;
        }
    } 
}

static httpd_handle_t _httpd_start(void)
{ 
    httpd_handle_t hd;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG(); 
    
    /* Reserve one additional URI slot for optional users such as the
     * separate WebSocket video endpoint. Keep web_ota independent of the
     * flight-controller configuration component. */
    config.max_uri_handlers = basic_handlers_no + 8;
    config.server_port = CONFIG_SERVER_PORT;
 
    /*
     * The Wi-Fi component also runs a dedicated MJPEG HTTP server and keeps
     * two UDP sockets open (control and video).  Reserving nearly every lwIP
     * socket for this server causes accept()/socket() to fail with ENFILE
     * when WebSocket and video are enabled together.
     *
     * Allow the old WebSocket, replacement WebSocket and short API requests
     * to overlap during a browser refresh, while staying inside the global
     * lwIP socket budget.
     */
    config.max_open_sockets = 4;
     
    config.lru_purge_enable = true; 

    if (httpd_start(&hd, &config) == ESP_OK) 
    { 
        return hd;
    }

    return NULL;
} 

httpd_handle_t start_web_ota_server(void)
{
    httpd_handle_t hd = _httpd_start();

    if (hd) 
    {
        register_basic_handlers(hd); 
    }  

    return hd;
}

void stop_web_ota_server(httpd_handle_t hd)
{
    httpd_stop(hd);
}
