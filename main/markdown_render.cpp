#include "markdown_render.h"
#include "font_renderer.h"
#include "ui_helpers.h"

#include <algorithm>

extern "C" {
    extern void u8g2_SetDrawColor(void *u8g2, int color);
    extern void u8g2_DrawBox(void *u8g2, int x, int y, int w, int h);
    extern void u8g2_DrawHLine(void *u8g2, int x, int y, int w);
}
struct u8g2_struct;
typedef struct u8g2_struct u8g2_t;
extern u8g2_t *g_u8g2;

namespace {

struct MdSeg {
    int start, end;         // byte range in the raw line
    TextStyle ts;           // style to render
    std::string drawText;   // replacement text, width == raw width of [start,end)
};

bool s_mdEnabled = true;  // cleared when "Markdown渲染" setting is off

// Spaces whose width matches the raw byte range [from,to). Keeps the width
// invariant even if the raw range contains non-ASCII bytes.
std::string spacesForWidth(const std::string &line, int from, int to) {
    std::string sp;
    int target = g_font.textWidth(line.substr(from, to - from).c_str());
    while (g_font.textWidth(sp.c_str()) < target) sp += ' ';
    return sp;
}

// Next inline marker byte, skipping escaped characters. Returns len if none.
int nextMarker(const std::string &line, int from, int len) {
    for (int i = from; i < len; i++) {
        char c = line[i];
        if (c == '\\') { i++; continue; }
        if (c == '*' || c == '`' || c == '[' || c == '~' || c == '=') return i;
    }
    return len;
}

// Byte length of the "- [ ]"/"- [x]" marker: 6 when followed by a space,
// 5 otherwise, so the space (or first content char) never gets swallowed.
int taskPrefixEnd(const std::string &line) {
    return ((int)line.size() >= 6 && line[5] == ' ') ? 6 : 5;
}

int findStars(const std::string &line, int from, int len, int n) {
    for (int i = from; i <= len - n; i++) {
        bool ok = true;
        for (int k = 0; k < n; k++) if (line[i + k] != '*') { ok = false; break; }
        if (ok) return i;
    }
    return -1;
}

// Marker-aware visual width (px) of line[from,to): each inline marker pair
// renders as a single space on each side (not one per marker char), so styled
// regions get uniform 1-cell padding. Links and escapes stay width-neutral.
// Mirrors mdParseInline pairing so the cursor/selection match what's drawn.
static int mdContentWidth(const std::string &line, int from, int to) {
    int len = (int)line.size();
    if (to > len) to = len;
    if (from >= to) return 0;
    int px = 0;
    int i = from;
    while (i < to) {
        int m = nextMarker(line, i, to);
        if (m >= to) { px += g_font.textWidth(line.substr(i, to - i).c_str()); break; }
        if (m > i) { px += g_font.textWidth(line.substr(i, m - i).c_str()); i = m; }
        char c = line[m];
        int openLen = 1, closeIdx = -1, closeLen = 1, contentStart = m + 1;
        if (c == '*') {
            openLen = 1;
            while (m + openLen < len && line[m + openLen] == '*') openLen++;
            if (openLen > 3) openLen = 3;
            int cc = findStars(line, m + openLen, len, openLen);
            if (cc >= 0) { closeIdx = cc; closeLen = openLen; contentStart = m + openLen; }
        } else if (c == '`') {
            int cc = (int)line.find('`', m + 1);
            if (cc >= 0) { closeIdx = cc; contentStart = m + 1; }
        } else if (c == '~' && m + 1 < len && line[m + 1] == '~') {
            int cc = (int)line.find("~~", m + 2);
            if (cc >= 0) { closeIdx = cc; closeLen = 2; contentStart = m + 2; openLen = 2; }
        } else if (c == '=' && m + 1 < len && line[m + 1] == '=') {
            int cc = (int)line.find("==", m + 2);
            if (cc >= 0) { closeIdx = cc; closeLen = 2; contentStart = m + 2; openLen = 2; }
        } else if (c == '[') {
            int p = (int)line.find("](", m + 1);
            if (p >= 0 && p < len) {
                int cp = (int)line.find(')', p + 2);
                if (cp >= 0 && cp < len) {  // links stay width-neutral
                    int linkEnd = cp + 1;
                    if (linkEnd >= to) { px += g_font.textWidth(line.substr(m, to - m).c_str()); break; }
                    px += g_font.textWidth(line.substr(m, linkEnd - m).c_str());
                    i = linkEnd;
                    continue;
                }
            }
        }
        if (closeIdx < 0) { px += g_font.halfAdvance(); i = m + 1; continue; }  // unmatched literal
        int openEnd = m + openLen;
        if (to <= openEnd) { px += g_font.halfAdvance(); break; }  // inside open marker
        int closeEnd = closeIdx + closeLen;
        if (to <= closeIdx) {  // inside styled content
            px += g_font.halfAdvance() + g_font.textWidth(line.substr(contentStart, to - contentStart).c_str());
            break;
        }
        px += g_font.halfAdvance() + g_font.textWidth(line.substr(contentStart, closeIdx - contentStart).c_str());
        if (to >= closeEnd) px += g_font.halfAdvance();
        i = closeEnd;
    }
    return px;
}

// Scan [from, len) for paired inline markers. Each emitted segment preserves
// the width of its raw byte range.
void mdParseInline(const std::string &line, int from, const TextStyle &base,
                   std::vector<MdSeg> &segs) {
    int len = (int)line.size();
    int plainStart = from;
    while (plainStart < len) {
        int m = nextMarker(line, plainStart, len);
        if (m == len) break;
        if (m > plainStart)
            segs.push_back({plainStart, m, base, line.substr(plainStart, m - plainStart)});

        char c = line[m];
        if (c == '\\') {  // escape: backslash becomes a space, next char literal
            if (m + 1 < len) {
                segs.push_back({m, m + 1, base, " "});
                segs.push_back({m + 1, m + 2, base, line.substr(m + 1, 1)});
                plainStart = m + 2;
            } else {
                plainStart = m + 1;
            }
            continue;
        }
        if (c == '*') {
            int n = 1;
            while (m + n < len && line[m + n] == '*') n++;
            if (n > 3) n = 3;
            int cclose = findStars(line, m + n, len, n);
            if (cclose < 0) {  // unmatched → literal star
                segs.push_back({m, m + 1, base, line.substr(m, 1)});
                plainStart = m + 1;
                continue;
            }
            TextStyle st = base;
            if (n >= 3) { st.bold = true; st.underline = true; }
            else if (n == 2) st.bold = true;
            else st.underline = true;  // single * = italic → underline
            segs.push_back({m, m + n, base, " "});
            if (cclose > m + n)
                segs.push_back({m + n, cclose, st, line.substr(m + n, cclose - (m + n))});
            segs.push_back({cclose, cclose + n, base, " "});
            plainStart = cclose + n;
            continue;
        }
        if (c == '`') {
            int cclose = (int)line.find('`', m + 1);
            if (cclose < 0 || cclose >= len) {  // unmatched → literal
                segs.push_back({m, m + 1, base, line.substr(m, 1)});
                plainStart = m + 1;
                continue;
            }
            TextStyle st = base;
            st.invert = true;
            segs.push_back({m, m + 1, base, " "});
            segs.push_back({m + 1, cclose, st, line.substr(m + 1, cclose - (m + 1))});
            segs.push_back({cclose, cclose + 1, base, " "});
            plainStart = cclose + 1;
            continue;
        }
        if (c == '~' && m + 1 < len && line[m + 1] == '~') {
            int cclose = (int)line.find("~~", m + 2);
            if (cclose < 0 || cclose >= len) {  // unmatched → literal
                segs.push_back({m, m + 1, base, line.substr(m, 1)});
                plainStart = m + 1;
                continue;
            }
            TextStyle st = base;
            st.strike = true;
            segs.push_back({m, m + 2, base, " "});
            segs.push_back({m + 2, cclose, st, line.substr(m + 2, cclose - (m + 2))});
            segs.push_back({cclose, cclose + 2, base, " "});
            plainStart = cclose + 2;
            continue;
        }
        if (c == '=' && m + 1 < len && line[m + 1] == '=') {
            int cclose = (int)line.find("==", m + 2);
            if (cclose < 0 || cclose >= len) {  // unmatched → literal
                segs.push_back({m, m + 1, base, line.substr(m, 1)});
                plainStart = m + 1;
                continue;
            }
            TextStyle st = base;
            st.emph = true;
            segs.push_back({m, m + 2, base, " "});
            segs.push_back({m + 2, cclose, st, line.substr(m + 2, cclose - (m + 2))});
            segs.push_back({cclose, cclose + 2, base, " "});
            plainStart = cclose + 2;
            continue;
        }
        if (c == '[') {
            int p = (int)line.find("](", m + 1);
            if (p >= 0 && p < len) {
                int cp = (int)line.find(')', p + 2);
                if (cp >= 0 && cp < len) {
                    TextStyle st = base;
                    st.invert = true;
                    st.underline = true;
                    segs.push_back({m, m + 1, base, " "});
                    segs.push_back({m + 1, p, st, line.substr(m + 1, p - (m + 1))});
                    segs.push_back({p, cp + 1, base, spacesForWidth(line, p, cp + 1)});
                    plainStart = cp + 1;
                    continue;
                }
            }
        }
        plainStart = m + 1;  // unhandled marker char → emit as literal content
        segs.push_back({m, m + 1, base, line.substr(m, 1)});
    }
    if (plainStart < len)
        segs.push_back({plainStart, len, base, line.substr(plainStart)});
}

void mdParseLine(const std::string &line, const MdLineInfo &info, std::vector<MdSeg> &segs) {
    int len = (int)line.size();
    if (!s_mdEnabled) {
        segs.push_back({0, len, TextStyle{}, line});
        return;
    }
    TextStyle base;
    if (info.headingLevel > 0) { base.bold = true; base.underline = true; }

    if (info.inCodeBlock) {
        TextStyle st = base;
        st.invert = true;
        segs.push_back({0, len, st, line});
        return;
    }
    if (info.hr) {
        segs.push_back({0, len, base, spacesForWidth(line, 0, len)});
        return;
    }

    int pos = 0;
    if (info.headingLevel > 0) {
        int n = info.headingLevel;
        static const char *kLevelGlyph[6] = {
            "\xF3\xB0\x8E\xA4", "\xF3\xB0\x8E\xA7", "\xF3\xB0\x8E\xAA",
            "\xF3\xB0\x8E\xAD", "\xF3\xB0\x8E\xB1", "\xF3\xB0\x8E\xB3",
        };
        // Heading glyph advance is 2 cells; content starts right after it.
        segs.push_back({0, n, base, kLevelGlyph[info.headingLevel - 1]});
        pos = n;
    } else if (info.task) {
        bool checked = len >= 5 && (line[3] == 'x' || line[3] == 'X');
        int pe = taskPrefixEnd(line);
        std::string repl = checked ? " ✓ " : " ☐ ";  // box at cell 2, content at cell 4
        segs.push_back({0, pe, base, repl});
        pos = pe;
    } else if (info.list) {
        // bullet at cell 2, content at cell 4 (whole marker shifted right 1 cell)
        segs.push_back({0, 2, base, " \xE2\x80\xA2 "});
        pos = 2;
    } else if (info.quote) {
        segs.push_back({0, 2, base, "    "});  // bar at cell 4, 1-space gap, content at cell 5
        pos = 2;
    }

    mdParseInline(line, pos, base, segs);
}

std::string sliceDraw(const MdSeg &seg, int s, int e) {
    if (s == seg.start && e == seg.end) return seg.drawText;
    int so = s - seg.start;
    int n = e - s;
    if ((int)seg.drawText.size() >= so + n) return seg.drawText.substr(so, n);
    return seg.drawText;  // symbol segments are never partially sliced
}

}  // namespace

void mdSetRenderEnabled(bool on) { s_mdEnabled = on; }

std::vector<MdLineInfo> mdClassifyLines(const std::vector<std::string> &lines) {
    std::vector<MdLineInfo> out(lines.size());
    bool inCode = false;
    for (size_t i = 0; i < lines.size(); i++) {
        const std::string &ln = lines[i];
        MdLineInfo &info = out[i];
        int end = (int)ln.size();
        while (end > 0 && (ln[end - 1] == ' ' || ln[end - 1] == '\t')) end--;

        if (ln.compare(0, 3, "```") == 0) {
            info.hr = true;  // fence line renders as a horizontal rule
            inCode = !inCode;
            continue;
        }
        if (inCode) { info.inCodeBlock = true; continue; }

        int h = 0;
        while (h < end && ln[h] == '#') h++;
        if (h >= 1 && h <= 6 && h < end && ln[h] == ' ') {
            info.headingLevel = h;
            continue;
        }
        if (end >= 3) {  // horizontal rule: only - * _ (and whitespace), >= 3
            bool hr = true;
            int cnt = 0;
            for (int k = 0; k < end; k++) {
                char c = ln[k];
                if (c == ' ' || c == '\t') continue;
                if (c == '-' || c == '*' || c == '_') { cnt++; continue; }
                hr = false;
                break;
            }
            if (hr && cnt >= 3) { info.hr = true; continue; }
        }
        if (end >= 5 && ln.compare(0, 5, "- [ ]") == 0) { info.task = true; continue; }
        if (end >= 5 && (ln.compare(0, 5, "- [x]") == 0 || ln.compare(0, 5, "- [X]") == 0)) {
            info.task = true;
            continue;
        }
        if (end >= 2 && ln[0] == '-' && ln[1] == ' ') { info.list = true; continue; }
        if (end >= 2 && (ln[0] == '*' || ln[0] == '+') && ln[1] == ' ') {
            info.list = true;
            continue;
        }
        if (end >= 2 && ln[0] == '>' && ln[1] == ' ') { info.quote = true; continue; }
    }
    return out;
}

int mdVisualX(const std::string &line, const MdLineInfo &info, int bytePos) {
    if (!s_mdEnabled) return g_font.textWidth(line.substr(0, bytePos).c_str());
    int cell = g_font.halfAdvance();
    if (info.headingLevel > 0) {
        if (bytePos >= info.headingLevel)
            return 2 * cell + mdContentWidth(line, info.headingLevel, bytePos);
        return 0;
    }
    if (info.task) {
        int pe = taskPrefixEnd(line);
        if (bytePos >= pe)
            return 3 * cell + mdContentWidth(line, pe, bytePos);
        return std::min(mdContentWidth(line, 0, bytePos), 3 * cell);
    }
    if (info.list) {
        if (bytePos >= 2)
            return 3 * cell + mdContentWidth(line, 2, bytePos);
        return mdContentWidth(line, 0, bytePos);
    }
    if (info.quote) {
        if (bytePos >= 2)
            return 4 * cell + 2 + mdContentWidth(line, 2, bytePos);
        return mdContentWidth(line, 0, bytePos);
    }
    return mdContentWidth(line, 0, bytePos);
}

// Visual x (px) of raw bytePos within a vrow that starts at raw byte vrowStart.
// Continuation vrows (vrowStart > 0) of heading/task/quote lines repeat the
// prefix indent so wrapped text stays aligned under the first line. Without
// this, wrapped lines were placed at their whole-line x, i.e. off-screen.
int mdVrowX(const std::string &line, const MdLineInfo &info, int bytePos, int vrowStart) {
    if (!s_mdEnabled)
        return g_font.textWidth(line.substr(vrowStart, bytePos - vrowStart).c_str());
    int cell = g_font.halfAdvance();
    int prefix = 0;
    if (vrowStart > 0) {
        if (info.headingLevel > 0) prefix = 2 * cell;
        else if (info.task) prefix = 3 * cell;
        else if (info.quote) prefix = 4 * cell + 2;
        else if (info.list) prefix = 3 * cell;
    }
    return mdVisualX(line, info, bytePos) - mdVisualX(line, info, vrowStart) + prefix;
}

void mdDrawVrow(int x, int y, const std::string &line, int start, int end,
                const MdLineInfo &info) {
    std::vector<MdSeg> segs;
    mdParseLine(line, info, segs);
    int len = (int)line.size();
    if (end > len) end = len;

    u8g2_SetDrawColor(g_u8g2, 0);  // text draws in ink; overlays must not leak color 1

    for (auto &seg : segs) {
        if (seg.start >= end) break;
        if (seg.end <= start) continue;
        int s = std::max(seg.start, start);
        int e = std::min(seg.end, end);
        int cx = x + mdVrowX(line, info, s, start);
        std::string draw = sliceDraw(seg, s, e);
        if (!draw.empty()) g_font.drawTextStyled(cx, y, draw.c_str(), seg.ts);
    }

    if (info.quote) {
        u8g2_SetDrawColor(g_u8g2, 0);
        u8g2_DrawBox(g_u8g2, x + 3 * g_font.halfAdvance(), y - g_font.ascent(), 2, g_font.lineHeight());
        // do NOT flip color back to 1 here: the next vrow's text would then be
        // drawn light-on-light and vanish (wrapped quote lines showed no text)
    } else if (info.hr) {
        int hy = y - g_font.ascent() + g_font.lineHeight() / 2;
        u8g2_SetDrawColor(g_u8g2, 0);
        u8g2_DrawHLine(g_u8g2, x, hy, SCREEN_W - 2 * x);
    }
}
