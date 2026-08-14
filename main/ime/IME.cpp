#include "IME.h"
#include "seg_table.h"
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <esp_log.h>
#include <sys/stat.h>

static const char *IME_TAG = "IME";
static const uint8_t IME_MAGIC[4] = {'I', 'M', 'E', '3'};

// 用户词典持久化到 SD 卡(与设置同目录)。NVS 分区仅 24KB 且写入失败会静默
// 丢失/启动时整区擦除,改用 SD 文件后容量无上限,重启与重刷固件均保留。
static const char *USERDICT_PATH = "/sdcard/settings/userdict.txt";

static void appendUtf8(uint32_t cp, std::string &out) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

// Embedded dictionary symbols (from CMakeLists EMBED_FILES "ime/ime_table_pinyin.bin")
extern const uint8_t ime_table_pinyin_bin_start[] asm("_binary_ime_table_pinyin_bin_start");
extern const uint8_t ime_table_pinyin_bin_end[]   asm("_binary_ime_table_pinyin_bin_end");

// Embedded liangfen dictionary
extern const uint8_t liangfen_bin_start[] asm("_binary_liangfen_bin_start");
extern const uint8_t liangfen_bin_end[]   asm("_binary_liangfen_bin_end");

static inline std::string str_trim(const std::string &s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

bool IME::parseHeader(const uint8_t *hdrIndex, size_t total) {
    if (memcmp(hdrIndex, IME_MAGIC, 4) != 0) {
        ESP_LOGE(IME_TAG, "bad dictionary magic");
        return false;
    }
    _scheme = (Scheme)hdrIndex[4];
    _codeLen = hdrIndex[5];
    if (_codeLen < 1 || _codeLen > MAX_CODE_LEN) {
        ESP_LOGE(IME_TAG, "bad codeLen %d", _codeLen);
        return false;
    }
    _recordSize = _codeLen + HANZI_SIZE + FLAG_SIZE;
    switch (_scheme) {
    case PINYIN:    _maxCode = 20; break;
    case SHUANGPIN: _maxCode = 2; break;
    case WUBI:
    default:        _maxCode = 4; break;
    }
    _count = (uint32_t)hdrIndex[8] | ((uint32_t)hdrIndex[9] << 8) |
             ((uint32_t)hdrIndex[10] << 16) | ((uint32_t)hdrIndex[11] << 24);
    _recordBase = HEADER_SIZE + (size_t)INDEX_ENTRIES * 4;
    size_t need = _recordBase + (size_t)_count * _recordSize;
    if (_count == 0 || need > total) {
        ESP_LOGE(IME_TAG, "dictionary size mismatch: need %u, have %u",
                 (unsigned)need, (unsigned)total);
        return false;
    }
    _index.resize(INDEX_ENTRIES);
    for (int k = 0; k < INDEX_ENTRIES; k++) {
        const uint8_t *p = hdrIndex + HEADER_SIZE + (size_t)k * 4;
        _index[k] = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
    size_t wordBase = _recordBase + (size_t)_count * _recordSize;
    if (wordBase + 4 <= total) {
        const uint8_t *wp = hdrIndex + wordBase;
        _wordCount = (uint32_t)wp[0] | ((uint32_t)wp[1] << 8) |
                     ((uint32_t)wp[2] << 16) | ((uint32_t)wp[3] << 24);
        if (_wordCount > 0 && wordBase + 4 + INDEX_ENTRIES * 4 <= total) {
            wp += 4;
            _wordIndex.resize(INDEX_ENTRIES);
            for (int k = 0; k < INDEX_ENTRIES; k++) {
                const uint8_t *p = wp + (size_t)k * 4;
                _wordIndex[k] = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                                ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
            }
            wp += INDEX_ENTRIES * 4;
            _wordData = wp;
            _wordDataSize = total - (_recordBase + _count * _recordSize)
                            - 4 - INDEX_ENTRIES * 4;
            size_t predBase = wordBase + 4 + INDEX_ENTRIES * 4 + _wordDataSize;
            if (predBase + 4 <= total) {
                const uint8_t *pp = hdrIndex + predBase;
                _predCount = (uint32_t)pp[0] | ((uint32_t)pp[1] << 8) |
                             ((uint32_t)pp[2] << 16) | ((uint32_t)pp[3] << 24);
                if (_predCount > 0) {
                    _predData = pp + 4;
                    _predDataSize = total - predBase - 4;
                }
            }
        }
    }
    return true;
}

bool IME::begin() {
    if (_loaded) return true;
    _blob = ime_table_pinyin_bin_start;
    _blobSize = (size_t)(ime_table_pinyin_bin_end - ime_table_pinyin_bin_start);
    if (_blobSize < HEADER_SIZE || !parseHeader(_blob, _blobSize)) {
        _blob = nullptr;
        return false;
    }
    _loaded = true;
    loadUserDict();
    static const char *NAMES[] = {"Wubi", "Pinyin", "Shuangpin"};
    ESP_LOGI(IME_TAG, "ready: %s, %u records, codeLen %d",
             NAMES[_scheme <= SHUANGPIN ? _scheme : 0], (unsigned)_count, _codeLen);
    return true;
}

void IME::loadUserDict() {
    _userWords.clear();
    FILE *f = fopen(USERDICT_PATH, "r");
    if (!f) return;

    // Read the whole file
    std::string allData;
    char buf[256];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) allData.append(buf, n);
    fclose(f);
    if (allData.empty()) return;

    // Parse lines (same format as before: "code word count" per line)
    const size_t MAX_READ = 64 * 1024;
    bool hadDuplicates = false;
    size_t pos = 0;
    size_t bytesRead = 0;
    while (pos < allData.length() && bytesRead < MAX_READ) {
        size_t nl = allData.find('\n', pos);
        std::string line;
        if (nl == std::string::npos) {
            line = allData.substr(pos);
            pos = allData.length();
        } else {
            line = allData.substr(pos, nl - pos);
            pos = nl + 1;
        }
        bytesRead += line.length() + 1;
        line = str_trim(line);
        if (line.length() < 3) continue;
        auto sp1 = line.find(' ');
        if (sp1 == std::string::npos || sp1 < 1) continue;
        std::string code = line.substr(0, sp1);
        auto sp2 = line.find(' ', sp1 + 1);
        std::string word;
        int count = 1;
        bool trad = false;
        if (sp2 != std::string::npos) {
            word = line.substr(sp1 + 1, sp2 - sp1 - 1);
            std::string tail = line.substr(sp2 + 1);
            auto sp3 = tail.find(' ');
            // 纯数字解析:std::stoi 遇到非数字会抛异常,而本工程 C++ 异常关闭,
            // 畸形/被手工编辑的词库文件会让 stoi 直接 abort 重启。非法计数回退为 1。
            count = 1;
            std::string cntStr = (sp3 == std::string::npos) ? tail : tail.substr(0, sp3);
            bool validCount = !cntStr.empty();
            for (char cc : cntStr)
                if (cc < '0' || cc > '9') { validCount = false; break; }
            if (validCount) {
                count = 0;
                for (char cc : cntStr) count = count * 10 + (cc - '0');
            }
            if (count < 1) count = 1;
            if (sp3 != std::string::npos) {
                std::string fl = tail.substr(sp3 + 1);
                if (fl == "1" || fl == "t" || fl == "T") trad = true;
            }
        } else {
            word = line.substr(sp1 + 1);
        }
        if (code.length() >= 1 && word.length() >= 2) {
            bool isDup = false;
            for (auto &existing : _userWords) {
                if (existing.code == code && existing.word == word && existing.trad == trad) {
                    isDup = true;
                    hadDuplicates = true;
                    if (existing.count < count) existing.count = count;
                    break;
                }
            }
            if (!isDup) _userWords.push_back({code, word, count, trad});
        }
    }
    if (hadDuplicates) {
        // 只在真正合并了计数时才需要保存
        _userDirty = true;
        saveUserDict();
    } else {
        _userDirty = false;  // 无重复，无需保存
    }
    if (_userWords.size() > 0)
        ESP_LOGI(IME_TAG, "loaded %zu user words", _userWords.size());
}

void IME::saveUserDict() {
    if (!_userDirty) return;
    mkdir("/sdcard/settings", 0777);
    FILE *f = fopen(USERDICT_PATH, "w");
    if (!f) {
        ESP_LOGE(IME_TAG, "failed to open userdict file %s", USERDICT_PATH);
        return;
    }

    for (auto &p : _userWords) {
        std::string line = p.code + " " + p.word + " " + std::to_string(p.count)
                         + (p.trad ? " 1" : "") + "\n";
        fwrite(line.data(), 1, line.size(), f);
    }

    if (fclose(f) != 0)
        ESP_LOGE(IME_TAG, "failed to flush userdict file");
    _userDirty = false;
}

void IME::addUserWord(const std::string &code, const std::string &word) {
    if (word.length() < 3 || code.length() == 0) return;
    for (auto &p : _userWords)
        if (p.code == code && p.word == word && p.trad == _trad) return;
    if (_userWords.size() >= 1000)
        _userWords.erase(_userWords.begin());
    _userWords.push_back({code, word, 0, _trad});
    _userDirty = true;
    saveUserDict();
}

void IME::removeUserWord(const std::string &code, const std::string &word) {
    for (auto it = _userWords.begin(); it != _userWords.end(); ++it) {
        if (it->code == code && it->word == word && it->trad == _trad) {
            _userWords.erase(it);
            _userDirty = true;
            saveUserDict();
            return;
        }
    }
}

void IME::clearUserDict() {
    if (_userWords.empty()) return;
    _userWords.clear();
    _userDirty = true;
    saveUserDict();
}

void IME::pruneUserDict(int minCount) {
    size_t before = _userWords.size();
    auto it = _userWords.begin();
    while (it != _userWords.end()) {
        if (it->count < minCount) {
            it = _userWords.erase(it);
            _userDirty = true;
        } else ++it;
    }
    if (_userDirty) saveUserDict();
}

void IME::loadLfDict() {
    if (_lfBlob) return;
    _lfBlob = liangfen_bin_start;
    size_t total = (size_t)(liangfen_bin_end - liangfen_bin_start);
    if (total < 1354 + 16) { _lfBlob = nullptr; return; }

    // Parse index: 677 × uint16 LE
    _lfIndex.resize(INDEX_ENTRIES);
    for (int k = 0; k < INDEX_ENTRIES; k++) {
        _lfIndex[k] = (uint16_t)_lfBlob[k * 2] | ((uint16_t)_lfBlob[k * 2 + 1] << 8);
    }

    _lfRecordBase = INDEX_ENTRIES * 2;  // 1354
    _lfCount = (uint32_t)((total - _lfRecordBase) / 16);
    ESP_LOGI(IME_TAG, "Liangfen dict loaded: %u records", (unsigned)_lfCount);
}

void IME::searchLfWindow(const char *code, int len, uint32_t &lo, uint32_t &hi) {
    lo = 0; hi = _lfCount;
    if (_lfIndex.empty() || len < 1) return;
    int c0 = code[0] - 'a'; if (c0 < 0 || c0 >= 26) return;
    if (len == 1) { lo = _lfIndex[c0*26]; hi = _lfIndex[(c0+1)*26]; return; }
    int c1 = code[1] - 'a'; if (c1 < 0 || c1 >= 26) return;
    int k = c0 * 26 + c1;
    lo = _lfIndex[k]; hi = _lfIndex[k + 1];
}

bool IME::readLfCode(uint16_t i, char out[13]) {
    if (!_lfBlob || i >= _lfCount) return false;
    const uint8_t *rec = _lfBlob + _lfRecordBase + i * 16;
    int n = 0;
    for (; n < 12 && rec[n]; n++) out[n] = (char)rec[n];
    out[n] = '\0';
    return true;
}

bool IME::readLfHanzi(uint16_t i, char out[4]) {
    if (!_lfBlob || i >= _lfCount) return false;
    const uint8_t *rec = _lfBlob + _lfRecordBase + i * 16 + 12;
    out[0] = (char)rec[0]; out[1] = (char)rec[1]; out[2] = (char)rec[2]; out[3] = '\0';
    return true;
}

void IME::bumpFrequency(const std::string &code, const std::string &word) {
    for (auto it = _userWords.begin(); it != _userWords.end(); ++it) {
        if (it->code == code && it->word == word) {
            it->count++;
            _userDirty = true;
            saveUserDict();
            return;
        }
    }
    if (word.length() >= 3 && code.length() >= 1) {
        addUserWord(code, word);
        for (auto &p : _userWords)
            if (p.code == code && p.word == word) { p.count++; break; }
    }
}

bool IME::readCode(uint32_t i, char out[MAX_CODE_LEN + 1]) {
    const uint8_t *rec = _blob + _recordBase + (size_t)i * _recordSize;
    int n = 0;
    for (; n < _codeLen && rec[n]; n++) out[n] = (char)rec[n];
    out[n] = '\0';
    return true;
}

bool IME::readHanzi(uint32_t i, char out[HANZI_SIZE + 1]) {
    const uint8_t *rec = _blob + _recordBase + (size_t)i * _recordSize + _codeLen;
    out[0] = (char)rec[0]; out[1] = (char)rec[1];
    out[2] = (char)rec[2]; out[3] = '\0';
    return true;
}

uint8_t IME::readRecordFlag(uint32_t i) {
    const uint8_t *rec = _blob + _recordBase + (size_t)i * _recordSize;
    return rec[_codeLen + HANZI_SIZE];
}

uint8_t IME::readLfFlag(uint16_t i) {
    const uint8_t *rec = _lfBlob + _lfRecordBase + (size_t)i * 16;
    return rec[15];
}

void IME::setActive(bool on) {
    _active = on;
    if (on) loadUserDict();
    reset();
}

void IME::reset() {
    _code.clear();
    _displayCodeDirty = true;
    _all.clear();
    _candLen.clear();
    if (_all.capacity() > MAX_CANDIDATES * 2) _all.shrink_to_fit();
    else if (_all.capacity() < MAX_CANDIDATES / 3) _all.reserve(MAX_CANDIDATES / 3);
    _page.clear();
    if (_page.capacity() > _pageSize * 2) _page.shrink_to_fit();
    else if (_page.capacity() < _pageSize) _page.reserve(_pageSize);
    _pageStart = 0;
    _curPage = 0;
    _prefix.clear();
    _remainder.clear();
    _lfMode = false;
    _deleteMode = false;
}

int IME::pinyinPrefixLen(const std::string &code) {
    int i = 0;
    while (i < (int)code.length() && code[i] >= 'a' && code[i] <= 'z') i++;
    return i;
}

void IME::searchWindow(const char *code, int len, uint32_t &lo, uint32_t &hi) {
    lo = 0; hi = _count;
    if (_index.empty() || len < 1) return;
    int c0 = code[0] - 'a';
    if (c0 < 0 || c0 >= 26) return;
    if (len == 1) {
        lo = _index[c0 * 26];
        hi = _index[(c0 + 1) * 26];
        return;
    }
    int c1 = code[1] - 'a';
    if (c1 < 0 || c1 >= 26) return;
    int k = c0 * 26 + c1;
    lo = _index[k];
    hi = _index[k + 1];
}

void IME::lookup() {
    _all.clear();
    _candLen.clear();
    _pageStart = 0;
    _curPage = 0;
    _maxMatchLen = 0;
    if (_prefix.length() == 0) _codeOrig = _code;
    static bool dictLoaded = false;
    if (!dictLoaded) { dictLoaded = true; loadUserDict(); loadLfDict(); }

    if (!_loaded || (_code.length() == 0 && !_deleteMode)) {
        buildPage();
        return;
    }

    // 单引号编码分词: 显式按音节分段匹配(词组/补充表/用户词典)
    if (_code.find('\'') != std::string::npos) {
        lookupSegmented();
        buildPage();
        return;
    }

    const char *q = _code.c_str();
    int qlen = (int)_code.length();

    int pinyinLen = pinyinPrefixLen(_code);
    std::string pinyinCode = _code.substr(0, pinyinLen);

    // First char uppercase: treat as literal, use partial match for remainder
    if (pinyinLen == 0 && _code.length() > 0) {
        _all.push_back(_code.substr(0, 1));
        _candLen.push_back(0);
        _partialStart = 0;
        _remainder = _code.substr(1);
        buildPage();
        return;
    }

    q = pinyinCode.c_str();
    qlen = pinyinLen;

    if (_lfMode && _lfBlob) {
        uint32_t llo, lhi;
        searchLfWindow(q, qlen, llo, lhi);
        while (llo < lhi) {
            uint32_t mid = llo + (lhi - llo) / 2;
            char code[13]; if (!readLfCode(mid, code)) break;
            if (strncmp(code, q, qlen) < 0) llo = mid + 1;
            else lhi = mid;
        }
        for (uint32_t i = llo; i < _lfCount && _all.size() < MAX_CANDIDATES; i++) {
            char code[13]; if (!readLfCode(i, code)) break;
            if (strncmp(code, q, qlen) != 0) break;
            uint8_t f = readLfFlag(i);
            if (f & (_trad ? 0x01 : 0x02)) continue;
            char hz[4]; if (!readLfHanzi(i, hz)) break;
            std::string h(hz);
            bool dup = false;
            for (auto &e : _all) if (e == h) { dup = true; break; }
            if (!dup) {
                _all.push_back(h);
                _candLen.push_back(0);
            }
        }
        buildPage();
        return;
    }

    if (_deleteMode) {
        std::vector< std::pair<int, std::string> > userMatches;
        for (auto &p : _userWords) {
            if (p.trad != _trad) continue;
            if (qlen == 0 ||
                ((int)p.code.length() >= qlen && strncmp(p.code.c_str(), q, qlen) == 0)) {
                bool found = false;
                for (auto &m : userMatches) {
                    if (m.second == p.word) {
                        found = true;
                        if (m.first < p.count) m.first = p.count;
                        break;
                    }
                }
                if (!found) userMatches.push_back({p.count, p.word});
            }
        }
        std::sort(userMatches.begin(), userMatches.end(),
            [](const std::pair<int,std::string> &a, const std::pair<int,std::string> &b) {
                return a.first > b.first;
            });
        for (auto &m : userMatches) {
            _all.push_back(m.second);
            _candLen.push_back(0);
            if (_all.size() >= MAX_CANDIDATES) break;
        }
        buildPage();
        return;
    }

    // Phase 1: user dict single chars — highest priority (phrases emitted in Phase 3)
    std::vector< std::pair<int, std::string> > userWordFreq;
    {
        std::vector< std::pair<int, std::string> > userSingleFreq;
        for (auto &p : _userWords) {
            if (p.trad != _trad) continue;
            if ((int)p.code.length() < qlen) continue;
            if (strncmp(p.code.c_str(), q, qlen) != 0) continue;
            if (p.word.length() <= 3) {
                // single char
                bool found = false;
                for (auto &uf : userSingleFreq) {
                    if (uf.second == p.word) {
                        found = true;
                        if (uf.first < p.count) uf.first = p.count;
                        break;
                    }
                }
                if (!found) userSingleFreq.push_back({p.count, p.word});
            } else {
                // phrase
                bool found = false;
                for (auto &uf : userWordFreq) {
                    if (uf.second == p.word) {
                        found = true;
                        if (uf.first < p.count) uf.first = p.count;
                        break;
                    }
                }
                if (!found) userWordFreq.push_back({p.count, p.word});
            }
        }
        std::sort(userSingleFreq.begin(), userSingleFreq.end(),
            [](const std::pair<int,std::string> &a, const std::pair<int,std::string> &b) {
                return a.first > b.first;
            });
        for (auto &f : userSingleFreq) {
            _all.push_back(f.second);
            _candLen.push_back(0);
            if (_all.size() >= MAX_CANDIDATES) break;
        }
        if (_all.size() >= MAX_CANDIDATES) { buildPage(); return; }
    }

    // Phase 2: single char prefix match (dictionary)
    bool hasVowel = false;
    for (int i = 0; i < qlen; i++) {
        if (strchr("aeiouv", q[i])) { hasVowel = true; break; }
    }
    if (hasVowel) {
        uint32_t lo, hi;
        searchWindow(q, qlen, lo, hi);
        uint32_t scanEnd = hi;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            char code[7];
            if (!readCode(mid, code)) break;
            if (strncmp(code, q, qlen) < 0) lo = mid + 1;
            else hi = mid;
        }
        for (uint32_t i = lo; i < scanEnd && _all.size() < MAX_CANDIDATES; i++) {
            char code[7];
            if (!readCode(i, code)) break;
            if (strncmp(code, q, qlen) != 0) break;
            uint8_t f = readRecordFlag(i);
            if (f & (_trad ? 0x01 : 0x02)) continue;
            char hz[4];
            if (!readHanzi(i, hz)) break;
            std::string h(hz);
            bool dup = false;
            for (auto &e : _all) if (e == h) { dup = true; break; }
            if (!dup) {
                _all.push_back(h);
                _candLen.push_back((int)strlen(code));
                if ((int)strlen(code) > _maxMatchLen) _maxMatchLen = (int)strlen(code);
            }
        }
    }
    if (_all.size() >= MAX_CANDIDATES) { buildPage(); return; }

    // 补充词典表全码精确匹配: xian→西安, anguang→暗光 (优先级: 单字之后, 词组之前)
    if (_all.size() < MAX_CANDIDATES) {
        for (int i = 0; i < SEG_TABLE_COUNT && _all.size() < MAX_CANDIDATES; i++) {
            std::string entryCode;
            for (const char *p = SEG_TABLE[i].syllables; *p; p++)
                if (*p != ' ') entryCode += *p;
            if ((int)entryCode.length() != qlen ||
                strncmp(entryCode.c_str(), q, qlen) != 0) continue;
            std::string w = SEG_TABLE[i].word;
            bool dup = false;
            for (auto &e : _all) if (e == w) { dup = true; break; }
            if (!dup) {
                _all.push_back(w);
                _candLen.push_back(qlen);
                if (qlen > _maxMatchLen) _maxMatchLen = qlen;
            }
        }
    }
    if (_all.size() >= MAX_CANDIDATES) { buildPage(); return; }

    // Phase 3: user dict phrases — after dictionary single chars, before dictionary phrases
    {
        std::sort(userWordFreq.begin(), userWordFreq.end(),
            [](const std::pair<int,std::string> &a, const std::pair<int,std::string> &b) {
                return a.first > b.first;
            });
        for (auto &f : userWordFreq) {
            _all.push_back(f.second);
            _candLen.push_back(0);
            if (_all.size() >= MAX_CANDIDATES) break;
        }
        if (_all.size() >= MAX_CANDIDATES) { buildPage(); return; }
    }
    size_t p4Start = _all.size();  // 词典词组排序起点(不含用户词组)

    // Phase 4: phrase prefix match (word dictionary)
    if (hasVowel && _wordCount > 0 && _wordData) {
        size_t wlo = 0, whi = _wordDataSize;
        if (qlen >= 2) {
            int k = (q[0] - 'a') * 26 + (q[1] - 'a');
            if (k >= 0 && k < INDEX_ENTRIES) {
                wlo = _wordIndex[k];
                whi = (k + 1 < INDEX_ENTRIES) ? _wordIndex[k + 1] : _wordDataSize;
            }
        }
        size_t wpos = wlo;
        int safety = 0;
        while (wpos < whi && _all.size() < MAX_CANDIDATES && safety++ < 5000) {
            uint8_t cl = _wordData[wpos];
            if (cl == 0 || wpos + 1 + cl > whi) break;
            const char *wc = (const char *)_wordData + wpos + 1;
            wpos += 1 + cl;
            if (wpos >= whi) break;
            uint8_t n = _wordData[wpos++];
            int matchLen = std::min((int)cl, qlen);
            if (strncmp(wc, q, matchLen) == 0) {
                for (uint8_t j = 0; j < n && wpos < whi; j++) {
                    uint8_t wl = _wordData[wpos++];
                    if (wl == 0 || wpos + wl + 1 > whi) break;
                    uint8_t wf = _wordData[wpos + wl];
                    if ((int)cl < qlen && wl <= 3) {
                        wpos += wl + 1;
                        continue;
                    }
                    if (!_trad || wf == 0) {
                        std::string w;
                        w.append((const char *)_wordData + wpos, wl);
                        bool dup = false;
                        for (auto &e : _all) if (e == w) { dup = true; break; }
                        if (!dup) {
                            _all.push_back(w);
                            _candLen.push_back(std::min((int)cl, qlen));
                            if ((int)cl > _maxMatchLen) _maxMatchLen = (int)cl;
                        }
                    }
                    wpos += wl + 1;
                }
            } else {
                for (uint8_t j = 0; j < n && wpos < whi; j++) {
                    uint8_t wl = _wordData[wpos++];
                    if (wpos + wl + 1 > whi) break;
                    wpos += wl + 1;
                }
            }
        }
    }
    // Sort Phase 4 entries by consumed length descending
    {
        size_t p4End = _all.size();
        size_t p4Count = p4End - p4Start;
        if (p4Count > 1) {
            std::vector<int> order(p4Count);
            for (size_t i = 0; i < order.size(); i++) order[i] = (int)(p4Start + i);
            std::stable_sort(order.begin(), order.end(),
                [this](int a, int b) { return _candLen[a] > _candLen[b]; });
            std::vector<std::string> sortedAll(_all.begin(), _all.begin() + p4Start);
            std::vector<int> sortedLen(_candLen.begin(), _candLen.begin() + p4Start);
            sortedAll.reserve(_all.size());
            sortedLen.reserve(_candLen.size());
            for (int i : order) {
                sortedAll.push_back(std::move(_all[i]));
                sortedLen.push_back(_candLen[i]);
            }
            _all.swap(sortedAll);
            _candLen.swap(sortedLen);
        }
    }
    if (_all.size() >= MAX_CANDIDATES) { buildPage(); return; }

    // Phase 5: user dict initial match
    if (!hasVowel && _userWords.size() > 0) {
        std::vector< std::pair<int, std::string> > userInitFreq;
        for (auto &p : _userWords) {
            if (p.trad != _trad) continue;
            if (p.code.find('\'') != std::string::npos) continue;  // 撇号码只在分词路径匹配
            int cl = (int)p.code.length();
            if (cl < 2) continue;
            const char *wc = p.code.c_str();
            char init[13]; int o = 0;
            for (int i = 0; i < cl && o < 12; ) {
                if (strchr("aeiouv", wc[i])) {
                    while (i < cl && strchr("aeiouvngr", wc[i]) && o < 12)
                        init[o++] = wc[i++];
                    continue;
                }
                if (i+1 < cl && (wc[i]=='z'||wc[i]=='c'||wc[i]=='s') && wc[i+1]=='h') {
                    init[o++] = wc[i];
                    init[o++] = wc[i+1];
                    i += 2;
                } else {
                    init[o++] = wc[i];
                    i++;
                }
                while (i < cl && strchr("aeiouv", wc[i])) i++;
                if (i < cl && strchr("ngr", wc[i])) {
                    int j = i;
                    while (j < cl && strchr("ngr", wc[j])) j++;
                    if (j >= cl || !strchr("aeiouv", wc[j])) i = j;
                }
            }
            init[o] = 0;
            if (o >= qlen && strncmp(init, q, qlen) == 0) {
                bool found = false;
                for (auto &uf : userInitFreq) {
                    if (uf.second == p.word) {
                        found = true;
                        if (uf.first < p.count) uf.first = p.count;
                        break;
                    }
                }
                if (!found) userInitFreq.push_back({p.count, p.word});
            }
        }
        std::sort(userInitFreq.begin(), userInitFreq.end(),
            [](const std::pair<int,std::string> &a, const std::pair<int,std::string> &b) {
                return a.first > b.first;
            });
        for (auto &f : userInitFreq) {
            _all.push_back(f.second);
            _candLen.push_back(0);
            if (_all.size() >= MAX_CANDIDATES) break;
        }
        if (_all.size() >= MAX_CANDIDATES) { buildPage(); return; }
    }

    // Phase 6: initial match (no vowel, consonant-only)
    if (!hasVowel && _wordCount > 0 && _wordData) {
        size_t slo = 0, shi = _wordDataSize;
        if (qlen >= 1 && q[0] >= 'a' && q[0] <= 'z') {
            int k = (q[0] - 'a') * 26;
            slo = _wordIndex[k];
            shi = (k + 26 < INDEX_ENTRIES) ? _wordIndex[k + 26] : _wordDataSize;
        }
        size_t spos = slo;
        int safety = 0;
        while (spos < shi && _all.size() < MAX_CANDIDATES && safety++ < 60000) {
            uint8_t cl = _wordData[spos];
            if (cl == 0 || spos + 1 + cl > shi) break;
            const char *wc = (const char *)_wordData + spos + 1;
            spos += 1 + cl;
            if (spos >= shi) break;
            uint8_t n = _wordData[spos++];
            char init[13]; int o = 0;
            for (int i = 0; i < cl && o < 12; ) {
                if (strchr("aeiouv", wc[i])) {
                    while (i < cl && strchr("aeiouvngr", wc[i]) && o < 12)
                        init[o++] = wc[i++];
                    continue;
                }
                if (i+1 < cl && (wc[i]=='z'||wc[i]=='c'||wc[i]=='s') && wc[i+1]=='h') {
                    init[o++] = wc[i];
                    init[o++] = wc[i+1];
                    i += 2;
                } else {
                    init[o++] = wc[i];
                    i++;
                }
                while (i < cl && strchr("aeiouv", wc[i])) i++;
                if (i < cl && strchr("ngr", wc[i])) {
                    int j = i;
                    while (j < cl && strchr("ngr", wc[j])) j++;
                    if (j >= cl || !strchr("aeiouv", wc[j])) i = j;
                }
            }
            init[o] = 0;
            if (o >= qlen && strncmp(init, q, qlen) == 0) {
                for (uint8_t j = 0; j < n && spos < shi; j++) {
                    uint8_t wl = _wordData[spos++];
                    if (wl == 0 || spos + wl + 1 > shi) break;
                    uint8_t wf = _wordData[spos + wl];
                    if (_all.size() < MAX_CANDIDATES && (!_trad || wf == 0)) {
                        std::string w;
                        w.append((const char *)_wordData + spos, wl);
                        bool dup = false;
                        for (auto &e : _all) if (e == w) { dup = true; break; }
                        if (!dup) {
                            _all.push_back(w);
                            _candLen.push_back((int)cl);
                            if ((int)cl > _maxMatchLen) _maxMatchLen = (int)cl;
                        }
                    }
                    spos += wl + 1;
                }
            } else {
                for (uint8_t j = 0; j < n && spos < shi; j++) {
                    uint8_t wl = _wordData[spos++];
                    if (spos + wl + 1 > shi) break;
                    spos += wl + 1;
                }
            }
        }
    }

    // Phase 7: shorthand + tail match
    {
        bool shorthandTail = false;
        std::string typedInit;
        std::string typedTail;
        if (qlen >= 3 && hasVowel) {
            int lastSylStart = qlen;
            for (int i = qlen - 1; i >= 1; i--) {
                if (strchr("aeiouv", q[i])) {
                    int j = i;
                    while (j > 0 && strchr("aeiouv", q[j-1])) j--;
                    if (j > 0 && strchr("bcdfghjklmnpqrstwxyz", q[j-1])) {
                        lastSylStart = j;
                        break;
                    }
                }
            }
            if (lastSylStart >= 2 && lastSylStart < qlen) {
                bool isPureConsonant = true;
                for (int i = 0; i < lastSylStart; i++) {
                    if (strchr("aeiouv", q[i])) { isPureConsonant = false; break; }
                }
                if (isPureConsonant) {
                    shorthandTail = true;
                    typedInit = std::string(q, lastSylStart);
                    typedTail = std::string(q + lastSylStart, qlen - lastSylStart);
                }
            }
        }
        if (shorthandTail && _wordCount > 0 && _wordData && _all.size() < MAX_CANDIDATES) {
            size_t slo = 0, shi = _wordDataSize;
            if (typedInit.length() >= 1 && typedInit[0] >= 'a' && typedInit[0] <= 'z') {
                int k = (typedInit[0] - 'a') * 26;
                slo = _wordIndex[k];
                shi = (k + 26 < INDEX_ENTRIES) ? _wordIndex[k + 26] : _wordDataSize;
            }
            size_t spos = slo;
            int safety = 0;
            while (spos < shi && _all.size() < MAX_CANDIDATES && safety++ < 60000) {
                uint8_t cl = _wordData[spos];
                if (cl == 0 || spos + 1 + cl > shi) break;
                const char *wc = (const char *)_wordData + spos + 1;
                spos += 1 + cl;
                if (spos >= shi) break;
                uint8_t n = _wordData[spos++];
                char init[13]; int o = 0;
                for (int i = 0; i < cl && o < 12; ) {
                    if (strchr("aeiouv", wc[i])) {
                        while (i < cl && strchr("aeiouvngr", wc[i]) && o < 12)
                            init[o++] = wc[i++];
                        continue;
                    }
                    if (i+1 < cl && (wc[i]=='z'||wc[i]=='c'||wc[i]=='s') && wc[i+1]=='h') {
                        init[o++] = wc[i];
                        init[o++] = wc[i+1];
                        i += 2;
                    } else {
                        init[o++] = wc[i];
                        i++;
                    }
                    while (i < cl && strchr("aeiouv", wc[i])) i++;
                    if (i < cl && strchr("ngr", wc[i])) {
                        int j = i;
                        while (j < cl && strchr("ngr", wc[j])) j++;
                        if (j >= cl || !strchr("aeiouv", wc[j])) i = j;
                    }
                }
                init[o] = 0;
                if (o < (int)typedInit.length() || strncmp(init, typedInit.c_str(), typedInit.length()) != 0) {
                    for (uint8_t j = 0; j < n && spos < shi; j++) {
                        uint8_t wl = _wordData[spos++];
                        if (spos + wl + 1 > shi) break;
                        spos += wl + 1;
                    }
                    continue;
                }
                int lastSylStart = cl;
                for (int i = cl - 1; i >= 0; i--) {
                    if (strchr("aeiouv", wc[i])) {
                        int j = i;
                        while (j > 0 && strchr("aeiouv", wc[j-1])) j--;
                        if (j > 0) { lastSylStart = j; break; }
                    }
                }
                const char *candTailStart = wc + lastSylStart;
                int candTailLen = cl - lastSylStart;
                bool tailMatch = false;
                if (candTailLen >= (int)typedTail.length())
                    tailMatch = (strncmp(candTailStart, typedTail.c_str(), typedTail.length()) == 0);
                else
                    tailMatch = (strncmp(typedTail.c_str(), candTailStart, candTailLen) == 0);
                if (!tailMatch) {
                    for (uint8_t j = 0; j < n && spos < shi; j++) {
                        uint8_t wl = _wordData[spos++];
                        if (spos + wl + 1 > shi) break;
                        spos += wl + 1;
                    }
                    continue;
                }
                for (uint8_t j = 0; j < n && spos < shi; j++) {
                    uint8_t wl = _wordData[spos++];
                    if (wl == 0 || spos + wl + 1 > shi) break;
                    uint8_t wf = _wordData[spos + wl];
                    if (_all.size() < MAX_CANDIDATES && (!_trad || wf == 0)) {
                        std::string w;
                        w.append((const char *)_wordData + spos, wl);
                        bool dup = false;
                        for (auto &e : _all) if (e == w) { dup = true; break; }
                        if (!dup) {
                            _all.push_back(w);
                            _candLen.push_back((int)cl);
                            if ((int)cl > _maxMatchLen) _maxMatchLen = (int)cl;
                        }
                    }
                    spos += wl + 1;
                }
            }
        }
    }

    // Phase 8: partial (逐字) match
    _partialStart = (int)_all.size();
    _remainder.clear();
    if (qlen > 1 && _all.size() < MAX_CANDIDATES) {
        uint32_t zlo, zhi;
        int maxTry = qlen - 1;
        if (_maxMatchLen > 0 && _maxMatchLen < maxTry)
            maxTry = _maxMatchLen - 1;
        for (int tryLen = maxTry; tryLen >= 1 && _all.size() < MAX_CANDIDATES; tryLen--) {
            searchWindow(q, tryLen, zlo, zhi);
            uint32_t sEnd = zhi;
            int bcount = 0;
            while (zlo < zhi && bcount++ < 200) {
                uint32_t mid = zlo + (zhi - zlo) / 2;
                char code[7]; if (!readCode(mid, code)) break;
                if (strncmp(code, q, tryLen) < 0) zlo = mid + 1;
                else zhi = mid;
            }
            for (uint32_t i = zlo; i < sEnd && _all.size() < MAX_CANDIDATES; i++) {
                char code[7]; if (!readCode(i, code)) break;
                if (strncmp(code, q, tryLen) != 0) break;
                uint8_t f = readRecordFlag(i);
                if (f & (_trad ? 0x01 : 0x02)) continue;
                char hz[4]; if (!readHanzi(i, hz)) break;
                std::string h(hz);
                bool dup = false;
                for (auto &e : _all) if (e == h) { dup = true; break; }
                if (!dup) {
                    _all.push_back(h);
                    _candLen.push_back(0);
                }
            }
            if (_all.size() > (size_t)_partialStart) {
                _remainder = _code.substr(tryLen);
                break;
            }
        }
    }
    buildPage();
}

// 单引号分词查词: 编码形如 "xi'an" / "an'guang", 按 ' 切成音节段。
// 1) 补充词典表(seg_table.h) 分段前缀匹配; 2) 用户词典带撇号码精确匹配;
// 3) 主词典词组: 拼接码精确匹配且字数=段数;
// 4) 逐字匹配: 首段单字候选, 选中后按 seg0Next 消费跳下一段续拼(见 commit 的 candContinue)。
// 全部视为整码消费。
void IME::lookupSegmented() {
    std::vector<std::string> segs;
    std::string cur;
    for (char c : _code) {
        if (c == '\'') {
            if (!cur.empty()) segs.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) segs.push_back(cur);
    if (segs.empty()) return;
    std::string q;
    for (auto &s : segs) q += s;
    int fullLen = (int)_code.length();

    // 1) 补充词典表
    for (int i = 0; i < SEG_TABLE_COUNT && _all.size() < MAX_CANDIDATES; i++) {
        const char *enc = SEG_TABLE[i].syllables;
        std::vector<std::string> entrySyl;
        {
            std::string s;
            for (const char *p = enc; *p; p++) {
                if (*p == ' ') { if (!s.empty()) entrySyl.push_back(s); s.clear(); }
                else s += *p;
            }
            if (!s.empty()) entrySyl.push_back(s);
        }
        if (segs.size() > entrySyl.size()) continue;
        bool ok = true;
        for (size_t k = 0; k < segs.size() && ok; k++) {
            if (strncmp(segs[k].c_str(), entrySyl[k].c_str(), segs[k].length()) != 0)
                ok = false;
        }
        if (!ok) continue;
        std::string entryCode;
        for (auto &s : entrySyl) entryCode += s;
        if (strncmp(q.c_str(), entryCode.c_str(), q.length()) != 0) continue;
        std::string w = SEG_TABLE[i].word;
        bool dup = false;
        for (auto &e : _all) if (e == w) { dup = true; break; }
        if (!dup) {
            _all.push_back(w);
            _candLen.push_back(fullLen);
        }
    }

    // 2) 用户词典: 带撇号码的整码精确匹配(用户曾用该分段码提交过的词)
    for (auto &p : _userWords) {
        if (_all.size() >= MAX_CANDIDATES) break;
        if (p.trad != _trad) continue;
        if (p.code.find('\'') == std::string::npos) continue;
        if (p.code != _code) continue;
        bool dup = false;
        for (auto &e : _all) if (e == p.word) { dup = true; break; }
        if (!dup) {
            _all.push_back(p.word);
            _candLen.push_back(fullLen);
        }
    }

    // 3) 主词典词组: 拼接码精确匹配 + 字数/3 == 段数
    if (_wordCount > 0 && _wordData && q.length() >= 2 && _all.size() < MAX_CANDIDATES) {
        int k = (q[0] - 'a') * 26 + (q[1] - 'a');
        size_t wlo = (k >= 0 && k < INDEX_ENTRIES) ? _wordIndex[k] : 0;
        size_t whi = (k + 1 < INDEX_ENTRIES) ? _wordIndex[k + 1] : _wordDataSize;
        size_t wpos = wlo;
        int safety = 0;
        while (wpos < whi && _all.size() < MAX_CANDIDATES && safety++ < 5000) {
            uint8_t cl = _wordData[wpos];
            if (cl == 0 || wpos + 1 + cl > whi) break;
            const char *wc = (const char *)_wordData + wpos + 1;
            wpos += 1 + cl;
            if (wpos >= whi) break;
            uint8_t n = _wordData[wpos++];
            if ((int)cl == (int)q.length() && strncmp(wc, q.c_str(), cl) == 0) {
                for (uint8_t j = 0; j < n && wpos < whi; j++) {
                    uint8_t wl = _wordData[wpos++];
                    if (wl == 0 || wpos + wl + 1 > whi) break;
                    uint8_t wf = _wordData[wpos + wl];
                    if ((int)wl == (int)segs.size() * 3 && (!_trad || wf == 0)) {
                        std::string w((const char *)_wordData + wpos, wl);
                        bool dup = false;
                        for (auto &e : _all) if (e == w) { dup = true; break; }
                        if (!dup) {
                            _all.push_back(w);
                            _candLen.push_back(fullLen);
                        }
                    }
                    wpos += wl + 1;
                }
            } else {
                for (uint8_t j = 0; j < n && wpos < whi; j++) {
                    uint8_t wl = _wordData[wpos++];
                    if (wpos + wl + 1 > whi) break;
                    wpos += wl + 1;
                }
            }
        }
    }

    // 4) 逐字匹配: 首段单字前缀候选(词组之后)。选中后消费 seg0Next 字节跳到
    //    下一段续拼(如 xi'an 选"西"后余下 "an" 查字), 由 commit 的 candContinue 推进。
    if (_all.size() < MAX_CANDIDATES) {
        int seg0Next = (segs.size() > 1) ? ((int)segs[0].length() + 1) : fullLen;
        appendSingleCharCandidates(segs[0], seg0Next);
    }

    _partialStart = (int)_all.size();
}

// 主词典单字前缀匹配: 与 lookup() Phase 2 相同扫描, 但消费长度由调用方指定
// (分词逐字续拼时是跳到下一段的字节偏移, 而非词典码长)。
void IME::appendSingleCharCandidates(const std::string &prefix, int candLen) {
    int qlen = (int)prefix.length();
    if (qlen < 1 || _all.size() >= MAX_CANDIDATES) return;
    uint32_t lo, hi;
    searchWindow(prefix.c_str(), qlen, lo, hi);
    uint32_t scanEnd = hi;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        char code[7];
        if (!readCode(mid, code)) break;
        if (strncmp(code, prefix.c_str(), qlen) < 0) lo = mid + 1;
        else hi = mid;
    }
    for (uint32_t i = lo; i < scanEnd && _all.size() < MAX_CANDIDATES; i++) {
        char code[7];
        if (!readCode(i, code)) break;
        if (strncmp(code, prefix.c_str(), qlen) != 0) break;
        uint8_t f = readRecordFlag(i);
        if (f & (_trad ? 0x01 : 0x02)) continue;
        char hz[4];
        if (!readHanzi(i, hz)) break;
        std::string h(hz);
        bool dup = false;
        for (auto &e : _all) if (e == h) { dup = true; break; }
        if (!dup) {
            _all.push_back(h);
            _candLen.push_back(candLen);
        }
    }
}

void IME::beginPredict(const std::string &text) {
    if (!_predData || _predDataSize < 3) return;
    reset();
    _predChar = text;
    _predicting = true;
    const char *first = text.c_str();
    const uint8_t *p = _predData;
    while (p + 3 <= _predData + _predDataSize) {
        int charLen = 0;
        if ((*p & 0xE0) == 0xC0) charLen = 2;
        else if ((*p & 0xF0) == 0xE0) charLen = 3;
        else charLen = 1;
        if (charLen < 1 || p + charLen + 1 > _predData + _predDataSize) break;
        if (charLen == (int)text.length() && memcmp(p, first, charLen) == 0) {
            p += charLen;
            uint8_t n = *p++;
            for (uint8_t j = 0; j < n && p < _predData + _predDataSize; j++) {
                uint8_t wl = *p++;
                if (wl == 0 || p + wl > _predData + _predDataSize) break;
                std::string word;
                word.append((const char *)p, wl);
                p += wl;
                _all.push_back(word);
                _candLen.push_back(0);
            }
            buildPage();
            return;
        }
        uint8_t n = p[charLen];
        p += charLen + 1;
        for (size_t j = 0; j < n && p < _predData + _predDataSize; j++) {
            if (p + 1 > _predData + _predDataSize) break;
            uint8_t wl = *p;
            p += 1 + wl;
        }
    }
}

void IME::buildPage() {
    _page.clear();
    if (_all.empty()) {
        _pageStart = 0;
        _curPage = 0;
        _pageStarts.clear();
        return;
    }
    if (_widthFn && _displayWidth > 0) {
        // 按显示宽度分页: 与各界面候选行渲染一致, " 编号." 前缀 + 候选文本,
        // 一行放不下则把该候选归入下一页, 保证候选不被隐藏。
        _pageStarts.clear();
        _pageStarts.push_back(0);
        int lineW = 0;
        for (int i = 0; i < (int)_all.size(); i++) {
            char num[16];
            snprintf(num, sizeof(num), " %d.", i - (int)_pageStarts.back() + 1);
            std::string part = std::string(num) + _all[i];
            int partW = _widthFn(part.c_str());
            if (lineW > 0 && lineW + partW > _displayWidth) {
                _pageStarts.push_back(i);
                lineW = 0;
                snprintf(num, sizeof(num), " 1.");
                partW = _widthFn((std::string(num) + _all[i]).c_str());
            }
            lineW += partW;
        }
        if (_curPage < 0) _curPage = 0;
        if (_curPage >= (int)_pageStarts.size()) _curPage = (int)_pageStarts.size() - 1;
        _pageStart = _pageStarts[_curPage];
        int end = (_curPage + 1 < (int)_pageStarts.size()) ? _pageStarts[_curPage + 1] : (int)_all.size();
        for (int i = _pageStart; i < end; i++) _page.push_back(_all[i]);
    } else {
        // 退化: 未注册宽度回调时按固定每页数量分页
        _pageStarts.clear();
        for (int i = 0; i < (int)_all.size(); i += _pageSize) _pageStarts.push_back(i);
        if (_curPage < 0) _curPage = 0;
        if (_curPage >= (int)_pageStarts.size()) _curPage = (int)_pageStarts.size() - 1;
        _pageStart = _pageStarts[_curPage];
        for (int i = _pageStart; i < (int)_all.size() && (int)_page.size() < _pageSize; i++)
            _page.push_back(_all[i]);
    }
}

bool IME::commit(int idx, std::string &out) {
    if (idx < 0 || idx >= (int)_page.size()) return false;
    out = _page[idx];
    if (_deleteMode) {
        for (auto it = _userWords.begin(); it != _userWords.end(); ++it) {
            if (it->word == out && it->trad == _trad) {
                _userWords.erase(it);
                _userDirty = true;
                saveUserDict();
                break;
            }
        }
        out.clear();
        reset();
        return true;
    }
    int partialRel = _partialStart - _pageStart;
    bool partial = (_remainder.length() > 0 && idx >= partialRel);
    int pLen = pinyinPrefixLen(_code);
    // 大写后缀才拼进输出; 撇号码("xi'an")剩余部分是纯小写+分隔符, 不能当后缀
    bool hasUpperSuffix = false;
    for (int i = pLen; i < (int)_code.length(); i++) {
        if (_code[i] >= 'A' && _code[i] <= 'Z') { hasUpperSuffix = true; break; }
    }
    // Use per-candidate code length from _candLen for continuation
    int candIdx = idx + _pageStart;
    int consumedLen = (candIdx < (int)_candLen.size()) ? _candLen[candIdx] : 0;
    bool candContinue = (!partial && consumedLen > 0
                         && consumedLen < (int)_code.length()
                         && consumedLen <= 17);
    if (partial || candContinue) {
        if (!partial)
            _remainder = _code.substr(consumedLen);
        if (_remainder.length() == 0 || _remainder.length() >= _code.length()) {
            _prefix.clear();
            _displayCodeDirty = true;
            _remainder.clear();
            reset();
            return true;
        }
        _prefix += out;
        _displayCodeDirty = true;
        _code = _remainder;
        _remainder.clear();
        _partialStart = 0;
        _maxMatchLen = 0;
        out.clear();
        lookup();
        return false;
    }
    if (_prefix.length() > 0) {
        _prefix += out;
        if (hasUpperSuffix) _prefix += _code.substr(pLen);
        _displayCodeDirty = true;
        if (!hasUpperSuffix) {
            addUserWord(_codeOrig, _prefix);
            bumpFrequency(_codeOrig, _prefix);
        }
        out = _prefix;
    } else {
        if (hasUpperSuffix) out += _code.substr(pLen);
        if (!hasUpperSuffix) bumpFrequency(_code, out);
    }
    _prefix.clear();
    _displayCodeDirty = true;
    _codeOrig.clear();
    reset();
    return true;
}

bool IME::handleFullwidthPunct(int key, std::string &out) {
    // Map ASCII punctuation to fullwidth equivalents when IME is active
    // Only convert specific punctuation, others remain half-width
    switch (key) {
    case ',':  out = "，"; return true; // ，
    case '.':  out = "。"; return true; // 。
    case '?':  out = "？"; return true; // ？
    case ';':  out = "；"; return true; // ；
    case ':':  out = "："; return true; // ：
    case '!':  out = "！"; return true; // ！
    case '(':  out = "（"; return true; // （
    case ')':  out = "）"; return true; // ）
    case '[':  out = "【"; return true; // 【
    case ']':  out = "】"; return true; // 】
    case '{':  out = "「"; return true; // 「
    case '}':  out = "」"; return true; // 」
    case '\\': out = "、"; return true; // 、
    case '^':  out = "……"; return true; // ……
    case '<':  out = "《"; return true; // 《
    case '>':  out = "》"; return true; // 》
    case '`':  out = "·"; return true; // ·
    case '_':  out = "——"; return true; // ——
    case '$':  out = "¥"; return true; // ¥
    case '\'':
        // Single quote pairing: first press = ‘, second press = ’
        if (_singleQuoteOpen) {
            out = "’";
            _singleQuoteOpen = false;
        } else {
            out = "‘";
            _singleQuoteOpen = true;
        }
        return true;
    case '"':
        // Double quote pairing: first press = “, second press = ”
        if (_doubleQuoteOpen) {
            out = "”";
            _doubleQuoteOpen = false;
        } else {
            out = "“";
            _doubleQuoteOpen = true;
        }
        return true;
    default:   return false; // Other characters remain half-width
    }
}

bool IME::handleFullwidthChar(int key, std::string &out) {
    // Fullwidth mode: map ASCII letters, digits, space, and remaining symbols to fullwidth
    if (key >= 'A' && key <= 'Z') {
        // U+FF21 = fullwidth A
        uint32_t cp = 0xFF21 + (key - 'A');
        appendUtf8(cp, out);
        return true;
    }
    if (key >= 'a' && key <= 'z') {
        // U+FF41 = fullwidth a
        uint32_t cp = 0xFF41 + (key - 'a');
        appendUtf8(cp, out);
        return true;
    }
    if (key >= '0' && key <= '9') {
        // U+FF10 = fullwidth 0
        uint32_t cp = 0xFF10 + (key - '0');
        appendUtf8(cp, out);
        return true;
    }
    if (key == ' ') {
        // U+3000 = ideographic space (fullwidth space)
        appendUtf8(0x3000, out);
        return true;
    }
    // Remaining printable ASCII not already handled by handleFullwidthPunct
    if (key >= 0x21 && key <= 0x7E) {
        // U+FF01 = fullwidth !, offset from '!' is key - 0x21
        uint32_t cp = 0xFF01 + (key - 0x21);
        appendUtf8(cp, out);
        return true;
    }
    return false;
}

bool IME::handleKey(int key, std::string &out) {
    if (!_active) return false;
    if (_english) return false;  // 临时英文模式: 按键直接透传给编辑器
    if (_predicting) {
        if ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z')) {
            _predicting = false;
            _code = (char)key;
            _displayCodeDirty = true;
            lookup();
            return true;
        }
        if (key >= '1' && key <= '9') {
            int idx = key - '1';
            if (idx < (int)_page.size()) {
                out = _page[idx];
                _predicting = false;
            }
            return true;
        }
        if (key == ' ') {
            if (_page.size() > 0) {
                out = _page[0];
                _predicting = false;
            }
            return true;
        }
        if (key == '-' || key == ';' || key == ',') {
            if (_curPage > 0) { _curPage--; buildPage(); }
            return true;
        }
        if (key == '=' || key == '\'' || key == '.') {
            if (_curPage + 1 < (int)_pageStarts.size()) { _curPage++; buildPage(); }
            return true;
        }
        if (key == '\b' || key == 27 || key == '\n') {
            _predicting = false;
            return true;
        }
        _predicting = false;
        return false;
    }
    // In fullwidth mode with no composition, output letters/digits/space as fullwidth
    if (_fullwidth && _code.length() == 0 && !_deleteMode && !_lfMode) {
        if (((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z')) ||
            (key >= '0' && key <= '9') || key == ' ') {
            return handleFullwidthChar(key, out);
        }
    }
    if ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z')) {
        char cl = (char)tolower(key);
        char c = (char)key;
        if (_code.length() == 0 && !_deleteMode && !_lfMode && cl == 'v') {
            if (_userWords.size() > 0) {
                _deleteMode = true;
                lookup();
                return true;
            }
        }
        if (_code.length() == 0 && !_deleteMode && !_lfMode && cl == 'u') {
            loadLfDict();
            if (_lfBlob) { _lfMode = true; _maxCode = 12; return true; }
        }
        if ((int)_code.length() < _maxCode) {
            _code += c;
            _displayCodeDirty = true;
            lookup();
        }
        return true;
    }
    if (_code.length() == 0) {
        if (handleFullwidthPunct(key, out)) return true;
        if (_fullwidth && handleFullwidthChar(key, out)) return true;
        return false;
    }
    // 单引号编码分词: 拼音模式下 ' 显式分隔音节(如 xi'an); 两分模式保留翻页
    if (key == '\'' && !_lfMode) {
        if (_code.back() == '\'') return true;
        if ((int)_code.length() < _maxCode) {
            _code += '\'';
            _displayCodeDirty = true;
            lookup();
        }
        return true;
    }
    if (key >= '1' && key <= '9') {
        commit(key - '1', out);
        return true;
    }
    if (key == ' ') {
        if (_page.size() > 0) commit(0, out);
        else reset();
        return true;
    }
    if (key == '\n') {
        out = _code;
        reset();
        return true;
    }
    if (key == '\b') {
        if (_prefix.length() > 0) {
            _code = _codeOrig;
            if (_code.length() > 0) {
                _code.erase(_code.length() - 1);
            }
            _prefix.clear();
            _remainder.clear();
            _partialStart = 0;
            _maxMatchLen = 0;
            _displayCodeDirty = true;
            if (_code.length() == 0) reset();
            else lookup();
        } else if (_code.length() > 0) {
            _code.erase(_code.length() - 1);
            _displayCodeDirty = true;
            if (_code.length() == 0) reset();
            else lookup();
        }
        return true;
    }
    if (key == 27) {
        reset();
        return true;
    }
    if (key == '-' || key == ';' || key == ',') {
        if (_curPage > 0) { _curPage--; buildPage(); }
        return true;
    }
    if (key == '=' || key == '.') {
        if (_curPage + 1 < (int)_pageStarts.size()) { _curPage++; buildPage(); }
        return true;
    }
    if (_page.size() > 0) {
        commit(0, out);
        return true;
    }
    reset();
    return true;
}
