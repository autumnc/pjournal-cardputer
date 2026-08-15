#include "usb_drive.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"

#include "u8g2.h"
#include "font_renderer.h"
#include "ui_helpers.h"

extern u8g2_t *g_u8g2;

static const char *TAG = "UsbDrive";

// Same SD pins as journal_storage.cpp (SPI2_HOST): SCLK=40, MOSI=14, MISO=39, CS=12
#define USB_SD_HOST SPI2_HOST
#define USB_SD_CLK  GPIO_NUM_40
#define USB_SD_MOSI GPIO_NUM_14
#define USB_SD_MISO GPIO_NUM_39
#define USB_SD_CS   GPIO_NUM_12

// ── TinyUSB MSC descriptors ────────────────────────────────────────────────
#define EPNUM_MSC 1
#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

enum {
    ITF_NUM_MSC = 0,
    ITF_NUM_TOTAL,
};

enum {
    EDPT_MSC_OUT = 0x01,
    EDPT_MSC_IN  = 0x81,
};

static tusb_desc_device_t s_device_desc = {
    .bLength = sizeof(s_device_desc),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,  // Espressif VID
    .idProduct = 0x4002,
    .bcdDevice = 0x100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

static const uint8_t s_msc_config_desc[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUSB_DESC_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    // Interface number, string index, EP Out & EP In address, EP size
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EDPT_MSC_OUT, EDPT_MSC_IN, 64),
};

static const char *s_string_desc[] = {
    (const char[]) { 0x09, 0x04 },  // 0: language English (0x0409)
    "M5Stack",                      // 1: Manufacturer
    "PJournal Cardputer",           // 2: Product
    "PJ0001",                       // 3: Serial
    "Cardputer SD",                 // 4: MSC interface
};

static sdmmc_card_t *s_card = NULL;
static tinyusb_msc_storage_handle_t s_storage = NULL;

// Raw-init the SD card on SPI2 without mounting FAT. Mirrors the sequence in
// esp_vfs_fat_sdspi_mount() so the host PC owns the raw sectors.
static bool usb_drive_sd_init(void)
{
    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = USB_SD_MOSI;
    bus_cfg.miso_io_num = USB_SD_MISO;
    bus_cfg.sclk_io_num = USB_SD_CLK;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 16384;
    esp_err_t ret = spi_bus_initialize(USB_SD_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return false;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    ret = (*host.init)();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sdspi_host_init failed: %s", esp_err_to_name(ret));
        return false;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = USB_SD_CS;
    slot_config.host_id = USB_SD_HOST;
    int card_handle;
    ret = sdspi_host_init_device(&slot_config, &card_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sdspi_host_init_device failed: %s", esp_err_to_name(ret));
        return false;
    }
    host.slot = card_handle;

    s_card = (sdmmc_card_t *)calloc(1, sizeof(sdmmc_card_t));
    if (!s_card) {
        ESP_LOGE(TAG, "No memory for sdmmc_card_t");
        return false;
    }
    ret = sdmmc_card_init(&host, s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sdmmc_card_init failed: %s", esp_err_to_name(ret));
        free(s_card);
        s_card = NULL;
        return false;
    }
    sdmmc_card_print_info(stdout, s_card);
    return true;
}

bool usb_drive_begin(void)
{
    if (!usb_drive_sd_init()) {
        ESP_LOGE(TAG, "SD card init failed, cannot enter USB mode");
        return false;
    }

    tinyusb_msc_storage_config_t storage_cfg = {};
    storage_cfg.medium.card = s_card;
    storage_cfg.fat_fs.base_path = NULL;               // not mounted locally
    storage_cfg.fat_fs.config.max_files = 5;
    storage_cfg.fat_fs.do_not_format = true;           // never let the host wipe the card
    storage_cfg.mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB;  // host owns it immediately

    esp_err_t ret = tinyusb_msc_new_storage_sdmmc(&storage_cfg, &s_storage);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_msc_new_storage_sdmmc failed: %s", esp_err_to_name(ret));
        return false;
    }

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = &s_device_desc;
    tusb_cfg.descriptor.full_speed_config = s_msc_config_desc;
    tusb_cfg.descriptor.string = s_string_desc;
    tusb_cfg.descriptor.string_count = sizeof(s_string_desc) / sizeof(s_string_desc[0]);

    ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install failed: %s", esp_err_to_name(ret));
        return false;
    }
    ESP_LOGI(TAG, "USB Mass Storage mode ready");
    return true;
}

// Status screen, drawn only when state changes (ui_commit snapshots the frame).
static void usb_drive_draw_screen(bool mounted)
{
    ui_clear();
    g_font.setSize(22);

    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, 0, 0, SCREEN_W, FONT_H + 3);
    u8g2_SetDrawColor(g_u8g2, 0);
    const char *title = "USB 存储模式";
    int tx = (SCREEN_W - g_font.textWidth(title)) / 2;
    g_font.drawText(tx, g_font.ascent() + 1, title, false);

    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawHLine(g_u8g2, 0, FONT_H + 4, SCREEN_W);

    // SD capacity
    uint32_t sectors = 0, sector_size = 0;
    char cap[48];
    if (s_storage &&
        tinyusb_msc_get_storage_capacity(s_storage, &sectors) == ESP_OK &&
        tinyusb_msc_get_storage_sector_size(s_storage, &sector_size) == ESP_OK) {
        uint64_t mb = (uint64_t)sectors * sector_size / (1024 * 1024);
        snprintf(cap, sizeof(cap), "SD 卡容量: %llu MB", (unsigned long long)mb);
    } else {
        snprintf(cap, sizeof(cap), "SD 卡容量: 未知");
    }
    int y = FONT_H + 4 + g_font.lineHeight() + 8;
    g_font.drawText((SCREEN_W - g_font.textWidth(cap)) / 2, y, cap, false);

    // Connection status
    const char *status = mounted ? "已连接 - 电脑可访问 SD 卡" : "等待 USB 连接...";
    y += g_font.lineHeight() + 12;
    g_font.drawText((SCREEN_W - g_font.textWidth(status)) / 2, y, status, false);

    const char *hint = "复制完文件后请安全弹出再拔出";
    y += g_font.lineHeight() + 16;
    g_font.drawText((SCREEN_W - g_font.textWidth(hint)) / 2, y, hint, false);

    u8g2_SetDrawColor(g_u8g2, 0);
}

void usb_drive_run(void)
{
    bool was_mounted = false;
    bool first = true;
    for (;;) {
        bool mounted = tud_mounted();
        if (first || mounted != was_mounted) {
            first = false;
            was_mounted = mounted;
            usb_drive_draw_screen(mounted);
            ui_commit();
            ESP_LOGI(TAG, "USB host %s", mounted ? "mounted" : "disconnected");
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
