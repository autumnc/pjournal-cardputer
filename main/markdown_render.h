#pragma once

#include <string>
#include <vector>

// Markdown live rendering for the editor.
// Block markers (#, -, >, task) are width-preserving: replaced by same-width
// symbols. Inline markers (** , ==, ~~, `, *) render as a single space on each
// side (uniform 1-cell padding); mdVisualX/mdVrowX are marker-aware so cursor /
// selection / wrapped-row placement stay aligned with what's drawn.

struct MdLineInfo {
    int headingLevel = 0;  // 1..6, 0 = not a heading
    bool list = false;
    bool task = false;     // "- [ ]" / "- [x]"
    bool quote = false;
    bool inCodeBlock = false;  // between code fences
    bool hr = false;           // horizontal rule or code fence line
};

// Classify every line of the document in one pass.
std::vector<MdLineInfo> mdClassifyLines(const std::vector<std::string> &lines);

// Draw the [start, end) byte slice of `line` (a vrow) at (x, y) with markdown
// styles. y is the text baseline. Byte offsets match buildVrows output.
void mdDrawVrow(int x, int y, const std::string &line, int start, int end,
                const MdLineInfo &info);

// Visual x (px from the line's left edge) at which raw byte bytePos renders.
// For heading/task/quote lines the content sits at a fixed prefix offset
// (2/3/4 cells) instead of its raw width. Cursor / selection must use this to
// stay aligned with draw.
int mdVisualX(const std::string &line, const MdLineInfo &info, int bytePos);

// Visual x (px) of bytePos within a vrow that starts at raw byte vrowStart.
// Continuation vrows repeat the prefix indent. Use for all vrow drawing /
// cursor / selection positioning.
int mdVrowX(const std::string &line, const MdLineInfo &info, int bytePos, int vrowStart);

// Enable/disable markdown rendering (Settings "Markdown渲染"). When disabled,
// every line is drawn as raw text with the marker characters untouched.
void mdSetRenderEnabled(bool on);
