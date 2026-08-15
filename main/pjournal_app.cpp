#include "pjournal_app.h"
#include "clipboard.h"
#include "font_renderer.h"
#include "journal_storage.h"
#include "builtin_prompts.h"
#include "settings_manager.h"
#include "markdown_render.h"
#include "u8g2_st7789.h"
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

extern u8g2_t *g_u8g2;

std::string g_clipboard;  // global clipboard definition
std::string g_flomoPendingText;
AppState g_flomoReturnTo = APP_EDITOR;

bool g_quickEdit = false;   // 编辑模式:快捷编辑(true)/个人日记(false)
int g_quickFile = 0;        // 当前快捷编辑文件 0-9

// ── Screen state ─────────────────────────────────────────────────────────
static struct { int selection = 0; int scroll = 0; } g_browser;
static struct { std::vector<std::string> lines; int scroll = 0; std::string filename;
    std::string dateStr;
    std::vector<VRow> cachedVrows; bool vrowsDirty = true; } g_viewer;

static const std::vector<VRow>& getViewerVrows() {
    if (g_viewer.vrowsDirty) {
        g_viewer.cachedVrows = buildVrows(g_viewer.lines);
        g_viewer.vrowsDirty = false;
    }
    return g_viewer.cachedVrows;
}

// ── Main Screen ────────────────────────────────────────────────────────
void screen_main_init() {
    static bool seeded = false;
    if (!seeded) {
        srand(esp_random());
        seeded = true;
    }
}

// 主菜单:保留原样式(居中 [键] 标签),滚动条 + 方向键选择,快捷键直达
static int g_mainMenuSel = 0;
static int g_mainMenuScroll = 0;

static const struct { char key; const char *label; } MAIN_MENU[] = {
    {'p', "提示写作"},
    {'f', "自由写作"},
    {'v', "查看过往日记"},
    {'w', "同步WebDAV"},
    {'o', "大纲写作"},
    {'s', "设置"},
};
static const int MAIN_MENU_COUNT = (int)(sizeof(MAIN_MENU) / sizeof(MAIN_MENU[0]));

AppState screen_main_handle(int key, ScreenContext &ctx) {
    // `;` `.` `,` `/` 模拟方向键(右下角菱形按键簇)
    if (key == ';') key = KEY_UP;
    else if (key == ',') key = KEY_LEFT;
    else if (key == '.') key = KEY_DOWN;
    else if (key == '/') key = KEY_RIGHT;

    // Ctrl+= / Ctrl+- 亮度调节(0x8E/0x8F 不匹配任何菜单键,落入 activate 无动作)
    if (key == KEY_CTRL_EQUALS && g_lcd_dev) {
        int nb = (int)u8g2_st7789_get_backlight(g_lcd_dev) + 32;
        if (nb > 255) nb = 255;
        u8g2_st7789_set_backlight(g_lcd_dev, (uint8_t)nb);
    }
    if (key == KEY_CTRL_MINUS && g_lcd_dev) {
        int nb = (int)u8g2_st7789_get_backlight(g_lcd_dev) - 32;
        if (nb < 32) nb = 32;
        u8g2_st7789_set_backlight(g_lcd_dev, (uint8_t)nb);
    }

    auto activate = [&](char k) {
        if (k=='p'||k=='P') { ctx.promptMode=true; ctx.promptText=BUILTIN_PROMPTS[rand()%BUILTIN_PROMPT_COUNT]; ctx.prevState=APP_MAIN; ctx.nextState=APP_EDITOR; }
        else if (k=='f'||k=='F') { ctx.promptMode=false; ctx.promptText=""; ctx.prevState=APP_MAIN; ctx.nextState=APP_EDITOR; }
        else if ((k=='v'||k=='V') && g_journal.totalEntries()>0) ctx.nextState=APP_BROWSER;
        else if (k=='w'||k=='W') ctx.nextState=APP_SYNC_WEBDAV;
        else if (k=='s'||k=='S') ctx.nextState=APP_SETTINGS;
        else if (k=='o'||k=='O') ctx.nextState=APP_OUTLINE;
        else if (k=='q'||k=='Q') ctx.nextState=APP_QUIT;
    };

    if (key == KEY_UP) { if (g_mainMenuSel > 0) g_mainMenuSel--; }
    else if (key == KEY_DOWN) { if (g_mainMenuSel < MAIN_MENU_COUNT-1) g_mainMenuSel++; }
    else if (key == 0x0A || key == 0x0D) { activate(MAIN_MENU[g_mainMenuSel].key); }
    else activate(key);

    ui_clear(); int y = 18;
    time_t now_t; time(&now_t); struct tm *tm = localtime(&now_t);
    char dateStr[32]; strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", tm);
    char title[64]; snprintf(title, sizeof(title), "个人日记 %s", dateStr);
    ui_draw_text_centered(y, title, false, true); y += FONT_H;

    char buf[48];
    snprintf(buf, sizeof(buf), "连续:%d天 总计:%d篇", g_journal.getStreak(), g_journal.totalEntries());
    ui_draw_text_centered(y, buf); y += FONT_H;
    int tc = g_journal.countToday();
    if (tc > 0) { snprintf(buf, sizeof(buf), "✓ 今日已写%d篇", tc); ui_draw_text_centered(y, buf, false, true); }
    else ui_draw_text_centered(y, "今日尚未写日记");
    y += FONT_H;

    // 居中菜单(保留原样式):滚动条 + 方向键选择,选中项高亮
    int menuY = y;
    int vis = (STATUS_Y - menuY + FONT_H - 1) / FONT_H;
    if (vis < 1) vis = 1;
    if (vis > MAIN_MENU_COUNT) vis = MAIN_MENU_COUNT;
    if (g_mainMenuSel < g_mainMenuScroll) g_mainMenuScroll = g_mainMenuSel;
    if (g_mainMenuSel >= g_mainMenuScroll + vis) g_mainMenuScroll = g_mainMenuSel - vis + 1;
    if (g_mainMenuScroll > MAIN_MENU_COUNT - vis) g_mainMenuScroll = MAIN_MENU_COUNT - vis;
    if (g_mainMenuScroll < 0) g_mainMenuScroll = 0;

    for (int i = 0; i < vis; i++) {
        int idx = g_mainMenuScroll + i;
        char item[40]; snprintf(item, sizeof(item), "[%c] %s", MAIN_MENU[idx].key, MAIN_MENU[idx].label);
        ui_draw_text_centered(menuY + i * FONT_H, item, idx == g_mainMenuSel);
    }

    // 右侧滚动条
    if (MAIN_MENU_COUNT > vis) {
        int trackH = vis * FONT_H;
        int thumbH = trackH * vis / MAIN_MENU_COUNT;
        if (thumbH < 4) thumbH = 4;
        int thumbY = menuY + (trackH - thumbH) * g_mainMenuScroll / (MAIN_MENU_COUNT - vis);
        u8g2_SetDrawColor(g_u8g2, 0);
        u8g2_DrawBox(g_u8g2, SCREEN_W - 4, thumbY, 3, thumbH);
        u8g2_SetDrawColor(g_u8g2, 0);
    }

    std::string batteryGroup = battery_status_text();
    if (!batteryGroup.empty()) {
        int pw = g_font.textWidth(batteryGroup.c_str());
        g_font.drawText(SCREEN_W - pw - 4, STATUS_Y + g_font.ascent(), batteryGroup.c_str(), false);
    }
    ui_commit();

    return ctx.nextState;
}

// ── Browser Screen ─────────────────────────────────────────────────────
void screen_browser_init() { g_browser.selection = g_browser.scroll = 0; }

AppState screen_browser_handle(int key, ScreenContext &ctx) {
    auto entries = g_journal.listEntries();
    if (entries.empty()) { ctx.nextState = APP_MAIN; return APP_MAIN; }
    if (g_browser.selection >= (int)entries.size()) g_browser.selection = (int)entries.size() - 1;

    if (key == 'q' || key == 'Q' || key == 0x1B) { ctx.nextState = APP_MAIN; return APP_MAIN; }
    if (key == 'j' || key == '.' || key == KEY_DOWN) { g_browser.selection++; if (g_browser.selection>=(int)entries.size()) g_browser.selection=(int)entries.size()-1; }
    if (key == 'k' || key == ';' || key == KEY_UP) { g_browser.selection--; if (g_browser.selection<0) g_browser.selection=0; }
    if (key == 0x0A || key == 0x0D) { ctx.selectedEntry = entries[g_browser.selection].filename; ctx.nextState = APP_VIEWER; return APP_VIEWER; }
    if (key == 'd' || key == 'D') {
        g_journal.deleteEntry(entries[g_browser.selection].filename);
        entries = g_journal.listEntries();
        if (entries.empty()) { ctx.nextState=APP_MAIN; return APP_MAIN; }
        if (g_browser.selection>=(int)entries.size()) g_browser.selection=(int)entries.size()-1;
    }
    if (key == 'e' || key == 'E') {
        ctx.prevState = APP_BROWSER;
        std::string content = g_journal.readEntry(entries[g_browser.selection].filename);
        if (!content.empty()) {
            ctx.editContent = extractBody(content);
            ctx.editFilename = entries[g_browser.selection].filename;
            ctx.promptMode = false;
            ctx.promptText = "";
            ctx.nextState = APP_EDITOR;
            return APP_EDITOR;
        }
    }
    if (key == 0x13) {
        auto content = g_journal.readEntry(entries[g_browser.selection].filename);
        if (!content.empty()) {
            auto body = extractBody(content);
            if (!body.empty()) {
                g_flomoPendingText = body;
                g_flomoReturnTo = APP_BROWSER;
                ctx.nextState = APP_SYNC_SEND_FLOMO;
                return APP_SYNC_SEND_FLOMO;
            }
        }
    }

    ui_clear(); int y = FONT_H;
    ui_draw_text(4, y, "过往日记", false, true);
    u8g2_DrawHLine(g_u8g2, 0, y + 7, SCREEN_W);
    y = y + 7 + LINE_SPACING - 4;
    int visible = (SCREEN_H - y + LINE_SPACING - 1) / LINE_SPACING;
    if (g_browser.selection < g_browser.scroll) g_browser.scroll = g_browser.selection;
    if (g_browser.selection >= g_browser.scroll + visible)
        g_browser.scroll = g_browser.selection - visible + 1;

    for (int i = 0; i < visible && (g_browser.scroll + i) < (int)entries.size(); i++) {
        auto &e = entries[g_browser.scroll + i]; bool sel = (g_browser.scroll + i == g_browser.selection);
        std::string dateDisplay;
        if (e.filename.length() >= 10) dateDisplay = e.filename.substr(0, 10);
        else dateDisplay = e.date;
        std::string preview = e.preview.empty() ? e.title : e.preview;
        char buf[80];
        snprintf(buf, sizeof(buf), "%s %s", dateDisplay.c_str(), preview.c_str());
        ui_draw_text(8, y + i * LINE_SPACING, buf, sel);
    }
    ui_commit();
    return APP_BROWSER;
}

// ── Viewer Screen ──────────────────────────────────────────────────────
void screen_viewer_init(const std::string &filename) {
    g_viewer.filename = filename; g_viewer.scroll = 0; g_viewer.lines.clear();
    g_viewer.vrowsDirty = true;
    if (filename.length() >= 15)
        g_viewer.dateStr = filename.substr(0,10) + " " + filename.substr(11,2) + ":" + filename.substr(13,2);
    else g_viewer.dateStr = filename;
    std::string content = g_journal.readEntry(filename);
    if (content.empty()) return;
    size_t pos = 0;
    while (pos < content.length()) {
        size_t nl = content.find('\n', pos);
        g_viewer.lines.push_back((nl == std::string::npos) ? content.substr(pos) : content.substr(pos, nl - pos));
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
}

AppState screen_viewer_handle(int key, ScreenContext &ctx) {
    if (key == 'q' || key == 'Q' || key == 0x1B) { ctx.nextState = APP_BROWSER; return APP_BROWSER; }
    if (key == 'j' || key == '.' || key == KEY_DOWN) g_viewer.scroll++;
    if (key == 'k' || key == ';' || key == KEY_UP) { if (g_viewer.scroll > 0) g_viewer.scroll--; }
    if (key == 'e' || key == 'E') {
        ctx.prevState = APP_VIEWER;
        ctx.selectedEntry = g_viewer.filename;
        std::string content = g_journal.readEntry(g_viewer.filename);
        if (!content.empty()) {
            ctx.editContent = extractBody(content);
            ctx.editFilename = g_viewer.filename;
            ctx.promptMode = false;
            ctx.promptText = "";
            ctx.nextState = APP_EDITOR;
            return APP_EDITOR;
        }
    }
    if (key == 'f' || key == 'F') {
        std::string content = g_journal.readEntry(g_viewer.filename);
        if (!content.empty()) {
            auto body = extractBody(content);
            if (!body.empty()) {
                g_flomoPendingText = body;
                g_flomoReturnTo = APP_VIEWER;
                ctx.nextState = APP_SYNC_SEND_FLOMO;
                return APP_SYNC_SEND_FLOMO;
            }
        }
    }

    const auto& vrows = getViewerVrows();

    const int headerY = FONT_H;
    const int sepY = headerY + g_font.descent();
    const int contentY = sepY + 26;
    const int contentMaxY = STATUS_Y;
    int visible = (contentMaxY - contentY + LINE_SPACING - 1) / LINE_SPACING;
    if (visible < 1) visible = 1;
    int maxScroll = (int)vrows.size() - visible;
    if (maxScroll < 0) maxScroll = 0;
    if (g_viewer.scroll > maxScroll) g_viewer.scroll = maxScroll;

    ui_clear();

    std::string header = g_viewer.dateStr;
    if (header.empty()) header = g_viewer.filename;
    ui_draw_text(4, headerY, header.c_str(), true);

    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawHLine(g_u8g2, 4, sepY, SCREEN_W - 8);

    bool mdOn = g_settings.markdownRender();
    mdSetRenderEnabled(mdOn);
    std::vector<MdLineInfo> mdInfo;
    if (mdOn) {
        mdInfo = mdClassifyLines(g_viewer.lines);
    } else {
        mdInfo.assign(g_viewer.lines.size(), MdLineInfo{});
    }
    for (int i = 0; i < visible && (g_viewer.scroll + i) < (int)vrows.size(); i++) {
        auto &vr = vrows[g_viewer.scroll + i];
        mdDrawVrow(4, contentY + i * LINE_SPACING, g_viewer.lines[vr.lineIdx], vr.start, vr.end, mdInfo[vr.lineIdx]);
    }

    if (g_viewer.scroll > 0 && maxScroll > 0) {
        int pct = (g_viewer.scroll * 100) / maxScroll;
        char pctStr[16];
        snprintf(pctStr, sizeof(pctStr), "%d%%", pct);
        int headerW = g_font.textWidth(header.c_str());
        int pctW = g_font.textWidth(pctStr);
        if (headerW + pctW + 12 < SCREEN_W) {
            ui_draw_text(SCREEN_W - pctW - 4, headerY, pctStr);
        }
    }

    ui_commit();
    return APP_VIEWER;
}
