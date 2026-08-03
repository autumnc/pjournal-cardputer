#!/usr/bin/env python3
"""繁体模式: 隐藏简体 + 把每个繁体对应字以副本插到其简体原字之后。

单字 flag 字节 (ime_table_pinyin.bin byte9 / liangfen.bin byte15):
  bit0 = 隐藏简体 (OpenCC_s2t(c)!=c 且 c 不在 KEEP), 繁体模式跳过
  bit1 = 繁体对应副本 (紧跟在简体原字后的重复记录), 简体模式跳过

词组 word_data: 每词追加 1 字节 flag = OpenCC_s2t(w)!=w (bit0, 繁体模式隐藏)。

固件配合:
  单字扫描处 `flags & (_trad ? 0x01 : 0x02)` 跳过:
  - 繁体模式: 简体字(bit0)被隐藏, 其繁体对应副本(bit1)顶替到原位置
    → 单字区呈 简繁按频率交错 顺序 (是時事十世市師實始)。
  - 简体模式: 副本(bit1)被跳过, 词库深处的原繁体字仍在原位
    → 简体候选顺序与旧版逐位一致。
"""
import re, struct, bisect
from collections import Counter
import opencc
cc = opencc.OpenCC("s2t")

# 多义字(简体本身是某个义项的规范繁体)+ 常见标准繁体被 OpenCC 转成冷僻异体的字
KEEP = set(
    "丑丰么于云亘仆余党冲况凄准凉几凶划匀占台叶吁吃后吴咏咨咸唇叙够夸奥宫尸"
    "岩岳峰干庄床异弃弥强彦征恒悦户托抛挂据斗昵晋朴杰栖毁涂涌淀温游烟熏痴皂"
    "禀秘笋羡群芈范荆虚袅辟郁采里隽雇韵卧"
)

PINYIN = "/home/ywz/pjournal-esp32/main/ime/ime_table_pinyin.bin"
LIANGFEN = "/home/ywz/pjournal-esp32/main/ime/liangfen.bin"
H = 12; IE = 677; REC = 10
CODELEN = 6

def char_flag(c):
    return 1 if (cc.convert(c) != c and c not in KEEP) else 0

def counterpart(c):
    t = cc.convert(c)
    return t if len(t) == 1 and t != c else None

def rank(code):
    if len(code) < 2:
        return code + 'a'
    return code[:2]

def build_pinyin_index(codes):
    """idx[k] = count(rank < key(k)), 与 dict_add_trad_gb18030.py 一致。"""
    idx = [0] * IE
    counts = Counter(rank(c) for c in codes)
    tot = 0
    for c0 in range(26):
        for c1 in range(26):
            k = c0 * 26 + c1
            idx[k] = tot
            tot += counts.get(chr(97 + c0) + chr(97 + c1), 0)
    idx[IE - 1] = tot
    return idx

def insert_counterparts(records):
    """records: [(code, hanzi)] 按 (rank,code) 有序, 同码连续。
    返回 [(code, hanzi, flag)]; 对每个隐藏简体, 把其繁体对应字复制一份(flag=2)紧随其后。"""
    out = []
    inserted = 0
    i = 0; n = len(records)
    while i < n:
        code = records[i][0]
        j = i
        while j < n and records[j][0] == code:
            j += 1
        byhz = {hz for _, hz in records[i:j]}
        done = set()
        for _, hz in records[i:j]:
            hid = char_flag(hz)
            out.append((code, hz, hid))
            # 只给被隐藏的简体字插入繁体对应副本, 顶替其槽位
            if hid:
                t = counterpart(hz)
                if t and t in byhz and t not in done:
                    out.append((code, t, 2))
                    done.add(t)
                    inserted += 1
        i = j
    return out, inserted

# ---------- 单字区: 副本插入 + 重建索引 ----------
blob = open(PINYIN, "rb").read()
assert blob[:4] == b"IME3"
count = int.from_bytes(blob[8:12], "little")
rec_base = H + IE * 4
recs = []
for i in range(count):
    rec = blob[rec_base + i * REC : rec_base + (i + 1) * REC]
    code = rec[:CODELEN].split(b"\x00")[0].decode("ascii")
    hz = rec[CODELEN:CODELEN + 3].decode("utf-8")
    recs.append((code, hz))

uniq_chars = {hz for _, hz in recs}
converted = sorted(c for c in uniq_chars if cc.convert(c) != c)
hidden = sorted(c for c in converted if c not in KEEP)
print(f"单字: {count} 记录 / {len(uniq_chars)} 字 / 隐藏 {len(hidden)} 字")

new_recs, inserted = insert_counterparts(recs)
print(f"插入繁体对应副本: {inserted} 条 (新总记录 {len(new_recs)})")

new_count = len(new_recs)
hdr = blob[:8] + struct.pack("<I", new_count)
new_idx = build_pinyin_index([r[0] for r in new_recs])
idx_bytes = b"".join(struct.pack("<I", v) for v in new_idx)
rec_bytes = b"".join(
    r[0].encode().ljust(CODELEN, b"\x00") + r[1].encode("utf-8") + bytes([r[2]])
    for r in new_recs
)

# ---------- 词区: 原样重建(每词 +1 flag 字节) ----------
wordBase = rec_base + count * REC
wc = int.from_bytes(blob[wordBase:wordBase + 4], "little")
wi = [int.from_bytes(blob[wordBase + 4 + k * 4:wordBase + 4 + k * 4 + 4], "little")
      for k in range(IE)]
ds = wordBase + 4 + IE * 4
data = blob[ds:ds + wi[-1]]
blocks = []
pos = 0
total_words = 0
while pos < len(data):
    cl = data[pos]; pos += 1
    if cl == 0: break
    code = data[pos:pos + cl].decode("ascii"); pos += cl
    n = data[pos]; pos += 1
    ws = []
    for _ in range(n):
        wl = data[pos]; pos += 1
        w = data[pos:pos + wl].decode("utf-8"); pos += wl
        fl = data[pos]; pos += 1
        ws.append((w, fl))
    blocks.append((code, ws))
    total_words += len(ws)

parts = []
off = 0
off_map = {}
for code, ws in blocks:
    off_map[code] = off
    b = bytes([len(code)]) + code.encode() + bytes([len(ws)])
    for w, _ in ws:
        wb = w.encode("utf-8")
        fl = 1 if cc.convert(w) != w else 0
        b += bytes([len(wb)]) + wb + bytes([fl])
    parts.append(b)
    off += len(b)
new_data = b"".join(parts)
new_wc = total_words

new_wi = []
for k in range(IE):
    if k < IE - 1:
        key = chr(97 + k // 26) + chr(97 + k % 26)
        idx = bisect.bisect_left([b[0] for b in blocks], key)
        new_wi.append(off_map[blocks[idx][0]] if idx < len(blocks) else len(new_data))
    else:
        new_wi.append(len(new_data))
wi_bytes = b"".join(struct.pack("<I", v) for v in new_wi)

out = hdr + idx_bytes + rec_bytes + struct.pack("<I", new_wc) + wi_bytes + new_data
print(f"\n旧词库 {len(blob):,} B -> 新词库 {len(out):,} B (增加 {len(out)-len(blob):,} B)")
open(PINYIN, "wb").write(out)
print(f"已写入 {PINYIN}")

# ---------- liangfen.bin: 只打 bit0 隐藏标记(生僻字字典, 不做繁体对应副本) ----------
lf = bytearray(open(LIANGFEN, "rb").read())
lf_base = IE * 2
lf_count = (len(lf) - lf_base) // 16
lf_recs = []
for i in range(lf_count):
    rec = lf[lf_base + i * 16:lf_base + (i + 1) * 16]
    code = rec[:12].split(b"\x00")[0].decode("ascii")
    hz = rec[12:15].decode("utf-8")
    lf_recs.append((code, hz))

new_lf = [(c, h, char_flag(h)) for c, h in lf_recs]
lf_hidden = sum(1 for r in new_lf if r[2] & 1)
lf_codes = [r[0] for r in new_lf]
lf_index = [bisect.bisect_left(lf_codes, chr(97 + c0) + chr(97 + c1))
            for c0 in range(26) for c1 in range(26)] + [len(lf_codes)]
lf_idx_bytes = b"".join(struct.pack("<H", v) for v in lf_index)
lf_rec_bytes = b"".join(
    r[0].encode("ascii").ljust(12, b"\x00")[:12] + r[1].encode("utf-8").ljust(3, b"\x00")[:3]
    + bytes([r[2]]) for r in new_lf
)
new_lf = lf_idx_bytes + lf_rec_bytes
print(f"\nliangfen: {lf_count} 条, 隐藏 {lf_hidden}")
open(LIANGFEN, "wb").write(new_lf)
print(f"已写入 {LIANGFEN}")
