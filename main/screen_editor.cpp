#include "screen_editor.h"
#include "font_renderer.h"
#include "journal_storage.h"
#include "deepseek_client.h"
#include "wifi_manager.h"
#include "settings_manager.h"
#include "ui_helpers.h"
#include "markdown_render.h"
#include "ime/IME.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

extern u8g2_t *g_u8g2;

extern "C" {
    extern void u8g2_SetDrawColor(void *u8g2, int color);
    extern void u8g2_DrawBox(void *u8g2, int x, int y, int w, int h);
    extern void u8g2_DrawHLine(void *u8g2, int x, int y, int w);
    extern void u8g2_DrawFrame(void *u8g2, int x, int y, int w, int h);
}

#include "clipboard.h"

// 组合输入时正文区下边界(候选栏+码行占状态栏行,正文只留 3 行)
#define IME_CAND_Y (STATUS_Y - FONT_H + g_font.ascent() - 3)
#define EDITOR_MAX_CELLS (SCREEN_W / g_font.halfAdvance())

// ── Editor state ─────────────────────────────────────────────────────────

static struct {
    std::vector<std::string> lines;
    int cx = 0, cy = 0;
    int scroll = 0;
    bool wantPromptView = false;  // 光标滚回顶部时把提示区拉回视野(一次性)
    int targetCx = -1;
    std::string promptText;
    bool promptMode = false;
    bool imeActive = false;
    bool confirmSave = false;
    bool vrowsDirty = true;
    std::vector<VRow> cachedVrows;
    int cachedWordCount = 0;
    bool wordCountDirty = true;
    int64_t autoSaveTime = 0;
    bool modifiedSinceSave = false;
    std::string savedFilename;

    // Selection
    bool hasSelection = false;
    int selAnchorCy = 0, selAnchorCx = 0;
} g_editor;

// ── Selection helpers ────────────────────────────────────────────────────
// Selection is defined by anchor (selAnchorCy, selAnchorCx) and cursor (cy, cx).
// The "start" is the earlier position, "end" is the later one.

struct TextPos { int cy, cx; };

static bool posLess(const TextPos &a, const TextPos &b) {
    if (a.cy != b.cy) return a.cy < b.cy;
    return a.cx < b.cx;
}

static void getSelRange(TextPos &start, TextPos &end) {
    if (!g_editor.hasSelection) {
        start = {g_editor.cy, g_editor.cx};
        end = start;
        return;
    }
    TextPos anchor = {g_editor.selAnchorCy, g_editor.selAnchorCx};
    TextPos cursor = {g_editor.cy, g_editor.cx};
    if (posLess(anchor, cursor)) { start = anchor; end = cursor; }
    else { start = cursor; end = anchor; }
}

static std::string getSelectedText() {
    TextPos start, end;
    getSelRange(start, end);
    if (start.cy == end.cy && start.cx == end.cx) return "";
    std::string result;
    if (start.cy == end.cy) {
        result = g_editor.lines[start.cy].substr(start.cx, end.cx - start.cx);
    } else {
        result = g_editor.lines[start.cy].substr(start.cx) + "\n";
        for (int i = start.cy + 1; i < end.cy; i++)
            result += g_editor.lines[i] + "\n";
        result += g_editor.lines[end.cy].substr(0, end.cx);
    }
    return result;
}

static void deleteSelection() {
    TextPos start, end;
    getSelRange(start, end);
    if (start.cy == end.cy && start.cx == end.cx) return;
    // Keep text before start and after end, join on same line
    g_editor.lines[start.cy] = g_editor.lines[start.cy].substr(0, start.cx)
        + g_editor.lines[end.cy].substr(end.cx);
    // Remove lines between start and end
    if (end.cy > start.cy)
        g_editor.lines.erase(g_editor.lines.begin() + start.cy + 1,
                             g_editor.lines.begin() + end.cy + 1);
    g_editor.cy = start.cy;
    g_editor.cx = start.cx;
    g_editor.hasSelection = false;
    g_editor.targetCx = -1;
    g_editor.vrowsDirty = true;
    g_editor.wordCountDirty = true;
    g_editor.autoSaveTime = esp_timer_get_time() + 3000000;
    g_editor.modifiedSinceSave = true;
}

static void clearSelection() {
    g_editor.hasSelection = false;
}

static void extendSelection() {
    if (!g_editor.hasSelection) {
        g_editor.selAnchorCy = g_editor.cy;
        g_editor.selAnchorCx = g_editor.cx;
        g_editor.hasSelection = true;
    }
}

static const std::vector<VRow>& getVrows() {
    if (g_editor.vrowsDirty) {
        g_editor.cachedVrows = buildVrows(g_editor.lines);
        g_editor.vrowsDirty = false;
    }
    return g_editor.cachedVrows;
}

static int getWordCount() {
    if (g_editor.wordCountDirty) {
        std::string fullText;
        for (auto &l : g_editor.lines) {
            if (!fullText.empty()) fullText += '\n';
            fullText += l;
        }
        g_editor.cachedWordCount = countVisibleChars(fullText);
        g_editor.wordCountDirty = false;
    }
    return g_editor.cachedWordCount;
}

// ── Editor drawing ────────────────────────────────────────────────────────
// When true, drawEditor renders the content but skips the IME strip and the
// status bar — used by the voice dictation screen as a transparent background.
static bool s_skipStatusBarAndIme = false;

static void drawEditor() {
    int y = FONT_H;

    // 提示区:作为文档顶部可滚动块(提示行 + 一条分隔线),滚动时随正文一起滚出
    std::vector<VRow> pvrows;
    std::string promptStr;
    if (g_editor.promptMode && !g_editor.promptText.empty()) {
        promptStr = g_editor.promptText;
        pvrows = buildVrows({promptStr});
    }
    int promptN = (int)pvrows.size();
    int headerN = promptN + (promptN > 0 ? 1 : 0);  // + 分隔线行

    const auto& vrows = getVrows();
    bool composing = g_ime.composing() && !s_skipStatusBarAndIme;
    int contentEndY = composing ? IME_CAND_Y : STATUS_Y;
    int visibleVrows = (contentEndY - y + LINE_SPACING - 1) / LINE_SPACING;
    if (visibleVrows < 1) visibleVrows = 1;

    int cursorVR = -1;
    for (int vi = 0; vi < (int)vrows.size(); vi++) {
        if (vrows[vi].lineIdx == g_editor.cy && vrows[vi].start <= g_editor.cx && g_editor.cx <= vrows[vi].end) {
            cursorVR = vi;
            break;
        }
    }

    int normalVisibleVrows = (STATUS_Y - y + LINE_SPACING - 1) / LINE_SPACING;
    // 组合输入时底部只占一行(候选栏并入状态栏行),正文比旧布局多出一行
    int effectiveVisibleVrows = composing ? (normalVisibleVrows - 1) : normalVisibleVrows;
    if (effectiveVisibleVrows < 1) effectiveVisibleVrows = 1;

    // scroll 索引"提示区 + 正文"联合行;光标在正文,坐标为 headerN + cursorVR
    int cursorAll = headerN + (cursorVR >= 0 ? cursorVR : 0);
    if (g_editor.wantPromptView && cursorVR >= 0) {
        // 光标滚回顶部时尽量把提示区拉回视野(同时保证光标可见)
        g_editor.scroll = std::max(0, headerN - effectiveVisibleVrows + 1);
        g_editor.wantPromptView = false;
    } else {
        if (cursorAll < g_editor.scroll) g_editor.scroll = cursorAll;
    }
    if (cursorVR >= 0 && cursorAll >= g_editor.scroll + effectiveVisibleVrows)
        g_editor.scroll = cursorAll - effectiveVisibleVrows + 1;
    if (g_editor.scroll < 0) g_editor.scroll = 0;

    bool mdOn = g_settings.markdownRender();
    mdSetRenderEnabled(mdOn);
    std::vector<MdLineInfo> mdInfo;
    if (mdOn) {
        mdInfo = mdClassifyLines(g_editor.lines);
    } else {
        mdInfo.assign(g_editor.lines.size(), MdLineInfo{});
    }
    int allN = headerN + (int)vrows.size();
    for (int i = 0; i < visibleVrows && (g_editor.scroll + i) < allN; i++) {
        int allIdx = g_editor.scroll + i;
        if (allIdx < promptN) {
            auto &pvr = pvrows[allIdx];
            std::string rowText = promptStr.substr(pvr.start, pvr.end - pvr.start);
            ui_draw_text(4, y + i * LINE_SPACING, rowText.c_str(), false, true);
        } else if (promptN > 0 && allIdx == promptN) {
            u8g2_DrawHLine(g_u8g2, 0, y + i * LINE_SPACING, SCREEN_W);
        } else {
            auto &vr = vrows[allIdx - headerN];
            mdDrawVrow(4, y + i * LINE_SPACING, g_editor.lines[vr.lineIdx], vr.start, vr.end, mdInfo[vr.lineIdx]);
        }
    }

    // Selection highlight
    if (g_editor.hasSelection) {
        TextPos selStart, selEnd;
        getSelRange(selStart, selEnd);
        for (int i = 0; i < visibleVrows && (g_editor.scroll + i) < allN; i++) {
            int allIdx = g_editor.scroll + i;
            if (allIdx < headerN) continue;  // 提示区/分隔线不参与选区
            auto &vr = vrows[allIdx - headerN];
            int lineIdx = vr.lineIdx;
            int rowStart = vr.start, rowEnd = vr.end;
            if (lineIdx < selStart.cy || lineIdx > selEnd.cy) continue;
            // Calculate overlap of [rowStart, rowEnd) with selection on this line
            int hlStart = rowStart, hlEnd = rowEnd;
            if (lineIdx == selStart.cy) hlStart = std::max(hlStart, selStart.cx);
            if (lineIdx == selEnd.cy) hlEnd = std::min(hlEnd, selEnd.cx);
            if (hlStart >= hlEnd) continue;
            // Highlight range [hlStart, hlEnd) on this vrow
            const MdLineInfo &mdi = mdInfo[lineIdx];
            std::string sel = g_editor.lines[lineIdx].substr(hlStart, hlEnd - hlStart);
            int xOff = 4 + mdVrowX(g_editor.lines[lineIdx], mdi, hlStart, rowStart);
            int selW = g_font.textWidth(sel.c_str());
            int ly = y + i * LINE_SPACING;
            u8g2_SetDrawColor(g_u8g2, 2);  // XOR mode
            u8g2_DrawBox(g_u8g2, xOff, ly - g_font.ascent(), selW, FONT_H);
            u8g2_SetDrawColor(g_u8g2, 1);  // restore
        }
    }

    if (cursorVR >= 0 && cursorAll >= g_editor.scroll && cursorAll < g_editor.scroll + visibleVrows) {
        auto &vr = vrows[cursorVR];
        const std::string &line = g_editor.lines[vr.lineIdx];
        const MdLineInfo &mdi = mdInfo[vr.lineIdx];
        int cx = 4 + mdVrowX(line, mdi, g_editor.cx, vr.start);
        int cy_draw = y + (cursorAll - g_editor.scroll) * LINE_SPACING;
        int cw = g_font.halfAdvance();
        if (g_editor.cx < (int)line.length()) {
            const char *cp = line.c_str() + g_editor.cx;
            unsigned char b = (unsigned char)*cp;
            std::string oneChar;
            if (b < 0x80) oneChar = line.substr(g_editor.cx, 1);
            else if ((b & 0xE0) == 0xC0) oneChar = line.substr(g_editor.cx, 2);
            else if ((b & 0xF0) == 0xE0) oneChar = line.substr(g_editor.cx, 3);
            else if ((b & 0xF8) == 0xF0) oneChar = line.substr(g_editor.cx, 4);
            if (!oneChar.empty()) cw = g_font.textWidth(oneChar.c_str());
        }
        u8g2_SetDrawColor(g_u8g2, 0);
        u8g2_DrawBox(g_u8g2, cx, cy_draw + 4, cw, 3);
        u8g2_SetDrawColor(g_u8g2, 1);
    }

    if (composing) {
        std::string code = g_ime.displayCode();
        int pageSize = g_ime.pageSize();
        int curPage = g_ime.currentPage();
        int totalPages = g_ime.totalPages();
        if (totalPages < 1) totalPages = 1;
        char pageInfo[32];
        snprintf(pageInfo, sizeof(pageInfo), "%d/%d", curPage, totalPages);

        // 候选栏直接占用状态栏位置(整行同状态栏样式),此状态下不再画状态栏。
        // 其顶线(STATUS_Y)即唯一分割线,码行浮在它上面。
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
        u8g2_SetDrawColor(g_u8g2, 0);
        u8g2_DrawHLine(g_u8g2, 0, STATUS_Y, SCREEN_W);
        u8g2_SetDrawColor(g_u8g2, 1);
        u8g2_DrawBox(g_u8g2, 0, STATUS_Y + 1, SCREEN_W, FONT_H + 3);
        u8g2_SetDrawColor(g_u8g2, 0);
        g_font.drawText(4, STATUS_Y + 1 + g_font.ascent(), candLine.c_str(), false);
        u8g2_SetDrawColor(g_u8g2, 1);

        // 码行下移到候选栏上方(顶沿盖住候选栏顶线),给正文多出一行。
        // 基线取 STATUS_Y - descent - 1:字形+下伸的总底端=108,不越分割线(109)
        int codeBaseline = STATUS_Y - g_font.descent() - 1;
        int codeBoxTop = STATUS_Y - FONT_H;  // 85:框顶固定,不随文字抬升盖住第3行光标
        {
            int cw = g_font.textWidth(code.c_str()) + 8;
            u8g2_SetDrawColor(g_u8g2, 1);
            u8g2_DrawBox(g_u8g2, 4, codeBoxTop, cw, FONT_H);
            u8g2_SetDrawColor(g_u8g2, 0);
            g_font.drawText(4, codeBaseline, code.c_str(), false);
            u8g2_SetDrawColor(g_u8g2, 1);
        }
        {
            int tw = g_font.textWidth(pageInfo);
            int pw = tw + 8;
            int px = SCREEN_W - pw - 4;
            u8g2_SetDrawColor(g_u8g2, 1);
            u8g2_DrawBox(g_u8g2, px, codeBoxTop, pw, FONT_H);
            u8g2_SetDrawColor(g_u8g2, 0);
            g_font.drawText(px + 4, codeBaseline, pageInfo, false);
            u8g2_SetDrawColor(g_u8g2, 1);
        }
    }

    if (!s_skipStatusBarAndIme && !composing) {
        int wc = getWordCount();
        char left[48];
        if (g_quickEdit) snprintf(left, sizeof(left), "[%d]", g_quickFile);
        else snprintf(left, sizeof(left), "%s", g_editor.promptMode ? "提示" : "自由");
        std::string imeLabel;
        if (!g_editor.imeActive) imeLabel = "EN";
        else if (g_ime.english()) imeLabel = "[英]";
        else {
            imeLabel = "[中]";
            imeLabel += g_ime.fullwidth() ? "\xe2\x97\x8f" : "\xe2\x97\x90"; // ● or ◐
            imeLabel += g_ime.trad() ? "繁" : "简";
        }
        std::string right = std::to_string(wc) + "字 " + imeLabel;
        std::string bt = battery_icon_status_text();
        if (!bt.empty()) right += bt;  // 与 [中]●繁 之间不留空格,空间留给字数

        ui_draw_status(left, right.c_str());
    }
}

// Draw only the editor content (no IME strip, no status bar) so the voice
// dictation screen can show the editor as a live-transparent background.
void screen_editor_draw_voice_bg() {
    g_ime.cancelComposition();  // cancel any in-progress composition
    s_skipStatusBarAndIme = true;
    drawEditor();
    s_skipStatusBarAndIme = false;
}

static void drawConfirmDialog() {
    int bw = SCREEN_W - 16, bh = 82;  // 240x135 屏内居中
    int bx = (SCREEN_W - bw) / 2, by = (SCREEN_H - bh) / 2 - 10;
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, bx, by, bw, bh);
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawFrame(g_u8g2, bx, by, bw, bh);
    ui_draw_text_centered(by + 28, "是否保存当前内容？");
    ui_draw_text_centered(by + 52, "Enter=保存");
    ui_draw_text_centered(by + 76, "ESC=放弃");
    u8g2_SetDrawColor(g_u8g2, 1);
}

// ── Editor save helper ────────────────────────────────────────────────────
static bool saveCurrentContent() {
    std::string text;
    for (auto &l : g_editor.lines) { text += l; text += '\n'; }
    while (!text.empty() && text.back() == '\n') text.pop_back();

    // 快捷编辑:原文写入 SD 根目录 0.txt..9.txt,不加日记头部
    if (g_quickEdit) {
        return g_journal.saveQuickFile(g_quickFile, text);
    }
    if (text.empty()) return false;

    time_t now; time(&now); struct tm *tm = localtime(&now);
    char ts[32]; strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);
    int wc = getWordCount();
    std::string headerStr; headerStr.resize(128);
    int hlen = snprintf(&headerStr[0], 128, "日期: %s\n字数: %d\n\n", ts, wc);
    headerStr.resize(hlen);
    std::string fullText;
    if (g_editor.promptMode)
        fullText = headerStr + "提示词: " + g_editor.promptText + "\n\n" + text;
    else
        fullText = headerStr + "自由写作\n\n" + text;

    if (g_editor.savedFilename.empty()) {
        char fname[32];
        strftime(fname, sizeof(fname), "%Y-%m-%d_%H%M%S", tm);
        g_editor.savedFilename = std::string(fname) + ".txt";
    }
    return g_journal.saveEntryRaw(g_editor.savedFilename, fullText);
}

static AppState finishEditor(ScreenContext &ctx) {
    g_editor.modifiedSinceSave = false;
    if (saveCurrentContent()) {
        ctx.nextState = ctx.prevState;
        return ctx.prevState;
    }
    ctx.statusMessage = "保存失败，请检查SD卡";
    ctx.nextState = ctx.prevState;
    return ctx.prevState;
}

// 把文本按行拆进编辑器,光标落末尾(空内容 → 单个空行)
static void editorLoadText(const std::string &content) {
    g_editor.lines.clear();
    if (content.empty()) {
        g_editor.lines.push_back("");
        g_editor.cx = g_editor.cy = 0;
        return;
    }
    size_t pos = 0;
    while (pos < content.length()) {
        size_t nl = content.find('\n', pos);
        g_editor.lines.push_back((nl == std::string::npos) ? content.substr(pos) : content.substr(pos, nl - pos));
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    while (g_editor.lines.size() > 1 && g_editor.lines.back().empty())
        g_editor.lines.pop_back();
    g_editor.cx = (int)g_editor.lines.back().length();
    g_editor.cy = (int)g_editor.lines.size() - 1;
    g_editor.hasSelection = false;
    g_editor.targetCx = -1;
}

// ── Screen entry points ──────────────────────────────────────────────────
void screen_editor_init(ScreenContext &ctx) {
    g_editor.lines.clear();
    g_editor.autoSaveTime = 0;

    if (g_quickEdit) {
        editorLoadText(g_journal.readQuickFile(g_quickFile));
        ctx.editContent.clear();
    } else if (!ctx.editContent.empty()) {
        editorLoadText(ctx.editContent);
        ctx.editContent.clear();
    } else {
        g_editor.lines.push_back("");
        g_editor.cx = g_editor.cy = 0;
    }

    g_editor.scroll = 0;
    g_editor.wantPromptView = false;
    g_editor.targetCx = -1;
    g_editor.imeActive = false;
    g_ime.setActive(false);
    g_ime.setFullwidth(false);
    g_ime.setEnglish(false);
    g_editor.confirmSave = false;
    g_editor.modifiedSinceSave = false;
    g_editor.vrowsDirty = true; g_editor.wordCountDirty = true;
    g_editor.promptText = ctx.promptText;
    g_editor.promptMode = ctx.promptMode;
    g_editor.savedFilename = ctx.editFilename;
    ctx.editFilename.clear();
}

AppState screen_editor_handle(int key, ScreenContext &ctx) {
    const auto& vrows = getVrows();

    if (g_editor.confirmSave) {
        if (key == 0x0A || key == 0x0D || key == 'y' || key == 'Y') {
            g_editor.confirmSave = false;
            return finishEditor(ctx);
        }
        if (key == 0x1B || key == 'n' || key == 'N') {
            g_editor.confirmSave = false;
            ctx.nextState = ctx.prevState;
            return ctx.prevState;
        }
        ui_clear(); drawEditor(); drawConfirmDialog(); ui_commit();
        return APP_EDITOR;
    }

    // ── 快捷编辑:Fn+0..9 切换文件(先自动保存当前,再加载新文件) ────────
    if (g_quickEdit && key >= KEY_QUICK_BASE && key < KEY_QUICK_BASE + 10) {
        int newFile = key - KEY_QUICK_BASE;
        if (newFile != g_quickFile) {
            if (g_editor.modifiedSinceSave) saveCurrentContent();
            g_editor.modifiedSinceSave = false;
            g_editor.autoSaveTime = 0;
            g_ime.cancelComposition();
            g_ime.setActive(false);
            g_editor.imeActive = false;
            g_quickFile = newFile;
            g_settings.setQuickFile(newFile);
            editorLoadText(g_journal.readQuickFile(newFile));
            g_editor.scroll = 0;
            g_editor.vrowsDirty = true;
            g_editor.wordCountDirty = true;
        }
        ui_clear(); drawEditor(); ui_commit(); return APP_EDITOR;
    }

    if (g_editor.imeActive && key != 0) {
        std::string imeOut;
        if (g_ime.handleKey(key, imeOut)) {
            editorInsertText(imeOut);
            ui_clear(); drawEditor(); ui_commit(); return APP_EDITOR;
        }
    }

    if (key == 9) {
        g_editor.lines[g_editor.cy].insert(g_editor.cx, 4, ' ');
        g_editor.cx += 4;
        g_editor.targetCx = -1;
        g_editor.vrowsDirty = true; g_editor.wordCountDirty = true;
        g_editor.autoSaveTime = esp_timer_get_time() + 3000000;
        g_editor.modifiedSinceSave = true;
        ui_clear(); drawEditor(); ui_commit(); return APP_EDITOR;
    }
    if (key == 0x10 && !g_quickEdit) { // Ctrl+P (快捷编辑不生成 AI 提示)
        bool wifiWasConnected = g_wifi.isConnected();
        if (ensure_wifi_connected()) {
            g_editor.promptMode = true;
            ui_clear();
            ui_show_message_centered("AI生成提示中...");
            std::string context;
            std::string exp = g_settings.personalExperience();
            std::string hob = g_settings.personalHobbies();
            if (!exp.empty()) context += "我的经历:" + exp + ";";
            if (!hob.empty()) context += "我的爱好:" + hob + ";";
            if (context.empty()) context = "一个普通用户";
            auto result = g_deepseek.generatePrompt(context);
            if (result.success && !result.content.empty()) {
                g_editor.promptText = result.content;
            } else if (g_editor.promptText.empty()) {
                g_editor.promptText = "今天发生了什么？";
            }
        } else {
            ui_clear();
            ui_show_message_centered("WiFi连接失败");
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
        restore_wifi_state(wifiWasConnected);
        ui_clear(); drawEditor(); ui_commit();
        return APP_EDITOR;
    }
    if (key == 0x1B) {
        if (g_quickEdit) {
            // 快捷编辑:自动保存,直接回设置面板
            if (g_editor.modifiedSinceSave) saveCurrentContent();
            g_editor.modifiedSinceSave = false;
            ctx.nextState = ctx.prevState; return ctx.prevState;
        }
        bool hasContent = g_editor.lines.size() > 1 ||
            (g_editor.lines.size() == 1 && !g_editor.lines[0].empty());
        if (hasContent && g_editor.modifiedSinceSave) {
            g_editor.confirmSave = true;
            ui_clear(); drawEditor(); drawConfirmDialog(); ui_commit();
            return APP_EDITOR;
        }
        ctx.nextState = ctx.prevState; return ctx.prevState;
    }
    if (key == 0x13) {
        std::string text;
        for (auto &l : g_editor.lines) { text += l; text += '\n'; }
        while (!text.empty() && text.back() == '\n') text.pop_back();
        if (!text.empty()) {
            if (saveCurrentContent()) ctx.statusMessage = "已保存";
            else ctx.statusMessage = "保存失败";
        }
        ui_clear(); drawEditor(); ui_commit();
        g_editor.autoSaveTime = 0;
        g_editor.modifiedSinceSave = false;
        return APP_EDITOR;
    }  // Ctrl+S
    if (key == 0x11) { ctx.nextState = ctx.prevState; return ctx.prevState; }  // Ctrl+Q
    if (key == 0x06) {  // Ctrl+F
        ctx.nextState = APP_SYNC_SEND_FLOMO;
        return APP_SYNC_SEND_FLOMO;
    }

    // ── Clipboard operations (Ctrl+C, Ctrl+X, Ctrl+V) ────────────────
    if (key == 0x03) { // Ctrl+C — copy
        if (g_editor.hasSelection) {
            g_clipboard = getSelectedText();
            clearSelection();
        }
        ui_clear(); drawEditor(); ui_commit(); return APP_EDITOR;
    }
    if (key == 0x18) { // Ctrl+X — cut
        if (g_editor.hasSelection) {
            g_clipboard = getSelectedText();
            deleteSelection();
        }
        ui_clear(); drawEditor(); ui_commit(); return APP_EDITOR;
    }
    if (key == 0x16) { // Ctrl+V — paste
        if (!g_clipboard.empty()) {
            if (g_editor.hasSelection) {
                deleteSelection(); // removes selection, then paste at cursor
            }
            g_editor.lines[g_editor.cy].insert(g_editor.cx, g_clipboard);
            g_editor.cx += (int)g_clipboard.length();
            g_editor.targetCx = -1;
            g_editor.vrowsDirty = true; g_editor.wordCountDirty = true;
            g_editor.autoSaveTime = esp_timer_get_time() + 3000000;
            g_editor.modifiedSinceSave = true;
        }
        ui_clear(); drawEditor(); ui_commit(); return APP_EDITOR;
    }

    // ── Ctrl+;.,/: extend selection (Ctrl+;=上 . =下 , =左 / =右) ─────
    if (key == KEY_CTRL_LEFT) {
        if (g_editor.cx > 0) {
            extendSelection();
            g_editor.cx--;
            while (g_editor.cx > 0 && ((unsigned char)g_editor.lines[g_editor.cy][g_editor.cx] & 0xC0) == 0x80) g_editor.cx--;
        } else if (g_editor.cy > 0) {
            extendSelection();
            g_editor.cy--;
            g_editor.cx = (int)g_editor.lines[g_editor.cy].length();
        }
        g_editor.targetCx = -1;
        g_editor.vrowsDirty = true;
        ui_clear(); drawEditor(); ui_commit(); return APP_EDITOR;
    }
    if (key == KEY_CTRL_RIGHT) {
        if (g_editor.cx < (int)g_editor.lines[g_editor.cy].length()) {
            extendSelection();
            g_editor.cx++;
            while (g_editor.cx < (int)g_editor.lines[g_editor.cy].length() && ((unsigned char)g_editor.lines[g_editor.cy][g_editor.cx] & 0xC0) == 0x80) g_editor.cx++;
        } else if (g_editor.cy < (int)g_editor.lines.size() - 1) {
            extendSelection();
            g_editor.cy++;
            g_editor.cx = 0;
        }
        g_editor.targetCx = -1;
        g_editor.vrowsDirty = true;
        ui_clear(); drawEditor(); ui_commit(); return APP_EDITOR;
    }
    if (key == KEY_CTRL_UP) {
        if (g_editor.cy > 0) {
            extendSelection();
        }
        int curVR = -1;
        for (int vi = 0; vi < (int)vrows.size(); vi++) {
            if (vrows[vi].lineIdx == g_editor.cy && vrows[vi].start <= g_editor.cx && g_editor.cx <= vrows[vi].end) {
                curVR = vi; break;
            }
        }
        if (curVR > 0) {
            auto &prev = vrows[curVR - 1];
            if (g_editor.targetCx < 0)
                g_editor.targetCx = byteToCells(g_editor.lines[g_editor.cy], g_editor.cx);
            int visualCol = g_editor.targetCx % EDITOR_MAX_CELLS;
            g_editor.cy = prev.lineIdx;
            int vrowStartCells = byteToCells(g_editor.lines[g_editor.cy], prev.start);
            int targetCells = vrowStartCells + visualCol;
            g_editor.cx = cellsToByte(g_editor.lines[g_editor.cy], prev.start, prev.end, targetCells);
        }
        g_editor.vrowsDirty = true;
        ui_clear(); drawEditor(); ui_commit(); return APP_EDITOR;
    }
    if (key == KEY_CTRL_DOWN) {
        if (g_editor.cy < (int)g_editor.lines.size() - 1) {
            extendSelection();
        }
        int curVR = -1;
        for (int vi = 0; vi < (int)vrows.size(); vi++) {
            if (vrows[vi].lineIdx == g_editor.cy && vrows[vi].start <= g_editor.cx && g_editor.cx <= vrows[vi].end) {
                curVR = vi; break;
            }
        }
        if (curVR >= 0 && curVR < (int)vrows.size() - 1) {
            auto &next = vrows[curVR + 1];
            if (g_editor.targetCx < 0)
                g_editor.targetCx = byteToCells(g_editor.lines[g_editor.cy], g_editor.cx);
            int visualCol = g_editor.targetCx % EDITOR_MAX_CELLS;
            g_editor.cy = next.lineIdx;
            int vrowStartCells = byteToCells(g_editor.lines[g_editor.cy], next.start);
            int targetCells = vrowStartCells + visualCol;
            g_editor.cx = std::min(cellsToByte(g_editor.lines[g_editor.cy], next.start, next.end, targetCells),
                                   (int)g_editor.lines[g_editor.cy].length());
        }
        g_editor.vrowsDirty = true;
        ui_clear(); drawEditor(); ui_commit(); return APP_EDITOR;
    }

    // Navigation & editing
    if (key == 0x0A || key == 0x0D) { // Enter
        if (g_editor.hasSelection) deleteSelection();
        // 列表行在行尾回车时自动续行:下一行带上同款列表标记
        std::string prefix;
        if (g_editor.cx >= (int)g_editor.lines[g_editor.cy].length()) {
            const std::string &cur = g_editor.lines[g_editor.cy];
            MdListMarker m = mdListMarker(cur);
            if (m.ok) {
                bool emptyItem = cur.substr(m.start + m.len).find_first_not_of(" \t") == std::string::npos;
                if (!emptyItem) {
                    std::string lead = cur.substr(0, m.start);  // 嵌套缩进
                    if (m.task) {
                        prefix = lead + "- [ ] ";
                    } else if (m.ordered) {
                        int d = m.start;
                        while (d < (int)cur.length() && cur[d] >= '0' && cur[d] <= '9') d++;
                        if (d == m.start) {  // 中文序号:一、二、十、… 递增(一→二→…→十→十一)
                            int nlen = 0;
                            int n = mdCnNumValue(cur, m.start, nlen);
                            if (n >= 0)
                                prefix = lead + mdCnNumeral(n + 1) + cur.substr(m.start + nlen, m.len - nlen);
                            else
                                prefix = lead + cur.substr(m.start, m.len);
                        } else {
                            int n = 0;
                            for (int k = m.start; k < d; k++) n = n * 10 + (cur[k] - '0');
                            n++;
                            char num[16];
                            snprintf(num, sizeof(num), "%d", n);
                            prefix = lead + num + cur.substr(d, m.len - (d - m.start));
                        }
                    } else {
                        prefix = lead + cur.substr(m.start, 1) + " ";  // 保留 -/*/+
                    }
                }
            }
        }
        std::string rest = g_editor.lines[g_editor.cy].substr(g_editor.cx);
        g_editor.lines[g_editor.cy] = g_editor.lines[g_editor.cy].substr(0, g_editor.cx);
        g_editor.cx = 0; g_editor.cy++;
        g_editor.lines.insert(g_editor.lines.begin() + g_editor.cy, prefix + rest);
        g_editor.cx = (int)prefix.length();  // 光标落在续行标记之后
        g_editor.targetCx = -1;
        g_editor.vrowsDirty = true; g_editor.wordCountDirty = true;
        g_editor.autoSaveTime = esp_timer_get_time() + 3000000;
        g_editor.modifiedSinceSave = true;
    } else if (key == 0x7F || key == 0x08) { // Backspace
        if (g_editor.hasSelection) {
            deleteSelection();
        } else if (g_editor.cx > 0) {
            int prev = g_editor.cx - 1;
            while (prev > 0 && ((unsigned char)g_editor.lines[g_editor.cy][prev] & 0xC0) == 0x80) prev--;
            g_editor.lines[g_editor.cy].erase(prev, g_editor.cx - prev);
            g_editor.cx = prev;
        } else if (g_editor.cy > 0) {
            g_editor.cx = (int)g_editor.lines[g_editor.cy-1].length();
            g_editor.lines[g_editor.cy-1] += g_editor.lines[g_editor.cy];
            g_editor.lines.erase(g_editor.lines.begin() + g_editor.cy);
            g_editor.cy--;
        }
        g_editor.targetCx = -1;
        g_editor.vrowsDirty = true; g_editor.wordCountDirty = true;
        g_editor.autoSaveTime = esp_timer_get_time() + 3000000;
        g_editor.modifiedSinceSave = true;
    } else if (key >= 0x20 && key <= 0x7E) { // ASCII printable
        if (g_editor.hasSelection) deleteSelection();
        g_editor.lines[g_editor.cy].insert(g_editor.cx, 1, (char)key);
        g_editor.cx++;
        g_editor.targetCx = -1;
        g_editor.vrowsDirty = true; g_editor.wordCountDirty = true;
        g_editor.autoSaveTime = esp_timer_get_time() + 3000000;
        g_editor.modifiedSinceSave = true;
    } else if (key == KEY_LEFT) {
        clearSelection();
        if (g_editor.cx > 0) {
            g_editor.cx--;
            while (g_editor.cx > 0 && ((unsigned char)g_editor.lines[g_editor.cy][g_editor.cx] & 0xC0) == 0x80) g_editor.cx--;
        } else if (g_editor.cy > 0) {
            g_editor.cy--;
            g_editor.cx = (int)g_editor.lines[g_editor.cy].length();
            if (g_editor.cy == 0) g_editor.wantPromptView = true;
        }
        g_editor.targetCx = -1;
    } else if (key == KEY_RIGHT) {
        clearSelection();
        if (g_editor.cx < (int)g_editor.lines[g_editor.cy].length()) {
            g_editor.cx++;
            while (g_editor.cx < (int)g_editor.lines[g_editor.cy].length() && ((unsigned char)g_editor.lines[g_editor.cy][g_editor.cx] & 0xC0) == 0x80) g_editor.cx++;
        } else if (g_editor.cy < (int)g_editor.lines.size() - 1) {
            g_editor.cy++;
            g_editor.cx = 0;
        }
        g_editor.targetCx = -1;
    } else if (key == KEY_UP) {
        clearSelection();
        int curVR = -1;
        for (int vi = 0; vi < (int)vrows.size(); vi++) {
            if (vrows[vi].lineIdx == g_editor.cy && vrows[vi].start <= g_editor.cx && g_editor.cx <= vrows[vi].end) {
                curVR = vi; break;
            }
        }
        if (curVR > 0) {
            auto &prev = vrows[curVR - 1];
            if (g_editor.targetCx < 0)
                g_editor.targetCx = byteToCells(g_editor.lines[g_editor.cy], g_editor.cx);
            int visualCol = g_editor.targetCx % EDITOR_MAX_CELLS;
            g_editor.cy = prev.lineIdx;
            int vrowStartCells = byteToCells(g_editor.lines[g_editor.cy], prev.start);
            int targetCells = vrowStartCells + visualCol;
            g_editor.cx = cellsToByte(g_editor.lines[g_editor.cy], prev.start, prev.end, targetCells);
        }
        if (g_editor.cy == 0) g_editor.wantPromptView = true;  // 回到顶部,把提示区滚回来
    } else if (key == KEY_DOWN) {
        clearSelection();
        int curVR = -1;
        for (int vi = 0; vi < (int)vrows.size(); vi++) {
            if (vrows[vi].lineIdx == g_editor.cy && vrows[vi].start <= g_editor.cx && g_editor.cx <= vrows[vi].end) {
                curVR = vi; break;
            }
        }
        if (curVR >= 0 && curVR < (int)vrows.size() - 1) {
            auto &next = vrows[curVR + 1];
            if (g_editor.targetCx < 0)
                g_editor.targetCx = byteToCells(g_editor.lines[g_editor.cy], g_editor.cx);
            int visualCol = g_editor.targetCx % EDITOR_MAX_CELLS;
            g_editor.cy = next.lineIdx;
            int vrowStartCells = byteToCells(g_editor.lines[g_editor.cy], next.start);
            int targetCells = vrowStartCells + visualCol;
            g_editor.cx = std::min(cellsToByte(g_editor.lines[g_editor.cy], next.start, next.end, targetCells),
                                   (int)g_editor.lines[g_editor.cy].length());
        }
    }

    // Auto-save on idle ticks (快捷编辑无视 auto_save 开关,始终自动保存)
    if (key == 0 && g_editor.autoSaveTime > 0 && esp_timer_get_time() > g_editor.autoSaveTime) {
        g_editor.autoSaveTime = 0;
        if (g_settings.autoSave() || g_quickEdit) {
            if (saveCurrentContent()) g_editor.modifiedSinceSave = false;
        }
    }

    ui_clear(); drawEditor(); ui_commit();
    return APP_EDITOR;
}

// ── App-level helpers ────────────────────────────────────────────────────
bool app_ime_active() {
    return g_editor.imeActive;
}

void app_toggle_ime() {
    g_editor.imeActive = !g_editor.imeActive;
    g_ime.setActive(g_editor.imeActive);
}

bool app_ime_fullwidth() {
    return g_ime.fullwidth();
}

void app_toggle_fullwidth() {
    g_ime.toggleFullwidth();
}

void app_toggle_trad() {
    g_ime.toggleTrad();
}

void app_toggle_english() {
    g_ime.toggleEnglish();
}

static bool g_editorNeedsReinit = false;

void app_editor_request_reinit() {
    g_editorNeedsReinit = true;
}

bool app_editor_needs_reinit() {
    if (g_editorNeedsReinit) {
        g_editorNeedsReinit = false;
        return true;
    }
    return false;
}

std::string app_get_editor_text() {
    std::string text;
    for (auto &l : g_editor.lines) { text += l; text += '\n'; }
    while (!text.empty() && text.back() == '\n') text.pop_back();
    return text;
}

// Insert text at the cursor, shared by IME commit and voice dictation.
void editorInsertText(const std::string &text) {
    if (text.empty()) return;
    if (g_editor.hasSelection) deleteSelection();
    g_editor.lines[g_editor.cy].insert(g_editor.cx, text);
    g_editor.cx += (int)text.length();
    g_editor.targetCx = -1;
    g_editor.vrowsDirty = true; g_editor.wordCountDirty = true;
    g_editor.autoSaveTime = esp_timer_get_time() + 3000000;
    g_editor.modifiedSinceSave = true;
}
