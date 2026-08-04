#!/usr/bin/env python3
"""
Regenerate fullwidth English letters (U+FF21-FF3A, U+FF41-FF5A) and digits
(U+FF10-FF19) from SourceHanSerifCN-Medium.ttf, replacing the SPTICIHAI-derived
glyphs currently in the .fnt blobs and symbol_glyphs.h.

Vertical position = SourceHan's natural freetype position (bitmap_top), NOT the
half-width-English centered alignment used by center_fullwidth.py, then shifted
by YO_EXTRA (currently -2 px, user preference).

NOTE: after running this, do NOT re-run center_fullwidth.py -- it would re-center
these fullwidth glyphs and undo the natural-position choice.

Usage:
    python3 scripts/regen_fullwidth.py            # patch blobs + symbol_glyphs.h in place
    python3 scripts/regen_fullwidth.py --check    # report what would change without writing
"""

import struct
import os
import re
import sys

import freetype

SH_TTF = "/media/sf_share/SourceHanSerifCN-Medium.ttf"
FONT_DIR = os.path.join(os.path.dirname(__file__), "..", "main")
FONTS = ["terminus28.fnt", "terminus22.fnt"]
SYMBOL_H = os.path.join(FONT_DIR, "symbol_glyphs.h")

# Fullwidth digits, uppercase, lowercase
CODES = list(range(0xFF10, 0xFF1A)) + list(range(0xFF21, 0xFF3B)) + list(range(0xFF41, 0xFF5B))

# 28pt fullwidth lowercase letters that live in symbol_glyphs.h (rest are in the blob)
SYMBOL_LETTERS = [0xFF41, 0xFF43, 0xFF45, 0xFF47, 0xFF4D, 0xFF4E, 0xFF4F, 0xFF50, 0xFF51,
                  0xFF52, 0xFF53, 0xFF55, 0xFF56, 0xFF57, 0xFF58, 0xFF59, 0xFF5A]

# Extra vertical shift (pixels, signed) applied on top of SourceHan's natural
# position. Negative = move glyphs down on screen (draw_y = y - yo - h / y - yo).
YO_EXTRA = -2


def signed(b):
    return struct.unpack('<b', bytes([b]))[0]


class Font:
    def __init__(self, data):
        self.data = bytearray(data)
        assert self.data[:4] == b'PJFN'
        self.glyph_count = struct.unpack('<H', self.data[12:14])[0]
        self.meta_base = struct.unpack('<I', self.data[22:26])[0] + 10
        self.data_base = struct.unpack('<I', self.data[26:30])[0] + 10
        self.other_abs = struct.unpack('<I', self.data[30:34])[0] + 10
        self._blocks = []
        for tbl_off in (18, 30):
            off = struct.unpack('<I', self.data[tbl_off:tbl_off + 4])[0]
            p = off + 10
            count = struct.unpack('<H', self.data[p:p + 2])[0]
            p += 2
            for _ in range(count):
                s, e, fm = struct.unpack('<III', self.data[p:p + 12])
                p += 12
                self._blocks.append((s, e, fm))

    def glyph_idx(self, cp):
        for s, e, fm in self._blocks:
            if s <= cp <= e:
                return fm + (cp - s)
        return None

    def meta_of(self, idx):
        p = self.meta_base + idx * 12
        w, h = struct.unpack('<HH', self.data[p:p + 4])
        xo, yo = signed(self.data[p + 4]), signed(self.data[p + 5])
        adv = self.data[p + 6]
        boff = struct.unpack('<I', self.data[p + 8:p + 12])[0]
        return w, h, xo, yo, adv, boff

    def glyph_raw(self, idx):
        w, h, xo, yo, adv, boff = self.meta_of(idx)
        rb = (w + 7) // 8
        start = self.data_base + boff
        return w, h, xo, yo, adv, bytes(self.data[start:start + rb * h])

    def rebuild(self, replace, write):
        """Rebuild meta + bitmap sections with replaced glyphs. Returns size delta."""
        metas = [self.glyph_raw(i) for i in range(self.glyph_count)]
        for idx, (w, h, xo, yo, adv, bitmap) in replace.items():
            old_w, old_h, *_ = metas[idx]
            metas[idx] = (w, h, xo, yo, adv, bitmap)
        bitmap_data = bytearray()
        offsets = []
        for w, h, xo, yo, adv, raw in metas:
            rb = (w + 7) // 8
            offsets.append(len(bitmap_data))
            bitmap_data.extend(raw[:rb * h])

        new_file = bytearray(self.data[:self.meta_base])  # header + ascii + cjk tables
        for (w, h, xo, yo, adv, _), boff in zip(metas, offsets):
            new_file.extend(struct.pack('<HH', w, h))
            new_file.extend(bytes([xo & 0xFF, yo & 0xFF, adv & 0xFF, 0x00]))
            new_file.extend(struct.pack('<I', boff))
        new_file.extend(bitmap_data)
        new_file.extend(self.data[self.other_abs:])  # other block table (unchanged)

        new_other_abs = self.meta_base + self.glyph_count * 12 + len(bitmap_data)
        struct.pack_into('<I', new_file, 30, new_other_abs - 10)
        assert len(new_file) == new_other_abs + (len(self.data) - self.other_abs)

        if write:
            with open(self._path, 'wb') as f:
                f.write(new_file)
        return len(new_file) - len(self.data)


def render(face, cp, size):
    face.set_pixel_sizes(0, size)
    flags = freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_MONO
    face.load_char(cp, flags)
    g = face.glyph
    w, h = g.bitmap.width, g.bitmap.rows
    raw = bytes(g.bitmap.buffer)
    rb = (w + 7) // 8
    out = bytearray()
    for r in range(h):
        out.extend(raw[r * g.bitmap.pitch:r * g.bitmap.pitch + rb])
    return w, h, g.bitmap_left, g.bitmap_top, g.advance.x >> 6, bytes(out)


def format_bits(bits):
    lines = []
    for i, b in enumerate(bits):
        lines.append(f"0x{b:02X},")
    out = []
    for i in range(0, len(lines), 16):
        out.append("    " + "".join(lines[i:i + 16]))
    return "\n".join(out)


def patch_blob(path, face, write):
    with open(path, 'rb') as f:
        font = Font(f.read())
    font._path = path
    size = 28 if '28' in os.path.basename(path) else 22
    replace = {}
    done = []
    for cp in CODES:
        idx = font.glyph_idx(cp)
        if idx is None:
            continue
        w, h, xo, ytop, adv, bitmap = render(face, cp, size)
        old = font.glyph_raw(idx)[:5]
        yo = ytop - h + YO_EXTRA  # blob convention: draw_y = y - yo - h
        replace[idx] = (w, h, xo, yo, adv, bitmap)
        done.append((cp, old, (w, h, xo, yo, adv)))
    delta = font.rebuild(replace, write)
    print(f"=== {os.path.basename(path)} ({size}pt): {len(done)} glyphs, size delta {delta:+d} bytes ===")
    for cp, old, new in done:
        print(f"  U+{cp:04X} {chr(cp)}: {old[0]}x{old[1]} yo={old[3]:+d} adv={old[4]}"
              f"  ->  {new[0]}x{new[1]} yo={new[3]:+d} adv={new[4]}")


def patch_symbol_h(path, face, write):
    src = open(path).read()
    name_for = {cp: "FW_" + chr(0x61 + (cp - 0xFF41)) for cp in SYMBOL_LETTERS}
    changes = []
    for cp in SYMBOL_LETTERS:
        name = name_for[cp]
        w, h, xo, ytop, adv, bitmap = render(face, cp, 28)  # symbol: yo = bitmap_top
        # Replace bitmap array body
        pat = re.compile(r'(static const uint8_t SYM_%s_28_BITS\[\] = \{)(.*?)(\};)' % name, re.S)
        m = pat.search(src)
        if not m:
            print(f"  !! SYM_{name}_28_BITS array not found")
            continue
        new_body = "\n" + format_bits(bitmap) + "\n"
        src = src[:m.start(2)] + new_body + src[m.end(2):]
        # Replace table entry
        pat2 = re.compile(r'\{ \d+, \d+, -?\d+, -?\d+, \d+, SYM_%s_28_BITS \}' % name)
        entry = f"{{ {w}, {h}, {xo}, {ytop + YO_EXTRA}, {adv}, SYM_{name}_28_BITS }}"
        if not pat2.search(src):
            print(f"  !! table entry for SYM_{name}_28_BITS not found")
            continue
        src = pat2.sub(entry, src, count=1)
        changes.append((cp, name, (w, h, xo, ytop + YO_EXTRA, adv)))
    # Note the SourceHan source in the header comment
    src = src.replace("// Symbol glyphs from NF-Mono.ttf + SPTICIHAI3.0-Medium_p1.ttf",
                      "// Symbol glyphs from NF-Mono.ttf + SPTICIHAI3.0-Medium_p1.ttf + SourceHanSerifCN-Medium.ttf")
    if write:
        with open(path, 'w') as f:
            f.write(src)
    print(f"=== symbol_glyphs.h: {len(changes)} letters replaced (28pt) ===")
    for cp, name, new in changes:
        print(f"  U+{cp:04X} {chr(cp)} {name}: {new[0]}x{new[1]} xo={new[2]} yo={new[3]:+d} adv={new[4]}")


def main():
    write = '--check' not in sys.argv
    face = freetype.Face(SH_TTF)
    for name in FONTS:
        patch_blob(os.path.join(FONT_DIR, name), face, write)
    patch_symbol_h(SYMBOL_H, face, write)
    print("dry run" if not write else "patched")


if __name__ == '__main__':
    main()
