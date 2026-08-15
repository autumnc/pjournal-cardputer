#include "cardputer_keyboard.h"

#include <cctype>
#include <cstring>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "CardputerKb";

#define SCAN_INTERVAL_MS  5
#define KEY_ROWS          4
#define KEY_COLS          14
#define MAX_HELD          6
#define REPEAT_DELAY_MS   500
#define REPEAT_INTERVAL_MS 50

// ── Raw Cardputer key codes (from M5Cardputer Keyboard_def.h) ────────────
#define RAW_LEFT_CTRL  0x80
#define RAW_LEFT_SHIFT 0x81
#define RAW_LEFT_ALT   0x82
#define RAW_FN         0xff
#define RAW_OPT        0x00
#define RAW_F1         0x3a
#define RAW_F12        0x45
#define RAW_RIGHT      0x4f
#define RAW_LEFT       0x50
#define RAW_DOWN       0x51
#define RAW_UP         0x52
#define RAW_BACKSPACE  0x2a
#define RAW_TAB        0x2b
#define RAW_ENTER      0x28
#define RAW_ESCAPE     0x29
#define RAW_DELETE     0x4c
#define RAW_NONE       0x00

// ── TCA8418(ADV 键盘控制器)寄存器 ─────────────────────────────────
// 地址 0x34,SYS I2C = SCL GPIO9 / SDA GPIO8。寄存器布局与 M5Cardputer
// 移植版 Adafruit_TCA8418 一致。
#define TCA_I2C_ADDR        0x34
#define TCA_REG_CFG          0x01
#define TCA_REG_INT_STAT     0x02
#define TCA_REG_KEY_LCK_EC   0x03
#define TCA_REG_KEY_EVENT_A  0x04
#define TCA_REG_GPIO_INT_STAT_1 0x11
#define TCA_REG_GPIO_INT_EN_1 0x1A
#define TCA_REG_GPIO_INT_EN_2 0x1B
#define TCA_REG_GPIO_INT_EN_3 0x1C
#define TCA_REG_KP_GPIO_1    0x1D
#define TCA_REG_KP_GPIO_2    0x1E
#define TCA_REG_GPI_EM_1     0x20
#define TCA_REG_GPI_EM_2     0x21
#define TCA_REG_GPI_EM_3     0x22
#define TCA_REG_GPIO_DIR_1   0x23
#define TCA_REG_GPIO_DIR_2   0x24
#define TCA_REG_GPIO_DIR_3   0x25
#define TCA_REG_GPIO_INT_LVL_1 0x26
#define TCA_REG_GPIO_INT_LVL_2 0x27
#define TCA_REG_GPIO_INT_LVL_3 0x28
#define TCA_CFG_GPI_IEN      0x02
#define TCA_CFG_KE_IEN       0x01

struct KeyValue {
    uint8_t first;
    uint8_t second;
    uint8_t third;
};

// Same physical layout as M5Cardputer _key_value_map[4][14]:
// row = y (0 top..3 bottom), col = x (0 left..13 right).
static const KeyValue s_keymap[KEY_ROWS][KEY_COLS] = {
    {{'`','~',RAW_ESCAPE}, {'1','!',RAW_F1}, {'2','@',(uint8_t)(RAW_F1+1)},
     {'3','#',(uint8_t)(RAW_F1+2)}, {'4','$',(uint8_t)(RAW_F1+3)}, {'5','%',(uint8_t)(RAW_F1+4)},
     {'6','^',(uint8_t)(RAW_F1+5)}, {'7','&',(uint8_t)(RAW_F1+6)}, {'8','*',(uint8_t)(RAW_F1+7)},
     {'9','(',(uint8_t)(RAW_F1+8)}, {'0',')',(uint8_t)(RAW_F1+9)}, {'-','_',(uint8_t)(RAW_F1+10)},
     {'=','+',RAW_F12}, {RAW_BACKSPACE,RAW_BACKSPACE,RAW_DELETE}},
    {{RAW_TAB,RAW_TAB,RAW_NONE}, {'q','Q',RAW_NONE}, {'w','W',RAW_NONE}, {'e','E',RAW_NONE},
     {'r','R',RAW_NONE}, {'t','T',RAW_NONE}, {'y','Y',RAW_NONE}, {'u','U',RAW_NONE},
     {'i','I',RAW_NONE}, {'o','O',RAW_NONE}, {'p','P',RAW_NONE}, {'[','{',RAW_NONE},
     {']','}',RAW_NONE}, {'\\','|',RAW_NONE}},
    {{RAW_FN,RAW_FN,RAW_FN}, {RAW_LEFT_SHIFT,RAW_LEFT_SHIFT,RAW_NONE},
     {'a','A',RAW_NONE}, {'s','S',RAW_NONE}, {'d','D',RAW_NONE}, {'f','F',RAW_NONE},
     {'g','G',RAW_NONE}, {'h','H',RAW_NONE}, {'j','J',RAW_NONE}, {'k','K',RAW_NONE},
     {'l','L',RAW_NONE}, {';',':',RAW_UP}, {'\'','\"',RAW_NONE}, {RAW_ENTER,RAW_ENTER,RAW_NONE}},
    {{RAW_LEFT_CTRL,RAW_LEFT_CTRL,RAW_NONE}, {RAW_OPT,RAW_OPT,RAW_NONE},
     {RAW_LEFT_ALT,RAW_LEFT_ALT,RAW_NONE}, {'z','Z',RAW_NONE}, {'x','X',RAW_NONE},
     {'c','C',RAW_NONE}, {'v','V',RAW_NONE}, {'b','B',RAW_NONE}, {'n','N',RAW_NONE},
     {'m','M',RAW_NONE}, {',','<',RAW_LEFT}, {'.','>',RAW_DOWN}, {'/','?',RAW_RIGHT},
     {' ',' ',RAW_NONE}},
};

// ── I/O matrix (from M5Cardputer IOMatrix.cpp) ───────────────────────────
static const gpio_num_t s_output_pins[3] = {GPIO_NUM_8, GPIO_NUM_9, GPIO_NUM_11};
static const gpio_num_t s_input_pins[7] = {GPIO_NUM_13, GPIO_NUM_15, GPIO_NUM_3,
                                           GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_6, GPIO_NUM_7};

static const struct {
    uint8_t value;
    uint8_t x_1;  // even columns (output 4..7)
    uint8_t x_2;  // odd columns (output 0..3)
} s_xmap[7] = {
    {1, 0, 1}, {2, 2, 3}, {4, 4, 5}, {8, 6, 7},
    {16, 8, 9}, {32, 10, 11}, {64, 12, 13},
};

static void set_output(uint8_t out)
{
    gpio_set_level(s_output_pins[0], out & 0x01);
    gpio_set_level(s_output_pins[1], (out >> 1) & 0x01);
    gpio_set_level(s_output_pins[2], (out >> 2) & 0x01);
}

static uint8_t get_input(void)
{
    uint8_t v = 0;
    for (int i = 0; i < 7; i++) {
        if (gpio_get_level(s_input_pins[i]) == 0) v |= (1u << i);
    }
    return v;
}

static void scan_raw(uint8_t raw[KEY_ROWS][KEY_COLS])
{
    for (int o = 0; o < 8; o++) {
        set_output(o);
        esp_rom_delay_us(10);  // 让输出稳定后再读输入
        uint8_t in = get_input();
        if (in == 0) continue;
        int y = 3 - (o % 4);
        for (int j = 0; j < 7; j++) {
            if (in & (1u << j)) {
                int x = (o > 3) ? s_xmap[j].x_1 : s_xmap[j].x_2;
                raw[y][x] = 1;
            }
        }
    }
}

// ── TCA8418 低层 I2C ─────────────────────────────────────────────────────
static i2c_master_bus_handle_t s_tca_bus = nullptr;
static i2c_master_dev_handle_t s_tca_dev = nullptr;

static esp_err_t tca_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_tca_dev, buf, 2, 20);
}

static esp_err_t tca_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_tca_dev, &reg, 1, val, 1, 20);
}

// 排空键盘事件 FIFO 并清中断状态(空 FIFO 读 KEY_EVENT_A 返回 0)
static void tca_flush(void)
{
    for (int i = 0; i < 16; i++) {
        uint8_t ev = 0;
        if (tca_read(TCA_REG_KEY_EVENT_A, &ev) != ESP_OK) break;
        if (ev == 0) break;
    }
    uint8_t s = 0;
    tca_read(TCA_REG_GPIO_INT_STAT_1, &s);
    tca_read(TCA_REG_GPIO_INT_STAT_1 + 1, &s);
    tca_read(TCA_REG_GPIO_INT_STAT_1 + 2, &s);
    tca_write(TCA_REG_INT_STAT, 0x03);
}

// 释放 SYS I2C 总线(回退到 IOMatrix 时调用)
static void tca_cleanup(void)
{
    if (s_tca_dev) {
        i2c_master_bus_rm_device(s_tca_dev);
        s_tca_dev = nullptr;
    }
    if (s_tca_bus) {
        i2c_del_master_bus(s_tca_bus);
        s_tca_bus = nullptr;
    }
}

// 初始化 SYS I2C 并配置 TCA8418。流程复刻官方 TCA8418KeyboardReader:
//   Adafruit begin()(全部引脚输入/事件模式/下降沿/中断使能)
//   → matrix(7,8)(KP_GPIO_1=0x7F, KP_GPIO_2=0xFF)
//   → flush()(清 FIFO + INT_STAT)
// 键盘事件始终进入 FIFO,不依赖中断使能,故 CFG 保持 0(顺带清掉旧固件残留的 KE_IEN,
// 避免 INT 引脚 GPIO11 被持续拉低)。
static bool tca8418_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = GPIO_NUM_8,
        .scl_io_num = GPIO_NUM_9,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        // trans_queue_depth=0 走纯同步路径:不建 ops 队列,不存在
        // "ops list is full" 丢写的问题,也不要求 probe 兼容。
        .trans_queue_depth = 0,
        .flags = { .enable_internal_pullup = true },
    };
    if (i2c_new_master_bus(&bus_cfg, &s_tca_bus) != ESP_OK) {
        ESP_LOGE(TAG, "TCA: i2c_new_master_bus failed (SDA=8 SCL=9)");
        return false;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TCA_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(s_tca_bus, &dev_cfg, &s_tca_dev) != ESP_OK) {
        ESP_LOGE(TAG, "TCA: i2c_master_bus_add_device failed");
        tca_cleanup();
        return false;
    }
    // 不用 i2c_master_probe:它走独立的硬编码 100kHz/20ms 超时路径,
    // 在 SYS I2C 上不可靠。直接读 CFG 寄存器即可确认芯片在位(实测 ESP_OK)。
    uint8_t v = 0;
    esp_err_t rc = tca_read(TCA_REG_CFG, &v);
    ESP_LOGW(TAG, "TCA: read CFG rc=%s val=0x%02x", esp_err_to_name(rc), v);
    if (rc != ESP_OK) {
        tca_cleanup();
        return false;
    }

    // 初始化写寄存器,失败即上报,不再静默丢弃
    struct { uint8_t reg; uint8_t val; } wr[] = {
        // Adafruit begin():全部引脚设为输入、事件模式、下降沿中断
        { TCA_REG_GPIO_DIR_1,    0x00 },
        { TCA_REG_GPIO_DIR_2,    0x00 },
        { TCA_REG_GPIO_DIR_3,    0x00 },
        { TCA_REG_GPI_EM_1,      0xFF },
        { TCA_REG_GPI_EM_2,      0xFF },
        { TCA_REG_GPI_EM_3,      0xFF },
        { TCA_REG_GPIO_INT_LVL_1, 0x00 },
        { TCA_REG_GPIO_INT_LVL_2, 0x00 },
        { TCA_REG_GPIO_INT_LVL_3, 0x00 },
        { TCA_REG_GPIO_INT_EN_1,  0xFF },
        { TCA_REG_GPIO_INT_EN_2,  0xFF },
        { TCA_REG_GPIO_INT_EN_3,  0xFF },
        // matrix(7,8):7 行 × 8 列
        { TCA_REG_KP_GPIO_1,      0x7F },
        { TCA_REG_KP_GPIO_2,      0xFF },
        { TCA_REG_CFG,            0x00 },
    };
    for (size_t i = 0; i < sizeof(wr) / sizeof(wr[0]); i++) {
        esp_err_t e = tca_write(wr[i].reg, wr[i].val);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "TCA: init write reg=0x%02x val=0x%02x -> %s",
                     wr[i].reg, wr[i].val, esp_err_to_name(e));
            tca_cleanup();
            return false;
        }
    }
    tca_flush();
    ESP_LOGW(TAG, "TCA: init done");
    return true;
}

// ── State ────────────────────────────────────────────────────────────────
enum KbdMode { KBD_IOMATRIX, KBD_TCA8418 };
static KbdMode s_mode = KBD_IOMATRIX;

static QueueHandle_t s_queue = nullptr;
static TaskHandle_t s_scan_task = nullptr;
static bool s_connected = false;

static uint8_t s_raw[KEY_ROWS][KEY_COLS];
static uint8_t s_prev[KEY_ROWS][KEY_COLS];
static uint8_t s_stable[KEY_ROWS][KEY_COLS];

// 本次扫描中发生按下/释放边沿的键,先收齐再统一处理,保证修饰键状态完整
static uint8_t s_press_edges[KEY_ROWS * KEY_COLS];
static uint8_t s_release_edges[KEY_ROWS * KEY_COLS];
static int s_press_n = 0;
static int s_release_n = 0;

// TCA8418 模式:单次轮询内收齐的按下/释放事件(TCA8418 FIFO 最深 10 条)
static uint8_t s_tca_press[10];
static uint8_t s_tca_release[10];
static int s_tca_press_n = 0;
static int s_tca_release_n = 0;

struct HeldKey {
    uint8_t key_id;   // (y<<4)|x, 0 表示空闲
    uint8_t app_code; // 按下时刻生成的应用码(连击重发)
    int64_t press_us;
    int64_t last_repeat_us;
};
static HeldKey s_held[MAX_HELD];

static bool s_shift_tap_armed = false;

static int64_t now_us(void)
{
    return (int64_t)esp_timer_get_time();
}

static uint8_t key_id(uint8_t x, uint8_t y) { return (uint8_t)((y << 4) | x); }

static bool is_modifier(uint8_t raw)
{
    return raw == RAW_FN || raw == RAW_LEFT_SHIFT || raw == RAW_LEFT_CTRL ||
           raw == RAW_LEFT_ALT || raw == RAW_OPT;
}

static void push_key(uint8_t code)
{
    if (code != 0) {
        xQueueSendToBack(s_queue, &code, 0);
    }
}

// 把 Cardputer 原始码翻译成应用码。shift 用于箭头 → 带 Shift 的箭头变体。
static uint8_t raw_to_app(uint8_t raw, bool shift)
{
    switch (raw) {
    case RAW_BACKSPACE: return 0x08;
    case RAW_TAB:       return 0x09;
    case RAW_ENTER:     return 0x0a;
    case RAW_ESCAPE:    return 0x1b;
    case RAW_DELETE:    return 0x7f;  // 编辑器把 0x7f 当退格处理
    case RAW_UP:        return shift ? CPK_KEY_SHIFT_UP   : CPK_KEY_UP;
    case RAW_LEFT:      return shift ? CPK_KEY_SHIFT_LEFT : CPK_KEY_LEFT;
    case RAW_DOWN:      return shift ? CPK_KEY_SHIFT_DOWN : CPK_KEY_DOWN;
    case RAW_RIGHT:     return shift ? CPK_KEY_SHIFT_RIGHT : CPK_KEY_RIGHT;
    default:
        break;
    }
    // Fn+1..Fn+0 → 快捷编辑文件 1..9,0 (0x94..0x9D)
    if (raw >= RAW_F1 && raw <= RAW_F1 + 9)
        return (uint8_t)(CPK_KEY_QUICK_BASE + ((raw - RAW_F1 + 1) % 10));
    if (raw >= RAW_F1 && raw <= RAW_F12) return 0;  // 应用无 F 键功能
    return raw;  // ASCII
}

static bool repeatable(uint8_t code)
{
    if (code == 0) return false;
    switch (code) {
    case CPK_KEY_IME_TOGGLE:
    case CPK_KEY_CTRL_I:
    case CPK_KEY_FULLWIDTH_TOGGLE:
    case CPK_KEY_TRAD_TOGGLE:
    case CPK_KEY_LSHIFT_TAP:
        return false;
    default:
        break;
    }
    if (code >= CPK_KEY_QUICK_BASE && code <= CPK_KEY_QUICK_BASE + 9) return false;  // 文件切换不连发
    return true;
}

static void hold_add(uint8_t x, uint8_t y, uint8_t code)
{
    if (!repeatable(code)) return;
    uint8_t id = key_id(x, y);
    for (int i = 0; i < MAX_HELD; i++) {
        if (s_held[i].key_id == 0) {
            s_held[i].key_id = id;
            s_held[i].app_code = code;
            s_held[i].press_us = now_us();
            s_held[i].last_repeat_us = 0;
            return;
        }
    }
}

static void hold_remove(uint8_t x, uint8_t y)
{
    uint8_t id = key_id(x, y);
    for (int i = 0; i < MAX_HELD; i++) {
        if (s_held[i].key_id == id) {
            s_held[i].key_id = 0;
            return;
        }
    }
}

// 生成一次按下对应的事件;返回是否需要压入连击跟踪。
static void process_press(uint8_t x, uint8_t y)
{
    const KeyValue kv = s_keymap[y][x];
    uint8_t raw = kv.first;

    if (raw == RAW_LEFT_SHIFT) {
        s_shift_tap_armed = true;
        return;
    }
    if (is_modifier(raw)) {
        return;  // fn/ctrl/alt/opt 只作修饰键
    }
    s_shift_tap_armed = false;  // 任意非修饰键按下取消 Shift 单击

    // 修饰键状态取自本扫描稳定后的矩阵
    bool fn    = s_stable[2][0] != 0;
    bool shift = s_stable[2][1] != 0;
    bool ctrl  = s_stable[3][0] != 0;

    if (fn && kv.third != RAW_NONE) {
        uint8_t code = raw_to_app(kv.third, shift);
        push_key(code);
        hold_add(x, y, code);
        return;
    }
    if (fn) {
        return;  // Fn 层死键(与 M5 PASS2 一致:非 fn 键在 fn 模式下不输出)
    }

    // 非 fn 层
    if (raw == RAW_BACKSPACE || raw == RAW_TAB) {
        uint8_t code = raw_to_app(raw, false);
        push_key(code);
        hold_add(x, y, code);
        return;
    }
    if (raw == RAW_ENTER) {
        uint8_t code = ctrl ? CPK_KEY_CTRL_ENTER : 0x0a;
        push_key(code);
        hold_add(x, y, code);
        return;
    }
    if (raw == ' ') {
        if (ctrl) {
            push_key(CPK_KEY_IME_TOGGLE);  // Ctrl+Space
            return;
        }
        if (shift) {
            push_key(CPK_KEY_FULLWIDTH_TOGGLE);  // Shift+Space
            return;
        }
        push_key(' ');
        hold_add(x, y, ' ');
        return;
    }

    // 可打印键
    if (ctrl && shift && (raw == 'f' || raw == 'F')) {
        push_key(CPK_KEY_TRAD_TOGGLE);  // Ctrl+Shift+F
        return;
    }
    if (ctrl && (raw == 'i' || raw == 'I')) {
        push_key(CPK_KEY_CTRL_I);  // Ctrl+I
        return;
    }
    if (ctrl && raw == '=') {  // Ctrl+= 增加亮度
        push_key(CPK_KEY_CTRL_EQUALS);
        hold_add(x, y, CPK_KEY_CTRL_EQUALS);
        return;
    }
    if (ctrl && raw == '-') {  // Ctrl+- 减少亮度
        push_key(CPK_KEY_CTRL_MINUS);
        hold_add(x, y, CPK_KEY_CTRL_MINUS);
        return;
    }
    if (ctrl && std::isalpha(raw)) {
        uint8_t cc = (uint8_t)(std::toupper(raw) - 'A' + 1);  // Ctrl+A..Z → 0x01..0x1a
        push_key(cc);
        hold_add(x, y, cc);
        return;
    }
    if (ctrl && raw == ';') { push_key(CPK_KEY_CTRL_UP);    hold_add(x, y, CPK_KEY_CTRL_UP);    return; }
    if (ctrl && raw == '.') { push_key(CPK_KEY_CTRL_DOWN);  hold_add(x, y, CPK_KEY_CTRL_DOWN);  return; }
    if (ctrl && raw == ',') { push_key(CPK_KEY_CTRL_LEFT);  hold_add(x, y, CPK_KEY_CTRL_LEFT);  return; }
    if (ctrl && raw == '/') { push_key(CPK_KEY_CTRL_RIGHT); hold_add(x, y, CPK_KEY_CTRL_RIGHT); return; }
    uint8_t base = shift ? kv.second : kv.first;
    push_key(base);
    hold_add(x, y, base);
}

static void process_release(uint8_t x, uint8_t y)
{
    hold_remove(x, y);
    if (s_keymap[y][x].first == RAW_LEFT_SHIFT) {
        if (s_shift_tap_armed) {
            push_key(CPK_KEY_LSHIFT_TAP);
        }
        s_shift_tap_armed = false;
    }
}

// TCA8418 模式:轮询键盘事件 FIFO,重映射到 Cardputer 4×14 坐标后送入统一处理。
// 官方 remap:new_col = tca_row*2 + (tca_col>3 ? 1 : 0); new_row = (tca_col+4)%4。
static void scan_tca(void)
{
    uint8_t ec = 0;
    if (tca_read(TCA_REG_KEY_LCK_EC, &ec) != ESP_OK) return;  // 总线错误,下个周期重试
    uint8_t count = ec & 0x0f;
    if (count == 0) return;
    if (count > 10) count = 10;  // FIFO 实际最深 10 条,缓冲数组也按 10 分配

    s_tca_press_n = 0;
    s_tca_release_n = 0;
    for (int i = 0; i < count; i++) {
        uint8_t ev = 0;
        if (tca_read(TCA_REG_KEY_EVENT_A, &ev) != ESP_OK) break;
        if (ev == 0) break;  // FIFO 已空
        bool pressed = (ev & 0x80) != 0;
        int code = (int)(ev & 0x7f) - 1;
        if (code < 0) continue;
        int r = code / 10;
        int c = code % 10;
        int col = r * 2 + (c > 3 ? 1 : 0);
        int row = (c + 4) % 4;
        if (row >= KEY_ROWS || col >= KEY_COLS) {
            ESP_LOGW(TAG, "TCA: oob ev=0x%02x r=%d c=%d -> row=%d col=%d", ev, r, c, row, col);
            continue;
        }
        if (pressed) {
            if (!s_stable[row][col]) s_tca_press[s_tca_press_n++] = key_id(col, row);
            s_stable[row][col] = 1;
        } else {
            if (s_stable[row][col]) s_tca_release[s_tca_release_n++] = key_id(col, row);
            s_stable[row][col] = 0;
        }
    }
    for (int i = 0; i < s_tca_press_n; i++)
        process_press(s_tca_press[i] & 0x0f, s_tca_press[i] >> 4);
    for (int i = 0; i < s_tca_release_n; i++)
        process_release(s_tca_release[i] & 0x0f, s_tca_release[i] >> 4);
}

// IOMatrix(初代)模式:每 5ms 读 GPIO 矩阵 → 去抖 → 处理边沿
static void scan_iomatrix(void)
{
    memset(s_raw, 0, sizeof(s_raw));
    scan_raw(s_raw);

    // 去抖:连续两次读到按下才算稳定;一次读到释放即松开
    s_press_n = 0;
    s_release_n = 0;
    for (int y = 0; y < KEY_ROWS; y++) {
        for (int x = 0; x < KEY_COLS; x++) {
            bool r = s_raw[y][x] != 0;
            bool p = s_prev[y][x] != 0;
            bool s = s_stable[y][x] != 0;
            if (r && p) {
                if (!s) s_press_edges[s_press_n++] = key_id(x, y);
                s = true;
            } else if (!r) {
                if (s) s_release_edges[s_release_n++] = key_id(x, y);
                s = false;
            }
            s_stable[y][x] = s;
        }
    }
    memcpy(s_prev, s_raw, sizeof(s_prev));

    for (int i = 0; i < s_press_n; i++)
        process_press(s_press_edges[i] & 0x0f, s_press_edges[i] >> 4);
    for (int i = 0; i < s_release_n; i++)
        process_release(s_release_edges[i] & 0x0f, s_release_edges[i] >> 4);
}

// 扫描任务每 5ms:按机型轮询 TCA8418 或 IOMatrix → 连击。连击由本任务产生,
// 避免与主循环共享可变量(主循环仅通过队列读取)。
static void scan_task(void *arg)
{
    while (true) {
        if (s_mode == KBD_TCA8418) {
            scan_tca();
        } else {
            scan_iomatrix();
        }

        int64_t now = now_us();

        // 连击
        for (int i = 0; i < MAX_HELD; i++) {
            HeldKey *h = &s_held[i];
            if (h->key_id == 0 || h->app_code == 0) continue;
            if (now - h->press_us < REPEAT_DELAY_MS * 1000LL) continue;
            if (h->last_repeat_us == 0) {
                h->last_repeat_us = h->press_us + REPEAT_DELAY_MS * 1000LL;
            }
            if (now >= h->last_repeat_us + REPEAT_INTERVAL_MS * 1000LL) {
                push_key(h->app_code);
                h->last_repeat_us = now;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(SCAN_INTERVAL_MS));
    }
}

// ── Public API ───────────────────────────────────────────────────────────
CardputerKeyboard g_keyboard;

esp_err_t CardputerKeyboard::init()
{
    if (s_queue) return ESP_OK;  // 已初始化

    s_queue = xQueueCreate(32, sizeof(uint8_t));
    if (!s_queue) return ESP_FAIL;

    memset(s_raw, 0, sizeof(s_raw));
    memset(s_prev, 0, sizeof(s_prev));
    memset(s_stable, 0, sizeof(s_stable));
    memset(s_held, 0, sizeof(s_held));
    s_shift_tap_armed = false;

    // ADV:优先初始化 TCA8418(地址 0x34);初代探测不到则回退 IOMatrix
    if (tca8418_init()) {
        s_mode = KBD_TCA8418;
        s_connected = true;
        xTaskCreate(scan_task, "kbd_scan", 4096, NULL, 4, &s_scan_task);
        ESP_LOGI(TAG, "Cardputer ADV keyboard (TCA8418) initialized");
        return ESP_OK;
    }
    ESP_LOGW(TAG, "TCA8418 not found -> fallback to IOMatrix (original Cardputer)");
    tca_cleanup();
    s_mode = KBD_IOMATRIX;

    // 与 M5Cardputer begin() 一致:先复位引脚,再配置,避免残留外设功能/上拉状态
    for (int i = 0; i < 3; i++) gpio_reset_pin(s_output_pins[i]);
    for (int i = 0; i < 7; i++) gpio_reset_pin(s_input_pins[i]);

    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << s_output_pins[0]) | (1ULL << s_output_pins[1]) |
                        (1ULL << s_output_pins[2]),
        // INPUT_OUTPUT 让 gpio_get_level 读回真实 pad 电平(纯 OUTPUT 时输入缓冲关闭,读回不可靠)
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t e1 = gpio_config(&out_cfg);
    set_output(0);

    uint64_t in_mask = 0;
    for (int i = 0; i < 7; i++) in_mask |= (1ULL << s_input_pins[i]);
    gpio_config_t in_cfg = {
        .pin_bit_mask = in_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t e2 = gpio_config(&in_cfg);
    if (e1 != ESP_OK || e2 != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: out=%s in=%s",
                 esp_err_to_name(e1), esp_err_to_name(e2));
    } else {
        ESP_LOGI(TAG, "gpio_config OK (out pins 8/9/11, in pins 13/15/3/4/5/6/7)");
    }

    s_connected = true;
    xTaskCreate(scan_task, "kbd_scan", 4096, NULL, 4, &s_scan_task);
    ESP_LOGI(TAG, "Cardputer keyboard (IOMatrix) initialized");
    return ESP_OK;
}

void CardputerKeyboard::deinit()
{
    if (s_scan_task) {
        vTaskSuspend(s_scan_task);
    }
    // 清掉去抖/连击状态,唤醒 resume 后从干净状态重新扫描
    memset(s_raw, 0, sizeof(s_raw));
    memset(s_prev, 0, sizeof(s_prev));
    memset(s_stable, 0, sizeof(s_stable));
    memset(s_held, 0, sizeof(s_held));
    s_shift_tap_armed = false;
    if (s_queue) xQueueReset(s_queue);
    // TCA8418:丢弃睡眠期间缓存的按键,避免唤醒后连发
    if (s_mode == KBD_TCA8418 && s_tca_dev) tca_flush();
    s_connected = false;
}

void CardputerKeyboard::resume()
{
    if (s_scan_task && s_queue) {
        s_connected = true;
        if (s_mode == KBD_TCA8418 && s_tca_dev) tca_flush();
        vTaskResume(s_scan_task);
    }
}

uint8_t CardputerKeyboard::readKey()
{
    uint8_t c = 0;
    if (s_queue && xQueueReceive(s_queue, &c, 0) == pdTRUE) return c;
    return 0;
}

void CardputerKeyboard::flushKeys()
{
    if (s_queue) xQueueReset(s_queue);
}

void CardputerKeyboard::checkKeyRepeat()
{
    // 连击由扫描任务产生,这里无操作
}

bool CardputerKeyboard::isConnected() const { return s_connected; }
bool CardputerKeyboard::isInitialized() const { return s_connected; }
