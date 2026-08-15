#include "user_config.h"
#include "font_renderer.h"
#include "cardputer_keyboard.h"
#include "wifi_manager.h"
#include "settings_manager.h"
#include "journal_storage.h"
#include "webdav_client.h"
#include "flomo_client.h"
#include "ime/IME.h"
#include "pjournal_app.h"
#include "screen_editor.h"
#include "screen_settings.h"
#include "screen_outline.h"
#include "screen_file_manager.h"
#include "u8g2_st7789.h"
#include "pcf85063.h"
#include "usb_drive.h"

#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_pm.h>
#include <esp_sleep.h>
#include <nvs_flash.h>
#include <esp_sntp.h>
#include <driver/gpio.h>
#include <sys/time.h>
#include <cstdio>

static const char *TAG = "Main";

// Display device (global, used by font_renderer.cpp)
static u8g2_st7789_t s_lcd_dev;
u8g2_t *g_u8g2 = nullptr;

static bool initDisplay() {
    ESP_LOGI(TAG, "Initializing display...");
    u8g2_st7789_config_t cfg = u8g2_st7789_default_config();
    cfg.rotation = U8G2_R0;  // 旋转由驱动 MADCTL=0x6C 完成,软件侧 R0
    cfg.tile_buf_height = U8G2_ST7789_TILE_BUF_FULL;

    esp_err_t ret = u8g2_st7789_init(&s_lcd_dev, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Display init failed: %d", ret);
        return false;
    }
    g_u8g2 = u8g2_st7789_get_u8g2(&s_lcd_dev);
    g_lcd_dev = &s_lcd_dev;
    return true;
}

// ── Light sleep 空闲休眠 ────────────────────────────────────────────────
// 键盘输入 ≥10 分钟无动作后进入 ESP light sleep(RAM 保留),空闲电流显著下降。
// 休眠期间键盘扫描任务挂起,只能按 BOOT 键(GPIO0)唤醒。
#define AUTO_SLEEP_TIMEOUT_US   (10 * 60 * 1000000LL)
#define AUTO_SLEEP_GRACE_US     (2 * 60 * 1000000LL)
#define SLEEP_WAKE_MASK         (1ULL << PIN_BOOT)

// 最近一次用户输入(键盘或物理按键)的时间,0 表示启动后尚未记录
static int64_t s_last_activity_us = 0;
// BOOT 唤醒后的首次短按释放不应再次触发休眠(唤醒按键与休眠按键是同一个键)
static bool s_boot_wake_release_pending = false;

// 物理按键时间判定(不依赖主循环节拍)
#define BTN_DEBOUNCE_US       (30000)     // 30ms 防抖
#define BTN_LONG_PRESS_US     (1000000)   // 1s 判定长按

// 按住 E 开机进入 USB 存储模式的检测窗口
#define BOOT_E_DETECT_MS      700

static void enterLightSleep(void) {
    // 休眠提示画在底部状态栏位置、居中,不遮挡/清空上方画面
    const char *hint = "休眠中 按BOOT键唤醒";
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawHLine(g_u8g2, 0, STATUS_Y, SCREEN_W);
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, 0, STATUS_Y + 1, SCREEN_W, FONT_H + 3);
    u8g2_SetDrawColor(g_u8g2, 0);
    g_font.drawText((SCREEN_W - g_font.textWidth(hint)) / 2,
                    STATUS_Y + 1 + g_font.ascent(), hint, false);
    u8g2_SetDrawColor(g_u8g2, 1);
    ui_commit();

    // 休眠屏保策略:保留画面(开)时让显示相关 GPIO 在休眠期间保持原状态,
    // 防止 GPIO 隔离工作区把面板 RST 浮空导致复位清空 GRAM;白屏(关)则恢复隔离(默认)。
    const gpio_num_t display_pins[] = {TFT_RST_PIN, TFT_CS_PIN, TFT_DC_PIN,
                                       TFT_SCLK_PIN, TFT_MOSI_PIN, TFT_BL_PIN};
    bool retain = g_settings.sleepScreen();
    for (size_t i = 0; i < sizeof(display_pins) / sizeof(display_pins[0]); i++) {
        if (retain) gpio_sleep_sel_dis(display_pins[i]);
        else        gpio_sleep_sel_en(display_pins[i]);
    }

    // 挂起键盘扫描任务,键盘 GPIO 不再变化
    g_keyboard.deinit();

    // BOOT 键(GPIO0)是 RTC GPIO、内部上拉、低电平有效,按下唤醒
    esp_err_t ext1_ret = esp_sleep_enable_ext1_wakeup(SLEEP_WAKE_MASK, ESP_EXT1_WAKEUP_ANY_LOW);
    if (ext1_ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_sleep_enable_ext1_wakeup failed: %d", ext1_ret);
    }

    ESP_LOGI(TAG, "Entering light sleep...");
    esp_err_t ret = esp_light_sleep_start();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_light_sleep_start failed: %d", ret);
    }
    ESP_LOGI(TAG, "Woke up, wakeup cause=%d", esp_sleep_get_wakeup_cause());
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1 &&
        (esp_sleep_get_ext1_wakeup_status() & (1ULL << PIN_BOOT))) {
        s_boot_wake_release_pending = true;  // 被 BOOT 唤醒,忽略它本次释放以免立即再睡
    }

    // 白屏模式下休眠时面板被隔离复位,必须重新初始化;保留画面模式下面板未复位,
    // 但统一重新初始化 + 强制整屏重绘也无害,保证画面回到当前 UI
    u8g2_InitDisplay(g_u8g2);
    u8g2_SetPowerSave(g_u8g2, 0);
    ui_invalidate_snapshot();
    // InitDisplay 会复位为 INVON(白底),dark mode 开启时需重新应用
    if (g_lcd_dev) {
        u8g2_st7789_set_dark_mode(g_lcd_dev, g_settings.getString("dark_mode") == "1");
    }

    // 软件时钟在休眠期间冻结,从持久 RTC 重同步,保证日记时间戳正确
    time_t t = g_rtc.getTime();
    if (t > 0) {
        struct timeval tv = {(time_t)t, 0};
        settimeofday(&tv, NULL);
    }

    // 恢复键盘扫描任务
    g_keyboard.resume();
}

static void checkLightSleep(AppState state) {
    (void)state;
    if (!g_settings.autoSleep()) return;
    if (g_wifi.isConnected()) return;  // 网络操作中不休眠

    static int64_t s_last_wake_us = 0;
    int64_t now = esp_timer_get_time();

    // 启动后首次调用:以当前时刻作为空闲计时起点
    if (s_last_activity_us == 0) {
        s_last_activity_us = now;
        return;
    }

    // 唤醒后 2 分钟 grace,覆盖用户操作
    if (s_last_wake_us != 0 && (now - s_last_wake_us) < AUTO_SLEEP_GRACE_US) return;

    // 键盘空闲超过 10 分钟 → 进入休眠
    if (now - s_last_activity_us >= AUTO_SLEEP_TIMEOUT_US) {
        enterLightSleep();
        s_last_wake_us = esp_timer_get_time();
        s_last_activity_us = s_last_wake_us;  // 重置空闲计时基准,避免唤醒后立即再睡
    }
}

// ── Application Main Loop ──────────────────────────────────────────────

extern "C" void app_main() {
    ESP_LOGI(TAG, "pjournal-cardputer v" PJOURNAL_VERSION " starting...");

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

    // Dynamic frequency scaling: CPU drops to 80MHz when idle (main loop delay),
    // ramps back to 240MHz while WiFi is active (holds PM locks).
    esp_pm_config_t pm_cfg = {
        .max_freq_mhz = 240,
        .min_freq_mhz = 80,
        .light_sleep_enable = false,
    };
    if (esp_pm_configure(&pm_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "esp_pm_configure failed");
    }

    // BOOT button (GPIO0) with pull-up for stable reading (also light-sleep wake)
    gpio_reset_pin(PIN_BOOT);
    gpio_set_direction(PIN_BOOT, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_BOOT, GPIO_PULLUP_ONLY);

    // 字体 + 显示必须先于 SD 挂载:USB 模式需要屏显,且该模式不挂 FAT
    g_font.begin();
    g_font.setSize(22);
    initDisplay();
    ui_clear();
    ui_commit();

    // 键盘(纯 GPIO 扫描,不占 SPI;先于 SD,用于检测按住 E)
    g_keyboard.init();

    // ── 按住 E 开机 → USB 存储模式 ─────────────────────────────────────
    // 检测窗口不显示任何提示,开机即黑屏直进应用(E 检测仍有效)
    {
        ui_clear();
        ui_commit();

        bool wantUsb = false;
        int64_t deadline = esp_timer_get_time() + BOOT_E_DETECT_MS * 1000LL;
        while (esp_timer_get_time() < deadline) {
            uint8_t k = g_keyboard.readKey();
            if (k == 'e' || k == 'E') { wantUsb = true; break; }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (wantUsb) {
            ESP_LOGI(TAG, "E held at boot, entering USB drive mode");
            u8g2_st7789_set_backlight(g_lcd_dev, 255);  // USB 模式状态屏需要背光(默认关闭)
            g_keyboard.deinit();
            if (usb_drive_begin()) {
                usb_drive_run();  // never returns
            }
            ESP_LOGE(TAG, "USB drive init failed, falling back to normal boot");
            g_keyboard.resume();  // deinit() 已挂起扫描任务,init() 因队列存在会直接返回
        }
    }

    // Initialize SD card (needed before settings on SD)
    if (!g_journal.begin()) {
        ESP_LOGE(TAG, "SD card initialization failed! System halted.");
        u8g2_st7789_set_backlight(g_lcd_dev, 255);  // 错误提示需要背光(默认关闭)
        ui_clear();
        ui_draw_text_centered(100, "SD卡初始化失败");
        ui_draw_text_centered(135, "请检查SD卡");
        ui_commit();
        while(1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }
    ESP_LOGI(TAG, "Journal entries: %d", g_journal.totalEntries());

    // Initialize settings (stored on SD card)
    g_settings.begin();
    // 快捷编辑模式跳过时间同步,开机以最短时间进入编辑器
    g_quickEdit = g_settings.quickEditMode();

    // 应用显示模式(黑底白字/白底黑字),以正确底色画出首帧后再开背光,避免开机白屏
    if (g_lcd_dev) {
        u8g2_st7789_set_dark_mode(g_lcd_dev, g_settings.getString("dark_mode") == "1");
    }
    ui_clear();
    ui_commit();
    u8g2_st7789_set_backlight(g_lcd_dev, 255);

    // 快捷编辑模式不需要时间,跳过 RTC 初始化与时间同步,最快进入编辑器
    if (!g_quickEdit) {
        // Initialize soft RTC
        if (g_rtc.begin()) {
            ESP_LOGI(TAG, "Soft RTC initialized");
        } else {
            ESP_LOGW(TAG, "Soft RTC not available or invalid time");
        }
    }

    // Initialize battery ADC
    battery_init();

    // 时间同步仅个人日记模式需要;快捷编辑跳过
    if (!g_quickEdit) {
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
    
                    ui_clear();
                    ui_draw_text_centered(90, "正在同步时间...");
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
    }  // !g_quickEdit: 个人日记模式时间同步结束

    // Seed RNG with hardware random for prompt selection
    srand(esp_random());

    // Initialize IME
    auto &ime = IME::getInstance();
    ime.begin();
    // Candidate page size based on the fixed 22pt font
    ime.setPageSize(7);
    // 候选字按显示宽度动态分页: 宽度回调复用当前字体, 可用宽度与候选行渲染的
    // curW+partW+8>SCREEN_W 截断阈值一致。
    ime.setWidthFn([](const char *s) -> int { return g_font.textWidth(s); });
    ime.setDisplayWidth(SCREEN_W - 8);

    // 清掉 E 检测窗口与挂载/同步期间积累的按键
    g_keyboard.flushKeys();

    ESP_LOGI(TAG, "Ready!");

    // ── App State Machine ────────────────────────────────────────────────
    // 编辑模式:个人日记(默认) / 快捷编辑(重启生效);g_quickEdit 已在设置加载后赋值
    g_quickFile = g_settings.quickFile();
    if (g_quickEdit) g_journal.ensureQuickFiles();  // 预创建 SD 根目录 0-9.txt

    AppState currentState;
    ScreenContext ctx;
    static AppState inspReturnTo = APP_MAIN;
    if (g_quickEdit) {
        // 快捷编辑:开机直接进编辑器,Esc ↔ 设置面板
        currentState = APP_EDITOR;
        ctx.promptMode = false;
        ctx.promptText = "";
        ctx.editContent = "";
        ctx.editFilename = "";
        ctx.prevState = APP_SETTINGS;
    } else {
        currentState = APP_MAIN;
        ctx.prevState = APP_MAIN;
    }

    // BOOT 键状态(时间制,不依赖主循环节拍): 长按(≥1s)松开进入休眠,单击无动作
    struct { int64_t press_start_us = 0; bool long_fired = false; } btn_boot;
    #define PIN_LOW(gpio) (gpio_get_level(gpio) == 0)

    while (currentState != APP_QUIT) {
        checkLightSleep(currentState);

        int key = g_keyboard.readKey();
        if (key < 0) key = 0;

        // ` 在非编辑界面充当 Esc 返回(编辑器/各输入子界面保留为字符)
        if (key == '`' && currentState != APP_EDITOR
            && !(currentState == APP_OUTLINE && outline_in_edit_mode())
            && !(currentState == APP_SETTINGS && settings_in_edit_mode())
            && !(currentState == APP_INSPIRATION && inspiration_in_edit_mode())) {
            key = 0x1b;
        }

        // 键盘输入视为活动,重置空闲休眠计时
        if (key > 0) s_last_activity_us = esp_timer_get_time();

        // Key repeat (no-op: 连击由扫描任务产生)
        g_keyboard.checkKeyRepeat();

        // Global Ctrl+Space IME toggle (only for editor)
        if (key == KEY_IME_TOGGLE && currentState == APP_EDITOR) {
            app_toggle_ime();
            key = 0;
        }
        // Shift+Space fullwidth toggle (only when IME active in editor)
        if (key == KEY_FULLWIDTH_TOGGLE && currentState == APP_EDITOR && app_ime_active()) {
            app_toggle_fullwidth();
            key = 0;
        }
        // Ctrl+Shift+F simplified/traditional toggle (only when IME active in editor)
        if (key == KEY_TRAD_TOGGLE && currentState == APP_EDITOR && app_ime_active()) {
            app_toggle_trad();
            key = 0;
        }
        // Left Shift tap → temp English mode toggle (only when IME active in editor)
        if (key == KEY_LSHIFT_TAP && currentState == APP_EDITOR && app_ime_active()) {
            app_toggle_english();
            key = 0;
        }

        // Global Ctrl+I → inspiration panel (works from any screen including editor)
        if (key == KEY_CTRL_I && currentState != APP_INSPIRATION) {
            inspReturnTo = currentState;
            currentState = APP_INSPIRATION;
            key = 0;
        }

        // ── BOOT button (GPIO0) ──────────────────────────────────────────
        {
            bool held = PIN_LOW(PIN_BOOT);
            if (held) {
                if (btn_boot.press_start_us == 0) {
                    btn_boot.press_start_us = esp_timer_get_time();
                    s_last_activity_us = btn_boot.press_start_us;
                }
                if (!btn_boot.long_fired &&
                    (esp_timer_get_time() - btn_boot.press_start_us) >= BTN_LONG_PRESS_US) {
                    btn_boot.long_fired = true;
                }
            } else {
                // 唤醒按键可能已在扫描间隙松开,直接清除挂起的唤醒保护标记
                if (s_boot_wake_release_pending && btn_boot.press_start_us == 0) {
                    s_boot_wake_release_pending = false;
                }
                if (btn_boot.press_start_us != 0) {
                    int64_t now = esp_timer_get_time();
                    int64_t dur = now - btn_boot.press_start_us;
                    btn_boot.press_start_us = 0;
                    if (dur >= BTN_DEBOUNCE_US && btn_boot.long_fired) {
                        if (s_boot_wake_release_pending) {
                            // 这是 BOOT 唤醒按键的释放,不进入休眠,避免唤醒即再睡
                            s_boot_wake_release_pending = false;
                        } else {
                            // 长按 BOOT → 进入休眠(单击不再休眠)
                            enterLightSleep();
                            key = 0;  // 丢弃休眠前遗留的按键
                            s_last_activity_us = esp_timer_get_time();
                        }
                    }
                    btn_boot.long_fired = false;
                }
            }
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
            if (app_editor_needs_reinit()) editorInited = false;
            if (!editorInited) { screen_editor_init(ctx); editorInited = true; }
            if (key > 0) currentState = screen_editor_handle(key, ctx);
            else { screen_editor_handle(0, ctx); vTaskDelay(pdMS_TO_TICKS(50)); }
            // Preserve editorInited when going to inspiration via Ctrl+I (editor should resume)
            if (currentState != APP_EDITOR && currentState != APP_SYNC_SEND_FLOMO) {
                if (currentState == APP_INSPIRATION) {
                    // Ctrl+I from editor → preserve editor state
                } else {
                    editorInited = false;
                }
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

        case APP_FILE_MANAGER: {
            g_font.setSize(22);
            static bool fileMgrInited = false;
            if (!fileMgrInited) { screen_file_manager_init(); fileMgrInited = true; }
            if (key > 0) currentState = screen_file_manager_handle(key, ctx);
            else { screen_file_manager_handle(0, ctx); vTaskDelay(pdMS_TO_TICKS(200)); }
            if (currentState != APP_FILE_MANAGER) fileMgrInited = false;
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
