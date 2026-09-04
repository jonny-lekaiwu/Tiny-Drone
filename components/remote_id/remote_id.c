#include "remote_id.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "app_channel.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "motors.h"
#include "stabilizer.h"
#include "system.h"
#include "esp_mac.h"

static const char *TAG = "remote_id";

#define RID_FLIGHT_MOTOR_SUM_THRESHOLD 1000
#define RID_GROUND_CONFIRM_SAMPLES 2
#define RID_GYRO_STATIC_DPS 5.0f

static uint8_t update_operation_state(void)
{
    static bool airborne;
    static uint8_t ground_samples;
    float thrust = 0.0f;
    Axis3f gyro = {0};
    int motor_sum = 0;
    for (unsigned i = 0; i < 4; ++i) motor_sum += motorsGetRatio(i);

    bool snapshot_valid = stabilizerGetFlightStatusSnapshot(&thrust, &gyro);
    bool attitude_static = snapshot_valid &&
                           fabsf(gyro.x) <= RID_GYRO_STATIC_DPS &&
                           fabsf(gyro.y) <= RID_GYRO_STATIC_DPS &&
                           fabsf(gyro.z) <= RID_GYRO_STATIC_DPS;
    bool zero_throttle = thrust <= 0.0f && motor_sum <= RID_FLIGHT_MOTOR_SUM_THRESHOLD;

    if (!systemIsArmed()) {
        airborne = false;
        ground_samples = 0;
    } else if (!zero_throttle && motor_sum > RID_FLIGHT_MOTOR_SUM_THRESHOLD) {
        airborne = true;
        ground_samples = 0;
    } else if (airborne && zero_throttle && attitude_static) {
        if (++ground_samples >= RID_GROUND_CONFIRM_SAMPLES) {
            airborne = false;
            ground_samples = 0;
        }
    } else {
        ground_samples = 0;
    }
    return airborne ? 0x02 : 0x01;
}

uint8_t remoteIdGetOperationState(void)
{
    /* Operation-state telemetry is independent of RID beacon broadcasting. */
    return update_operation_state();
}

#if CONFIG_REMOTE_ID_ENABLE

#define RID_FRAME_MAX 160
#define GB46750_CONTENT_LEN 66
#define GB46750_PACKET_LEN (3 + 3 + GB46750_CONTENT_LEN)
#define RID_UNKNOWN_POSITION ((int32_t)-1)
#define RID_UNKNOWN_U16 0xffffU
#define RID_STATION_UPDATE_TYPE 0x52
#define RID_STATION_UPDATE_VERSION 1
#define RID_STATION_POSITION_VALID 0x01
#define RID_STATION_ALTITUDE_VALID 0x02
#define RID_STATION_TIME_VALID 0x04
#define RID_STATION_DATA_TIMEOUT_MS 5000
#define RID_STATION_PACKET_LEN 24

typedef struct {
    bool position_valid;
    bool altitude_valid;
    bool time_valid;
    int32_t longitude_e7;
    int32_t latitude_e7;
    int32_t altitude_cm;
    uint64_t unix_ms;
    uint8_t time_accuracy;
    TickType_t received_tick;
} rid_station_data_t;

static rid_station_data_t station_data;

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, int32_t v)
{
    uint32_t u = (uint32_t)v;
    p[0] = (uint8_t)u;
    p[1] = (uint8_t)(u >> 8);
    p[2] = (uint8_t)(u >> 16);
    p[3] = (uint8_t)(u >> 24);
}

static void put_le48(uint8_t *p, uint64_t v)
{
    for (unsigned i = 0; i < 6; ++i) p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t get_le32u(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_le48(const uint8_t *p)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 6; ++i) value |= (uint64_t)p[i] << (8 * i);
    return value;
}

static void receive_station_update(void)
{
    uint8_t data[APPCHANNEL_MTU];
    size_t length;
    while ((length = appchannelReceivePacket(data, sizeof(data), 0)) != 0) {
        if (length != RID_STATION_PACKET_LEN || data[0] != RID_STATION_UPDATE_TYPE ||
            data[1] != RID_STATION_UPDATE_VERSION) continue;

        uint8_t flags = data[2];
        int32_t lon = (int32_t)get_le32u(data + 4);
        int32_t lat = (int32_t)get_le32u(data + 8);
        uint64_t unix_ms = get_le48(data + 16);
        uint8_t time_accuracy = data[22];
        station_data.position_valid = (flags & RID_STATION_POSITION_VALID) &&
                                      lon >= -1800000000 && lon <= 1800000000 &&
                                      lat >= -900000000 && lat <= 900000000;
        station_data.altitude_valid = (flags & RID_STATION_ALTITUDE_VALID) != 0;
        station_data.time_valid = (flags & RID_STATION_TIME_VALID) &&
                                  unix_ms != 0 && time_accuracy <= 8;
        station_data.longitude_e7 = lon;
        station_data.latitude_e7 = lat;
        station_data.altitude_cm = (int32_t)get_le32u(data + 12);
        station_data.unix_ms = unix_ms;
        station_data.time_accuracy = time_accuracy;
        station_data.received_tick = xTaskGetTickCount();
    }
}

static bool station_data_fresh(void)
{
    return station_data.received_tick != 0 &&
           (xTaskGetTickCount() - station_data.received_tick) <=
               pdMS_TO_TICKS(RID_STATION_DATA_TIMEOUT_MS);
}

static uint16_t altitude_from_cm(int32_t altitude_cm)
{
    int64_t value = ((int64_t)altitude_cm + 100000) / 50;
    return value > 0 && value <= UINT16_MAX ? (uint16_t)value : 0;
}

static uint8_t self_mac[6];

static uint16_t build_gb46750_packet(uint8_t *packet)
{
    uint16_t p = 0;
    bool station_fresh = station_data_fresh();
    bool station_position_valid = station_fresh && station_data.position_valid;
    bool station_altitude_valid = station_fresh && station_data.altitude_valid;
    packet[p++] = 0xff;          /* data type: operational identification */
    packet[p++] = 0x20;          /* version V1.0: major bits 001, minor 0 */
    packet[p++] = GB46750_CONTENT_LEN;
    packet[p++] = 0xdf;          /* mandatory 001,002,004..007 + extension */
    packet[p++] = 0xe5;          /* mandatory 008,009,010,013 + extension */
    packet[p++] = 0xfe;          /* mandatory 015..021, end of flags */

    memset(packet + p, 0, 20);
  
    char config_remote_id_uas_id[21]={0}; 

    snprintf(config_remote_id_uas_id, sizeof(config_remote_id_uas_id),"DY00TD00%02X%02X%02X%02X%02X%02X", self_mac[0], self_mac[1], self_mac[2], self_mac[3], self_mac[4], self_mac[5]);

    memcpy(packet + p, config_remote_id_uas_id, strnlen(config_remote_id_uas_id, 20));
    p += 20;                     /* 001 unique product ID */
    memset(packet + p, 0, 8);
    memcpy(packet + p, CONFIG_REMOTE_ID_REGISTRATION_ID,
           strnlen(CONFIG_REMOTE_ID_REGISTRATION_ID, 8));
    p += 8;                      /* 002 registration ID, last 8 chars */
    packet[p++] = 0x00;          /* 004 micro UA */
    packet[p++] = station_position_valid ? 0x01 : 0x00;
    put_le32(packet + p, station_position_valid ? station_data.longitude_e7 : RID_UNKNOWN_POSITION); p += 4;
    put_le32(packet + p, station_position_valid ? station_data.latitude_e7 : RID_UNKNOWN_POSITION); p += 4;
    put_le16(packet + p, station_altitude_valid ? altitude_from_cm(station_data.altitude_cm) : 0x0000); p += 2;
    put_le32(packet + p, RID_UNKNOWN_POSITION); p += 4;
    put_le32(packet + p, RID_UNKNOWN_POSITION); p += 4; /* 008 unknown lon,lat */
    put_le16(packet + p, RID_UNKNOWN_U16); p += 2; /* 009 track unknown */
    put_le16(packet + p, RID_UNKNOWN_U16); p += 2; /* 010 ground speed unknown */
    put_le16(packet + p, 0x0000); p += 2;          /* 013 geometric altitude unknown */
    packet[p++] = remoteIdGetOperationState();     /* 015 ground / airborne */
    packet[p++] = 0x00;                           /* 016 WGS-84 */
    packet[p++] = 0x00;                           /* 017 horizontal accuracy unknown */
    packet[p++] = 0x00;                           /* 018 vertical accuracy unknown */
    packet[p++] = 0x00;                           /* 019 speed accuracy unknown */
    uint64_t unix_ms = 0;
    uint8_t time_accuracy = 0;
    if (station_fresh && station_data.time_valid) {
        TickType_t elapsed = xTaskGetTickCount() - station_data.received_tick;
        unix_ms = station_data.unix_ms + (uint64_t)elapsed * portTICK_PERIOD_MS;
        time_accuracy = station_data.time_accuracy;
    } else {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        if (tv.tv_sec >= 1704067200) unix_ms = (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    }
    put_le48(packet + p, unix_ms); p += 6;         /* 020 Unix milliseconds */
    packet[p++] = time_accuracy;                   /* 021 timestamp accuracy */
    return p;
}

static uint16_t build_frame(uint8_t *frame, uint8_t counter)
{
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    uint16_t p = 0;
    frame[p++] = 0x80; frame[p++] = 0x00; frame[p++] = 0; frame[p++] = 0;
    memset(frame + p, 0xff, 6); p += 6;
    memcpy(frame + p, mac, 6); p += 6;
    memcpy(frame + p, mac, 6); p += 6;
    frame[p++] = 0; frame[p++] = 0;
    memset(frame + p, 0, 8); p += 8;
    put_le16(frame + p, 100); p += 2;
    frame[p++] = 0x21; frame[p++] = 0x04;

    frame[p++] = 0xdd;
    frame[p++] = 3 + 1 + 1 + GB46750_PACKET_LEN;
    frame[p++] = 0xfa; frame[p++] = 0x0b; frame[p++] = 0xbc;
    frame[p++] = 0x0d;
    frame[p++] = counter;
    p += build_gb46750_packet(frame + p);
    return p;
}

static void remote_id_task(void *arg)
{
    (void)arg;
 
    esp_read_mac(self_mac, ESP_MAC_EFUSE_FACTORY); 

    wifi_mode_t mode;
    while (esp_wifi_get_mode(&mode) != ESP_OK) vTaskDelay(pdMS_TO_TICKS(250));

    // if (strlen(CONFIG_REMOTE_ID_UAS_ID) != 20 ||
    //     strlen(CONFIG_REMOTE_ID_REGISTRATION_ID) != 8) {
    //     ESP_LOGW(TAG, "GB46750 RID has placeholder ID/registration; configure before flight");
    // }

    uint8_t counter = 0;
    for (;;) {
        receive_station_update();
        uint8_t frame[RID_FRAME_MAX];
        uint16_t length = build_frame(frame, counter++);
        esp_err_t err = esp_wifi_80211_tx(WIFI_IF_AP, frame, length, true);
        if (err != ESP_OK) ESP_LOGW(TAG, "beacon transmit failed: %s", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
#endif

void remoteIdStart(void)
{
#if CONFIG_REMOTE_ID_ENABLE
    xTaskCreate(remote_id_task, "remote_id", 3072, NULL, 2, NULL);
#endif
}
