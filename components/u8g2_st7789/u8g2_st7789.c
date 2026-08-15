#include "u8g2_st7789.h"

#include <string.h>

#include "driver/ledc.h"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define ST7789_SPI_CLOCK_HZ 40000000
// M5Cardputer LCD: ST7789 GRAM 240x320, 可视玻璃区域仅 240x135 横屏,
// 在 GRAM 中位于 (40,53)(M5GFX rotation 1 的 colstart/rowstart)。
#define ST7789_TILE_WIDTH 30     // 240/8
#define ST7789_TILE_HEIGHT 17    // ceil(135/8);逻辑高 135px,缓冲多 1 行(不可见)
#define ST7789_COL_OFFSET 40     // GRAM column start (MADCTL=0x6C 旋转帧)
#define ST7789_ROW_OFFSET 53     // GRAM row start
#define ST7789_FULL_BUFFER_HEIGHT ST7789_TILE_HEIGHT
#define ST7789_BUFFER_ROW_BYTES (ST7789_TILE_WIDTH * 8)
// Max RGB565 bytes for one tile row: 8 pixel rows x (30*8) pixels x 2 bytes
#define ST7789_TILEROW_RGB_BYTES (8 * ST7789_TILE_WIDTH * 8 * 2)

// Cardputer LCD 背光由 ESP32 LEDC PWM 直接驱动 GPIO38(M5Unified Light_PWM)
#define ST7789_BACKLIGHT_IO GPIO_NUM_38
#define ST7789_BACKLIGHT_CHANNEL LEDC_CHANNEL_7
#define ST7789_BACKLIGHT_TIMER LEDC_TIMER_3

static const char *TAG = "u8g2_st7789";

static const u8x8_display_info_t st7789_display_info = {
    /* chip_enable_level = */ 0,
    /* chip_disable_level = */ 1,
    /* post_chip_enable_wait_ns = */ 0,
    /* pre_chip_disable_wait_ns = */ 0,
    /* reset_pulse_width_ms = */ 20,
    /* post_reset_wait_ms = */ 50,
    /* sda_setup_time_ns = */ 0,
    /* sck_pulse_width_ns = */ 0,
    /* sck_clock_hz = */ ST7789_SPI_CLOCK_HZ,
    /* spi_mode = */ 0,
    /* i2c_bus_clock_100kHz = */ 4,
    /* data_setup_time_ns = */ 0,
    /* write_pulse_width_ns = */ 0,
    /* tile_width = */ ST7789_TILE_WIDTH,
    /* tile_height = */ ST7789_TILE_HEIGHT,
    /* default_x_offset = */ 0,
    /* flip_mode_x_offset = */ 0,
    /* pixel_width = */ 240,
    /* pixel_height = */ 135,
};

// Scratch buffer for 1bpp -> RGB565 conversion of one tile row.
// Single display, synchronous (polling) SPI: no reentrancy.
static uint8_t s_rgb_buf[ST7789_TILEROW_RGB_BYTES];

// 全局显示设备指针(u8g2_st7789_init 后由 main 赋值)
u8g2_st7789_t *g_lcd_dev = NULL;

// 当前背光占空比。init 后默认 0(关闭),由主程序设置好底色后再开启,避免开机白屏
static uint8_t s_backlight_duty = 0;
// LEDC 背光通道是否已初始化(懒初始化:首次 set_backlight 时才配置)
static bool s_backlight_init_done = false;

static esp_err_t st7789_spi_write(u8g2_st7789_t *dev, const uint8_t *data, size_t len)
{
    if (len == 0) {
        return ESP_OK;
    }

    spi_transaction_t tx = {
        .length = len * 8,
        .tx_buffer = data,
    };
    return spi_device_polling_transmit(dev->spi, &tx);
}

static esp_err_t st7789_write_cmd(u8g2_st7789_t *dev, uint8_t cmd)
{
    gpio_set_level(dev->dc_io, 0);
    gpio_set_level(dev->cs_io, 0);
    esp_err_t ret = st7789_spi_write(dev, &cmd, 1);
    gpio_set_level(dev->cs_io, 1);
    return ret;
}

static esp_err_t st7789_write_data(u8g2_st7789_t *dev, const uint8_t *data, size_t len)
{
    gpio_set_level(dev->dc_io, 1);
    gpio_set_level(dev->cs_io, 0);
    esp_err_t ret = st7789_spi_write(dev, data, len);
    gpio_set_level(dev->cs_io, 1);
    return ret;
}

static esp_err_t st7789_write_cmd_data(u8g2_st7789_t *dev, uint8_t cmd, const uint8_t *data, size_t len)
{
    gpio_set_level(dev->dc_io, 0);
    gpio_set_level(dev->cs_io, 0);
    esp_err_t ret = st7789_spi_write(dev, &cmd, 1);
    if (ret == ESP_OK && len > 0) {
        gpio_set_level(dev->dc_io, 1);
        ret = st7789_spi_write(dev, data, len);
    }
    gpio_set_level(dev->cs_io, 1);
    return ret;
}

static void st7789_reset(u8g2_st7789_t *dev)
{
    if (dev->reset_io < 0) {
        return;
    }

    gpio_set_level(dev->reset_io, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(dev->reset_io, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(dev->reset_io, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

// ST7789V 240x320 GRAM,16bpp (RGB565)。Cardputer 面板为 240x135 横屏可视窗,
// 与 M5GFX rotation 1 完全一致:MADCTL=0x6C(MV|MX|BGR)、INVON(面板反色),
// 写入窗口加 GRAM 偏移 (40,53)。旋转交给控制器硬件,软件用 U8G2_R0。
static void st7789_full_init(u8g2_st7789_t *dev)
{
    st7789_reset(dev);

    const uint8_t madctl[] = {0x6C};
    const uint8_t colmod[] = {0x55};

    ESP_ERROR_CHECK_WITHOUT_ABORT(st7789_write_cmd(dev, 0x11));  // SLPOUT
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_ERROR_CHECK_WITHOUT_ABORT(st7789_write_cmd_data(dev, 0x36, madctl, sizeof(madctl)));
    ESP_ERROR_CHECK_WITHOUT_ABORT(st7789_write_cmd_data(dev, 0x3A, colmod, sizeof(colmod)));
    ESP_ERROR_CHECK_WITHOUT_ABORT(st7789_write_cmd(dev, 0x21));  // INVON (Cardputer 面板需反色)
    ESP_ERROR_CHECK_WITHOUT_ABORT(st7789_write_cmd(dev, 0x13));  // NORON
    ESP_ERROR_CHECK_WITHOUT_ABORT(st7789_write_cmd(dev, 0x29));  // DISPON
}

static uint8_t u8g2_st7789_byte_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    u8g2_st7789_t *dev = (u8g2_st7789_t *)u8x8_GetUserPtr(u8x8);

    switch (msg) {
    case U8X8_MSG_BYTE_INIT:
        return 1;
    case U8X8_MSG_BYTE_START_TRANSFER:
        gpio_set_level(dev->cs_io, 0);
        return 1;
    case U8X8_MSG_BYTE_SEND:
        return st7789_spi_write(dev, (const uint8_t *)arg_ptr, arg_int) == ESP_OK;
    case U8X8_MSG_BYTE_END_TRANSFER:
        gpio_set_level(dev->cs_io, 1);
        return 1;
    case U8X8_MSG_BYTE_SET_DC:
        gpio_set_level(dev->dc_io, arg_int ? 1 : 0);
        return 1;
    default:
        return 0;
    }
}

static uint8_t u8g2_st7789_gpio_and_delay_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    (void)arg_ptr;
    u8g2_st7789_t *dev = (u8g2_st7789_t *)u8x8_GetUserPtr(u8x8);

    switch (msg) {
    case U8X8_MSG_GPIO_AND_DELAY_INIT:
        return 1;
    case U8X8_MSG_DELAY_MILLI:
        vTaskDelay(pdMS_TO_TICKS(arg_int));
        return 1;
    case U8X8_MSG_DELAY_10MICRO:
        esp_rom_delay_us((uint32_t)arg_int * 10U);
        return 1;
    case U8X8_MSG_DELAY_100NANO:
        esp_rom_delay_us(1);
        return 1;
    case U8X8_MSG_GPIO_CS:
        gpio_set_level(dev->cs_io, arg_int ? 1 : 0);
        return 1;
    case U8X8_MSG_GPIO_DC:
        gpio_set_level(dev->dc_io, arg_int ? 1 : 0);
        return 1;
    case U8X8_MSG_GPIO_RESET:
        if (dev->reset_io >= 0) {
            gpio_set_level(dev->reset_io, arg_int ? 1 : 0);
        }
        return 1;
    default:
        return 1;
    }
}

static uint8_t u8g2_st7789_display_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    u8g2_st7789_t *dev = (u8g2_st7789_t *)u8x8_GetUserPtr(u8x8);

    switch (msg) {
    case U8X8_MSG_DISPLAY_SETUP_MEMORY:
        u8x8_d_helper_display_setup_memory(u8x8, &st7789_display_info);
        return 1;
    case U8X8_MSG_DISPLAY_INIT:
        st7789_full_init(dev);
        return 1;
    case U8X8_MSG_DISPLAY_SET_POWER_SAVE:
        ESP_ERROR_CHECK_WITHOUT_ABORT(st7789_write_cmd(dev, arg_int == 0 ? 0x29 : 0x28));
        return 1;
    case U8X8_MSG_DISPLAY_DRAW_TILE: {
        u8x8_tile_t *tile = (u8x8_tile_t *)arg_ptr;
        uint8_t cnt = tile->cnt;
        uint8_t x_pos = tile->x_pos;
        uint8_t y_pos = tile->y_pos;

        int x_start = x_pos * 8 + ST7789_COL_OFFSET;
        int x_end = (x_pos + cnt) * 8 - 1 + ST7789_COL_OFFSET;
        int y_start = y_pos * 8 + ST7789_ROW_OFFSET;
        int y_end = y_pos * 8 + 7 + ST7789_ROW_OFFSET;

        uint8_t col_bounds[4] = {
            (uint8_t)(x_start >> 8), (uint8_t)(x_start & 0xFF),
            (uint8_t)(x_end >> 8), (uint8_t)(x_end & 0xFF),
        };
        uint8_t row_bounds[4] = {
            (uint8_t)(y_start >> 8), (uint8_t)(y_start & 0xFF),
            (uint8_t)(y_end >> 8), (uint8_t)(y_end & 0xFF),
        };

        // u8g2 缓冲为 vertical_top_lsb:每个字节 = 8 个垂直像素(bit0 = 顶部行 y%8)。
        // tile->tile_ptr 指向该 tile 组起始字节;字节列 c 对应显示列 x_pos*8+c,
        // 字节内 bit r 对应显示行 y_pos*8+r。逐行输出 RGB565(高字节在前)。
        const uint8_t *tb = tile->tile_ptr;
        uint8_t *dst = s_rgb_buf;
        for (int r = 0; r < 8; r++) {
            for (int c = 0; c < cnt * 8; c++) {
                uint16_t px = (tb[c] >> r) & 1 ? 0xFFFF : 0x0000;
                *dst++ = (uint8_t)(px >> 8);
                *dst++ = (uint8_t)(px & 0xFF);
            }
        }

        ESP_ERROR_CHECK_WITHOUT_ABORT(st7789_write_cmd_data(dev, 0x2A, col_bounds, sizeof(col_bounds)));
        ESP_ERROR_CHECK_WITHOUT_ABORT(st7789_write_cmd_data(dev, 0x2B, row_bounds, sizeof(row_bounds)));
        ESP_ERROR_CHECK_WITHOUT_ABORT(st7789_write_cmd_data(dev, 0x2C, s_rgb_buf, 8 * cnt * 8 * 2));
        return 1;
    }
    default:
        return 0;
    }
}

u8g2_st7789_config_t u8g2_st7789_default_config(void)
{
    u8g2_st7789_config_t config = {
        .mosi_io = GPIO_NUM_35,
        .sclk_io = GPIO_NUM_36,
        .dc_io = GPIO_NUM_34,
        .cs_io = GPIO_NUM_37,
        .reset_io = GPIO_NUM_33,
        .spi_host = SPI3_HOST,
        .clock_hz = ST7789_SPI_CLOCK_HZ,
        .tile_buf_height = U8G2_ST7789_TILE_BUF_FULL,
        .rotation = U8G2_R0,
        .prefer_psram = false,
    };
    return config;
}

// Cardputer 背光:LEDC PWM 驱动 GPIO38。首次开启时懒初始化 LEDC。
static void st7789_backlight_init(uint8_t duty)
{
    s_backlight_duty = duty;
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = ST7789_BACKLIGHT_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&timer) != ESP_OK) {
        ESP_LOGW(TAG, "backlight timer config failed");
        return;
    }
    ledc_channel_config_t chan = {
        .gpio_num = ST7789_BACKLIGHT_IO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = ST7789_BACKLIGHT_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = ST7789_BACKLIGHT_TIMER,
        .duty = duty,
        .hpoint = 0,
    };
    if (ledc_channel_config(&chan) != ESP_OK) {
        ESP_LOGW(TAG, "backlight channel config failed");
        return;
    }
    ledc_update_duty(LEDC_LOW_SPEED_MODE, ST7789_BACKLIGHT_CHANNEL);
    s_backlight_init_done = true;
}

esp_err_t u8g2_st7789_init(u8g2_st7789_t *dev, const u8g2_st7789_config_t *config)
{
    ESP_RETURN_ON_FALSE(dev != NULL, ESP_ERR_INVALID_ARG, TAG, "dev is NULL");
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is NULL");

    memset(dev, 0, sizeof(*dev));
    dev->spi_host = config->spi_host;
    dev->dc_io = config->dc_io;
    dev->cs_io = config->cs_io;
    dev->reset_io = config->reset_io;

    gpio_config_t gpio_conf = {
        .pin_bit_mask = (1ULL << config->dc_io) | (1ULL << config->cs_io),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (config->reset_io >= 0) {
        gpio_conf.pin_bit_mask |= (1ULL << config->reset_io);
    }
    ESP_RETURN_ON_ERROR(gpio_config(&gpio_conf), TAG, "gpio_config failed");
    gpio_set_level(config->cs_io, 1);
    gpio_set_level(config->dc_io, 1);
    if (config->reset_io >= 0) {
        gpio_set_level(config->reset_io, 1);
    }

    spi_bus_config_t buscfg = {
        .mosi_io_num = config->mosi_io,
        .miso_io_num = -1,
        .sclk_io_num = config->sclk_io,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = ST7789_TILEROW_RGB_BYTES,
    };
    esp_err_t ret = spi_bus_initialize(config->spi_host, &buscfg, SPI_DMA_CH_AUTO);
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "SPI bus already initialized, reusing host %d", config->spi_host);
        ret = ESP_OK;
        dev->owns_spi_bus = false;
    } else {
        ESP_RETURN_ON_ERROR(ret, TAG, "spi_bus_initialize failed");
        dev->owns_spi_bus = true;
    }

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = config->clock_hz > 0 ? config->clock_hz : ST7789_SPI_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    ret = spi_bus_add_device(config->spi_host, &devcfg, &dev->spi);
    if (ret != ESP_OK) {
        if (dev->owns_spi_bus) {
            spi_bus_free(config->spi_host);
        }
        return ret;
    }

    dev->tile_buf_height = config->tile_buf_height == U8G2_ST7789_TILE_BUF_FULL
                               ? ST7789_FULL_BUFFER_HEIGHT
                               : config->tile_buf_height;
    if (dev->tile_buf_height == 0 || dev->tile_buf_height > ST7789_TILE_HEIGHT) {
        dev->tile_buf_height = ST7789_FULL_BUFFER_HEIGHT;
    }

    dev->buffer_size = ST7789_BUFFER_ROW_BYTES * dev->tile_buf_height;
    uint32_t caps = MALLOC_CAP_8BIT;
    if (config->prefer_psram) {
        caps |= MALLOC_CAP_SPIRAM;
    }
    dev->buffer = (uint8_t *)heap_caps_malloc(dev->buffer_size, caps);
    if (dev->buffer == NULL && config->prefer_psram) {
        dev->buffer = (uint8_t *)heap_caps_malloc(dev->buffer_size, MALLOC_CAP_8BIT);
    }
    if (dev->buffer == NULL) {
        u8g2_st7789_deinit(dev);
        return ESP_ERR_NO_MEM;
    }
    memset(dev->buffer, 0, dev->buffer_size);

    const u8g2_cb_t *rotation = config->rotation != NULL ? config->rotation : U8G2_R0;
    u8g2_SetupDisplay(&dev->u8g2, u8g2_st7789_display_cb, u8x8_cad_empty,
                      u8g2_st7789_byte_cb, u8g2_st7789_gpio_and_delay_cb);
    u8g2_SetUserPtr(&dev->u8g2, dev);
    u8g2_SetupBuffer(&dev->u8g2, dev->buffer, dev->tile_buf_height,
                     u8g2_ll_hvline_vertical_top_lsb, rotation);
    u8g2_InitDisplay(&dev->u8g2);
    u8g2_SetPowerSave(&dev->u8g2, 0);

    // 背光默认关闭(避免开机瞬间白屏),设置好底色后再由主程序
    // u8g2_st7789_set_backlight 开启
    s_backlight_init_done = false;
    s_backlight_duty = 0;
    gpio_config_t bl_cfg = {
        .pin_bit_mask = (1ULL << ST7789_BACKLIGHT_IO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&bl_cfg);
    gpio_set_level(ST7789_BACKLIGHT_IO, 0);

    return ESP_OK;
}

void u8g2_st7789_deinit(u8g2_st7789_t *dev)
{
    if (dev == NULL) {
        return;
    }

    if (dev->spi != NULL) {
        spi_bus_remove_device(dev->spi);
        dev->spi = NULL;
    }
    if (dev->owns_spi_bus) {
        spi_bus_free(dev->spi_host);
        dev->owns_spi_bus = false;
    }
    if (dev->buffer != NULL) {
        heap_caps_free(dev->buffer);
        dev->buffer = NULL;
    }
}

// dark=true → 黑底白字(INVOFF), dark=false → 白底黑字(INVON, 默认)。
// 面板本就需反色(INVON),关闭反色即整体黑白互换,软件绘图无需改动。
void u8g2_st7789_set_dark_mode(u8g2_st7789_t *dev, bool dark)
{
    if (dev == NULL) {
        return;
    }
    st7789_write_cmd(dev, dark ? 0x20 : 0x21);
}

void u8g2_st7789_set_backlight(u8g2_st7789_t *dev, uint8_t duty)
{
    (void)dev;
    s_backlight_duty = duty;
    if (!s_backlight_init_done) {
        st7789_backlight_init(duty);  // 首次开启时初始化 LEDC
        return;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, ST7789_BACKLIGHT_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, ST7789_BACKLIGHT_CHANNEL);
}

uint8_t u8g2_st7789_get_backlight(u8g2_st7789_t *dev)
{
    (void)dev;
    return s_backlight_duty;
}
