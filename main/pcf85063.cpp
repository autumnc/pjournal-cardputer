#include "pcf85063.h"

#include <cstring>

#include "esp_log.h"
#include "esp_rtc_time.h"  // esp_rtc_get_time_us() (IDF v5.5 中 esp_rtc.h 已并入此头)
#include "nvs_flash.h"

static const char *TAG = "SoftRTC";
PCF85063 g_rtc;

// NVS 键
static const char *KEY_TIME = "rtc_time";  // i64: 基准 unix 秒
static const char *KEY_US   = "rtc_us";    // i64: 写入基准时的 esp_rtc_get_time_us()
static const char *KEY_VALID = "rtc_valid"; // u8: 基准是否有效

// esp_rtc_get_time_us() 由 RTC 慢时钟驱动,light sleep 期间持续走时,
// 是软件 RTC 的单调时间源。设备复位后从 0 重新计,因此用 NVS 里的
// 基准时刻作锚点,delta<0(复位/回绕)时退回基准本身。
static int64_t rtc_now_us(void)
{
    return esp_rtc_get_time_us();
}

PCF85063::PCF85063() : _initialized(false)
{
}

PCF85063::~PCF85063()
{
}

bool PCF85063::begin()
{
    if (_initialized) return true;

    nvs_handle_t h;
    if (nvs_open("softrtc", NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS open (read) failed");
        return false;
    }
    uint8_t valid = 0;
    esp_err_t r = nvs_get_u8(h, KEY_VALID, &valid);
    nvs_close(h);
    if (r != ESP_OK || valid != 1) {
        ESP_LOGW(TAG, "No valid RTC time stored");
        return false;
    }

    // 读锚点并校验一致性
    h = 0;
    if (nvs_open("softrtc", NVS_READONLY, &h) != ESP_OK) return false;
    int64_t t = 0, us = 0;
    esp_err_t rt = nvs_get_i64(h, KEY_TIME, &t);
    esp_err_t ru = nvs_get_i64(h, KEY_US, &us);
    nvs_close(h);
    if (rt != ESP_OK || ru != ESP_OK) {
        ESP_LOGW(TAG, "RTC anchor incomplete");
        return false;
    }

    _initialized = true;
    ESP_LOGI(TAG, "Soft RTC loaded: base=%lld (stored_us=%lld)", (long long)t, (long long)us);
    return true;
}

bool PCF85063::setTime(time_t unixTime)
{
    int64_t us = rtc_now_us();

    nvs_handle_t h;
    if (nvs_open("softrtc", NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "NVS open (write) failed");
        return false;
    }
    esp_err_t r1 = nvs_set_i64(h, KEY_TIME, (int64_t)unixTime);
    esp_err_t r2 = nvs_set_i64(h, KEY_US, us);
    esp_err_t r3 = nvs_set_u8(h, KEY_VALID, 1);
    esp_err_t rc = nvs_commit(h);
    nvs_close(h);

    if (r1 != ESP_OK || r2 != ESP_OK || r3 != ESP_OK || rc != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist soft RTC time");
        return false;
    }

    _initialized = true;
    ESP_LOGI(TAG, "Soft RTC time set: %lld (us=%lld)", (long long)unixTime, (long long)us);
    return true;
}

time_t PCF85063::getTime()
{
    if (!_initialized) return 0;

    nvs_handle_t h;
    if (nvs_open("softrtc", NVS_READONLY, &h) != ESP_OK) return 0;
    int64_t t = 0, us = 0;
    esp_err_t rt = nvs_get_i64(h, KEY_TIME, &t);
    esp_err_t ru = nvs_get_i64(h, KEY_US, &us);
    nvs_close(h);
    if (rt != ESP_OK || ru != ESP_OK) return 0;

    int64_t delta = rtc_now_us() - us;
    if (delta < 0) delta = 0;  // 复位后 RTC 计数器回零

    time_t now = (time_t)(t + delta / 1000000LL);
    return now;
}

bool PCF85063::hasValidTime()
{
    if (_initialized) return true;
    return begin();
}
