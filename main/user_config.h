#pragma once

// M5Stack Cardputer hardware definitions.

// Display: ST7789, Cardputer 240x135 landscape (GRAM 240x320, visible 240x135)
#define LCD_WIDTH      240
#define LCD_HEIGHT     135

#define TFT_MOSI_PIN  GPIO_NUM_35
#define TFT_SCLK_PIN  GPIO_NUM_36
#define TFT_DC_PIN    GPIO_NUM_34
#define TFT_CS_PIN    GPIO_NUM_37
#define TFT_RST_PIN   GPIO_NUM_33
#define TFT_BL_PIN    GPIO_NUM_38  // LCD 背光(LEDC PWM)

// Buttons
#define PIN_BOOT     GPIO_NUM_0

// SD card over SPI2: SCLK=40, MOSI=14, MISO=39, CS=12
#define SD_SPI_HOST  SPI2_HOST
#define SD_CLK_PIN   GPIO_NUM_40
#define SD_MOSI_PIN  GPIO_NUM_14
#define SD_MISO_PIN  GPIO_NUM_39
#define SD_CS_PIN    GPIO_NUM_12

// Battery ADC: ADC1_CH9 = GPIO10, voltage divider 2:1
#define BATTERY_ADC_CHAN ADC_CHANNEL_9
