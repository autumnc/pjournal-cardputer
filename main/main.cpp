#include "user_config.h"
#include "font_renderer.h"
#include "bt_keyboard.h"
#include "wifi_manager.h"
#include "settings_manager.h"
#include "journal_storage.h"
#include "webdav_client.h"
#include "flomo_client.h"
#include "ime/IME.h"
#include "pjournal_app.h"
#include "screen_editor.h"
#include "screen_settings.h"
#include "screen_gtd.h"
#include "screen_outline.h"
#include "screen_bt_manage.h"
#include "screen_file_manager.h"
#include "u8g2_st7305.h"
#include "pcf85063.h"

#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <nvs_flash.h>
#include <esp_sntp.h>
#include <driver/gpio.h>
#include <sys/time.h>
#include <cstdio>

static const char *TAG = "Main";

// Display device (global, used by font_renderer.cpp)
static u8g2_st7305_t s_lcd_dev;
void *g_u8g2 = nullptr;

static bool initDisplay() {
    ESP_LOGI(TAG, "Initializing display...");
    u8g2_st7305_config_t cfg = u8g2_st7305_default_config();
    cfg.mosi_io = RLCD_MOSI_PIN;
    cfg.sclk_io = RLCD_SCK_PIN;
    cfg.dc_io   = RLCD_DC_PIN;
    cfg.cs_io   = RLCD_CS_PIN;
    cfg.reset_io = RLCD_RST_PIN;
    cfg.rotation = U8G2_R1;
    cfg.tile_buf_height = U8G2_ST7305_TILE_BUF_FULL;

    esp_err_t ret = u8g2_st7305_init(&s_lcd_dev, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Display init failed: %d", ret);
        return false;
    }
    g_u8g2 = u8g2_st7305_get_u8g2(&s_lcd_dev);
    return true;
}

// ── Application Main Loop ──────────────────────────────────────────────

extern "C" void app_main() {
    ESP_LOGI(TAG, "pjournal-esp32 v" PJOURNAL_VERSION " starting...");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS...");
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed");
    }

    // Initialize buttons with pull-up for stable reading
    gpio_reset_pin(PIN_USER_BTN);
    gpio_set_direction(PIN_USER_BTN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_USER_BTN, GPIO_PULLUP_ONLY);
    gpio_reset_pin(PIN_BOOT);
    gpio_set_direction(PIN_BOOT, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_BOOT, GPIO_PULLUP_ONLY);

    // Initialize SD card (needed before settings on SD)
    if (!g_journal.begin()) {
        ESP_LOGE(TAG, "SD card initialization failed! System halted.");
        ui_clear();
        ui_draw_text_centered(100, "SD卡初始化失败");
        ui_draw_text_centered(135, "请检查SD卡");
        ui_commit();
        while(1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }
    ESP_LOGI(TAG, "Journal entries: %d", g_journal.totalEntries());

    // Initialize settings (stored on SD card)
    g_settings.begin();

    // Initialize RTC
    if (g_rtc.begin()) {
        ESP_LOGI(TAG, "PCF85063 RTC initialized");
    } else {
        ESP_LOGW(TAG, "PCF85063 RTC not available or invalid time");
    }

    // Initialize battery ADC
    battery_init();

    // Initialize font renderer (default 22pt for non-editor screens)
    g_font.begin();
    g_font.setSize(22);

    // Initialize display
    initDisplay();
    ui_clear();
    ui_draw_text_centered(100, "个人日记");
    char ver[32];
    snprintf(ver, sizeof(ver), "v" PJOURNAL_VERSION);
    ui_draw_text_centered(135, ver);
    ui_commit();

    // Initialize WiFi manager (但不自动连接)
    // WiFi 将在需要时按需连接（WebDAV同步、Flomo发送、Deepseek提示生成等）

    // Set timezone from settings (for local time display)
    {
        std::string tz = g_settings.timezone();
        if (tz.empty()) tz = "CST-8";
        setenv("TZ", tz.c_str(), 1);
        tzset();
    }

    // Time sync: prefer RTC if its time is recent (>= July 2026), otherwise NTP
    {
        time_t rtcTime = g_rtc.getTime();
        bool rtcRecent = (rtcTime >= 1782864000); // July 1, 2026 00:00:00 UTC

        if (rtcRecent) {
            struct timeval tv = {(time_t)rtcTime, 0};
            settimeofday(&tv, NULL);
            struct tm *tm = localtime(&rtcTime);
            char ts[32];
            strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);
            ESP_LOGI(TAG, "RTC time is recent, using directly: %s", ts);
        } else {
            ESP_LOGW(TAG, "RTC time (%lld) is before July 2026, attempting NTP sync...", (long long)rtcTime);
            std::string ssid = g_settings.wifiSsid();
            if (!ssid.empty()) {
                std::string ntp = g_settings.ntpServer();
                if (ntp.empty()) ntp = "pool.ntp.org";

                ui_draw_text_centered(165, "正在同步时间...");
                ui_commit();

                std::string pass = g_settings.wifiPassword();
                g_wifi.begin();
                if (g_wifi.connect(ssid.c_str(), pass.c_str())) {
                    vTaskDelay(pdMS_TO_TICKS(500));
                    esp_sntp_stop();
                    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
                    esp_sntp_setservername(0, ntp.c_str());
                    esp_sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
                    esp_sntp_init();

                    time_t now = 0;
                    for (int i = 0; i < 100; i++) {
                        vTaskDelay(pdMS_TO_TICKS(200));
                        if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
                            time(&now);
                            break;
                        }
                    }
                    if (now > 1782864000) {
                        struct tm *tm = localtime(&now);
                        char ts[32];
                        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);
                        ESP_LOGI(TAG, "NTP sync succeeded: %s", ts);
                        g_rtc.setTime(now);
                    } else {
                        ESP_LOGW(TAG, "NTP sync timeout (%s)", ntp.c_str());
                    }
                    esp_sntp_stop();
                } else {
                    ESP_LOGW(TAG, "WiFi connection failed for NTP sync");
                }
                g_wifi.disconnect();
            } else {
                ESP_LOGW(TAG, "WiFi not configured, cannot NTP sync");
            }
            // Fallback: use whatever RTC has, even if old
            if (rtcTime > 1704067200) {
                struct timeval tv = {(time_t)rtcTime, 0};
                settimeofday(&tv, NULL);
                ESP_LOGW(TAG, "Fallback to RTC time");
            }
        }
    }

    // Seed RNG with hardware random for prompt selection
    srand(esp_random());

    // Initialize IME
    auto &ime = IME::getInstance();
    ime.begin();
    // Set candidate page size based on default 22pt font
    ime.setPageSize(7);

    // Initialize Bluetooth keyboard
    ESP_LOGI(TAG, "Starting Bluetooth...");
    g_bt.init();

    // Auto-connect saved keyboard (background, non-blocking)
    {
        uint8_t saved_bda[6];
        esp_ble_addr_type_t saved_addr_type;
        if (g_bt.loadPairedDevice(saved_bda, saved_addr_type)) {
            ESP_LOGI(TAG, "Found saved keyboard, will auto-connect...");
            g_bt.connectBDA(saved_bda, saved_addr_type);
        }
    }

    ESP_LOGI(TAG, "Ready!");

    // ── App State Machine ────────────────────────────────────────────────
    // Always start at main screen; BT connects in background
    AppState currentState = APP_MAIN;
    ScreenContext ctx;
    static AppState inspReturnTo = APP_MAIN;

    // Button debounce counters
    struct { int count; bool fired_long; } btn_user = {}, btn_boot = {};

    while (currentState != APP_QUIT) {
        int key = g_bt.readKey();
        if (key < 0) key = 0;

        // Check for key repeat events
        g_bt.checkKeyRepeat();

        // Global Ctrl+Space IME toggle (only for editor)
        if (key == KEY_IME_TOGGLE && currentState == APP_EDITOR) {
            app_toggle_ime();
            key = 0;
        }

        // ── BT auto-reconnect retry ──────────────────────────────────────
        // 只在未连接且未正在连接时重试，间隔10秒
        // 一旦连接成功，停止重试
        {
            static int64_t last_bt_retry_us = 0;
            static int64_t last_bt_reload_us = 0;
            static bool bt_retry_loaded = false;
            static uint8_t bt_retry_bda[6];
            static esp_ble_addr_type_t bt_retry_addr_type;
            static bool bt_was_connected = false;

            if (g_bt.isConnected()) {
                if (!bt_was_connected) {
                    ESP_LOGI(TAG, "Bluetooth connected, stopping retry logic");
                }
                bt_was_connected = true;
                last_bt_retry_us = 0;
            } else {
                if (bt_was_connected) {
                    ESP_LOGW(TAG, "Bluetooth disconnected");
                    bt_was_connected = false;
                }

                if (!bt_was_connected) {
                    // Periodically re-check for paired device file
                    if (!bt_retry_loaded) {
                        int64_t now_us = esp_timer_get_time();
                        if (last_bt_reload_us == 0 || (now_us - last_bt_reload_us) > 30000000) {
                            last_bt_reload_us = now_us;
                            bt_retry_loaded = g_bt.loadPairedDevice(bt_retry_bda, bt_retry_addr_type);
                            if (bt_retry_loaded) {
                                ESP_LOGI(TAG, "BT paired device file found, will auto-reconnect");
                            }
                        }
                    }

                    if (bt_retry_loaded && !g_bt.isConnecting()) {
                        int64_t now_us = esp_timer_get_time();
                        if (last_bt_retry_us == 0 || (now_us - last_bt_retry_us) > 10000000) {
                            last_bt_retry_us = now_us;
                            ESP_LOGI(TAG, "BT auto-reconnect retry...");
                            g_bt.connectBDA(bt_retry_bda, bt_retry_addr_type);
                        }
                    }
                }
            }
        }

        // ── Physical button handling ──────────────────────────────────────
        // With pull-up: 1=released, 0=pressed (active LOW)
        // Simple counters, no auto-detection — just read the pin directly.
        #define PIN_LOW(gpio) (gpio_get_level(gpio) == 0)

        // USER button (GPIO 18)
        {
            static int64_t user_btn_last_release_us = 0;
            static bool user_btn_double = false;

            bool held = PIN_LOW(PIN_USER_BTN);
            if (held) {
                if (btn_user.count == 0) {
                    int64_t now = esp_timer_get_time();
                    if (user_btn_last_release_us > 0 &&
                        (now - user_btn_last_release_us) < 400000) {
                        user_btn_double = true;
                    }
                }
                btn_user.count++;
                if (btn_user.count == 3 && currentState == APP_BT_MANAGE) {
                    if (user_btn_double) {
                        key = 0x1B;
                        user_btn_double = false;
                        user_btn_last_release_us = 0;
                    } else {
                        key = KEY_UP;
                    }
                }
                if (btn_user.count == 14) {
                    if (currentState != APP_BT_MANAGE) {
                        currentState = APP_BT_MANAGE;
                        key = 0;
                    }
                }
            } else {
                if (btn_user.count >= 1 && btn_user.count < 14)
                    user_btn_last_release_us = esp_timer_get_time();
                btn_user.count = 0;
                user_btn_double = false;
            }
        }

        // BOOT button (GPIO 0)
        {
            bool held = PIN_LOW(PIN_BOOT);
            if (held) {
                btn_boot.count++;
                // Long press → confirm (in BT screen only)
                if (btn_boot.count == 14 && currentState == APP_BT_MANAGE) {
                    key = 0x0A;
                    btn_boot.fired_long = true;
                }
            } else {
                // Short press on release (only if didn't long-press)
                if (btn_boot.count >= 3 && btn_boot.count < 14 && !btn_boot.fired_long) {
                    if (currentState == APP_BT_MANAGE) key = KEY_DOWN;
                }
                btn_boot.count = 0;
                btn_boot.fired_long = false;
            }
        }

        // Global Ctrl+I → inspiration panel (works from any screen including editor)
        if (key == KEY_CTRL_I && currentState != APP_INSPIRATION) {
            inspReturnTo = currentState;
            currentState = APP_INSPIRATION;
            key = 0;
        }

        switch (currentState) {
        case APP_MAIN:
            g_font.setSize(22);
            if (key > 0) currentState = screen_main_handle(key, ctx);
            else { screen_main_handle(0, ctx); vTaskDelay(pdMS_TO_TICKS(200)); }  // 200ms for power saving
            break;

        case APP_EDITOR: {
            g_font.setSize(g_settings.fontSize());
            {
                int fs = g_font.fontSize();
                IME::getInstance().setPageSize(fs <= 22 ? 7 : 5);
            }
            static bool editorInited = false;
            if (!editorInited) { screen_editor_init(ctx); editorInited = true; }
            if (key > 0) currentState = screen_editor_handle(key, ctx);
            else { screen_editor_handle(0, ctx); vTaskDelay(pdMS_TO_TICKS(50)); }
            // Preserve editorInited when going to inspiration via Ctrl+I (editor should resume)
            // Reset editorInited when editor is opened FROM another screen (new content)
            if (currentState != APP_EDITOR && currentState != APP_SYNC_SEND_FLOMO) {
                if (currentState == APP_INSPIRATION) {
                    // Ctrl+I from editor → preserve editor state
                } else {
                    editorInited = false;
                }
            }
            // If another screen opened the editor, must re-init with new content
            if (currentState == APP_EDITOR && ctx.prevState != APP_MAIN && ctx.prevState != APP_EDITOR) {
                editorInited = false;
            }
            break;
        }

        case APP_BROWSER: {
            g_font.setSize(22);
            static bool browserInited = false;
            if (!browserInited) { screen_browser_init(); browserInited = true; }
            if (key > 0) currentState = screen_browser_handle(key, ctx);
            else { screen_browser_handle(0, ctx); vTaskDelay(pdMS_TO_TICKS(100)); }
            if (currentState != APP_BROWSER) browserInited = false;
            break;
        }

        case APP_VIEWER: {
            g_font.setSize(22);
            static bool viewerInited = false;
            if (!viewerInited) { screen_viewer_init(ctx.selectedEntry); viewerInited = true; }
            if (key > 0) currentState = screen_viewer_handle(key, ctx);
            else { screen_viewer_handle(0, ctx); vTaskDelay(pdMS_TO_TICKS(100)); }
            if (currentState != APP_VIEWER) viewerInited = false;
            break;
        }

        case APP_SETTINGS: {
            g_font.setSize(22);
            IME::getInstance().setPageSize(7);
            static bool settingsInited = false;
            if (!settingsInited) { screen_settings_init(); settingsInited = true; }
            if (key > 0) currentState = screen_settings_handle(key, ctx);
            else { screen_settings_handle(0, ctx); vTaskDelay(pdMS_TO_TICKS(100)); }
            if (currentState != APP_SETTINGS) settingsInited = false;
            break;
        }

        case APP_BT_MANAGE: {
            g_font.setSize(22);
            static bool btInited = false;
            if (!btInited) { screen_bt_manage_init(); btInited = true; }
            if (key > 0) currentState = screen_bt_manage_handle(key, ctx);
            else { screen_bt_manage_handle(0, ctx); vTaskDelay(pdMS_TO_TICKS(100)); }
            if (currentState != APP_BT_MANAGE) btInited = false;
            break;
        }

        case APP_FILE_MANAGER: {
            g_font.setSize(22);
            static bool fileMgrInited = false;
            if (!fileMgrInited) { screen_file_manager_init(); fileMgrInited = true; }
            if (key > 0) currentState = screen_file_manager_handle(key, ctx);
            else { screen_file_manager_handle(0, ctx); vTaskDelay(pdMS_TO_TICKS(200)); }
            if (currentState != APP_FILE_MANAGER) fileMgrInited = false;
            break;
        }

        case APP_GTD: {
            g_font.setSize(22);
            IME::getInstance().setPageSize(7);
            static bool gtdInited = false;
            if (!gtdInited) { screen_gtd_init(); gtdInited = true; }
            if (key > 0) currentState = screen_gtd_handle(key, ctx);
            else { screen_gtd_handle(0, ctx); vTaskDelay(pdMS_TO_TICKS(100)); }
            if (currentState != APP_GTD) gtdInited = false;
            break;
        }

        case APP_OUTLINE: {
            g_font.setSize(22);
            IME::getInstance().setPageSize(7);
            static bool outlineInited = false;
            if (!outlineInited) { screen_outline_init(); outlineInited = true; }
            if (key > 0) currentState = screen_outline_handle(key, ctx);
            else { screen_outline_handle(0, ctx); vTaskDelay(pdMS_TO_TICKS(100)); }
            if (currentState != APP_OUTLINE) outlineInited = false;
            break;
        }

        case APP_INSPIRATION: {
            g_font.setSize(22);
            IME::getInstance().setPageSize(7);
            static bool inspInited = false;
            if (!inspInited) {
                screen_inspiration_init(inspReturnTo);
                inspInited = true;
            }
            if (key > 0) currentState = screen_inspiration_handle(key, ctx);
            else { screen_inspiration_handle(0, ctx); vTaskDelay(pdMS_TO_TICKS(100)); }
            if (currentState != APP_INSPIRATION) inspInited = false;
            break;
        }

        case APP_SYNC_WEBDAV: {
            g_font.setSize(22);
            IME::getInstance().setPageSize(7);

            // Show sync panel immediately
            ui_clear();
            ui_draw_text_centered(100, "WebDAV 同步", false, true);
            ui_draw_text_centered(135, "正在连接WiFi...");
            ui_commit();

            // Auto-connect WiFi if needed
            bool wifiWasConnected = g_wifi.isConnected();
            if (!wifiWasConnected) {
                std::string ssid = g_settings.wifiSsid();
                std::string pass = g_settings.wifiPassword();
                if (!ssid.empty()) {
                    g_wifi.begin();
                    g_wifi.connect(ssid.c_str(), pass.c_str());
                    for (int i = 0; i < 100; i++) {
                        if (g_wifi.isConnected()) break;
                        vTaskDelay(pdMS_TO_TICKS(100));
                    }
                }
            }

            if (!g_wifi.isConnected()) {
                ui_clear();
                ui_draw_text_centered(100, "WebDAV 同步", false, true);
                ui_draw_text_centered(135, "WiFi连接失败");
                ui_commit();
                vTaskDelay(pdMS_TO_TICKS(2000));
                currentState = APP_MAIN;
                break;
            }

            ui_clear();
            ui_draw_text_centered(100, "WebDAV 同步", false, true);
            ui_draw_text_centered(135, "正在同步...");
            ui_commit();

            std::string url = g_settings.webdavUrl();
            std::string user = g_settings.webdavUsername();
            std::string pass = g_settings.webdavPassword();
            if (url.empty() || user.empty()) {
                ui_clear();
                ui_draw_text_centered(100, "WebDAV 同步", false, true);
                ui_draw_text_centered(135, "请先配置WebDAV");
                ui_commit();
                vTaskDelay(pdMS_TO_TICKS(2000));
            } else {
                g_webdav.configure(url, user, pass);
                auto result = g_webdav.sync("/sdcard/pjournal");
                ui_clear();
                ui_draw_text_centered(100, "WebDAV 同步", false, true);
                ui_draw_text_centered(135, result.message.c_str());
                ui_commit();
                vTaskDelay(pdMS_TO_TICKS(2000));
            }

            // Always disconnect WiFi after sync to save power
            g_wifi.disconnect();
            currentState = APP_MAIN;
            break;
        }

        case APP_SYNC_SEND_FLOMO: {
            static int flomoStep = 0;
            static std::string flomoText;
            static std::string flomoResult;
            static AppState flomoReturnTo = APP_EDITOR;
            if (flomoStep == 0) {
                if (!g_flomoPendingText.empty()) {
                    flomoText = std::move(g_flomoPendingText);
                    g_flomoPendingText.clear();
                    flomoReturnTo = g_flomoReturnTo;
                } else {
                    flomoText = app_get_editor_text();
                    flomoReturnTo = APP_EDITOR;
                }
                flomoResult.clear();
                if (flomoText.empty()) {
                    ui_clear(); ui_show_message_centered("内容为空");
                    ui_commit(); vTaskDelay(pdMS_TO_TICKS(1500));
                    g_wifi.disconnect();
                    flomoStep = 0;
                    currentState = flomoReturnTo;
                    break;
                }
                flomoStep = 1;
            }
            if (flomoStep == 1) {
                ui_clear(); ui_show_message_centered("正在连接WiFi...");
                ui_commit();
                if (!g_wifi.isConnected()) {
                    std::string ssid = g_settings.wifiSsid();
                    std::string pass = g_settings.wifiPassword();
                    if (!ssid.empty()) {
                        g_wifi.begin();
                        g_wifi.connect(ssid.c_str(), pass.c_str());
                        for (int i = 0; i < 100; i++) {
                            if (g_wifi.isConnected()) break;
                            vTaskDelay(pdMS_TO_TICKS(100));
                        }
                    }
                }
                if (!g_wifi.isConnected()) {
                    ui_clear(); ui_show_message_centered("WiFi未连接");
                    ui_commit(); vTaskDelay(pdMS_TO_TICKS(1500));
                    g_wifi.disconnect();
                    flomoStep = 0;
                    currentState = flomoReturnTo;
                    break;
                }
                flomoStep = 2;
            }
            if (flomoStep == 2) {
                ui_clear(); ui_show_message_centered("正在发送...");
                ui_commit();
                auto result = g_flomo.send(flomoText);
                flomoResult = result.message;
                ui_clear(); ui_show_message_centered(flomoResult.c_str());
                ui_commit(); vTaskDelay(pdMS_TO_TICKS(2000));
                g_wifi.disconnect();
                flomoStep = 0;
                currentState = flomoReturnTo;
            }
            break;
        }

        default:
            currentState = APP_MAIN;
            break;
        }

        if (!ctx.statusMessage.empty()) {
            ui_clear();
            ui_show_message_centered(ctx.statusMessage.c_str());
            ctx.statusMessage.clear();
            vTaskDelay(pdMS_TO_TICKS(1500));
        }
    }

    ESP_LOGI(TAG, "Goodbye.");
}
