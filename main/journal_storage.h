#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>
#include <unordered_set>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

struct JournalEntry {
    std::string filename;   // YYYY-MM-DD_HHMMSS.txt
    std::string date;       // YYYY-MM-DD
    std::string title;      // prompt or "自由写作"
    std::string preview;    // first line of body
    std::string full_text;  // full file content
};

class JournalStorage {
public:
    bool begin();
    void deinit();

    // Save a new entry
    bool saveEntry(const std::string &text);

    // Save entry with explicit filename (for sync downloads)
    bool saveEntryRaw(const std::string &filename, const std::string &content);

    // List all entries, newest first
    std::vector<JournalEntry> listEntries();

    // List filenames with modification times (for sync)
    std::vector<std::pair<std::string, time_t>> listFileMtimes();

    // Read entry content by filename
    std::string readEntry(const std::string &filename);

    // Delete entry by filename
    bool deleteEntry(const std::string &filename);

    // Check if date has entry
    bool hasEntry(const std::string &date);

    // Count entries for today
    int countToday();

    // Calculate streak
    int getStreak();

    // Total entries
    int totalEntries();

    // SD card status
    bool isMounted() const { return mounted_; }

    // 快捷编辑文件(SD根目录 0.txt..9.txt)
    std::string readQuickFile(int index);
    bool saveQuickFile(int index, const std::string &content);
    void ensureQuickFiles();  // 缺哪个文件就创建哪个(空文件)

    // Get SD card mutex for thread-safe access from external callers
    static SemaphoreHandle_t sdMutex();

private:
    std::string basePath();
    void ensureDir();

    // In-memory index of journal filenames (newest first) + distinct dates.
    // Avoids repeated full-directory scans during boot and main screen draw.
    void scanIndex();                  // rebuild both index structures
    void ensureIndex();                // lazy rebuild if invalid
    void indexAddFile(const std::string &fn);
    void indexRemoveFile(const std::string &fn);
    std::vector<std::string> m_fileIndex;      // newest first
    std::unordered_set<std::string> m_dateSet; // distinct YYYY-MM-DD dates
    bool m_indexValid = false;

    bool mounted_ = false;
};

extern JournalStorage g_journal;
