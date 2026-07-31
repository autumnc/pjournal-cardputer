#pragma once

#include <string>
#include <esp_err.h>

class WifiManager {
public:
    bool begin();
    bool connect(const char *ssid, const char *password);
    bool isConnected();
    void disconnect();
    std::string getIp();

private:
    bool _inited = false;
    bool _started = false;  // wifi radio started (station active)
};

extern WifiManager g_wifi;
