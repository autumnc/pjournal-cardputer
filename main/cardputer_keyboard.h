#pragma once

#include <cstdint>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── App-level key codes (must match pjournal_app.h) ──────────────────────
// 物理键盘扫描任务把 Cardputer 按键翻译成这些应用码,压入队列。
#define CPK_KEY_UP          0x80
#define CPK_KEY_DOWN        0x81
#define CPK_KEY_LEFT        0x82
#define CPK_KEY_RIGHT       0x83
#define CPK_KEY_IME_TOGGLE  0x84  // Ctrl+Space
#define CPK_KEY_CTRL_ENTER  0x85
#define CPK_KEY_SHIFT_UP    0x86
#define CPK_KEY_SHIFT_DOWN  0x87
#define CPK_KEY_SHIFT_LEFT  0x88
#define CPK_KEY_SHIFT_RIGHT 0x89
#define CPK_KEY_CTRL_I      0x8A
#define CPK_KEY_FULLWIDTH_TOGGLE 0x8B  // Shift+Space
#define CPK_KEY_TRAD_TOGGLE 0x8C  // Ctrl+Shift+F
#define CPK_KEY_LSHIFT_TAP  0x8D
#define CPK_KEY_CTRL_EQUALS 0x8E  // Ctrl+= 增加亮度
#define CPK_KEY_CTRL_MINUS  0x8F  // Ctrl+- 减少亮度
#define CPK_KEY_CTRL_UP     0x90  // Ctrl+;  向上选字
#define CPK_KEY_CTRL_DOWN   0x91  // Ctrl+.  向下选字
#define CPK_KEY_CTRL_LEFT   0x92  // Ctrl+,  向左选字
#define CPK_KEY_CTRL_RIGHT  0x93  // Ctrl+/  向右选字
#define CPK_KEY_QUICK_BASE  0x94  // Fn+0..9 → 0x94..0x9D(快捷编辑切换文件)

class CardputerKeyboard {
public:
    CardputerKeyboard() = default;

    esp_err_t init();      // 建队列、配 GPIO、起扫描任务(可重复调用)
    void deinit();         // 挂起扫描任务 + 清空队列(light sleep 前调用)
    void resume();         // light sleep 唤醒后恢复扫描任务

    uint8_t readKey();     // 取一个按键,无则返回 0
    void flushKeys();      // 清空队列

    void checkKeyRepeat(); // 空操作:连击由扫描任务产生(与 BtKeyboard 接口对齐)

    bool isConnected() const;  // 扫描任务存活
    bool isInitialized() const; // 与 isConnected 相同
};

extern CardputerKeyboard g_keyboard;

#ifdef __cplusplus
}
#endif
