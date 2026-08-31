#include <string.h>
#include <math.h>
#include <stdlib.h>

#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#if CONFIG_USING_CAMERA
#include "esp_camera.h"
#endif

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"

#include "queuemonitor.h"
#include "wifi_esp32.h"
#include "crtp_commander.h"
#include "pm_esplane.h"
#include "web_ota.h"
#include "stm32_legacy.h"
#define DEBUG_MODULE  "WIFI_UDP"
#include "debug_cf.h"

/* Set to 1 to enable the browser remote, WebSocket control and HTTP video.
 * Keep this switch local and simple so the original UDP path can be tested
 * without changing sdkconfig or CMake. */
#define WIFI_WEB_REMOTE_ENABLE 1
/* Experimental transport: carry joystick state and an optional JPEG chunk
 * in every WebSocket packet. Set to 0 to restore control WS + HTTP MJPEG. */
#define WIFI_WEB_COMBINED_WS_ENABLE 0
#define WIFI_WEB_SEPARATE_VIDEO_WS_ENABLE 1
#define WIFI_WEB_DIRECT_CAMERA_FB_ENABLE 0


#ifdef CONFIG_TARGET_TINY_DRONE_V1_0  
extern bool pwm_timmer_set_clock(uint16_t hz); 
#endif

#define SW_PRINT_WIFI 1

#if SW_PRINT_WIFI 
    #include "esp_log.h"
    static const char * TAG = "wifi_app";
    #define PRINT_WIFI(fmt, args...)  do{printf("%s: ", TAG); printf(fmt, ##args); printf("\r\n");} while(0)
#else
    #define PRINT_WIFI(...)
#endif


#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 4, 0)
#include "esp_mac.h"
#endif 

#define UDP_SERVER_PORT         2390
#define UDP_SERVER_BUFSIZE      64

static struct sockaddr_storage source_addr;

static char WIFI_SSID[32] = "";
static char WIFI_PWD[64] = CONFIG_WIFI_PASSWORD;
static uint8_t WIFI_CH = CONFIG_WIFI_CHANNEL;
#define WIFI_MAX_STA_CONN CONFIG_WIFI_MAX_STA_CONN

#ifndef MAC2STR
#define MAC2STR(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]
#define MACSTR "%02x:%02x:%02x:%02x:%02x:%02x"
#endif

static int sock_ctl;
static xQueueHandle udpDataRx;
static xQueueHandle udpDataTx;

static bool isInit = false;
static bool isUDPInit = false;
static volatile bool isUDPConnected = false;
static volatile TickType_t lastUdpControlTick = 0;

/* The physical controller sends control packets continuously. If they stop,
 * retire its video destination instead of sending JPEG fragments forever to
 * a stale IP and exhausting lwIP pbufs (sendto errno=ENOMEM). */
#define UDP_CONTROLLER_TIMEOUT_MS 1000U

static esp_err_t udp_server_create(void *arg);
static void web_remote_register_handlers(httpd_handle_t server);
#if CONFIG_USING_CAMERA
static httpd_handle_t web_stream_server_start(void);
#if WIFI_WEB_SEPARATE_VIDEO_WS_ENABLE
static esp_err_t web_video_ws_handler(httpd_req_t *req);
#endif
#endif
bool e_wifi_ap_any_connected(void);

#if CONFIG_USING_CAMERA
static esp_err_t camera_init_res = ESP_FAIL;

bool camera_init_ok(void)
{
    return (ESP_OK == camera_init_res);
}
#endif

static uint8_t calculate_cksum(void *data, size_t len)
{
    unsigned char *c = data;
    int i;
    unsigned char cksum = 0;

    for (i = 0; i < len; i++) {
        cksum += *(c++);
    }

    return cksum;
}


static httpd_handle_t ota_server = NULL;
#if CONFIG_USING_CAMERA
static httpd_handle_t stream_server = NULL;

static SemaphoreHandle_t webFrameMutex;
#if WIFI_WEB_COMBINED_WS_ENABLE && WIFI_WEB_DIRECT_CAMERA_FB_ENABLE
static camera_fb_t *webFrameFb;
static TickType_t webFrameLastUseTick;
static bool webFrameConsumerActive;
#endif
static uint8_t *webFrameData;
static size_t webFrameCapacity;
static size_t webFrameLength;
static uint32_t webFrameId;
static volatile uint32_t webStreamClients;
#endif
static SemaphoreHandle_t webControlMutex;

typedef struct __attribute__((packed))
{
    float roll;
    float pitch;
    float yaw;
    uint16_t thrust;
    uint8_t carefree;
} web_control_packet_t;

#if (WIFI_WEB_COMBINED_WS_ENABLE || WIFI_WEB_SEPARATE_VIDEO_WS_ENABLE) && CONFIG_USING_CAMERA
#define WEB_WS_PACKET_MAGIC      0x44575654UL /* "TWVD" little-endian */
/* Keep one combined WebSocket message close to the existing UDP video
 * packet size. The 34-byte unified header plus 1366 JPEG bytes is 1400
 * bytes, greatly reducing the time a video reply can delay control RX. */
#define WEB_WS_VIDEO_CHUNK_SIZE  1366U
#define WEB_DIRECT_FB_TIMEOUT_MS 1500U

typedef struct __attribute__((packed))
{
    uint32_t magic;
    web_control_packet_t control;
    uint8_t accepted;
    uint32_t frame_id;
    uint32_t total_size;
    uint32_t offset;
    uint16_t data_len;
} web_ws_packet_header_t;

static size_t webWsFrameOffset;
static uint32_t webWsFrameId = UINT32_MAX;
#if WIFI_WEB_SEPARATE_VIDEO_WS_ENABLE
static SemaphoreHandle_t webVideoClientMutex;
static volatile int webVideoClientFd = -1;
static volatile uint32_t webVideoClientGeneration;
static bool webVideoFrameSending;

typedef struct
{
    TaskHandle_t ownerTask;
    volatile esp_err_t result;
    volatile int sendErrno;
    uint8_t packetData[sizeof(web_ws_packet_header_t) + WEB_WS_VIDEO_CHUNK_SIZE];
} web_video_async_send_t;

static web_video_async_send_t webVideoAsyncSend;
#endif
#if WIFI_WEB_DIRECT_CAMERA_FB_ENABLE
static void web_frame_release_locked(void)
{
    if (webFrameFb)
    {
        esp_camera_fb_return(webFrameFb);
        webFrameFb = NULL;
    }
    webFrameLength = 0U;
    webWsFrameOffset = 0U;
    webWsFrameId = UINT32_MAX;
}
#endif
#endif

extern const uint8_t web_remote_html_start[]
    asm("_binary_web_remote_html_start");
extern const uint8_t web_remote_html_end[]
    asm("_binary_web_remote_html_end");

static esp_err_t web_remote_page_handler(httpd_req_t *req)
{
#if CONFIG_USING_CAMERA && !WIFI_WEB_COMBINED_WS_ENABLE
    if (camera_init_ok() && NULL == stream_server)
    {
        stream_server = web_stream_server_start();
    }
#endif
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req,
                           (const char *)web_remote_html_start,
                           web_remote_html_end - web_remote_html_start);
}

static esp_err_t web_favicon_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
    return httpd_resp_send(req, NULL, 0);
}

#if CONFIG_USING_CAMERA
static esp_err_t web_camera_status_handler(httpd_req_t *req)
{
#if !WIFI_WEB_COMBINED_WS_ENABLE
    if (camera_init_ok() && NULL == stream_server)
    {
        stream_server = web_stream_server_start();
    }
#endif
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req,
                           camera_init_ok() ? "1" : "0",
                           HTTPD_RESP_USE_STRLEN);
}
#endif

static esp_err_t web_features_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
#if CONFIG_USING_CAMERA
#if WIFI_WEB_COMBINED_WS_ENABLE || WIFI_WEB_SEPARATE_VIDEO_WS_ENABLE
    return httpd_resp_send(req, "{\"camera\":true,\"wsVideo\":true}", HTTPD_RESP_USE_STRLEN);
#else
    return httpd_resp_send(req, "{\"camera\":true,\"wsVideo\":false}", HTTPD_RESP_USE_STRLEN);
#endif
#else
    return httpd_resp_send(req, "{\"camera\":false,\"wsVideo\":false}", HTTPD_RESP_USE_STRLEN);
#endif
}

static esp_err_t web_battery_handler(httpd_req_t *req)
{
    char voltage[16];
    snprintf(voltage, sizeof(voltage), "%.3f", (double)pmGetBatteryVoltage());
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, voltage, HTTPD_RESP_USE_STRLEN);
}

static bool web_queue_control(const web_control_packet_t *command)
{
    CRTPPacket crtpPacket = {0};
    crtpPacket.port = CRTP_PORT_SETPOINT;
    crtpPacket.channel = 0;
    crtpPacket.reserved = command->carefree == 1U ? 1U : 0U;
    crtpPacket.size = sizeof(command->roll) + sizeof(command->pitch) +
                      sizeof(command->yaw) + sizeof(command->thrust);
    memcpy(crtpPacket.data, command, crtpPacket.size);

    UDPPacket inPacket = {0};
    inPacket.size = crtpPacket.size + 1U;
    memcpy(inPacket.data, crtpPacket.raw, inPacket.size);

    if (xQueueSend(udpDataRx, &inPacket, M2T(10)) != pdTRUE)
    {
        return false;
    }

    if (!isUDPConnected)
    {
        isUDPConnected = true;
    }
    return true;
}

static bool web_apply_control(const web_control_packet_t *command)
{
    if (!isfinite(command->roll) || !isfinite(command->pitch) ||
        !isfinite(command->yaw) || fabsf(command->roll) > 30.0f ||
        fabsf(command->pitch) > 30.0f || fabsf(command->yaw) > 360.0f)
    {
        return false;
    }

    if (!webControlMutex ||
        xSemaphoreTake(webControlMutex, M2T(10)) != pdTRUE)
    {
        return false;
    }

    if (!web_queue_control(command))
    {
        xSemaphoreGive(webControlMutex);
        return false;
    }

    xSemaphoreGive(webControlMutex);
    return true;
}

#if WIFI_WEB_REMOTE_ENABLE

static esp_err_t web_control_ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET)
    {
#if WIFI_WEB_COMBINED_WS_ENABLE && CONFIG_USING_CAMERA
        /* A refreshed page is a new video consumer. Do not resume the old
         * socket's partially transmitted JPEG on this connection. */
        if (webFrameMutex && xSemaphoreTake(webFrameMutex, M2T(100)) == pdTRUE)
        {
#if WIFI_WEB_DIRECT_CAMERA_FB_ENABLE
            web_frame_release_locked();
            webFrameConsumerActive = true;
#else
            webWsFrameOffset = 0U;
            webWsFrameId = UINT32_MAX;
#endif
            xSemaphoreGive(webFrameMutex);
        }

        int socketFd = httpd_req_to_sockfd(req);
        int enabled = 1;
        int sendBufferSize = 2 * 1024;
        struct timeval sendTimeout = {
            .tv_sec = 0,
            .tv_usec = 250000,
        };
        setsockopt(socketFd, IPPROTO_TCP, TCP_NODELAY,
                   &enabled, sizeof(enabled));
        setsockopt(socketFd, SOL_SOCKET, SO_SNDBUF,
                   &sendBufferSize, sizeof(sendBufferSize));
        setsockopt(socketFd, SOL_SOCKET, SO_SNDTIMEO,
                   &sendTimeout, sizeof(sendTimeout));
#endif
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {0};
    esp_err_t result = httpd_ws_recv_frame(req, &frame, 0);
    if (result != ESP_OK)
    {
        return result;
    }

    if (frame.type == HTTPD_WS_TYPE_CLOSE)
    {
        /* Do not cut the motors on a transient WebSocket disconnect. The
         * WebSocket-only watchdog below first levels the aircraft and gives
         * the browser a short reconnect window. */
        return ESP_OK;
    }

#if WIFI_WEB_COMBINED_WS_ENABLE && CONFIG_USING_CAMERA
    uint8_t accepted = 1U;
#endif
    web_control_packet_t command = {0};
    bool validControlPacket = false;

    if (frame.type == HTTPD_WS_TYPE_BINARY &&
        frame.len == sizeof(web_control_packet_t))
    {
        frame.payload = (uint8_t *)&command;
        result = httpd_ws_recv_frame(req, &frame, sizeof(command));
        if (result != ESP_OK)
        {
            return result;
        }
        validControlPacket = true;
    }
#if WIFI_WEB_COMBINED_WS_ENABLE && CONFIG_USING_CAMERA
    else if (frame.type == HTTPD_WS_TYPE_BINARY &&
             frame.len == sizeof(web_ws_packet_header_t))
    {
        web_ws_packet_header_t requestPacket;
        frame.payload = (uint8_t *)&requestPacket;
        result = httpd_ws_recv_frame(req, &frame, sizeof(requestPacket));
        if (result != ESP_OK)
        {
            return result;
        }
        if (requestPacket.magic == WEB_WS_PACKET_MAGIC &&
            requestPacket.data_len == 0U)
        {
            command = requestPacket.control;
            validControlPacket = true;
        }
    }
#endif

    if (validControlPacket)
    {
        #if 0
        static TickType_t lastControlLogTick;
        TickType_t now = xTaskGetTickCount();
        if ((now - lastControlLogTick) >= pdMS_TO_TICKS(1000))
        {
            lastControlLogTick = now;
            DEBUG_PRINTW("Web control: yaw=%.2f pitch=%.2f roll=%.2f thrust=%u carefree=%u\n",
                         (double)command.yaw,
                         (double)command.pitch,
                         (double)command.roll,
                         (unsigned int)command.thrust,
                         (unsigned int)command.carefree);
        }
        #endif
#if WIFI_WEB_COMBINED_WS_ENABLE && CONFIG_USING_CAMERA
        accepted = web_apply_control(&command) ? 0U : 1U;
#else
        (void)web_apply_control(&command);
#endif
    }

#if WIFI_WEB_COMBINED_WS_ENABLE && CONFIG_USING_CAMERA
    if (validControlPacket)
    {
        uint8_t packetData[sizeof(web_ws_packet_header_t) +
                           WEB_WS_VIDEO_CHUNK_SIZE];
        size_t packetLength = sizeof(web_ws_packet_header_t);
        size_t chunkLength = 0U;
        size_t chunkOffset = 0U;
        uint32_t chunkFrameId = UINT32_MAX;
        web_ws_packet_header_t *packet =
            (web_ws_packet_header_t *)packetData;
        *packet = (web_ws_packet_header_t){
            .magic = WEB_WS_PACKET_MAGIC,
            .control = command,
            .accepted = accepted,
            .frame_id = UINT32_MAX,
        };

        if (webFrameMutex &&
            xSemaphoreTake(webFrameMutex, M2T(10)) == pdTRUE)
        {
#if WIFI_WEB_DIRECT_CAMERA_FB_ENABLE
            webFrameConsumerActive = true;
            webFrameLastUseTick = xTaskGetTickCount();
#endif
            if ((webWsFrameId == UINT32_MAX ||
                 webWsFrameOffset >= webFrameLength) &&
                webFrameLength > 0U && webFrameId != webWsFrameId)
            {
                webWsFrameOffset = 0U;
                webWsFrameId = webFrameId;
            }

            size_t remaining = webWsFrameOffset < webFrameLength
                                   ? webFrameLength - webWsFrameOffset
                                   : 0U;
            chunkLength = remaining > WEB_WS_VIDEO_CHUNK_SIZE
                              ? WEB_WS_VIDEO_CHUNK_SIZE
                              : remaining;
            packetLength += chunkLength;
            chunkOffset = webWsFrameOffset;
            chunkFrameId = webWsFrameId;

            *packet = (web_ws_packet_header_t){
                .magic = WEB_WS_PACKET_MAGIC,
                .control = command,
                .accepted = accepted,
                .frame_id = webWsFrameId,
                .total_size = webFrameLength,
                .offset = webWsFrameOffset,
                .data_len = (uint16_t)chunkLength,
            };
            if (chunkLength > 0U)
            {
                memcpy(packetData + sizeof(*packet),
#if WIFI_WEB_DIRECT_CAMERA_FB_ENABLE
                       webFrameFb->buf + webWsFrameOffset,
#else
                       webFrameData + webWsFrameOffset,
#endif
                       chunkLength);
#if WIFI_WEB_DIRECT_CAMERA_FB_ENABLE
                webFrameLastUseTick = xTaskGetTickCount();
#endif
            }
            xSemaphoreGive(webFrameMutex);
        }

        httpd_ws_frame_t reply = {
            .final = true,
            .fragmented = false,
            .type = HTTPD_WS_TYPE_BINARY,
            .payload = packetData,
            .len = packetLength,
        };
        result = httpd_ws_send_frame(req, &reply);
        if (webFrameMutex && xSemaphoreTake(webFrameMutex, M2T(10)) == pdTRUE)
        {
            if (result == ESP_OK && webWsFrameId == chunkFrameId &&
                webWsFrameOffset == chunkOffset)
            {
                webWsFrameOffset += chunkLength;
#if WIFI_WEB_DIRECT_CAMERA_FB_ENABLE
                webFrameLastUseTick = xTaskGetTickCount();
                if (webWsFrameOffset >= webFrameLength)
                {
                    web_frame_release_locked();
                }
#endif
            }
#if WIFI_WEB_DIRECT_CAMERA_FB_ENABLE
            else if (result != ESP_OK)
            {
                web_frame_release_locked();
                webFrameConsumerActive = false;
            }
#endif
            xSemaphoreGive(webFrameMutex);
        }
        return result;
    }
#endif

    return ESP_OK;
}


static esp_err_t web_control_handler(httpd_req_t *req)
{
    if (req->content_len != sizeof(web_control_packet_t))
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid control packet");
        return ESP_FAIL;
    }

    web_control_packet_t command;
    size_t received = 0;
    while (received < sizeof(command))
    {
        int len = httpd_req_recv(req,
                                 ((char *)&command) + received,
                                 sizeof(command) - received);
        if (len <= 0)
        {
            return ESP_FAIL;
        }
        received += len;
    }

    if (!web_apply_control(&command))
    {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_send(req, "control rejected", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_send(req, "ok", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}


#if CONFIG_USING_CAMERA
static esp_err_t web_stream_handler(httpd_req_t *req)
{
    static const char boundary[] = "\r\n--frame\r\n";
    char header[96];
    uint32_t lastFrameId = UINT32_MAX;
    uint8_t *sendFrame = NULL;
    size_t sendFrameCapacity = 0;

    /* Keep TCP from accumulating several old JPEG frames. If the browser
     * cannot consume one frame promptly, close the stream and let it
     * reconnect instead of displaying seconds of stale video. */
    int socketFd = httpd_req_to_sockfd(req);
    int enabled = 1;
    int sendBufferSize = 8 * 1024;
    struct timeval sendTimeout = {
        .tv_sec = 1,
        .tv_usec = 500000,
    };
    setsockopt(socketFd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
    setsockopt(socketFd, SOL_SOCKET, SO_SNDBUF,
               &sendBufferSize, sizeof(sendBufferSize));
    setsockopt(socketFd, SOL_SOCKET, SO_SNDTIMEO,
               &sendTimeout, sizeof(sendTimeout));

    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=frame");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "X-Accel-Buffering", "no");
    webStreamClients++;

    esp_err_t result = ESP_OK;
    while (result == ESP_OK)
    {
        if (!webFrameMutex ||
            xSemaphoreTake(webFrameMutex, pdMS_TO_TICKS(100)) != pdTRUE)
        {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (webFrameLength == 0 || webFrameId == lastFrameId)
        {
            xSemaphoreGive(webFrameMutex);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (webFrameLength > sendFrameCapacity)
        {
            uint8_t *newBuffer = realloc(sendFrame, webFrameLength);
            if (!newBuffer)
            {
                xSemaphoreGive(webFrameMutex);
                result = ESP_ERR_NO_MEM;
                break;
            }
            sendFrame = newBuffer;
            sendFrameCapacity = webFrameLength;
        }

        size_t sendFrameLength = webFrameLength;
        uint32_t sendFrameId = webFrameId;
        memcpy(sendFrame, webFrameData, sendFrameLength);
        xSemaphoreGive(webFrameMutex);

        int headerLength = snprintf(header,
                                    sizeof(header),
                                    "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                                    (unsigned int)sendFrameLength);
        result = httpd_resp_send_chunk(req, boundary, sizeof(boundary) - 1);
        if (result == ESP_OK)
            result = httpd_resp_send_chunk(req, header, headerLength);
        if (result == ESP_OK)
            result = httpd_resp_send_chunk(req,
                                           (const char *)sendFrame,
                                           sendFrameLength);

        lastFrameId = sendFrameId;
    }

    webStreamClients--;
    free(sendFrame);
    return result;
}
#endif
 
static void web_remote_register_handlers(httpd_handle_t server)
{
#define REGISTER_WEB_URI(handlerDef) do {                               \
        esp_err_t registerResult =                                     \
            httpd_register_uri_handler(server, &(handlerDef));          \
        if (registerResult != ESP_OK)                                   \
            ESP_LOGE(TAG, "Failed to register URI %s: %s",             \
                     (handlerDef).uri, esp_err_to_name(registerResult));\
    } while (0)
    static const httpd_uri_t root = {
        .uri = "/", .method = HTTP_GET,
        .handler = web_remote_page_handler, .user_ctx = NULL};
    static const httpd_uri_t page = {
        .uri = "/control", .method = HTTP_GET,
        .handler = web_remote_page_handler, .user_ctx = NULL};
    static const httpd_uri_t favicon = {
        .uri = "/favicon.ico", .method = HTTP_GET,
        .handler = web_favicon_handler, .user_ctx = NULL};
    static const httpd_uri_t features = {
        .uri = "/api/features", .method = HTTP_GET,
        .handler = web_features_handler, .user_ctx = NULL};
    static const httpd_uri_t battery = {
        .uri = "/api/battery", .method = HTTP_GET,
        .handler = web_battery_handler, .user_ctx = NULL};
    static const httpd_uri_t control = {
        .uri = "/api/control", .method = HTTP_POST,
        .handler = web_control_handler, .user_ctx = NULL};
    static const httpd_uri_t controlWs = {
        .uri = "/ws/control", .method = HTTP_GET,
        .handler = web_control_ws_handler, .user_ctx = NULL,
        .is_websocket = true};

    REGISTER_WEB_URI(root);
    REGISTER_WEB_URI(page);
    REGISTER_WEB_URI(favicon);
#if CONFIG_USING_CAMERA
    static const httpd_uri_t camera = {
        .uri = "/api/camera", .method = HTTP_GET,
        .handler = web_camera_status_handler, .user_ctx = NULL};
    REGISTER_WEB_URI(camera);
#endif
    REGISTER_WEB_URI(features);
    REGISTER_WEB_URI(battery);
    REGISTER_WEB_URI(control);
    REGISTER_WEB_URI(controlWs);
#undef REGISTER_WEB_URI
}


#if CONFIG_USING_CAMERA
static httpd_handle_t web_stream_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 81;
    config.ctrl_port = 32769;
    config.max_uri_handlers = 1;
    /* Only one browser video stream is supported. Keep the remaining lwIP
     * descriptors available to the control/OTA server and UDP transport. */
    config.max_open_sockets = 2;
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK)
    {
        return NULL;
    }

    static const httpd_uri_t videoWs = {
        .uri = "/ws/video", .method = HTTP_GET,
        .handler = web_video_ws_handler, .user_ctx = NULL,
        .is_websocket = true};
    if (httpd_register_uri_handler(server, &videoWs) != ESP_OK)
    {
        httpd_stop(server);
        return NULL;
    }
    return server;
}
#endif

#endif

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *) event_data;
        DEBUG_PRINT_LOCAL("station" MACSTR "join, AID=%d", MAC2STR(event->mac), event->aid);

        if (NULL == ota_server)
        {
            ota_server = start_web_ota_server();
#if WIFI_WEB_REMOTE_ENABLE
            if (ota_server)
            {
                web_remote_register_handlers(ota_server);
            }
#endif
        }
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *) event_data;
        DEBUG_PRINT_LOCAL("station" MACSTR "leave, AID=%d", MAC2STR(event->mac), event->aid);
        if (ota_server && !e_wifi_ap_any_connected())
        {
            stop_web_ota_server(ota_server);

            ota_server = NULL;
        }
#if CONFIG_USING_CAMERA
        if (stream_server && !e_wifi_ap_any_connected())
        {
            httpd_stop(stream_server);
            stream_server = NULL;
        }
#endif
    }
}

bool wifiTest(void)
{
    return isInit;
};

bool wifiGetDataBlocking(UDPPacket *in)
{
    /* command step - receive  
      from udp rx queue */
    while (xQueueReceive(udpDataRx, in, portMAX_DELAY) != pdTRUE) {
        vTaskDelay(M2T(10));
    }; // Don't return until we get some data on the UDP

    return true;
};

bool wifiSendData(uint32_t size, uint8_t *data)
{
    UDPPacket outStage = {0};
    outStage.size = size;
    memcpy(outStage.data, data, size);
    // Dont' block when sending
    return (xQueueSend(udpDataTx, &outStage, M2T(100)) == pdTRUE);
};

static esp_err_t udp_server_create(void *arg)
{
    if (isUDPInit){
        return ESP_OK;
    }

    static struct sockaddr_in dest_addr = {0};
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(UDP_SERVER_PORT);

    sock_ctl = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock_ctl < 0) {
        DEBUG_PRINT_LOCAL("Unable to create socket: errno %d", errno);
        return ESP_FAIL;
    }
    DEBUG_PRINT_LOCAL("Socket created");

    int err = bind(sock_ctl, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err < 0) {
        DEBUG_PRINT_LOCAL("Socket unable to bind: errno %d", errno);
    }
    DEBUG_PRINT_LOCAL("Socket bound, port %d", UDP_SERVER_PORT);

    isUDPInit = true;
    return ESP_OK;
}
 


#define VIDEO_PORT         5000

#if CONFIG_USING_CAMERA
typedef struct
{
    struct in_addr addr;
    uint32_t tick;
    bool valid;
} video_client_info_t;

static xQueueHandle video_client_queue = NULL;

static struct sockaddr_in last_video_client = {0};
static bool last_video_client_valid = false;
#endif


static void udp_server_rx_task(void *pvParameters)
{
    socklen_t socklen = sizeof(source_addr);
    char rx_buffer[UDP_SERVER_BUFSIZE];
    UDPPacket inPacket = {0};

    while (true) {
        if(isUDPInit == false) {
            vTaskDelay(20);
            continue;
        }
        int len = recvfrom(sock_ctl, rx_buffer, sizeof(rx_buffer), 0, (struct sockaddr *)&source_addr, &socklen);
        /* command step - receive  01 from Wi-Fi UDP */
        if (len < 0) {
            DEBUG_PRINT_LOCAL("recvfrom failed: errno %d", errno);
            continue;
        } else if(len > WIFI_RX_TX_PACKET_SIZE) {
            DEBUG_PRINT_LOCAL("Received data length = %d > %d", len, WIFI_RX_TX_PACKET_SIZE);
            continue;
        } else {
            uint8_t cksum = rx_buffer[len - 1];
            //remove cksum, do not belong to CRTP
            //check packet
            if (cksum == calculate_cksum(rx_buffer, len - 1)) 
            {
                //PRINT_WIFI("cksum:%02x", cksum);

                #if CONFIG_USING_CAMERA
                struct sockaddr_in *src =  (struct sockaddr_in *)&source_addr;

                bool ip_changed = false;

                if (!last_video_client_valid)
                {
                    ip_changed = true;
                }
                else
                {
                    if (src->sin_addr.s_addr !=
                        last_video_client.sin_addr.s_addr)
                    {
                        ip_changed = true;
                    }
                } 

                if (ip_changed)
                {
                    video_client_info_t client =
                    {
                        .addr = src->sin_addr,
                        .valid = true,
                    };

                    xQueueOverwrite(
                        video_client_queue,
                        &client);

                    last_video_client = *src;
                    last_video_client_valid = true;

                    PRINT_WIFI("New video client: %s", inet_ntoa(src->sin_addr));
                }
                #endif

                //copy part of the UDP packet, the size not include cksum
                inPacket.size = len - 1;
                memcpy(inPacket.data, rx_buffer, inPacket.size);
                xQueueSend(udpDataRx, &inPacket, M2T(10));
                lastUdpControlTick = xTaskGetTickCount();
                if(!isUDPConnected) isUDPConnected = true;
            }else{
                PRINT_WIFI("udp packet cksum unmatched");
            }

#ifdef DEBUG_UDP
            printf("\nReceived size = %d cksum = %02X\n", inPacket.size, cksum);
            for (size_t i = 0; i < inPacket.size; i++) {
                printf("%02X ", inPacket.data[i]);
            }
            printf("\n");
#endif
        }
    }
}

static void udp_server_tx_task(void *pvParameters)
{
    UDPPacket outPacket = {0};
    while (TRUE) {
        if(isUDPInit == false) {
            vTaskDelay(20);
            continue;
        }
        if ((xQueueReceive(udpDataTx, &outPacket, portMAX_DELAY) == pdTRUE) && isUDPConnected) {
            // append cksum to the packet
            outPacket.data[outPacket.size] = calculate_cksum(outPacket.data, outPacket.size);
            outPacket.size += 1;

            int err = sendto(sock_ctl, outPacket.data, outPacket.size, 0, (struct sockaddr *)&source_addr, sizeof(source_addr));
            if (err < 0) {
                //DEBUG_PRINT_LOCAL("Error occurred during sending: errno %d", errno);
                continue;
            }
#ifdef DEBUG_UDP
            printf("\nSend size = %d checksum = %02X\n", outPacket.size, outPacket.data[outPacket.size - 1]);
            for (size_t i = 0; i < outPacket.size; i++) {
                printf("%02X ", outPacket.data[i]);
            }
            printf("\n");
#endif
        }
    }
}
 
#if CONFIG_USING_CAMERA
#include "esp_camera.h"

// OV2640标准引脚配置
#define CAM_PIN_PWDN -1
#define CAM_PIN_RESET 43   //software reset will be performed
#define CAM_PIN_VSYNC 44
#define CAM_PIN_HREF 2
#define CAM_PIN_PCLK 16
#define CAM_PIN_XCLK 6//-1
#define CAM_PIN_SIOD 38
#define CAM_PIN_SIOC 39
#define CAM_PIN_D0 18
#define CAM_PIN_D1 9
#define CAM_PIN_D2 46
#define CAM_PIN_D3 8
#define CAM_PIN_D4 17
#define CAM_PIN_D5 15
#define CAM_PIN_D6 7
#define CAM_PIN_D7 48

static camera_config_t camera_config = {
    .pin_pwdn = CAM_PIN_PWDN,
    .pin_reset = CAM_PIN_RESET,
    .pin_xclk = CAM_PIN_XCLK,
    .pin_sccb_sda = CAM_PIN_SIOD,
    .pin_sccb_scl = CAM_PIN_SIOC,

    .pin_d7 = CAM_PIN_D7,
    .pin_d6 = CAM_PIN_D6,
    .pin_d5 = CAM_PIN_D5,
    .pin_d4 = CAM_PIN_D4,
    .pin_d3 = CAM_PIN_D3,
    .pin_d2 = CAM_PIN_D2,
    .pin_d1 = CAM_PIN_D1,
    .pin_d0 = CAM_PIN_D0,
    .pin_vsync = CAM_PIN_VSYNC,
    .pin_href = CAM_PIN_HREF,
    .pin_pclk = CAM_PIN_PCLK,

    .xclk_freq_hz = 12000000,
    .ledc_timer = LEDC_TIMER_1,
    .ledc_channel = LEDC_CHANNEL_1,
    .pixel_format = PIXFORMAT_JPEG,
    .frame_size = FRAMESIZE_QVGA,//FRAMESIZE_QVGA,//FRAMESIZE_240X240, FRAMESIZE_VGA,FRAMESIZE_320X320     // 使用更大的分辨率确保足够数据
    .jpeg_quality = 16,//18,//20,//16,//10,//10,//20,                // 提高质量避免数据过小
    .fb_count = 1,                     // 增加帧缓冲数量
    .fb_location = CAMERA_FB_IN_DRAM,//CAMERA_FB_IN_DRAM,// CAMERA_FB_IN_PSRAM, // 使用PSRAM缓冲区
    .grab_mode = CAMERA_GRAB_LATEST,//CAMERA_GRAB_LATEST,CAMERA_GRAB_WHEN_EMPTY   // 使用最新帧
};


esp_err_t camera_init(void)
{
     // initialize the camera
    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Camera Init Failed");

        return -1;
    }

    #if 0
    sensor_t *s = esp_camera_sensor_get();
  
    s->set_hmirror(s, 1); // 左右镜像
    s->set_vflip(s, 1);   // 上下翻转
    #endif

    return err;
}

/*
 * The JPEG-over-UDP framing and packet transmission approach below is based
 * in part on esp-image-transmission-car by bob521yang:
 * https://gitee.com/bob521yang/esp-image-transmission-car
 *
 * Copyright (c) 2025 bob521yang
 * Licensed under the MIT License. See THIRD_PARTY_NOTICES.md in the project
 * root for the complete copyright and permission notice.
 */
typedef struct __attribute__((packed))
{
    uint32_t frame_id;      // 帧ID
    uint32_t total_size;    // JPEG总大小
    uint16_t total_packets; // 总包数
    uint16_t seq_num;       // 当前包序号
    uint16_t data_len;      // 当前包数据长度
} packet_header_t;
 
#define MAX_PACKET_SIZE    1400
#endif
 
bool e_wifi_ap_any_connected(void)
{
    wifi_mode_t r_mode;
    esp_wifi_get_mode(&r_mode);

    if ((r_mode != WIFI_MODE_APSTA) && (r_mode != WIFI_MODE_AP)) 
    {
        return false;
    }

    esp_err_t ret;
    wifi_sta_list_t list;
    
    ret = esp_wifi_ap_get_sta_list(&list);

    if (ret == ESP_OK) 
    {
        return list.num ? true : false;
    }  

    return false;
}

#if CONFIG_USING_CAMERA

static bool web_frame_publish(camera_fb_t *fb, uint32_t frameId)
{
#if WIFI_WEB_COMBINED_WS_ENABLE && WIFI_WEB_DIRECT_CAMERA_FB_ENABLE
    if (!webFrameMutex || !fb || !fb->buf || fb->len == 0U)
#else
    const uint8_t *data = fb ? fb->buf : NULL;
    size_t length = fb ? fb->len : 0U;
#if WIFI_WEB_COMBINED_WS_ENABLE
    if (!webFrameMutex || !data || length == 0)
#elif WIFI_WEB_SEPARATE_VIDEO_WS_ENABLE
    if (!webFrameMutex || !data || length == 0 || webVideoClientFd < 0)
#else
    if (webStreamClients == 0 || !webFrameMutex || !data || length == 0)
#endif
#endif
    {
        return false;
    }

    /* Never delay the physical controller's UDP video path for a slow web
     * client. If the HTTP handler is sending the previous frame, drop this
     * web frame and let UDP continue normally. */
    if (xSemaphoreTake(webFrameMutex, 0) != pdTRUE)
    {
        return false;
    }

#if WIFI_WEB_SEPARATE_VIDEO_WS_ENABLE
    /* Keep the JPEG currently being fragmented immutable. New camera frames
     * are intentionally dropped until it is complete; there is no historical
     * queue, and the first capture after completion becomes the next frame. */
    if (webVideoFrameSending)
    {
        xSemaphoreGive(webFrameMutex);
        return false;
    }
#endif

#if WIFI_WEB_COMBINED_WS_ENABLE
    /* The combined WebSocket reads chunks directly from this single frame
     * buffer. Keep it stable until that frame has been fully consumed, then
     * replace it with the newest camera frame. This removes the second full
     * JPEG copy without ever mixing chunks from different frames. */
#if WIFI_WEB_DIRECT_CAMERA_FB_ENABLE
    if (!webFrameConsumerActive ||
        (webWsFrameId != UINT32_MAX && webWsFrameOffset < webFrameLength))
#else
    if (webWsFrameId != UINT32_MAX && webWsFrameOffset < webFrameLength)
#endif
    {
        xSemaphoreGive(webFrameMutex);
        return false;
    }
#endif

#if WIFI_WEB_COMBINED_WS_ENABLE && WIFI_WEB_DIRECT_CAMERA_FB_ENABLE
    webFrameFb = fb;
    webFrameLength = fb->len;
    webFrameId = frameId;
    webFrameLastUseTick = xTaskGetTickCount();
#else
    if (length > webFrameCapacity)
    {
        uint8_t *newBuffer = realloc(webFrameData, length);
        if (!newBuffer)
        {
            xSemaphoreGive(webFrameMutex);
            return false;
        }
        webFrameData = newBuffer;
        webFrameCapacity = length;
    }

    memcpy(webFrameData, data, length);
    webFrameLength = length;
    webFrameId = frameId;
#endif
    xSemaphoreGive(webFrameMutex);
#if WIFI_WEB_COMBINED_WS_ENABLE && WIFI_WEB_DIRECT_CAMERA_FB_ENABLE
    return true;
#else
    /* The web path owns only its copied JPEG; the caller must immediately
     * return the camera driver's frame buffer for the legacy UDP pipeline. */
    return false;
#endif
}

#if CONFIG_USING_CAMERA && WIFI_WEB_SEPARATE_VIDEO_WS_ENABLE
static esp_err_t web_video_ws_handler(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (req->method == HTTP_GET)
    {
        int enabled = 1;
        int sendBufferSize = 4 * 1024;
        struct timeval sendTimeout = {.tv_sec = 0, .tv_usec = 250000};
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sendBufferSize, sizeof(sendBufferSize));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &sendTimeout, sizeof(sendTimeout));

        int oldFd = -1;
        if (webVideoClientMutex &&
            xSemaphoreTake(webVideoClientMutex, M2T(100)) == pdTRUE)
        {
            oldFd = webVideoClientFd;
            webVideoClientFd = fd;
            webVideoClientGeneration++;
            xSemaphoreGive(webVideoClientMutex);
        }
        /* A page refresh may establish the replacement before the old
         * browser socket reports CLOSE. Retire only the previous fd. */
        if (oldFd >= 0 && oldFd != fd)
        {
            httpd_sess_trigger_close(req->handle, oldFd);
        }
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {0};
    esp_err_t result = httpd_ws_recv_frame(req, &frame, 0);
    if (result == ESP_OK && frame.type == HTTPD_WS_TYPE_CLOSE &&
        webVideoClientMutex &&
        xSemaphoreTake(webVideoClientMutex, M2T(10)) == pdTRUE)
    {
        if (webVideoClientFd == fd)
        {
            webVideoClientFd = -1;
            webVideoClientGeneration++;
        }
        xSemaphoreGive(webVideoClientMutex);
    }
    return result;
}
#endif

#if WIFI_WEB_SEPARATE_VIDEO_WS_ENABLE
static void web_video_async_send_complete(esp_err_t result, int socket, void *arg)
{
    (void)socket;
    web_video_async_send_t *send = (web_video_async_send_t *)arg;
    /* This callback runs immediately after the real write in the video HTTPD
     * task, so errno still belongs to the task that called send(). */
    send->result = result;
    send->sendErrno = errno;
    xTaskNotifyGive(send->ownerTask);
}

static esp_err_t web_video_async_send_wait(httpd_handle_t server,
                                           int clientFd,
                                           httpd_ws_frame_t *frame,
                                           int *sendErrno)
{
    webVideoAsyncSend.ownerTask = xTaskGetCurrentTaskHandle();
    webVideoAsyncSend.result = ESP_FAIL;
    webVideoAsyncSend.sendErrno = 0;
    (void)ulTaskNotifyTake(pdTRUE, 0);

    esp_err_t result = httpd_ws_send_data_async(server,
                                                clientFd,
                                                frame,
                                                web_video_async_send_complete,
                                                &webVideoAsyncSend);
    if (result != ESP_OK)
    {
        *sendErrno = 0;
        return result;
    }

    /* There is exactly one in-flight video work item. Waiting here keeps the
     * fixed packet buffer immutable and prevents any HTTPD video backlog. */
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    *sendErrno = webVideoAsyncSend.sendErrno;
    return webVideoAsyncSend.result;
}

static void web_video_ws_task(void *arg)
{
    uint32_t lastFrameId = UINT32_MAX;
    TickType_t lastSendErrorLogTick = 0;
    while (true)
    {
        int clientFd = -1;
        uint32_t generation = 0U;
        if (webVideoClientMutex && xSemaphoreTake(webVideoClientMutex, M2T(10)) == pdTRUE)
        {
            clientFd = webVideoClientFd;
            generation = webVideoClientGeneration;
            xSemaphoreGive(webVideoClientMutex);
        }
        httpd_handle_t server = stream_server;
        if (clientFd < 0 || !server || !webFrameMutex ||
            xSemaphoreTake(webFrameMutex, M2T(10)) != pdTRUE)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (webFrameLength == 0U || webFrameId == lastFrameId)
        {
            xSemaphoreGive(webFrameMutex);
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        uint32_t sendFrameId = webFrameId;
        size_t sendFrameLength = webFrameLength;
        webVideoFrameSending = true;
        xSemaphoreGive(webFrameMutex);
        esp_err_t result = ESP_OK;
        int sendErrno = 0;
        for (size_t offset = 0U; offset < sendFrameLength;)
        {
            size_t remaining = sendFrameLength - offset;
            size_t chunkLength = remaining > WEB_WS_VIDEO_CHUNK_SIZE ?
                                 WEB_WS_VIDEO_CHUNK_SIZE : remaining;

            if (xSemaphoreTake(webFrameMutex, M2T(10)) != pdTRUE)
            {
                result = ESP_ERR_TIMEOUT;
                break;
            }
            /* Publishing is blocked by webVideoFrameSending, so this check is
             * only an integrity guard against unexpected state changes. */
            if (webFrameId != sendFrameId ||
                webFrameLength != sendFrameLength ||
                offset + chunkLength > webFrameLength)
            {
                result = ESP_ERR_INVALID_STATE;
                xSemaphoreGive(webFrameMutex);
                break;
            }
            web_ws_packet_header_t header = {
                .magic = WEB_WS_PACKET_MAGIC,
                .frame_id = sendFrameId,
                .total_size = sendFrameLength,
                .offset = offset,
                .data_len = (uint16_t)chunkLength,
            };
            memcpy(webVideoAsyncSend.packetData, &header, sizeof(header));
            memcpy(webVideoAsyncSend.packetData + sizeof(header),
                   webFrameData + offset,
                   chunkLength);
            xSemaphoreGive(webFrameMutex);
            httpd_ws_frame_t frame = {
                .final = true, .fragmented = false,
                .type = HTTPD_WS_TYPE_BINARY,
                .payload = webVideoAsyncSend.packetData,
                .len = sizeof(header) + chunkLength,
            };
            result = web_video_async_send_wait(server,
                                               clientFd,
                                               &frame,
                                               &sendErrno);
            if (result != ESP_OK)
            {
                break;
            }
            offset += chunkLength;
            /* Keep each WS burst below the video socket's send buffer. Two
             * roughly 1.4 KB messages are followed by a 1 ms yield so lwIP
             * and Wi-Fi can drain queued data before the next burst. */
            if (((offset / WEB_WS_VIDEO_CHUNK_SIZE) & 1U) == 0U) vTaskDelay(1);
        }

        xSemaphoreTake(webFrameMutex, portMAX_DELAY);
        webVideoFrameSending = false;
        xSemaphoreGive(webFrameMutex);

        if (result == ESP_OK)
        {
            lastFrameId = sendFrameId;
        }
        else if (webVideoClientMutex &&
                 xSemaphoreTake(webVideoClientMutex, M2T(10)) == pdTRUE)
        {
            bool closeCurrentClient = false;
            TickType_t now = xTaskGetTickCount();
            if ((now - lastSendErrorLogTick) >= pdMS_TO_TICKS(1000))
            {
                lastSendErrorLogTick = now;
                DEBUG_PRINTW("VIDEO_WS send failed, close client: fd=%d frame=%lu errno=%d result=%s\n",
                             clientFd, (unsigned long)sendFrameId, sendErrno,
                             esp_err_to_name(result));
            }
            if (webVideoClientGeneration == generation && webVideoClientFd == clientFd)
            {
                webVideoClientFd = -1;
                webVideoClientGeneration++;
                closeCurrentClient = true;
            }
            xSemaphoreGive(webVideoClientMutex);
            /* Force the browser's onclose path. A failed TCP send can leave
             * JavaScript seeing OPEN indefinitely unless the server retires
             * the HTTPD session explicitly. */
            if (closeCurrentClient)
            {
                httpd_sess_trigger_close(server, clientFd);
            }
        }
    }
}
#endif

static void udp_send_camera_fb_task(void *pvParameters)
{
    int sock = -1;
    uint32_t frame_id = 0;

    struct sockaddr_in dest_addr = {0}; 
    
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(VIDEO_PORT);

    video_client_info_t current_client =
    {
        .valid = false
    }; 

    while (1)
    { 
        video_client_info_t new_client;

        if (xQueueReceive(
                video_client_queue,
                &new_client,
                0) == pdTRUE)
        {
            
            current_client = new_client;

            ESP_LOGI(TAG, "Switch video client");

            if (sock >= 0)
            {
                shutdown(sock, 0);
                close(sock);
                sock = -1;
            }
        }

        if (!e_wifi_ap_any_connected())
        {
            current_client.valid = false;
            last_video_client_valid = false;
            isUDPConnected = false;

            if (sock >= 0)
            {
                shutdown(sock, 0);
                close(sock);
                sock = -1;
            }

            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (current_client.valid &&
            T2M(xTaskGetTickCount() - lastUdpControlTick) >=
                UDP_CONTROLLER_TIMEOUT_MS)
        {
            ESP_LOGW(TAG, "UDP controller timeout, wait for reconnect");

            current_client.valid = false;
            last_video_client_valid = false;
            isUDPConnected = false;

            if (sock >= 0)
            {
                shutdown(sock, 0);
                close(sock);
                sock = -1;
            }
        }
 
        if (current_client.valid && sock < 0)
        {
            if (!e_wifi_ap_any_connected())
            {
                vTaskDelay(pdMS_TO_TICKS(100));

                continue;
            }

            sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);

            if (sock < 0)
            {
                ESP_LOGE(TAG,
                         "socket create failed errno=%d",
                         errno);

                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }

            struct timeval timeout = {
                .tv_sec = 1,
                .tv_usec = 0};

            setsockopt(sock,
                       SOL_SOCKET,
                       SO_SNDTIMEO,
                       &timeout,
                       sizeof(timeout));

            ESP_LOGI(TAG, "UDP socket created");
        }

        TickType_t frame_start_tick = xTaskGetTickCount();
        camera_fb_t *fb = esp_camera_fb_get();

        if (fb == NULL)
        {
            ESP_LOGE(TAG, "Camera capture failed");
 
            packet_header_t header =
            {
                .frame_id = -1,
                .total_size = 0,
                .total_packets = 0,
                .seq_num = 0,
                .data_len = 0,
            };

            uint8_t packet[MAX_PACKET_SIZE];

            memcpy(packet,
                   &header,
                   sizeof(header)); 

            dest_addr.sin_addr = current_client.addr;   

            if (current_client.valid && sock >= 0)
            {
                int ret =
                    sendto(sock,
                           packet,
                           sizeof(header) + header.data_len,
                           0,
                           (struct sockaddr *)&dest_addr,
                           sizeof(dest_addr));

                if (ret < 0)
                {
                    ESP_LOGE(TAG,
                             "VIDEO: send failed errno=%d",
                             errno);
                }
            } 

            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        /*
         * 检查JPEG头
         */
        if ((fb->len < 2) ||
            (fb->buf[0] != 0xFF) ||
            (fb->buf[1] != 0xD8))
        {
            ESP_LOGW(TAG, "Invalid JPEG");

            esp_camera_fb_return(fb);

            continue;
        }

        size_t max_data_per_packet =
            MAX_PACKET_SIZE -
            sizeof(packet_header_t);

        uint16_t total_packets =
            (fb->len + max_data_per_packet - 1) /
            max_data_per_packet;

        bool send_failed = false;
        int send_errno = 0;

        if (current_client.valid && sock >= 0)
        {
            for (uint16_t seq = 0;
                 seq < total_packets;
                 seq++)
            {
                packet_header_t header =
                {
                    .frame_id = frame_id,
                    .total_size = fb->len,
                    .total_packets = total_packets,
                    .seq_num = seq,
                    .data_len = (seq == (total_packets - 1)) ? (fb->len - seq * max_data_per_packet) : max_data_per_packet,
                };

                uint8_t packet[MAX_PACKET_SIZE];

                memcpy(packet,
                       &header,
                       sizeof(header));

                memcpy(packet + sizeof(header),
                       fb->buf + seq * max_data_per_packet,
                       header.data_len);

            #if 1
            dest_addr.sin_addr = current_client.addr; 
            #else
            dest_addr.sin_addr.s_addr = inet_addr("192.168.43.43");
            #endif

            //PRINT_WIFI("sendto : %s", inet_ntoa(dest_addr.sin_addr));

                int ret =
                    sendto(sock,
                           packet,
                           sizeof(header) + header.data_len,
                           0,
                           (struct sockaddr *)&dest_addr,
                           sizeof(dest_addr));

                if (ret < 0)
                {
                    send_errno = errno;
                    ESP_LOGE(TAG,
                             "VIDEO: send failed errno=%d",
                             send_errno);

                    send_failed = true;
                    break;
                }

                /* High-quality JPEGs contain many more packets. Give lwIP a
                 * chance to release transmitted pbufs instead of filling the
                 * complete UDP send path in one uninterrupted burst. */
                if ((seq & 3U) == 3U)
                {
                    vTaskDelay(1);
                }
            }
        }

#if WIFI_WEB_REMOTE_ENABLE
        /* Publish every capture opportunity. If the previous browser JPEG is
         * still being sent, web_frame_publish() drops this capture instead of
         * queueing it or modifying the in-flight frame. */
        bool webOwnsFrame = false;
        /* Strict legacy isolation: when the physical UDP video client is
         * active, do not publish WebSocket frames or consume extra TCP/lwIP
         * buffers. The original UDP JPEG path remains the sole video path. */
        if (!current_client.valid)
        {
            webOwnsFrame = web_frame_publish(fb, frame_id);
        }
#else
        bool webOwnsFrame = false;
#endif
        if (!webOwnsFrame)
        {
            esp_camera_fb_return(fb);
        }
 
        if (send_failed)
        {
            if (send_errno == ENOMEM || send_errno == EAGAIN ||
                send_errno == EWOULDBLOCK)
            {
                /* A full lwIP/pbuf queue is transient. Drop only this stale
                 * frame and retain the socket; recreating it consumes more
                 * memory and causes a multi-second video interruption. */
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            else if (sock >= 0)
            {
                ESP_LOGW(TAG, "Recreate UDP socket");

                shutdown(sock, 0);
                close(sock);

                sock = -1;
                vTaskDelay(pdMS_TO_TICKS(2000));
            }

            continue;
        }

        frame_id++;

        /* Keep capture starts 50 ms apart.  A plain 50 ms delay here made the
         * true period equal to capture + packetisation + 50 ms. */
        vTaskDelayUntil(&frame_start_tick, pdMS_TO_TICKS(50));
    }

    if (sock >= 0)
    {
        shutdown(sock, 0);
        close(sock);
    }

    //end_task:
    
    vTaskDelete(NULL);
} 

#endif

void wifiInit(void)
{
    if (isInit) {
        return;
    }
    // This should probably be reduced to a CRTP packet size
    udpDataRx = xQueueCreate(16, sizeof(UDPPacket));
    DEBUG_QUEUE_MONITOR_REGISTER(udpDataRx);
    udpDataTx = xQueueCreate(16, sizeof(UDPPacket));
    DEBUG_QUEUE_MONITOR_REGISTER(udpDataTx);
#if CONFIG_USING_CAMERA
    video_client_queue = xQueueCreate(1,sizeof(video_client_info_t));
#if WIFI_WEB_REMOTE_ENABLE
    webFrameMutex = xSemaphoreCreateMutex();
#if WIFI_WEB_SEPARATE_VIDEO_WS_ENABLE
    webVideoClientMutex = xSemaphoreCreateMutex();
    ASSERT(webVideoClientMutex);
    xTaskCreate(web_video_ws_task, "web_video_ws", 4096, NULL, 3, NULL);
#endif
#endif
#endif
#if WIFI_WEB_REMOTE_ENABLE
    webControlMutex = xSemaphoreCreateMutex();
    ASSERT(webControlMutex);
    /* Commander WDT is the single source of truth for control loss.
     * Each accepted setpoint refreshes its timestamp; do not run a second
     * Wi-Fi-layer watchdog that can inject an independent zero-throttle packet. */
#endif

    esp_netif_t *ap_netif = NULL;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ap_netif = esp_netif_create_default_wifi_ap();
    uint8_t mac[6];

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                    ESP_EVENT_ANY_ID,
                    &wifi_event_handler,
                    NULL,
                    NULL));

    ESP_ERROR_CHECK(esp_wifi_get_mac(ESP_IF_WIFI_AP, mac));
    sprintf(WIFI_SSID, "%s_%02X%02X%02X%02X%02X%02X", CONFIG_WIFI_BASE_SSID, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    wifi_config_t wifi_config = {
        .ap = {
            .channel = WIFI_CH,
            .max_connection = WIFI_MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
        },
    };

    memcpy(wifi_config.ap.ssid, WIFI_SSID, strlen(WIFI_SSID) + 1) ;
    wifi_config.ap.ssid_len = strlen(WIFI_SSID);
    memcpy(wifi_config.ap.password, WIFI_PWD, strlen(WIFI_PWD) + 1) ;

    if (strlen(WIFI_PWD) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_channel(WIFI_CH, WIFI_SECOND_CHAN_NONE);   
    esp_netif_ip_info_t ip_info = {
        .ip.addr = ipaddr_addr("192.168.43.42"),
        .netmask.addr = ipaddr_addr("255.255.255.0"),
        .gw.addr      = ipaddr_addr("192.168.43.42"),
    };
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap_netif));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ip_info));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));
    DEBUG_PRINT_LOCAL("wifi_init_softap complete.SSID:%s password:%s", WIFI_SSID, WIFI_PWD);

    if (udp_server_create(NULL) == ESP_FAIL) {
        DEBUG_PRINT_LOCAL("UDP server create socket failed");
    } else {
        DEBUG_PRINT_LOCAL("UDP server create socket succeed");
    }
  
    xTaskCreate(udp_server_rx_task, UDP_RX_TASK_NAME, UDP_RX_TASK_STACKSIZE, NULL, UDP_RX_TASK_PRI, NULL);//UDP_RX_TASK_PRI 8
    xTaskCreate(udp_server_tx_task, UDP_TX_TASK_NAME, UDP_TX_TASK_STACKSIZE, NULL, UDP_TX_TASK_PRI, NULL);
       
    #if CONFIG_USING_CAMERA
      
    camera_init_res = camera_init();
    if (ESP_OK == camera_init_res)
    {  
        if (stream_server == NULL)
        {
            stream_server = web_stream_server_start();
        }
        xTaskCreatePinnedToCore(udp_send_camera_fb_task, "udp_send_camera", 2048 * 4, NULL, 7, NULL, 1);
    } 
    
    #endif  
  
    isInit = true;
}
