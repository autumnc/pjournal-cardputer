#pragma once

#include <cstdint>
#include <ctime>

// 初代 Cardputer 无 RTC 芯片,用 NVS 记录基准 unix 时间 + RTC 定时器时刻,
// 实现一个跨重启、跨 light sleep 的软件 RTC。
class PCF85063 {
public:
    PCF85063();
    ~PCF85063();

    // 从 NVS 读取基准时间
    bool begin();

    // 以当前系统时间(或 NTP 同步结果)更新基准
    bool setTime(time_t unixTime);

    // 基准 + 自基准以来的流逝时间;无有效基准返回 0
    time_t getTime();

    // NVS 中是否存有有效基准
    bool hasValidTime();

private:
    bool _initialized;
};

extern PCF85063 g_rtc;
