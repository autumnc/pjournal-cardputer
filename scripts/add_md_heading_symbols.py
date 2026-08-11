#!/usr/bin/env python3
"""
One-off: render heading-level marker glyphs uF03A4/uF03A7/uF03AA/uF03AD/uF03B1/uF03B3
(levels 1-6) from NF-Mono.ttf and print C code for main/symbol_glyphs.h.

The on-screen marker is deliberately larger than one cell (a prominent numbered
box): render the font at 50pt for a 25x25 box used at 28pt, and 40pt for a
20x20 box used at 22pt. advance = 2 cells (28/22px) so the heading content
starts at a clean 2-cell offset and the mirror (verify_markdown.py) stays in
integer cells. yo centers the box in the em box; underline at baseline+4 clears it.

Do NOT fold into extract_symbols.py — it clobbers hand-applied icons.
"""
import freetype


def render_glyph(face, cp, pixel_size):
    face.set_pixel_sizes(0, pixel_size)
    flags = freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_MONO
    face.load_char(cp, flags)
    bitmap = face.glyph.bitmap
    w = bitmap.width
    h = bitmap.rows
    raw = bytes(bitmap.buffer)
    row_bytes = (w + 7) // 8
    out = bytearray()
    for r in range(h):
        start = r * bitmap.pitch
        out.extend(raw[start:start + row_bytes])
    return w, h, bytes(out)


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


def preview(w, h, bits):
    lines = []
    rb = (w + 7) // 8
    for r in range(h):
        row = ""
        for c in range(w):
            byte = bits[r * rb + c // 8]
            on = (byte >> (7 - (c % 8))) & 1
            row += "#" if on else "."
        lines.append(row)
    return "\n".join(lines)


def main():
    ttf_path = "/media/sf_share/NF-Mono.ttf"
    face = freetype.Face(ttf_path)
    glyphs = [(0xF03A4, "HL1"), (0xF03A7, "HL2"), (0xF03AA, "HL3"),
              (0xF03AD, "HL4"), (0xF03B1, "HL5"), (0xF03B3, "HL6")]
    # (render_px, target_tag, w, h, xo, yo, advance)
    specs = [(50, 28, 25, 25, 0, 22, 28), (40, 22, 20, 20, 0, 17, 22)]
    entries = {}
    for size, tag, w, h, xo, yo, adv in specs:
        print(f"// --- {size}pt render -> {tag}pt use, {w}x{h} ---")
        entries[tag] = []
        for cp, name in glyphs:
            rw, rh, bits = render_glyph(face, cp, size)
            assert (rw, rh) == (w, h), f"{name}: got {rw}x{rh} want {w}x{h}"
            emit(name, tag, bits)
            entries[tag].append((name, w, h, xo, yo, adv, bits))
            print(f"  # {tag}pt {name} U+{cp:04X}: {w}x{h} xo={xo} yo={yo} adv={adv}")
        print()

    for tag, start_idx in ((28, 44), (22, 57)):
        print(f"// g_symbolGlyphs_{tag} entries (indices {start_idx}-{start_idx+5}):")
        for i, (name, w, h, xo, yo, adv, bits) in enumerate(entries[tag]):
            print(f"    {{ {w}, {h}, {xo}, {yo}, {adv}, SYM_{name}_{tag}_BITS }},  // U+{glyphs[i][0]:04X}")
        print()

    print("// getSymbolGlyph cases (ternary: 28pt else 22pt, no nullptr fall-through):")
    for i, (cp, name) in enumerate(glyphs):
        i28 = 44 + i
        i22 = 57 + i
        print(f"        case 0x{cp:04X}: return (font_size == 28) ? &g_symbolGlyphs_28[{i28}] : &g_symbolGlyphs_22[{i22}];")
    print()

    print("// --- 50pt previews (25x25) ---")
    for cp, name in glyphs:
        rw, rh, bits = render_glyph(face, cp, 50)
        print(f"{name} U+{cp:04X}:")
        print(preview(rw, rh, bits))


if __name__ == '__main__':
    main()
