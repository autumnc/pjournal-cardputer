#include "journal_storage.h"
#include <cstring>
#include <ctime>
#include <algorithm>
#include <functional>
#include <unordered_set>
#include <dirent.h>
#include <sys/stat.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <driver/sdspi_host.h>
#include <driver/spi_master.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static const char *TAG = "Journal";

static bool isJournalExt(const std::string &fn) {
    auto dot = fn.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = fn.substr(dot);
    return ext == ".txt" || ext == ".md";
}

// SD card mutex (recursive to handle nested public method calls)
static SemaphoreHandle_t s_sd_mutex = nullptr;

SemaphoreHandle_t JournalStorage::sdMutex() { return s_sd_mutex; }

// Cardputer SD card over SPI (SPI2_HOST): SCLK=40, MOSI=14, MISO=39, CS=12
#define SD_SPI_HOST SPI2_HOST
#define SD_CLK     GPIO_NUM_40
#define SD_MOSI    GPIO_NUM_14
#define SD_MISO    GPIO_NUM_39
#define SD_CS      GPIO_NUM_12

JournalStorage g_journal;

bool JournalStorage::begin() {
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 16;
    mount_config.allocation_unit_size = 16 * 1024;

    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = SD_MOSI;
    bus_cfg.miso_io_num = SD_MISO;
    bus_cfg.sclk_io_num = SD_CLK;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 16384;

    esp_err_t ret = spi_bus_initialize(SD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "SD SPI bus already initialized, reusing host %d", SD_SPI_HOST);
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD SPI bus init failed: %d", ret);
        mounted_ = false;
        return false;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_CS;
    slot_config.host_id = SD_SPI_HOST;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;

    sdmmc_card_t *card = nullptr;
    ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card mount failed: %d", ret);
        mounted_ = false;
        return false;
    }

    if (card) {
        sdmmc_card_print_info(stdout, card);
        mounted_ = true;
    }

    if (!s_sd_mutex) s_sd_mutex = xSemaphoreCreateRecursiveMutex();
    ensureDir();
    ESP_LOGI(TAG, "SD card ready at %s", basePath().c_str());
    return true;
}

void JournalStorage::deinit() {
    if (mounted_) {
        esp_vfs_fat_sdcard_unmount("/sdcard", nullptr);
        spi_bus_free(SD_SPI_HOST);
        mounted_ = false;
    }
}

std::string JournalStorage::basePath() {
    return "/sdcard/pjournal";
}

void JournalStorage::ensureDir() {
    mkdir("/sdcard/pjournal", 0777);
}

void JournalStorage::scanIndex() {
    m_fileIndex.clear();
    m_dateSet.clear();
    DIR *dir = opendir(basePath().c_str());
    if (dir) {
        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            if (de->d_type != DT_REG) continue;
            std::string fn = de->d_name;
            if (fn[0] == '.' || !isJournalExt(fn)) continue;
            m_fileIndex.push_back(fn);
            m_dateSet.insert(fn.substr(0, 10));
        }
        closedir(dir);
    }
    // Newest first
    std::sort(m_fileIndex.begin(), m_fileIndex.end(), std::greater<std::string>());
    m_indexValid = true;
}

void JournalStorage::ensureIndex() {
    if (!m_indexValid) scanIndex();
}

void JournalStorage::indexAddFile(const std::string &fn) {
    // Overwrite case (editor save, sync download of existing file) — already indexed
    for (const auto &f : m_fileIndex) {
        if (f == fn) return;
    }
    auto it = std::lower_bound(m_fileIndex.begin(), m_fileIndex.end(), fn, std::greater<std::string>());
    m_fileIndex.insert(it, fn);
    m_dateSet.insert(fn.substr(0, 10));
}

void JournalStorage::indexRemoveFile(const std::string &fn) {
    auto &v = m_fileIndex;
    v.erase(std::remove(v.begin(), v.end(), fn), v.end());
    std::string date = fn.substr(0, 10);
    for (const auto &f : v) {
        if (f.substr(0, 10) == date) return;  // date still has other files
    }
    m_dateSet.erase(date);
}

bool JournalStorage::saveEntry(const std::string &text) {
    if (!mounted_) return false;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
    ensureDir();
    time_t now;
    time(&now);
    struct tm *tm = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d_%H%M%S", tm);
    std::string fname = std::string(ts) + ".txt";

    FILE *f = fopen((basePath() + "/" + fname).c_str(), "w");
    if (!f) { if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex); return false; }
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
    ESP_LOGI(TAG, "Saved: %s", fname.c_str());
    if (m_indexValid) indexAddFile(fname);
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return true;
}

bool JournalStorage::saveEntryRaw(const std::string &filename, const std::string &content) {
    if (!mounted_) return false;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
    ensureDir();
    FILE *f = fopen((basePath() + "/" + filename).c_str(), "w");
    if (!f) { if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex); return false; }
    fwrite(content.data(), 1, content.size(), f);
    fclose(f);
    if (m_indexValid) indexAddFile(filename);
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return true;
}

std::vector<JournalEntry> JournalStorage::listEntries() {
    std::vector<JournalEntry> entries;
    if (!mounted_) return entries;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);

    // Rebuild index to pick up external changes; index is already newest-first
    scanIndex();

    for (const auto &fn : m_fileIndex) {
        JournalEntry e;
        e.filename = fn;
        e.date = fn.substr(0, 10);

        // Read only first 512 bytes for preview extraction (performance optimization)
        std::string content;
        std::string path = basePath() + "/" + fn;
        FILE *f = fopen(path.c_str(), "r");
        if (f) {
            char buf[512];
            int len = fread(buf, 1, sizeof(buf) - 1, f);
            if (len > 0) {
                buf[len] = 0;
                content = buf;
            }
            fclose(f);
        }

        if (!content.empty()) {
            size_t pos = content.find('\n');
            if (pos != std::string::npos) {
                std::string first = content.substr(0, pos);
                if (first.find("提示词:") == 0)
                    e.title = "提示写作";
                else if (first.find("自由写作") != std::string::npos)
                    e.title = "自由写作";
                size_t body_start = content.find("\n\n");
                if (body_start != std::string::npos && body_start < content.size()) {
                    std::string body = content.substr(body_start + 2);
                    std::string preview_text;
                    size_t start = 0;
                    while (start < body.size()) {
                        size_t nl = body.find('\n', start);
                        std::string line = (nl != std::string::npos) ? body.substr(start, nl - start) : body.substr(start);
                        // Skip metadata lines and prompt label
                        if (!line.empty() &&
                            line.find("日期:") != 0 &&
                            line.find("字数:") != 0 &&
                            line.find("提示词:") != 0 &&
                            line.find("自由写作") == std::string::npos) {
                            preview_text = line;
                            break;
                        }
                        if (nl == std::string::npos) break;
                        start = nl + 1;
                    }
                    e.preview = preview_text.substr(0, 40);
                }
            }
        }
        entries.push_back(e);
    }
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return entries;
}

std::vector<std::pair<std::string, time_t>> JournalStorage::listFileMtimes() {
    std::vector<std::pair<std::string, time_t>> result;
    if (!mounted_) return result;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);

    DIR *dir = opendir(basePath().c_str());
    if (!dir) { if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex); return result; }

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_type != DT_REG) continue;
        std::string fn = de->d_name;
        if (fn[0] == '.') continue;
        if (fn.size() >= 2 && fn[0] == '_' && fn[1] == '_') continue;  // skip temp files
        if (!isJournalExt(fn)) continue;

        struct stat st;
        std::string full = basePath() + "/" + fn;
        if (stat(full.c_str(), &st) == 0) {
            result.push_back({fn, st.st_mtime});
        }
    }
    closedir(dir);
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return result;
}

std::string JournalStorage::readEntry(const std::string &filename) {
    if (!mounted_) return "";
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
    FILE *f = fopen((basePath() + "/" + filename).c_str(), "r");
    if (!f) { if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex); return ""; }
    std::string result;
    char buf[256];
    int n;
    while ((n = fread(buf, 1, sizeof(buf) - 1, f)) > 0) {
        buf[n] = 0;
        result += buf;
    }
    fclose(f);
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return result;
}

// ── 快捷编辑文件(SD根目录)───────────────────────────────────────────────
std::string JournalStorage::readQuickFile(int index) {
    if (!mounted_ || index < 0 || index > 9) return "";
    char fname[16];
    snprintf(fname, sizeof(fname), "/sdcard/%d.txt", index);
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
    FILE *f = fopen(fname, "r");
    std::string result;
    if (f) {
        char buf[256];
        int n;
        while ((n = fread(buf, 1, sizeof(buf) - 1, f)) > 0) {
            buf[n] = 0;
            result += buf;
        }
        fclose(f);
    }
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return result;
}

bool JournalStorage::saveQuickFile(int index, const std::string &content) {
    if (!mounted_ || index < 0 || index > 9) return false;
    char fname[16];
    snprintf(fname, sizeof(fname), "/sdcard/%d.txt", index);
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
    FILE *f = fopen(fname, "w");
    if (!f) { if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex); return false; }
    fwrite(content.data(), 1, content.size(), f);
    fclose(f);
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return true;
}

void JournalStorage::ensureQuickFiles() {
    if (!mounted_) return;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
    for (int i = 0; i < 10; i++) {
        char fname[16];
        snprintf(fname, sizeof(fname), "/sdcard/%d.txt", i);
        FILE *f = fopen(fname, "r");
        if (!f) {
            f = fopen(fname, "w");
            if (f) fclose(f);
        } else {
            fclose(f);
        }
    }
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
}

bool JournalStorage::deleteEntry(const std::string &filename) {
    if (!mounted_) return false;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
    bool ok = remove((basePath() + "/" + filename).c_str()) == 0;
    if (ok && m_indexValid) indexRemoveFile(filename);
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return ok;
}

bool JournalStorage::hasEntry(const std::string &date) {
    if (!mounted_) return false;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
    ensureIndex();
    bool found = m_dateSet.count(date) > 0;
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return found;
}

int JournalStorage::countToday() {
    if (!mounted_) return 0;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
    ensureIndex();

    time_t now; time(&now);
    struct tm *tm = localtime(&now);
    char today[16];
    strftime(today, sizeof(today), "%Y-%m-%d", tm);

    int count = 0;
    for (const auto &fn : m_fileIndex) {
        if (fn.substr(0, 10) == today) count++;
    }
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return count;
}

int JournalStorage::getStreak() {
    if (!mounted_) return 0;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
    ensureIndex();

    int streak = 0;
    time_t now; time(&now);
    for (int i = 0; i < 365; i++) {
        time_t t = now - i * 86400;
        struct tm *tm2 = localtime(&t);
        char date[16];
        strftime(date, sizeof(date), "%Y-%m-%d", tm2);
        if (m_dateSet.count(date))
            streak++;
        else
            break;
    }
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return streak;
}

int JournalStorage::totalEntries() {
    if (!mounted_) return 0;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
    ensureIndex();
    int count = (int)m_fileIndex.size();
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return count;
}
