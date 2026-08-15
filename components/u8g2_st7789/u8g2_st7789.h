#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

#define U8G2_ST7789_TILE_BUF_FULL 0

typedef struct {
    gpio_num_t mosi_io;
    gpio_num_t sclk_io;
    gpio_num_t dc_io;
    gpio_num_t cs_io;
    gpio_num_t reset_io;
    spi_host_device_t spi_host;
    int clock_hz;
    uint8_t tile_buf_height;
    const u8g2_cb_t *rotation;
    bool prefer_psram;
} u8g2_st7789_config_t;

typedef struct {
    u8g2_t u8g2;
    spi_device_handle_t spi;
    spi_host_device_t spi_host;
    gpio_num_t dc_io;
    gpio_num_t cs_io;
    gpio_num_t reset_io;
    uint8_t *buffer;
    size_t buffer_size;
    uint8_t tile_buf_height;
    bool owns_spi_bus;
} u8g2_st7789_t;

u8g2_st7789_config_t u8g2_st7789_default_config(void);
esp_err_t u8g2_st7789_init(u8g2_st7789_t *dev, const u8g2_st7789_config_t *config);
void u8g2_st7789_deinit(u8g2_st7789_t *dev);

// 全局显示设备指针(init 后由 main 赋值),供设置/主界面调节显示
extern u8g2_st7789_t *g_lcd_dev;

// dark=true → 黑底白字(INVOFF), dark=false → 白底黑字(INVON, 默认)
void u8g2_st7789_set_dark_mode(u8g2_st7789_t *dev, bool dark);
// 背光亮度 0..255(LEDC PWM, 满占空比=最亮)
void u8g2_st7789_set_backlight(u8g2_st7789_t *dev, uint8_t duty);
uint8_t u8g2_st7789_get_backlight(u8g2_st7789_t *dev);

static inline u8g2_t *u8g2_st7789_get_u8g2(u8g2_st7789_t *dev)
{
    return dev == NULL ? NULL : &dev->u8g2;
}

#ifdef __cplusplus
}
#endif
