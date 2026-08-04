#!/usr/bin/env python3
"""
Vertically center fullwidth glyphs (U+FF01-U+FF5E) in the .fnt font blobs.

Problem: fullwidth Latin/numbers/symbols used to sit too low (baseline-anchored),
then looked too high when centered on the CJK ink center.
Fix: recompute y_off so each fullwidth glyph's ink bounding-box center aligns
with the average ink center of half-width lowercase English letters (a-z),
so fullwidth chars line up with the surrounding half-width text.

Chinese punctuation (REVERT_CODES) and the underscore keep their original
baseline position and are skipped during patching.

Usage:
    python3 scripts/center_fullwidth.py            # patch main/terminus{28,22}.fnt in place
    python3 scripts/center_fullwidth.py --report   # print per-glyph changes without writing
    python3 scripts/center_fullwidth.py --revert   # restore REVERT_CODES from git HEAD (repair)

The 28pt fullwidth letters that live in symbol_glyphs.h (not in the .fnt blob)
must be adjusted separately: subtract 2 from each centered y_off (the shift is
round(eng_center - cjk_center) = round(-8.90 + 6.88) = -2).
"""

import os
import struct
import sys

FONT_DIR = os.path.join(os.path.dirname(__file__), "..", "main")
FONTS = ["terminus28.fnt", "terminus22.fnt"]

# Half-width lowercase English letters used to measure the reference ink center
ENGLISH_MIN = 0x61   # 'a'
ENGLISH_MAX = 0x7B   # one past 'z'

FW_MIN = 0xFF01
FW_MAX = 0xFF5E

# Chinese punctuation (and underscore) produced by the IME in Chinese/fullwidth
# mode that fall in the FF01-FF5E block. These keep their original baseline
# (bottom-anchored) instead of being centered.
REVERT_CODES = [0xFF01, 0xFF08, 0xFF09, 0xFF0C, 0xFF0E,
                0xFF1A, 0xFF1B, 0xFF1F, 0xFF3F]


def signed(b):
    return struct.unpack('<b', bytes([b]))[0]


class Font:
    def __init__(self, data):
        self.data = bytearray(data)
        assert self.data[:4] == b'PJFN'
        self.line_h = struct.unpack('<H', self.data[6:8])[0]
        self.meta_base = struct.unpack('<I', self.data[22:26])[0] + 10
        self.data_base = struct.unpack('<I', self.data[26:30])[0] + 10
        self._blocks = []
        for tbl_off in (18, 30):  # cjk table then "other" table
            off = struct.unpack('<I', self.data[tbl_off:tbl_off + 4])[0]
            p = off + 10
            count = struct.unpack('<H', self.data[p:p + 2])[0]
            p += 2
            for _ in range(count):
                s, e, fm = struct.unpack('<III', self.data[p:p + 12])
                p += 12
                self._blocks.append((s, e, fm))

    def meta_of(self, idx):
        p = self.meta_base + idx * 12
        w, h = struct.unpack('<HH', self.data[p:p + 4])
        xo, yo = signed(self.data[p + 4]), signed(self.data[p + 5])
        adv = self.data[p + 6]
        boff = struct.unpack('<I', self.data[p + 8:p + 12])[0]
        return w, h, xo, yo, adv, boff

    def glyph_idx(self, cp):
        for s, e, fm in self._blocks:
            if s <= cp <= e:
                return fm + (cp - s)
        return None

    def ink_rows(self, idx):
        w, h, xo, yo, adv, boff = self.meta_of(idx)
        if w == 0 or h == 0 or w > 100:
            return None, None
        row_bytes = (w + 7) // 8
        bits = self.data[self.data_base + boff:self.data_base + boff + row_bytes * h]
        rows = []
        for r in range(h):
            for c in range(w):
                byte = bits[r * row_bytes + c // 8]
                if (byte >> (7 - (c % 8))) & 1:
                    rows.append(r)
                    break
        return w, h, xo, yo, adv, (min(rows), max(rows)) if rows else None

    # Baseline-relative ink center: negative = above baseline
    def ink_center(self, w, h, yo, ink):
        top = -yo - h
        return top + (ink[0] + ink[1] + 1) / 2.0

    def english_center(self):
        centers = []
        for cp in range(ENGLISH_MIN, ENGLISH_MAX):
            idx = cp - 0x20  # ASCII block is implicit: U+20 -> meta index 0
            res = self.ink_rows(idx)
            if res is None:
                continue
            w, h, xo, yo, adv, ink = res
            if ink is None:
                continue
            centers.append(self.ink_center(w, h, yo, ink))
        if not centers:
            raise RuntimeError("no ASCII lowercase sample glyphs found")
        return sum(centers) / len(centers)

    def set_yoff(self, idx, yo):
        p = self.meta_base + idx * 12 + 5
        self.data[p] = yo & 0xFF

    def fullwidth_glyphs(self):
        out = []
        for cp in range(FW_MIN, FW_MAX + 1):
            idx = self.glyph_idx(cp)
            if idx is None:
                continue
            res = self.ink_rows(idx)
            if res is None:
                continue
            w, h, xo, yo, adv, ink = res
            if ink is None:
                continue
            out.append((cp, w, h, xo, yo, adv, ink, idx))
        return out


def patch_font(path, write):
    with open(path, 'rb') as f:
        font = Font(f.read())
    center = font.english_center()
    changes = []
    for cp, w, h, xo, yo, adv, ink, idx in font.fullwidth_glyphs():
        if cp in REVERT_CODES:  # Chinese punctuation / underscore keep original baseline
            continue
        c = font.ink_center(w, h, yo, ink)
        new_yo = round(yo + c - center)
        if new_yo < -128 or new_yo > 127:
            print(f"  !! yo out of range U+{cp:04X}: {new_yo}")
            new_yo = max(-128, min(127, new_yo))
        changes.append((cp, w, h, yo, c, new_yo))
        if write:
            font.set_yoff(idx, new_yo)
    print(f"=== {os.path.basename(path)} (English-lowercase ink center = baseline{center:+.2f}) ===")
    for cp, w, h, yo, c, new_yo in changes:
        print(f"  U+{cp:04X} {chr(cp)!r}: h={h} yo {yo:+d} -> {new_yo:+d} (ink center baseline{c:+.1f})")
    if write:
        with open(path, 'wb') as f:
            f.write(font.data)
        print(f"  wrote {len(changes)} y_off patches to {os.path.basename(path)}")
    return changes


def revert_font(path):
    """Restore the original y_off (from git HEAD) for REVERT_CODES."""
    name = os.path.basename(path)
    import subprocess
    orig_bytes = subprocess.run(['git', 'show', f'HEAD:main/{name}'],
                                capture_output=True, check=True).stdout
    orig = Font(orig_bytes)
    cur = Font(open(path, 'rb').read())
    restored = 0
    print(f"=== {name} (revert Chinese punctuation to original baseline) ===")
    for cp in REVERT_CODES:
        idx = cur.glyph_idx(cp)
        if idx is None:
            print(f"  U+{cp:04X} {chr(cp)!r}: not in blob")
            continue
        old_yo = signed(orig.data[orig.meta_base + idx * 12 + 5])
        cur.set_yoff(idx, old_yo)
        restored += 1
    with open(path, 'wb') as f:
        f.write(cur.data)
    print(f"  restored {restored} y_off values in {name}")


def main():
    write = '--report' not in sys.argv
    if '--revert' in sys.argv:
        for name in FONTS:
            revert_font(os.path.join(FONT_DIR, name))
        return
    for name in FONTS:
        patch_font(os.path.join(FONT_DIR, name), write)


if __name__ == '__main__':
    main()
