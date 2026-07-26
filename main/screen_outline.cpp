#include "screen_outline.h"
#include "font_renderer.h"
#include "json_parser.h"
#include "journal_storage.h"
#include "ui_helpers.h"
#include "ime/IME.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <unistd.h>

extern void *g_u8g2;
extern "C" {
    extern void u8g2_SetDrawColor(void *g_u8g2, int color);
    extern void u8g2_DrawPixel(void *g_u8g2, int x, int y);
    extern void u8g2_DrawBox(void *g_u8g2, int x, int y, int w, int h);
    extern void u8g2_DrawHLine(void *g_u8g2, int x, int y, int w);
    extern void u8g2_DrawFrame(void *g_u8g2, int x, int y, int w, int h);
}

// ── File icon bitmap (from Go-Song2Propo-NF-R.ttf, U+F15B fa-file-text-o) ──
static const uint8_t FILE_ICON_BITS[] = {
    0x00, 0x00, 0x7E, 0x80, 0xFE, 0xC0, 0xFE, 0xE0, 0xFE, 0xF0, 0xFF, 0x00,
    0xFF, 0xF8, 0xFF, 0xF8, 0xFF, 0xF8, 0xFF, 0xF8, 0xFF, 0xF8, 0xFF, 0xF8,
    0xFF, 0xF8, 0xFF, 0xF8, 0xFF, 0xF8, 0xFF, 0xF8, 0xFF, 0xF0, 0x3F, 0xE0,
};
#define FILE_ICON_W 13
#define FILE_ICON_H 18
#define FILE_ICON_ROW_BYTES 2

static void drawFileIcon(int x, int baseline) {
    int top = baseline - FILE_ICON_H;
    for (int row = 0; row < FILE_ICON_H; row++) {
        for (int col = 0; col < FILE_ICON_W; col++) {
            int bi = row * FILE_ICON_ROW_BYTES + col / 8;
            int bit = 7 - (col % 8);
            if (FILE_ICON_BITS[bi] & (1 << bit))
                u8g2_DrawPixel(g_u8g2, x + col, top + row);
        }
    }
}

#define OUTLINE_DIR "/sdcard/outline"

enum Mode { M_PROJECTS, M_BROWSE, M_ADD_PROJECT, M_ADD_HEADING, M_ADD_SUB, M_FILTER, M_EDIT_NOTE, M_CONFIRM };

// ── State ────────────────────────────────────────────────────────────────
static struct {
    int mode = M_PROJECTS;
    int sel = 0;
    int scroll = 0;

    // project list
    std::vector<std::string> projects;
    int curProject = -1;     // index into projects

    // current project outline data
    JsonValue outlineData;   // { nodes: [...] }
    std::vector<JsonValue> *nodes = nullptr;
    size_t nodeCount = 0;

    // editing
    std::string editBuf;
    int editCur = 0;
    bool imeActive = false;
    int pendingLevel = 0;    // heading level for next add
    int insertAfter = -1;    // index in nodes to insert after, -1 = append

    // filter
    std::string filterText;

    // heading whose note is being edited (index into nodes)
    int editNoteIdx = -1;
    bool editingTitle = false;  // true = editing title, false = editing note

    // post-editor file copy
    std::string pendingOutlineTarget; // real path to copy editor output to
    std::string pendingJournalFile;   // temp journal filename used

    // confirm dialog
    std::string confirmMsg;
    int confirmAction = 0;  // 1=delete heading, 2=delete project, 3=clear file
    int confirmIdx = -1;    // subject index
} g;

// ── Helpers ──────────────────────────────────────────────────────────────

static std::vector<std::string> listProjects() {
    std::vector<std::string> result;
    DIR *dir = opendir(OUTLINE_DIR);
    if (!dir) return result;
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type == DT_DIR) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
            // check project.json exists
            std::string pj = std::string(OUTLINE_DIR) + "/" + entry->d_name + "/project.json";
            FILE *f = fopen(pj.c_str(), "rb");
            if (f) { fclose(f); result.push_back(entry->d_name); }
        }
    }
    closedir(dir);
    std::sort(result.begin(), result.end());
    return result;
}

static void loadOutline() {
    if (g.curProject < 0 || g.curProject >= (int)g.projects.size()) {
        g.nodes = nullptr;
        g.nodeCount = 0;
        return;
    }
    std::string path = std::string(OUTLINE_DIR) + "/" + g.projects[g.curProject] + "/project.json";
    g.outlineData = JsonValue::loadFromFile(path);
    if (g.outlineData.isNull() || !g.outlineData.has("nodes") || !g.outlineData["nodes"].isArray()) {
        g.outlineData = JsonValue::object();
        g.outlineData.set("nodes", JsonValue::array());
        g.nodes = nullptr;
        g.nodeCount = 0;
        return;
    }
    // Purge null-type nodes from old bug
    auto &nodes = g.outlineData["nodes"];
    int write = 0;
    for (int i = 0; i < (int)nodes.size(); i++) {
        if (!nodes[i].isNull())
            nodes.elements[write++] = nodes[i];
    }
    nodes.elements.resize(write);

    g.nodes = &g.outlineData["nodes"].elements;
    g.nodeCount = g.outlineData["nodes"].size();
}

static void saveOutline() {
    if (g.curProject < 0 || g.curProject >= (int)g.projects.size()) return;
    std::string path = std::string(OUTLINE_DIR) + "/" + g.projects[g.curProject] + "/project.json";
    if (!JsonValue::saveToFile(path, g.outlineData)) {
        mkdir(OUTLINE_DIR, 0777);
        mkdir((std::string(OUTLINE_DIR) + "/" + g.projects[g.curProject]).c_str(), 0777);
        JsonValue::saveToFile(path, g.outlineData);
    }
}

static std::string makeId() {
    time_t now; time(&now); struct tm *tm = localtime(&now);
    char buf[32];
    static int seq = 0;
    snprintf(buf, sizeof(buf), "n%02d%02d%02d_%d",
             tm->tm_hour, tm->tm_min, tm->tm_sec, seq++);
    return buf;
}

// Convert heading title to a safe filename
static std::string safeFilename(const std::string &title) {
    std::string out;
    for (char c : title) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            out += '_';
        else
            out += c;
    }
    if (out.empty()) out = "untitled";
    if (out.size() > 40) out = out.substr(0, 40);
    return out + ".txt";
}

// Ensure the outline content file exists
static std::string ensureContentFile(const std::string &project, const std::string &filename) {
    std::string dir = std::string(OUTLINE_DIR) + "/" + project;
    mkdir(OUTLINE_DIR, 0777);
    mkdir(dir.c_str(), 0777);
    std::string path = dir + "/" + filename;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        f = fopen(path.c_str(), "w");
        if (f) fclose(f);
    } else {
        fclose(f);
    }
    return path;
}

// Read content file, strip journal header if present
static std::string readContentFile(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return "";
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return ""; }
    std::string content(static_cast<size_t>(sz), '\0');
    if (fread(&content[0], 1, static_cast<size_t>(sz), f) != static_cast<size_t>(sz)) {
        fclose(f); return "";
    }
    fclose(f);
    return content;
}

// ── Filter helpers ───────────────────────────────────────────────────────
static std::vector<int> g_filteredIdx;

static void rebuildFilter() {
    g_filteredIdx.clear();
    if (!g.nodes) return;
    for (size_t i = 0; i < g.nodeCount; i++) {
        if (g.filterText.empty()) {
            g_filteredIdx.push_back((int)i);
        } else {
            std::string title = (*g.nodes)[i]["title"].asString();
            std::string note  = (*g.nodes)[i]["note"].asString();
            if (title.find(g.filterText) != std::string::npos ||
                note.find(g.filterText) != std::string::npos)
                g_filteredIdx.push_back((int)i);
        }
    }
    if (g.sel >= (int)g_filteredIdx.size()) g.sel = (int)g_filteredIdx.size() - 1;
    if (g.sel < 0) g.sel = 0;
}

// ── Markdown export ─────────────────────────────────────────────────────
static std::string exportMD() {
    std::string md;
    if (g.curProject < 0 || g.curProject >= (int)g.projects.size())
        return "# 大纲\n";
    md = "# " + g.projects[g.curProject] + "\n\n";
    if (!g.nodes) return md;
    for (size_t i = 0; i < g.nodeCount; i++) {
        auto &node = (*g.nodes)[i];
        int lvl = node["level"].asInt(0);
        std::string title = node["title"].asString();
        std::string file = node["file"].asString();

        // heading markers
        std::string prefix;
        for (int j = 0; j <= lvl && j < 6; j++) prefix += "#";
        md += prefix + " " + title + "\n";

        // read and append content
        if (!file.empty() && g.curProject >= 0) {
            std::string fpath = std::string(OUTLINE_DIR) + "/" + g.projects[g.curProject] + "/" + file;
            std::string content = readContentFile(fpath);
            if (!content.empty()) {
                md += "\n" + content + "\n\n";
            }
        }
    }
    return md;
}

// ── Tree prefix helpers ──────────────────────────────────────────────────
static bool isLastNodeAtLevel(int idx, int lvl) {
    if (!g.nodes) return true;
    for (int j = idx + 1; j < (int)g.nodeCount; j++) {
        int jlvl = (*g.nodes)[j]["level"].asInt(0);
        if (jlvl == lvl) return false;
        if (jlvl < lvl) break;
    }
    return true;
}

static std::string nodeTreePrefix(int idx) {
    if (!g.nodes || idx < 0 || idx >= (int)g.nodeCount) return "";
    int lvl = (*g.nodes)[idx]["level"].asInt(0);
    std::string prefix;
    for (int a = 0; a < lvl; a++) {
        int ancIdx = -1;
        for (int j = idx - 1; j >= 0; j--) {
            int jlvl = (*g.nodes)[j]["level"].asInt(0);
            if (jlvl == a) { ancIdx = j; break; }
            if (jlvl < a) break;
        }
        if (ancIdx >= 0 && !isLastNodeAtLevel(ancIdx, a))
            prefix += "│ ";
        else
            prefix += "  ";
    }
    if (lvl > 0)
        prefix += isLastNodeAtLevel(idx, lvl) ? "└─ " : "├─ ";
    else
        prefix = "◆ ";
    return prefix;
}

// ── Drawing ──────────────────────────────────────────────────────────────

#define IME_CODE_Y (STATUS_Y - 2*FONT_H + g_font.ascent())
#define IME_CAND_Y (STATUS_Y - FONT_H + g_font.ascent() - 3)

static void drawIMEStatus() {
    if (!g_ime.composing()) return;
    std::string code = g_ime.displayCode();
    int total = g_ime.totalCandidates();
    int pageSize = g_ime.pageSize();
    int curPage = g_ime.currentPage();
    int totalPages = (total + pageSize - 1) / pageSize;
    if (totalPages < 1) totalPages = 1;
    char pageInfo[32];
    snprintf(pageInfo, sizeof(pageInfo), "%d/%d", curPage, totalPages);
    int sepY = IME_CODE_Y - 4;
    int codeBaseline = sepY - 7;
    {
        int cw = g_font.textWidth(code.c_str()) + 8;
        u8g2_SetDrawColor(g_u8g2, 1);
        u8g2_DrawBox(g_u8g2, 4, codeBaseline - g_font.ascent(), cw, FONT_H);
        u8g2_SetDrawColor(g_u8g2, 0);
        g_font.drawText(4, codeBaseline, code.c_str(), false);
        u8g2_SetDrawColor(g_u8g2, 1);
    }
    {
        int tw = g_font.textWidth(pageInfo);
        int pw = tw + 8;
        int px = SCREEN_W - pw - 4;
        u8g2_SetDrawColor(g_u8g2, 1);
        u8g2_DrawBox(g_u8g2, px, codeBaseline - g_font.ascent(), pw, FONT_H);
        u8g2_SetDrawColor(g_u8g2, 0);
        g_font.drawText(px + 4, codeBaseline, pageInfo, false);
        u8g2_SetDrawColor(g_u8g2, 1);
    }
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawHLine(g_u8g2, 0, sepY, SCREEN_W);
    u8g2_SetDrawColor(g_u8g2, 1);
    auto &cands = g_ime.candidates();
    std::string candLine;
    for (int i = 0; i < (int)cands.size(); i++) {
        char idx[16];
        snprintf(idx, sizeof(idx), "%d.", (i % pageSize) + 1);
        std::string part = std::string(" ") + idx + cands[i];
        int curW = g_font.textWidth(candLine.c_str());
        int partW = g_font.textWidth(part.c_str());
        if (curW + partW + 8 > SCREEN_W) break;
        candLine += part;
    }
    {
        int cw = g_font.textWidth(candLine.c_str()) + 8;
        u8g2_SetDrawColor(g_u8g2, 1);
        u8g2_DrawBox(g_u8g2, 4, IME_CAND_Y - g_font.ascent(), cw, FONT_H);
        u8g2_SetDrawColor(g_u8g2, 0);
        g_font.drawText(4, IME_CAND_Y, candLine.c_str(), false);
        u8g2_SetDrawColor(g_u8g2, 1);
    }
}

static void drawInputOverlay(const char *title) {
    ui_clear();
    ui_draw_text_centered(28, title, false, true);
    u8g2_DrawHLine(g_u8g2, 0, 28 + g_font.descent() + 4, SCREEN_W);
    std::string display = g.editBuf.empty() ? " " : g.editBuf;
    int ty = 28 + g_font.descent() + 4 + g_font.ascent();
    ui_draw_text(4, ty, display.c_str());
    int cx = g_font.textWidth(g.editBuf.substr(0, g.editCur).c_str());
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawBox(g_u8g2, 4 + cx, ty + 4, 8, 3);
    u8g2_SetDrawColor(g_u8g2, 1);
    drawIMEStatus();
    ui_commit();
}

static void drawProjectList() {
    ui_clear();
    ui_draw_text(4, g_font.ascent(), "选择项目", false, true);
    u8g2_DrawHLine(g_u8g2, 0, FONT_H + 4, SCREEN_W);

    int y = FONT_H + 8 + LINE_SPACING;
    int vis = (STATUS_Y - y + LINE_SPACING - 1) / LINE_SPACING;
    if (vis < 1) vis = 1;

    if (g.sel < g.scroll) g.scroll = g.sel;
    if (g.sel >= g.scroll + vis) g.scroll = g.sel - vis + 1;

    for (int i = 0; i < vis && (g.scroll + i) < (int)g.projects.size(); i++) {
        bool sel = (g.scroll + i == g.sel);
        ui_draw_text(8, y + i * LINE_SPACING, g.projects[g.scroll + i].c_str(), sel);
    }
    ui_draw_status("n:新建 Enter:打开 d:删除", "");
}

static void drawOutline() {
    ui_clear();

    // project title
    std::string title;
    if (g.curProject >= 0 && g.curProject < (int)g.projects.size())
        title = g.projects[g.curProject];
    else
        title = "大纲";
    ui_draw_text(4, g_font.ascent(), title.c_str(), false, true);
    u8g2_DrawHLine(g_u8g2, 0, FONT_H + 4, SCREEN_W);

    int y = FONT_H + 8 + LINE_SPACING;
    bool composingFilter = (g.mode == M_FILTER && g_ime.composing());
    int maxY = composingFilter ? (IME_CODE_Y - 4) : STATUS_Y;
    int vis = (maxY - y + LINE_SPACING - 1) / LINE_SPACING;
    if (vis < 1) vis = 1;

    if (g.sel < g.scroll) g.scroll = g.sel;
    if (g.sel >= g.scroll + vis) g.scroll = g.sel - vis + 1;

    auto &filtered = g.filterText.empty() ? g_filteredIdx : g_filteredIdx;
    if (filtered.empty() && g.filterText.empty()) {
        // show all nodes
        if (!g.nodes) {
            ui_draw_text(8, y, "空项目 — 按a添加标题");
            ui_draw_status("a:标题 i:子标题 n:项目 Tab:切换", "");
            return;
        }
        filtered.clear();
        for (size_t i = 0; i < g.nodeCount; i++) filtered.push_back((int)i);
        g_filteredIdx = filtered;
    }

    for (int i = 0; i < vis && (g.scroll + i) < (int)filtered.size(); i++) {
        int ni = filtered[g.scroll + i];
        auto &node = (*g.nodes)[ni];
        bool sel = (g.scroll + i == g.sel);
        int lvl = node["level"].asInt(0);
        std::string title = node["title"].asString();

        char buf[96];
        std::string tprefix = g.filterText.empty() ? nodeTreePrefix(ni) : std::string(lvl * 2, ' ');
        snprintf(buf, sizeof(buf), "%s%s", tprefix.c_str(), title.c_str());
        ui_draw_text(8, y + i * LINE_SPACING, buf, sel);

        // show file indicator
        std::string file = node["file"].asString();
        if (!file.empty()) {
            drawFileIcon(SCREEN_W - FILE_ICON_W - 4, y + i * LINE_SPACING);
        }
    }

    if (!g.filterText.empty() && !composingFilter) {
        char fb[64];
        snprintf(fb, sizeof(fb), "筛选: %s", g.filterText.c_str());
        ui_draw_text(4, STATUS_Y - LINE_SPACING + 2, fb, true);
    }

    ui_draw_status("a:标题 i:子标题 e:重命名 d:删除 c:清除关联 Enter:编辑 Tab:项目 /:筛选", "");

    if (composingFilter) drawIMEStatus();
}

// ── Screen entry ─────────────────────────────────────────────────────────
void screen_outline_init() {
    mkdir(OUTLINE_DIR, 0777);

    // Handle post-editor file copy
    bool returningFromEditor = !g.pendingOutlineTarget.empty();
    if (returningFromEditor) {
        std::string tempPath = std::string("/sdcard/pjournal/") + g.pendingJournalFile;
        std::string content = readContentFile(tempPath);
        if (!content.empty()) {
            // Strip journal header
            std::string body = extractBody(content);
            if (body.empty()) body = content;

            // Ensure target directory exists
            size_t slash = g.pendingOutlineTarget.rfind('/');
            if (slash != std::string::npos) {
                std::string dir = g.pendingOutlineTarget.substr(0, slash);
                mkdir(dir.c_str(), 0777);
            }

            FILE *f = fopen(g.pendingOutlineTarget.c_str(), "w");
            if (f) {
                fwrite(body.data(), 1, body.size(), f);
                fclose(f);
            }

            // Clean up temp file
            remove(tempPath.c_str());
        }
        g.pendingOutlineTarget.clear();
        g.pendingJournalFile.clear();
        // Reload outline data after returning from editor
        if (g.curProject >= 0 && g.curProject < (int)g.projects.size()) {
            loadOutline();
            rebuildFilter();
        }
    }

    g.mode = returningFromEditor ? M_BROWSE : M_PROJECTS;
    if (!returningFromEditor) {
        g.sel = 0;
        g.scroll = 0;
        g.curProject = -1;
    }
    g.editBuf.clear();
    g.editCur = 0;
    g.imeActive = false;
    g.pendingLevel = 0;
    g.insertAfter = -1;
    g.filterText.clear();
    g.editNoteIdx = -1;
    g.editingTitle = false;
    g_ime.setActive(false);
    g.nodes = nullptr;
    g.nodeCount = 0;

    g.projects = listProjects();
    g_filteredIdx.clear();
}

// ── Main handle ──────────────────────────────────────────────────────────
AppState screen_outline_handle(int key, ScreenContext &ctx) {
    // ── M_ADD_PROJECT ────────────────────────────────────────────────
    if (g.mode == M_ADD_PROJECT) {
        if (g.imeActive && key != 0) {
            std::string imeOut;
            if (g_ime.handleKey(key, imeOut)) {
                if (!imeOut.empty()) {
                    g.editBuf.insert(g.editCur, imeOut);
                    g.editCur += (int)imeOut.length();
                }
                drawInputOverlay("新建项目"); return APP_OUTLINE;
            }
        }
        if (key == KEY_IME_TOGGLE) {
            g.imeActive = !g.imeActive; g_ime.setActive(g.imeActive);
            drawInputOverlay("新建项目"); return APP_OUTLINE;
        }
        if (key == 0x1B) {
            g.mode = (g.curProject >= 0) ? M_BROWSE : M_PROJECTS;
            g.imeActive = false; g_ime.setActive(false);
        } else if (key == 0x0A || key == 0x0D) {
            if (!g.editBuf.empty()) {
                std::string dir = std::string(OUTLINE_DIR) + "/" + g.editBuf;
                mkdir(OUTLINE_DIR, 0777);
                mkdir(dir.c_str(), 0777);
                JsonValue data;
                data.set("nodes", JsonValue::array());
                JsonValue::saveToFile(dir + "/project.json", data);
                g.projects = listProjects();
                for (size_t i = 0; i < g.projects.size(); i++)
                    if (g.projects[i] == g.editBuf) { g.curProject = (int)i; break; }
            }
            g.mode = (g.curProject >= 0) ? M_BROWSE : M_PROJECTS;
            g.imeActive = false; g_ime.setActive(false);
            loadOutline();
            rebuildFilter();
        } else if (key == 0x7F || key == 0x08) {
            if (g.editCur > 0) {
                int prev = g.editCur - 1;
                while (prev > 0 && ((unsigned char)g.editBuf[prev] & 0xC0) == 0x80) prev--;
                g.editBuf.erase(prev, g.editCur - prev); g.editCur = prev;
            }
        } else if (key == KEY_LEFT) {
            if (g.editCur > 0) { g.editCur--;
                while (g.editCur > 0 && ((unsigned char)g.editBuf[g.editCur] & 0xC0) == 0x80) g.editCur--; }
        } else if (key == KEY_RIGHT) {
            if (g.editCur < (int)g.editBuf.length()) { g.editCur++;
                while (g.editCur < (int)g.editBuf.length() && ((unsigned char)g.editBuf[g.editCur] & 0xC0) == 0x80) g.editCur++; }
        } else if (key >= 0x20 && key <= 0x7E) {
            g.editBuf.insert(g.editCur, 1, (char)key); g.editCur++;
        }
        drawInputOverlay("新建项目");
        return APP_OUTLINE;
    }

    // ── M_FILTER ─────────────────────────────────────────────────────
    if (g.mode == M_FILTER) {
        if (g.imeActive && key != 0) {
            std::string imeOut;
            if (g_ime.handleKey(key, imeOut)) {
                if (!imeOut.empty()) { g.filterText += imeOut; rebuildFilter(); }
                drawOutline(); ui_commit(); return APP_OUTLINE;
            }
        }
        if (key == KEY_IME_TOGGLE) {
            g.imeActive = !g.imeActive; g_ime.setActive(g.imeActive);
            drawOutline(); ui_commit(); return APP_OUTLINE;
        }
        if (key == 0x1B) {
            g.mode = M_BROWSE; g.imeActive = false; g_ime.setActive(false);
            g.filterText.clear(); rebuildFilter();
        } else if (key == 0x0A || key == 0x0D) {
            g.mode = M_BROWSE; g.imeActive = false; g_ime.setActive(false);
        } else if (key == 0x7F || key == 0x08) {
            if (!g.filterText.empty()) { g.filterText.pop_back(); rebuildFilter(); }
        } else if (key >= 0x20 && key <= 0x7E) {
            g.filterText += (char)key; rebuildFilter();
        }
        drawOutline(); ui_commit();
        return APP_OUTLINE;
    }

    // ── M_EDIT_NOTE ──────────────────────────────────────────────────
    if (g.mode == M_EDIT_NOTE) {
        if (g.imeActive && key != 0) {
            std::string imeOut;
            if (g_ime.handleKey(key, imeOut)) {
                if (!imeOut.empty()) {
                    g.editBuf.insert(g.editCur, imeOut);
                    g.editCur += (int)imeOut.length();
                }
                drawInputOverlay("编辑备注"); return APP_OUTLINE;
            }
        }
        if (key == KEY_IME_TOGGLE) {
            g.imeActive = !g.imeActive; g_ime.setActive(g.imeActive);
            drawInputOverlay("编辑备注"); return APP_OUTLINE;
        }
        if (key == 0x1B) {
            g.mode = M_BROWSE; g.imeActive = false; g_ime.setActive(false);
        } else if (key == 0x0A || key == 0x0D) {
            if (g.editNoteIdx >= 0 && g.nodes && (size_t)g.editNoteIdx < g.nodeCount) {
                if (g.editingTitle)
                    (*g.nodes)[g.editNoteIdx].set("title", g.editBuf);
                else
                    (*g.nodes)[g.editNoteIdx].set("note", g.editBuf);
                saveOutline();
            }
            g.mode = M_BROWSE; g.imeActive = false; g_ime.setActive(false);
        } else if (key == 0x7F || key == 0x08) {
            if (g.editCur > 0) {
                int prev = g.editCur - 1;
                while (prev > 0 && ((unsigned char)g.editBuf[prev] & 0xC0) == 0x80) prev--;
                g.editBuf.erase(prev, g.editCur - prev); g.editCur = prev;
            }
        } else if (key == KEY_LEFT) {
            if (g.editCur > 0) { g.editCur--;
                while (g.editCur > 0 && ((unsigned char)g.editBuf[g.editCur] & 0xC0) == 0x80) g.editCur--; }
        } else if (key == KEY_RIGHT) {
            if (g.editCur < (int)g.editBuf.length()) { g.editCur++;
                while (g.editCur < (int)g.editBuf.length() && ((unsigned char)g.editBuf[g.editCur] & 0xC0) == 0x80) g.editCur++; }
        } else if (key >= 0x20 && key <= 0x7E) {
            g.editBuf.insert(g.editCur, 1, (char)key); g.editCur++;
        }
        drawInputOverlay("编辑备注");
        return APP_OUTLINE;
    }

    // ── M_CONFIRM: confirmation dialog ────────────────────────────────
    if (g.mode == M_CONFIRM) {
        ui_clear();
        ui_draw_text_centered(SCREEN_H / 2, g.confirmMsg.c_str(), false, true);
        ui_draw_status("Enter确认 ESC取消", "");
        ui_commit();
        if (key == 0x0A || key == 0x0D) {
            // Confirmed
            if (g.confirmAction == 1 && g.confirmIdx >= 0 && g.nodes && (size_t)g.confirmIdx < g.nodeCount) {
                // Delete heading + associated file
                auto &node = (*g.nodes)[g.confirmIdx];
                std::string file = node["file"].asString();
                if (!file.empty() && g.curProject >= 0 && g.curProject < (int)g.projects.size()) {
                    std::string fpath = std::string(OUTLINE_DIR) + "/" + g.projects[g.curProject] + "/" + file;
                    remove(fpath.c_str());
                }
                g.nodes->erase(g.nodes->begin() + g.confirmIdx);
                g.nodeCount = g.nodes->size();
                if (g.sel >= (int)g.nodeCount) g.sel = (int)g.nodeCount - 1;
                if (g.sel < 0) g.sel = 0;
                saveOutline();
                rebuildFilter();
                ctx.statusMessage = "已删除";
            } else if (g.confirmAction == 2 && g.confirmIdx >= 0 && g.confirmIdx < (int)g.projects.size()) {
                // Delete project
                std::string dir = std::string(OUTLINE_DIR) + "/" + g.projects[g.confirmIdx];
                DIR *d = opendir(dir.c_str());
                if (d) {
                    struct dirent *ent;
                    while ((ent = readdir(d)) != nullptr) {
                        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
                        std::string fp = dir + "/" + ent->d_name;
                        remove(fp.c_str());
                    }
                    closedir(d);
                }
                rmdir(dir.c_str());
                g.projects.erase(g.projects.begin() + g.confirmIdx);
                if (g.sel >= (int)g.projects.size()) g.sel = (int)g.projects.size() - 1;
                if (g.sel < 0) g.sel = 0;
                ctx.statusMessage = "已删除项目";
            } else if (g.confirmAction == 3 && g.confirmIdx >= 0 && g.nodes && (size_t)g.confirmIdx < g.nodeCount) {
                // Clear file association
                auto &node = (*g.nodes)[g.confirmIdx];
                std::string file = node["file"].asString();
                if (!file.empty() && g.curProject >= 0 && g.curProject < (int)g.projects.size()) {
                    std::string fpath = std::string(OUTLINE_DIR) + "/" + g.projects[g.curProject] + "/" + file;
                    remove(fpath.c_str());
                }
                node.set("file", "");
                saveOutline();
                ctx.statusMessage = "已清除关联";
            }
            g.mode = M_BROWSE;
        } else if (key == 0x1B) {
            // Cancelled
            g.mode = (g.confirmAction == 2) ? M_PROJECTS : M_BROWSE;
        }
        return APP_OUTLINE;
    }

    // ── M_PROJECTS: project list ─────────────────────────────────────
    if (g.mode == M_PROJECTS) {
        if (key == 'q' || key == 'Q' || key == 0x1B) {
            g_ime.setActive(false);
            ctx.nextState = APP_MAIN; return APP_MAIN;
        }
        if (key == 'j' || key == KEY_DOWN) {
            if (g.sel < (int)g.projects.size() - 1) g.sel++;
        }
        if (key == 'k' || key == KEY_UP) {
            if (g.sel > 0) g.sel--;
        }
        if (key == 'n' || key == 'N') {
            g.mode = M_ADD_PROJECT;
            g.editBuf.clear(); g.editCur = 0;
            g.imeActive = true; g_ime.setActive(true);
        }
        if (key == 0x0A || key == 0x0D) {
            if (g.sel < (int)g.projects.size()) {
                g.curProject = g.sel;
                g.mode = M_BROWSE;
                g.sel = 0; g.scroll = 0;
                loadOutline();
                rebuildFilter();
            }
        }

        if ((key == 'd' || key == 'D') && g.sel < (int)g.projects.size()) {
            // Ask for confirmation
            g.confirmAction = 2;
            g.confirmIdx = g.sel;
            g.confirmMsg = std::string("删除项目「") + g.projects[g.sel] + "」?";
            g.mode = M_CONFIRM;
        }

        drawProjectList(); ui_commit();
        return APP_OUTLINE;
    }

    // ── M_BROWSE: outline tree ───────────────────────────────────────
    if (g.mode == M_BROWSE) {
        if (key == 'q' || key == 'Q' || key == 0x1B) {
            g.mode = M_PROJECTS;
            g.sel = g.curProject >= 0 ? g.curProject : 0;
            g.scroll = 0;
            g.nodes = nullptr; g.nodeCount = 0;
            g_filteredIdx.clear();
            drawProjectList(); ui_commit();
            return APP_OUTLINE;
        }

        if (key == '\t') {
            // switch project
            if (!g.projects.empty()) {
                g.curProject = (g.curProject + 1) % (int)g.projects.size();
                g.sel = 0; g.scroll = 0;
                loadOutline();
                rebuildFilter();
            }
        }

        if (key == 'k' || key == KEY_UP) {
            if (g.sel > 0) g.sel--;
        }
        if (key == 'j' || key == KEY_DOWN) {
            int maxIdx = g.filterText.empty() ? (int)g.nodeCount : (int)g_filteredIdx.size();
            if (g.sel < maxIdx - 1) g.sel++;
        }

        if (key == 'a' || key == 'A') {
            // add heading at same level as selected, insert after selected item's group
            if (!g.nodes) {
                g.outlineData = JsonValue::object();
                g.outlineData.set("nodes", JsonValue::array());
                g.nodes = &g.outlineData["nodes"].elements;
                g.nodeCount = 0;
            }
            int idx = g.filterText.empty() ? g.sel : (g.sel < (int)g_filteredIdx.size() ? g_filteredIdx[g.sel] : -1);
            if (idx >= 0 && (size_t)idx < g.nodeCount) {
                g.pendingLevel = (*g.nodes)[idx]["level"].asInt(0);
                // Find end of current item's group (skip sub-items at higher levels)
                int insertPos = idx + 1;
                while (insertPos < (int)g.nodeCount && (*g.nodes)[insertPos]["level"].asInt(0) > g.pendingLevel)
                    insertPos++;
                g.insertAfter = insertPos - 1;
            } else {
                g.pendingLevel = 0;
                g.insertAfter = -1;
            }
            g.mode = M_ADD_HEADING;
            g.editBuf.clear(); g.editCur = 0;
            g.imeActive = true; g_ime.setActive(true);
        }

        if (key == 'i' || key == 'I') {
            // add sub-heading at next level from selected, insert right after selected
            if (!g.nodes || (int)g.nodeCount == 0) {
                // create first heading
                if (!g.nodes) {
                    g.outlineData = JsonValue::object();
                    g.outlineData.set("nodes", JsonValue::array());
                    g.nodes = &g.outlineData["nodes"].elements;
                    g.nodeCount = 0;
                }
                g.mode = M_ADD_HEADING;
                g.insertAfter = -1;
            } else {
                int idx = g.filterText.empty() ? g.sel : (g.sel < (int)g_filteredIdx.size() ? g_filteredIdx[g.sel] : -1);
                g.insertAfter = (idx >= 0 && (size_t)idx < g.nodeCount) ? idx : -1;
                g.mode = M_ADD_SUB;
            }
            g.editBuf.clear(); g.editCur = 0;
            g.imeActive = true; g_ime.setActive(true);
        }

        if (key == '/') {
            g.mode = M_FILTER;
            g.imeActive = true; g_ime.setActive(true);
        }

        if (key == 'e' || key == 'E') {
            // Edit heading title/note
            int idx = g.filterText.empty() ? g.sel : (g.sel < (int)g_filteredIdx.size() ? g_filteredIdx[g.sel] : -1);
            if (idx >= 0 && g.nodes && (size_t)idx < g.nodeCount) {
                g.editNoteIdx = idx;
                g.editingTitle = true;
                g.editBuf = (*g.nodes)[idx]["title"].asString();
                g.editCur = (int)g.editBuf.length();
                g.imeActive = true;
                g_ime.setActive(true);
                g.mode = M_EDIT_NOTE;
                drawInputOverlay("编辑标题");
                ui_commit();
                return APP_OUTLINE;
            }
        }

        if (key == 0x05) {  // Ctrl+E — export
            std::string md = exportMD();
            time_t now; time(&now); struct tm *tm = localtime(&now);
            char fname[64];
            strftime(fname, sizeof(fname), "/sdcard/outline/export_%Y%m%d_%H%M%S.md", tm);
            FILE *f = fopen(fname, "w");
            if (f) {
                fwrite(md.data(), 1, md.size(), f);
                fclose(f);
                ctx.statusMessage = "已导出";
            }
        }

        if ((key == 'd' || key == 'D') && g.nodes && g.nodeCount > 0) {
            int idx = g.filterText.empty() ? g.sel : (g.sel < (int)g_filteredIdx.size() ? g_filteredIdx[g.sel] : -1);
            if (idx >= 0 && (size_t)idx < g.nodeCount) {
                g.confirmAction = 1;
                g.confirmIdx = idx;
                g.confirmMsg = std::string("删除标题「") + (*g.nodes)[idx]["title"].asString() + "」?";
                g.mode = M_CONFIRM;
            }
        }

        if ((key == 'c' || key == 'C') && g.nodes && g.nodeCount > 0) {
            int idx = g.filterText.empty() ? g.sel : (g.sel < (int)g_filteredIdx.size() ? g_filteredIdx[g.sel] : -1);
            if (idx >= 0 && (size_t)idx < g.nodeCount && !(*g.nodes)[idx]["file"].asString().empty()) {
                g.confirmAction = 3;
                g.confirmIdx = idx;
                g.confirmMsg = std::string("清除「") + (*g.nodes)[idx]["title"].asString() + "」的文件关联?";
                g.mode = M_CONFIRM;
            }
        }

        if (key == 'n' || key == 'N') {
            // new project (without leaving browse mode)
            g.mode = M_ADD_PROJECT;
            g.editBuf.clear(); g.editCur = 0;
            g.imeActive = true; g_ime.setActive(true);
        }

        if (key == 0x0A || key == 0x0D) {
            // Enter: open/create associated content file
            int idx = g.filterText.empty() ? g.sel : (g.sel < (int)g_filteredIdx.size() ? g_filteredIdx[g.sel] : -1);
            if (idx >= 0 && g.nodes && (size_t)idx < g.nodeCount) {
                auto &node = (*g.nodes)[idx];
                std::string file = node["file"].asString();
                std::string title = node["title"].asString();
                if (file.empty()) {
                    file = safeFilename(title);
                    node.set("file", file);
                    saveOutline();
                }

                std::string fullPath = ensureContentFile(g.projects[g.curProject], file);
                std::string content = readContentFile(fullPath);

                // strip journal header if present
                std::string body = extractBody(content);
                if (body.empty()) body = content;

                // set up editor
                ctx.editContent = body;
                g.pendingJournalFile = std::string("__outline_") + file;
                ctx.editFilename = g.pendingJournalFile;
                g.pendingOutlineTarget = fullPath;
                ctx.prevState = APP_OUTLINE;
                ctx.nextState = APP_EDITOR;
                return APP_EDITOR;
            }
        }

        drawOutline(); ui_commit();
        return APP_OUTLINE;
    }

    // ── M_ADD_HEADING / M_ADD_SUB ────────────────────────────────────
    if (g.mode == M_ADD_HEADING || g.mode == M_ADD_SUB) {
        const char *addTitle = (g.mode == M_ADD_SUB) ? "添加子标题" : "添加标题";
        if (g.imeActive && key != 0) {
            std::string imeOut;
            if (g_ime.handleKey(key, imeOut)) {
                if (!imeOut.empty()) {
                    g.editBuf.insert(g.editCur, imeOut);
                    g.editCur += (int)imeOut.length();
                }
                drawInputOverlay(addTitle); return APP_OUTLINE;
            }
        }
        if (key == KEY_IME_TOGGLE) {
            g.imeActive = !g.imeActive; g_ime.setActive(g.imeActive);
            drawInputOverlay(addTitle); return APP_OUTLINE;
        }
        if (key == 0x1B) {
            g.mode = M_BROWSE; g.imeActive = false; g_ime.setActive(false);
            g.insertAfter = -1;
        } else if (key == 0x0A || key == 0x0D) {
            if (!g.editBuf.empty()) {
                if (!g.nodes) {
                    g.outlineData = JsonValue::object();
                    g.outlineData.set("nodes", JsonValue::array());
                    g.nodes = &g.outlineData["nodes"].elements;
                    g.nodeCount = 0;
                }

                int newLevel = g.pendingLevel;
                if (g.mode == M_ADD_SUB && g.sel < (int)g.nodeCount) {
                    newLevel = (*g.nodes)[g.sel]["level"].asInt(0) + 1;
                }

                JsonValue node;
                node.set("id", makeId());
                node.set("title", g.editBuf);
                node.set("level", newLevel);
                node.set("file", "");
                node.set("note", "");
                if (g.insertAfter >= 0 && g.insertAfter < (int)g.nodes->size())
                    g.nodes->insert(g.nodes->begin() + g.insertAfter + 1, node);
                else
                    g.nodes->push_back(node);
                g.nodeCount = g.nodes->size();
                saveOutline();
                // Position cursor on the newly inserted node
                int newIdx = (g.insertAfter >= 0) ? g.insertAfter + 1 : (int)g.nodeCount - 1;
                g.sel = newIdx;
                rebuildFilter();
            }
            g.mode = M_BROWSE; g.imeActive = false; g_ime.setActive(false);
            g.insertAfter = -1;
            g.insertAfter = -1;
        } else if (key == 0x7F || key == 0x08) {
            if (g.editCur > 0) {
                int prev = g.editCur - 1;
                while (prev > 0 && ((unsigned char)g.editBuf[prev] & 0xC0) == 0x80) prev--;
                g.editBuf.erase(prev, g.editCur - prev); g.editCur = prev;
            }
        } else if (key == KEY_LEFT) {
            if (g.editCur > 0) { g.editCur--;
                while (g.editCur > 0 && ((unsigned char)g.editBuf[g.editCur] & 0xC0) == 0x80) g.editCur--; }
        } else if (key == KEY_RIGHT) {
            if (g.editCur < (int)g.editBuf.length()) { g.editCur++;
                while (g.editCur < (int)g.editBuf.length() && ((unsigned char)g.editBuf[g.editCur] & 0xC0) == 0x80) g.editCur++; }
        } else if (key >= 0x20 && key <= 0x7E) {
            g.editBuf.insert(g.editCur, 1, (char)key); g.editCur++;
        }
        drawInputOverlay(addTitle);
        return APP_OUTLINE;
    }

    // Fallback
    drawOutline(); ui_commit();
    return APP_OUTLINE;
}
