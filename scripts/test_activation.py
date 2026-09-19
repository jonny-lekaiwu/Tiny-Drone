"""Host regression tests for the real activation C implementation (requires GCC).

Mocks ESP-IDF IO/NVS only; no device is connected or flashed.
Run: python scripts/test_activation.py
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
STUB = r'''
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
typedef int nvs_handle_t;
#define ESP_OK 0
#define ESP_ERR_NVS_NOT_FOUND 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NO_MEM 3
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define ESP_MAC_EFUSE_FACTORY 0
#define CONFIG_IDF_TARGET_ESP32S3 1
#define CONFIG_VFS_SUPPORT_IO 1
#ifndef CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
#define CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED 1
#endif
#define pdMS_TO_TICKS(x) (x)
#define pdPASS 1
#define ESP_LOGI(tag, ...) ((void)(tag))
typedef struct { int rx_buffer_size; int tx_buffer_size; } usb_serial_jtag_driver_config_t;
int usb_serial_jtag_write_bytes(const void *, size_t, int);
int usb_serial_jtag_read_bytes(void *, size_t, int);
int usb_serial_jtag_driver_install(usb_serial_jtag_driver_config_t *);
void usb_serial_jtag_vfs_use_driver(void);
int xTaskCreate(void (*)(void *), const char *, int, void *, int, void *);
void vTaskDelay(int);
void esp_restart(void);
int esp_read_mac(uint8_t *, int);
int nvs_open(const char *, int, nvs_handle_t *);
void nvs_close(nvs_handle_t);
int nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
int nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
int nvs_get_str(nvs_handle_t, const char *, char *, size_t *);
int nvs_get_u8(nvs_handle_t, const char *, uint8_t *);
int nvs_commit(nvs_handle_t);
'''
TEST = r'''
#include <assert.h>
#include "device_activation.c"

static activation_record_t stored, pending;
static int present, legacy, writes, restarts, fail_write, fail_commit;
static char output[8192];
static size_t output_used;
static void (*delay_hook)(int);
int usb_serial_jtag_write_bytes(const void *p, size_t n, int ticks) {
    (void)ticks; assert(output_used + n < sizeof(output));
    memcpy(output + output_used, p, n); output_used += n; output[output_used] = 0; return (int)n;
}
int usb_serial_jtag_read_bytes(void *p, size_t n, int t) { (void)p; (void)n; (void)t; return 0; }
int usb_serial_jtag_driver_install(usb_serial_jtag_driver_config_t *c) { (void)c; return 0; }
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
void usb_serial_jtag_vfs_use_driver(void) {}
#endif
int xTaskCreate(void (*f)(void *), const char *n, int s, void *a, int p, void *h) {
    (void)f; (void)n; (void)s; (void)a; (void)p; (void)h; return pdPASS;
}
void vTaskDelay(int t) { if (delay_hook) delay_hook(t); }
void esp_restart(void) { ++restarts; }
int esp_read_mac(uint8_t *m, int t) { (void)t; const uint8_t mac[] = {1,2,3,4,5,6}; memcpy(m,mac,6); return 0; }
int nvs_open(const char *s, int m, nvs_handle_t *h) { (void)m; assert(!strcmp(s,"uom")); *h=1; return 0; }
void nvs_close(nvs_handle_t h) { (void)h; }
int nvs_get_blob(nvs_handle_t h, const char *k, void *p, size_t *n) {
    (void)h; assert(!strcmp(k,"record")); if (!present) return ESP_ERR_NVS_NOT_FOUND;
    assert(*n == sizeof(stored)); memcpy(p,&stored,sizeof(stored)); *n=sizeof(stored); return 0;
}
int nvs_set_blob(nvs_handle_t h, const char *k, const void *p, size_t n) {
    (void)h; assert(!strcmp(k,"record")); assert(n==sizeof(pending)); ++writes;
    if (fail_write) return ESP_ERR_NO_MEM;
    memcpy(&pending,p,n); return 0;
}
int nvs_commit(nvs_handle_t h) { (void)h; if (fail_commit) return ESP_ERR_NO_MEM; stored=pending; present=1; return 0; }
int nvs_get_u8(nvs_handle_t h, const char *k, uint8_t *p) { (void)h; (void)k; *p=legacy; return legacy ? 0 : 1; }
int nvs_get_str(nvs_handle_t h, const char *k, char *p, size_t *n) {
    (void)h; if (!legacy) return 1;
    const char *s = !strcmp(k,"cert_id") ? stored.certificate : !strcmp(k,"upic_msn") ? stored.product : stored.uas;
    assert(strlen(s)<*n); strcpy(p,s); *n=strlen(s)+1; return 0;
}
static void command(const char *s) {
    output_used=0; output[0]=0;
    while (*s) receive_byte((uint8_t)*s++);
    receive_byte('\r'); receive_byte('\n');
}
#define CERT "12345678-1234-4234-8234-123456789abc"
#define PRODUCT "DY00TD00010203040506"
#define VALID "AT+ACTIVATE=" CERT "|" PRODUCT "|UAS1234ABCD|正常"
static void rejected(const char *s) {
    int w=writes, r=restarts; command(s);
    assert(strstr(output,"ERROR:")); assert(!strstr(output,"+ACTIVATE:"));
    assert(writes==w && restarts==r && !deviceActivationIsActive());
}
int main(void) {
    assert(deviceActivationInit(ENABLE_ACTIVATION_MOTOR_LOCK != 0)==0 && !deviceActivationIsActive());
    test_motor_gates();
    test_activation_alarm();
    test_rid_fields(false);
    assert(!strcmp(deviceActivationProductId(),PRODUCT));
    command("AT+HELLO"); assert(strstr(output,"|0|" PRODUCT "|-"));
    rejected("AT+ACTIVATE=|" PRODUCT "|UAS1234ABCD|正常");
    rejected("AT+ACTIVATE=" CERT "|DY00TD00010203040507|UAS1234ABCD|正常");
    rejected("AT+ACTIVATE=" CERT "|" PRODUCT "|UAS1234abcd|正常");
    rejected("AT+ACTIVATE=" CERT "|" PRODUCT "|UAS1234ABCD|注销");
    rejected(VALID "|");
    rejected("AT+ACTIVATE=12345678-1234-1234-8234-123456789abc|" PRODUCT "|UAS1234ABCD|正常");
    /* An overflowing line must discard a valid-looking suffix too. */
    char long_line[600]; memset(long_line,'X',300); strcpy(long_line+300,VALID);
    rejected(long_line); command("AT+PING"); assert(strstr(output,"OK"));
    output_used=0; receive_byte('X'); receive_byte(0);
    const char *s=VALID; while (*s) receive_byte((uint8_t)*s++); receive_byte('\r');
    assert(!present && !restarts);
    fail_write=1; command(VALID); assert(strstr(output,"ERROR:") && !restarts && !present); fail_write=0;
    fail_commit=1; command(VALID); assert(strstr(output,"ERROR:") && !restarts && !present); fail_commit=0;
    command(VALID); assert(strstr(output,"+ACTIVATE:" PRODUCT "|UAS1234ABCD|正常"));
    assert(restarts==ENABLE_ACTIVATION_MOTOR_LOCK && present && !deviceActivationIsActive());
    load_record(); assert(deviceActivationIsActive());
    test_rid_fields(true);
    assert(!strcmp(deviceActivationRegistrationId(),"1234ABCD"));
    command("AT+HELLO"); assert(strstr(output,"|1|" PRODUCT "|UAS1234ABCD"));
    int w=writes; command(VALID); assert(strstr(output,"+ACTIVATE:") && writes==w && restarts==ENABLE_ACTIVATION_MOTOR_LOCK);
    command("AT+ACTIVATE=" CERT "|" PRODUCT "|UAS8765DCBA|正常");
    assert(strstr(output,"ERROR:") && writes==w && restarts==ENABLE_ACTIVATION_MOTOR_LOCK);
    activation_record_t good=stored;
    stored.product[19]='7'; load_record(); assert(!deviceActivationIsActive());
    stored=good; stored.uas[11]='X'; load_record(); assert(!deviceActivationIsActive());
    stored=good; stored.version=2; load_record(); assert(!deviceActivationIsActive());
    stored=good; present=0; legacy=1; load_record(); assert(deviceActivationIsActive());
    stored.product[19]='7'; load_record(); assert(!deviceActivationIsActive());
    printf("PASS (motor lock=%d, USB console=%d): activation, NVS failures, identity binding, motor/arming gates, throttle alarm, fixed tone, RID identity fields\n", ENABLE_ACTIVATION_MOTOR_LOCK, CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED);
    return 0;
}
'''

# Exercise actual motor/arming functions, with only hardware calls replaced.
def function_source(path, signature):
    text = (ROOT / path).read_text(encoding="utf-8")
    start = text.index(signature + "\n{")
    end = text.index("\n}", start) + 2
    return text[start:end]

GATES = r'''
#define ASSERT(x) assert(x)
#define NBR_OF_MOTORS 4
#define LEDC_LOW_SPEED_MODE 0
#define LEDC_TIMER_0 0
#define BRUSHED 1
#define ENABLE_THRUST_BAT_COMPENSATED
#define M2T(x) (x)
#define C6 1047
typedef uint32_t TickType_t;
static TickType_t mock_tick;
static TickType_t xTaskGetTickCount(void) { return mock_tick; }
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
static bool isInit=true, canFly=true, armed, forceArm;
static bool altHoldMode;
static unsigned frequency_now=15000, pwm_frequency, motor_ratios[4], duties[4];
static struct { int speed_mode; int channel; } motors_channel[] = {{0,0},{0,1},{0,2},{0,3}};
static struct motor_type { int drvType; } brushed = {BRUSHED};
static struct motor_type *motorMap[] = {&brushed,&brushed,&brushed,&brushed};
static unsigned motorsConv16ToBits(unsigned v) { return v; }
static float pmGetBatteryVoltage(void) { return 4.0f; }
static void ledc_set_freq(int m, int t, unsigned f) { (void)m; (void)t; pwm_frequency=f; }
static void ledc_set_duty(int m, int c, unsigned d) { (void)m; duties[c]=d; }
static void ledc_update_duty(int m, int c) { (void)m; (void)c; }
'''
for path, signatures in [
    ("components/drivers/general/motors/motors.c", [
        "bool motorsIsActivationLocked(void)",
        "static void motorsTestBeep(uint32_t motorId, bool enable, uint16_t frequency)",
        "void motorsSetRatio(uint32_t id, uint16_t ithrust)",
        "void motorsBeep(int id, bool enable, uint16_t frequency, uint16_t ratio)",
        "void play_activation_error(void)"]),
    ("components/core/crazyflie/modules/src/system.c", [
        "bool systemCanFly(void)", "void systemSetArmed(bool val)", "bool systemIsArmed()"]),
]:
    for signature in signatures:
        GATES += "\n" + function_source(path, signature) + "\n"
GATES += r'''
static void test_motor_gates(void) {
    for (int state=0; state<=1; ++state) {
        activated=state; forceArm=true; systemSetArmed(true);
        const bool allowed=state || !ENABLE_ACTIVATION_MOTOR_LOCK;
        assert(systemIsArmed()==allowed && systemCanFly()==allowed);
        for (unsigned i=0; i<4; ++i) {
            motorsSetRatio(i,65535); assert((duties[i]>0)==allowed);
            motorsBeep(i,true,440,100); assert((duties[i]>0)==allowed);
            motorsTestBeep(i,true,440); assert((duties[i]>0)==allowed);
            motorsSetRatio(i,0); assert(duties[i]==0);
        }
        systemSetArmed(false); forceArm=false;
        assert(!systemIsArmed());
        canFly=false; assert(!systemCanFly()); canFly=true;
    }
    activated=false; forceArm=false; armed=false;
}
'''
rpyt_path = "components/core/crazyflie/modules/src/crtp_commander_rpyt.c"
rpyt_source = (ROOT / rpyt_path).read_text(encoding="utf-8")
start = rpyt_source.index("static portMUX_TYPE activationAlarmMux")
end = rpyt_source.index("static TickType_t activationThrottleLastTick;", start)
end += len("static TickType_t activationThrottleLastTick;")
GATES += rpyt_source[start:end] + "\n"
for line in rpyt_source.splitlines():
    if line.startswith(("#define MIN_THRUST ", "#define ALT_HOLD_THRUST_CENTER ", "#define ALT_HOLD_THRUST_DEADZONE ")):
        GATES += line + "\n"
for signature in ["static void updateActivationThrottle(uint16_t rawThrust)",
                  "bool crtpCommanderConsumeActivationAlarmRequest(void)"]:
    GATES += function_source(rpyt_path, signature) + "\n"
GATES += r'''
static unsigned sounding_delays, silence_delays;
static void observe_tone(int ticks) {
    assert(ticks==70 && motorsIsActivationLocked() && !systemIsArmed());
    bool sounding=duties[0]>0;
    if (sounding) ++sounding_delays; else ++silence_delays;
    for (unsigned i=0; i<4; ++i) {
        assert(duties[i]==(sounding ? UINT16_MAX/20U : 0));
        /* Even during a tone, ordinary full-throttle writes are clamped. */
        motorsSetRatio(i,65535); assert(duties[i]==0);
    }
}
static void test_activation_alarm(void) {
    activated=false; mock_tick=0;
    updateActivationThrottle(0); assert(!crtpCommanderConsumeActivationAlarmRequest());
    updateActivationThrottle(999); assert(!crtpCommanderConsumeActivationAlarmRequest());
    updateActivationThrottle(1000);
    assert(crtpCommanderConsumeActivationAlarmRequest()==(bool)ENABLE_ACTIVATION_MOTOR_LOCK);
    updateActivationThrottle(65535); assert(!crtpCommanderConsumeActivationAlarmRequest());
    mock_tick=100; updateActivationThrottle(0); updateActivationThrottle(65535);
    assert(!crtpCommanderConsumeActivationAlarmRequest());
    mock_tick=2000; updateActivationThrottle(65535);
    assert(crtpCommanderConsumeActivationAlarmRequest()==(bool)ENABLE_ACTIVATION_MOTOR_LOCK);
    mock_tick=2100; updateActivationThrottle(0); updateActivationThrottle(65535);
    updateActivationThrottle(0); mock_tick=4000;
    assert(!crtpCommanderConsumeActivationAlarmRequest());
    updateActivationThrottle(65535); mock_tick=4501;
    assert(!crtpCommanderConsumeActivationAlarmRequest()); /* disconnected */
    mock_tick=4600; updateActivationThrottle(65535);
    assert(crtpCommanderConsumeActivationAlarmRequest()==(bool)ENABLE_ACTIVATION_MOTOR_LOCK);
    altHoldMode=true; mock_tick=7000;
    updateActivationThrottle(ALT_HOLD_THRUST_CENTER);
    assert(!crtpCommanderConsumeActivationAlarmRequest());
    updateActivationThrottle(ALT_HOLD_THRUST_CENTER+ALT_HOLD_THRUST_DEADZONE);
    assert(!crtpCommanderConsumeActivationAlarmRequest());
    updateActivationThrottle(ALT_HOLD_THRUST_CENTER+ALT_HOLD_THRUST_DEADZONE+1);
    assert(crtpCommanderConsumeActivationAlarmRequest()==(bool)ENABLE_ACTIVATION_MOTOR_LOCK);
    delay_hook=observe_tone; play_activation_error(); delay_hook=NULL;
    assert(sounding_delays==(ENABLE_ACTIVATION_MOTOR_LOCK ? 3U : 0U));
    assert(silence_delays==(ENABLE_ACTIVATION_MOTOR_LOCK ? 2U : 0U));
    if (ENABLE_ACTIVATION_MOTOR_LOCK) assert(pwm_frequency==frequency_now);
    for (unsigned i=0; i<4; ++i) assert(duties[i]==0);
    unsigned before=sounding_delays;
    isInit=false; delay_hook=observe_tone; play_activation_error(); isInit=true;
    activated=true; play_activation_error(); delay_hook=NULL;
    assert(sounding_delays==before);
    mock_tick=10000; updateActivationThrottle(0); updateActivationThrottle(65535);
    assert(!crtpCommanderConsumeActivationAlarmRequest());
    activated=false; altHoldMode=false;
}
'''
TEST = TEST.replace("int main(void) {", GATES + "\nint main(void) {")

rid_path = "components/remote_id/remote_id.c"
rid_source = (ROOT / rid_path).read_text(encoding="utf-8")
RID = "\n".join(line for line in rid_source.splitlines() if line.startswith("#define "))
RID += r'''
#define portTICK_PERIOD_MS 1
struct timeval { long tv_sec; long tv_usec; };
static int gettimeofday(struct timeval *t, void *z) { (void)z; t->tv_sec=0; t->tv_usec=0; return 0; }
static uint8_t remoteIdGetOperationState(void) { return 1; }
'''
start = rid_source.index("typedef struct {")
end = rid_source.index("} rid_station_data_t;", start) + len("} rid_station_data_t;")
RID += rid_source[start:end] + "\nstatic rid_station_data_t station_data;\n"
for signature in ["static void put_le16(uint8_t *p, uint16_t v)",
                  "static void put_le32(uint8_t *p, int32_t v)",
                  "static void put_le48(uint8_t *p, uint64_t v)",
                  "static bool station_data_fresh(void)",
                  "static uint16_t altitude_from_cm(int32_t altitude_cm)",
                  "static uint16_t build_gb46750_packet(uint8_t *packet)"]:
    RID += function_source(rid_path, signature) + "\n"
RID += r'''
static void test_rid_fields(bool active) {
    uint8_t packet[100]; memset(packet,0xaa,sizeof(packet));
    assert(build_gb46750_packet(packet)==72);
    assert(!memcmp(packet+6,PRODUCT,20));
    const uint8_t empty[8]={0};
    assert(!memcmp(packet+26,active ? (const uint8_t *)"1234ABCD" : empty,8));
    assert(packet[72]==0xaa);
}
'''
TEST = TEST.replace("int main(void) {", RID + "\nint main(void) {")

with tempfile.TemporaryDirectory(prefix="tinydrone-activation-") as temp:
    folder = Path(temp)
    (folder / "stub.h").write_text(STUB, encoding="utf-8")
    for name in ["esp_err.h", "driver/usb_serial_jtag.h", "driver/usb_serial_jtag_vfs.h", "esp_log.h", "esp_mac.h",
                 "esp_system.h", "freertos/FreeRTOS.h", "freertos/task.h", "nvs.h", "sdkconfig.h"]:
        header = folder / name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('#include "stub.h"\n', encoding="utf-8")
    (folder / "test.c").write_text(TEST, encoding="utf-8")
    component = ROOT / "components/device_activation"
    binary = folder / "test.exe"
    for enabled, console in ((1, 1), (0, 1), (1, 0), (0, 0)):
        subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        f"-DENABLE_ACTIVATION_MOTOR_LOCK={enabled}", "-I", str(folder),
                        f"-DCONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED={console}",
                        "-I", str(component), "-I", str(component / "include"),
                        str(folder / "test.c"), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
