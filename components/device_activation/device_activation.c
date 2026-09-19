#include "device_activation.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "driver/usb_serial_jtag.h"
#if CONFIG_VFS_SUPPORT_IO && CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
#include "driver/usb_serial_jtag_vfs.h"
#endif
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#define LINE_BYTES 256
#define RECORD_VERSION 1 

/* One NVS blob prevents mixing old and new fields after interrupted writes. */
typedef struct {
    uint8_t version;
    char certificate[37];
    char product[21];
    char uas[12];
} activation_record_t;

static const char *TAG = "activation"; 
static activation_record_t record;
static bool activated;
static bool restart_after_activation;

bool deviceActivationIsActive(void) { return activated; }
const char *deviceActivationProductId(void) { return record.product; }
const char *deviceActivationRegistrationId(void) { return activated ? record.uas + 3 : ""; }

static bool uppercase_alnum(const char *s)
{
    for (; *s; ++s) {
        if (!(*s >= 'A' && *s <= 'Z') && !(*s >= '0' && *s <= '9')) return false;
    }
    return true;
}
 
static bool valid_product_id(const char *s)
{
    return strlen(s) == 20 &&
           !strncmp(s, "DY00", 4) &&
           uppercase_alnum(s + 4);
}

static bool valid_uuid(const char *s)
{
    if (strlen(s) != 36) return false;
    for (unsigned i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-') return false;
        } else if (!((s[i] >= '0' && s[i] <= '9') ||
                     (s[i] >= 'a' && s[i] <= 'f') || (s[i] >= 'A' && s[i] <= 'F'))) return false;
    }
    return s[14] == '4' && strchr("89aAbB", s[19]) != NULL;
} 

static bool valid_record(const activation_record_t *r)
{
    return r->version == RECORD_VERSION &&
           r->certificate[36] == '\0' &&
           r->product[20] == '\0' &&
           r->uas[11] == '\0' &&
           valid_uuid(r->certificate) &&
           strlen(r->product) == 20 &&
           !strncmp(r->product, "DY00", 4) &&
           uppercase_alnum(r->product + 4) &&
           strlen(r->uas) == 11 &&
           !strncmp(r->uas, "UAS", 3) &&
           uppercase_alnum(r->uas + 3);
}

static void load_record(void)
{
    nvs_handle_t h;
    if (nvs_open("uom", NVS_READONLY, &h) != ESP_OK) return;
    size_t size = sizeof(record);
    esp_err_t err = nvs_get_blob(h, "record", &record, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* Accept the prototype's existing NVS only when it belongs to this chip. */
        uint8_t active = 0;
        size_t cert_len = sizeof(record.certificate), product_len = sizeof(record.product);
        size_t uas_len = sizeof(record.uas);
        record.version = RECORD_VERSION;
        err = nvs_get_u8(h, "activated", &active);
        if (err == ESP_OK && active == 1) {
            err = nvs_get_str(h, "cert_id", record.certificate, &cert_len);
            if (err == ESP_OK) err = nvs_get_str(h, "upic_msn", record.product, &product_len);
            if (err == ESP_OK) err = nvs_get_str(h, "uas_code", record.uas, &uas_len);
        } else err = ESP_ERR_INVALID_STATE;
    }
    nvs_close(h);
    activated = err == ESP_OK && size == sizeof(record) && valid_record(&record);
    if (!activated)
    { 
        memset(&record, 0, sizeof(record));
    } 
}

static esp_err_t save_record(const activation_record_t *r)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("uom", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, "record", r, sizeof(*r));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void send_line(const char *line)
{
    char output[LINE_BYTES];
    int length = snprintf(output, sizeof(output), "\r\n%s\r\n", line);
    if (length < 0 || (size_t)length >= sizeof(output)) return;
    const char *p = output;
    while (length > 0) {
        int n = usb_serial_jtag_write_bytes(p, length, pdMS_TO_TICKS(1000));
        if (n <= 0) return;
        p += n;
        length -= n;
    }
}

static void handle_activate(char *args)
{
    /* Unlike strtok, preserve empty fields and reject trailing separators. */
    char *fields[4] = {args};
    for (unsigned i = 1; i < 4; ++i) {
        char *separator = strchr(fields[i - 1], '|');
        if (!separator) { send_line("ERROR:参数数量错误"); return; }
        *separator = '\0';
        fields[i] = separator + 1;
    }
    if (strchr(fields[3], '|')) { send_line("ERROR:参数数量错误"); return; }
    if (!valid_uuid(fields[0])) { send_line("ERROR:凭证UUID格式错误"); return; } 
    if (!valid_product_id(fields[1])) { send_line("ERROR:产品序列号格式异常"); return; }
    if (strlen(fields[2]) != 11 || strncmp(fields[2], "UAS", 3) || !uppercase_alnum(fields[2] + 3)) {
        send_line("ERROR:实名登记标志格式异常"); return;
    }
    if (strcmp(fields[3], "正常")) { send_line("ERROR:实名登记状态不是正常"); return; }
    activation_record_t next = {.version = RECORD_VERSION};
    strcpy(next.certificate, fields[0]);
    strcpy(next.product, fields[1]);
    strcpy(next.uas, fields[2]);
    if (activated) {
        /* Never rewrite identity or remotely reboot a flight-enabled device. */
        if (strcmp(record.certificate, next.certificate) || strcmp(record.uas, next.uas)) {
            send_line("ERROR:设备已激活，不允许覆盖登记信息"); return;
        }
    } else if (save_record(&next) != ESP_OK) {
        send_line("ERROR:激活信息写入失败"); return;
    }
    char response[128];
    snprintf(response, sizeof(response), "+ACTIVATE:%s|%s|正常", next.product, next.uas);
    send_line(response);
    send_line("OK");
    if (!activated && restart_after_activation) {
        /* Motors stay locked through the acknowledgment and restart. */
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
}

static void handle_command(char *line)
{
    if (!strcmp(line, "AT") || !strcmp(line, "AT+PING")) send_line("OK");
    else if (!strcmp(line, "AT+HELLO")) {
        char response[160];
        snprintf(response, sizeof(response), "+HELLO:%s|usb_serial_jtag|Tiny-Drone-activation-1|%u|%s|%s",
#if CONFIG_IDF_TARGET_ESP32S3
                 "ESP32-S3",
#else
                 "ESP32-C3",
#endif
                 activated ? 1 : 0, record.product, activated ? record.uas : "-");
        send_line(response);
        send_line("OK");
    } else if (!strncmp(line, "AT+ACTIVATE=", 12)) handle_activate(line + 12);
    else send_line("ERROR:不支持的命令");
}

static char line_buffer[LINE_BYTES];
static size_t line_used;
static bool discard_line;

static void receive_byte(uint8_t ch)
{
    if (ch == '\r' || ch == '\n') {
        if (!discard_line && line_used) {
            line_buffer[line_used] = '\0';
            handle_command(line_buffer);
        }
        line_used = 0;
        discard_line = false;
    } else if (!discard_line) {
        if (ch == 0 || line_used == sizeof(line_buffer) - 1) {
            discard_line = true;
            line_used = 0;
            send_line("ERROR:命令过长或包含非法字符");
        } else line_buffer[line_used++] = (char)ch;
    }
}

static void activation_task(void *arg)
{
    (void)arg;
    char status[96];
    if (activated) snprintf(status, sizeof(status), "+STATUS:ACTIVATED|%s|%s", record.product, record.uas);
    else snprintf(status, sizeof(status), "+STATUS:NOT_ACTIVATED");
    send_line(status);
    uint8_t chunk[128];
    for (;;) {
        int n = usb_serial_jtag_read_bytes(chunk, sizeof(chunk), pdMS_TO_TICKS(100));
        for (int i = 0; i < n; ++i) receive_byte(chunk[i]);
    }
}

esp_err_t deviceActivationInit(bool restartAfterActivation)
{
    restart_after_activation = restartAfterActivation;
    
    esp_err_t err = -1;
    load_record();
    usb_serial_jtag_driver_config_t config = {.rx_buffer_size = 2048, .tx_buffer_size = 2048};
    err = usb_serial_jtag_driver_install(&config);
    if (err != ESP_OK) return err;
#if CONFIG_VFS_SUPPORT_IO && CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
    /* Console logs must share the TX queue instead of polling the same FIFO. */
    usb_serial_jtag_vfs_use_driver();
#endif
    if (xTaskCreate(activation_task, "activation", 4096, NULL, 2, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "%s", activated ? "activated" : "not activated; motors locked");
    return ESP_OK;
}
