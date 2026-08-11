#!/usr/bin/env python3
"""
One-off: render markdown symbols •(U+2022) ☐(U+2610) ✓(U+2713) ■(U+25A0)
from NF-Mono.ttf at 28/22px and print C code to hand-paste into
main/symbol_glyphs.h (appended at 28pt indices 40-43, 22pt indices 53-56).

Do NOT fold these into extract_symbols.py — it clobbers the hand-applied
battery_22 icons. New symbols are hand-applied too.
"""
import freetype
import sys


def render_glyph(face, cp, pixel_size):
    face.set_pixel_sizes(0, pixel_size)
    flags = freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_MONO
    face.load_char(cp, flags)
    bitmap = face.glyph.bitmap
    w = bitmap.width
    h = bitmap.rows
    xo = face.glyph.bitmap_left
    yo = face.glyph.bitmap_top
    adv = face.glyph.advance.x >> 6
    raw = bytes(bitmap.buffer)
    row_bytes = (w + 7) // 8
    out = bytearray()
    for r in range(h):
        start = r * bitmap.pitch
        out.extend(raw[start:start + row_bytes])
    return w, h, xo, yo, adv, bytes(out)


def emit(name, size, bits):
    print(f"static const uint8_t SYM_{name}_{size}_BITS[] = {{")
    for i, b in enumerate(bits):
        print(f"    0x{b:02X},", end="")
        if (i + 1) % 16 == 0:
            print()
            continue
        print()
    print("};")
    print()


def main():
    ttf_path = sys.argv[1] if len(sys.argv) > 1 else "/media/sf_share/NF-Mono.ttf"
    face = freetype.Face(ttf_path)
    glyphs = [(0x2022, "BULLET"), (0x2610, "CHECKBOX"),
              (0x2713, "CHECK"), (0x25A0, "SQUARE")]
    entries = {28: [], 22: []}
    for size in (28, 22):
        print(f"// --- {size}pt ---")
        for cp, name in glyphs:
            w, h, xo, yo, adv, bits = render_glyph(face, cp, size)
            emit(name, size, bits)
            entries[size].append((name, w, h, xo, yo, adv, bits))
            print(f"  # {size}pt {name} U+{cp:04X}: {w}x{h} xo={xo} yo={yo} adv={adv}")
        print()

    # Table entries appended at 28pt idx 40-43, 22pt idx 53-56
    for size, start_idx in ((28, 40), (22, 53)):
        print(f"// g_symbolGlyphs_{size} entries (indices {start_idx}-{start_idx+3}):")
        for i, (name, w, h, xo, yo, adv, bits) in enumerate(entries[size]):
            print(f"    {{ {w}, {h}, {xo}, {yo}, {adv}, SYM_{name}_{size}_BITS }},  // U+{glyphs[i][0]:04X}")
        print()

    print("// getSymbolGlyph cases:")
    for i, (cp, name) in enumerate(glyphs):
        i28 = 40 + i
        i22 = 53 + i
        print(f"        case 0x{cp:04X}: if (font_size == 28) return &g_symbolGlyphs_28[{i28}]; if (font_size == 22) return &g_symbolGlyphs_22[{i22}]; return nullptr;")
    print()


if __name__ == '__main__':
    main()
