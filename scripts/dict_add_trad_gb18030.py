#!/usr/bin/env python3
"""向现有 IME 词库追加 繁体+GB18030 单字。

范围: (TSCharacters 繁体专属字 ∪ (GB18030 Han ∩ kMandarin)) − 现有单字,
且字体能显示 (terminus28/22.fnt 覆盖 U+3400-4DB5 + U+4E00-9FEF)。
拼音: luna(优先) / kMandarin(去声调+ü→v+lve→lue+nve→nue) 兜底。

流程:
1. 解析现有 blob 单字记录, 验证索引语义 (重算索引比对, 全等才继续)
2. 计算目标单字集与拼音
3. 合并/去重/按(rank, code)稳定排序
4. 重建 header+index+单字区, 词区(含词头+词索引+词数据)逐字节复制
5. 补齐 0xFF 到 2MB, 输出到 /tmp/ime_table_pinyin_new.bin (确认后手动覆盖 ime_table_pinyin.bin)
"""
import re, sys, struct
from collections import Counter

BLOB = "/home/ywz/pjournal-esp32/main/ime/ime_table_pinyin.bin"
OUT  = "/home/ywz/pjournal-esp32/main/ime/ime_table_pinyin.bin"
LUNA = "/tmp/luna.dict.yaml"
KMAND = "/tmp/kMandarin.txt"
TSCHAR = "/tmp/TSCharacters.txt"
FONT = "/home/ywz/pjournal-esp32/main/terminus28.fnt"

HEADER = 12
INDEX_ENTRIES = 677
CODELEN = 6
REC = CODELEN + 3 + 1   # 10

def rank(code):
    if len(code) < 2:
        return code + 'a'
    return code[:2]

def build_index(records):
    """records: list of code strings (sorted by (rank,code)). idx[k]=count(rank<key(k))."""
    idx = [0]*INDEX_ENTRIES
    counts = Counter(rank(c) for c in records)
    # prefix sum over key space (aa..zz)
    tot = 0
    for c0 in range(26):
        for c1 in range(26):
            k = c0*26 + c1
            idx[k] = tot
            tot += counts.get(chr(97+c0)+chr(97+c1), 0)
    idx[INDEX_ENTRIES - 1] = tot   # 677th entry: sentinel = total
    return idx

# ---------- load existing blob ----------
blob = open(BLOB, "rb").read()
assert blob[:4] == b"IME3"
assert blob[5] == CODELEN
count = int.from_bytes(blob[8:12], "little")
rec_base = HEADER + INDEX_ENTRIES*4
print(f"现有词库: count={count} fileSize={len(blob)}")

recs = []   # (code, hanzi, flag)
for i in range(count):
    rec = blob[rec_base + i*REC : rec_base + (i+1)*REC]
    code = rec[:CODELEN].split(b'\x00')[0].decode('ascii')
    hz = rec[CODELEN:CODELEN+3].decode('utf-8')
    flag = rec[CODELEN+3]
    recs.append((code, hz, flag))

# verify internal sort: (rank, code) non-decreasing
codes = [r[0] for r in recs]
keys = [(rank(c), c) for c in codes]
sorted_ok = all(keys[i] <= keys[i+1] for i in range(len(keys)-1))
print(f"现有记录按 (rank,code) 有序: {sorted_ok}")

# verify index semantics: recompute from records, compare to blob bytes
idx = build_index(codes)
idx_bytes = [int.from_bytes(blob[HEADER+k*4:HEADER+k*4+4], "little") for k in range(INDEX_ENTRIES)]
mism = [k for k in range(INDEX_ENTRIES) if idx[k] != idx_bytes[k]]
print(f"索引重算校验: 不一致 {len(mism)} 处" + ("" if not mism else f" -> {mism[:10]}"))
if mism:
    print("索引语义与现有 blob 不符, 终止"); sys.exit(1)
print(f"idx[676]={idx[676]} (rank<zz), 末桶(z==) 记录 {count-idx[676]}")

# word section bounds
word_base = rec_base + count*REC
word_count = int.from_bytes(blob[word_base:word_base+4], "little")
wi = [int.from_bytes(blob[word_base+4+k*4:word_base+4+k*4+4], "little") for k in range(INDEX_ENTRIES)]
wdsz = wi[-1]
data_start = word_base + 4 + INDEX_ENTRIES*4
word_section = blob[word_base:data_start + wdsz]   # count+index+data 逐字节复制用
print(f"词区: word_count={word_count} wordDataSize={wdsz} word_section_len={len(word_section)}")

# ---------- target char set ----------
# 1. traditional-only from TSCharacters
trad = set()
for line in open(TSCHAR, encoding="utf-8"):
    line = line.strip()
    if not line or line.startswith("#"): continue
    p = line.split("\t")
    if len(p) < 2: continue
    if p[0] not in p[1].split():
        trad.add(p[0])
print(f"TSCharacters 繁体专属字: {len(trad)}")

# 2. kMandarin char -> codes
kmand = {}
for line in open(KMAND, encoding="utf-8"):
    line = line.rstrip()
    if not line or line.startswith("#"): continue
    idx2 = line.rfind("#")
    if idx2 < 0: continue
    ch = line[idx2+1:].strip()
    if len(ch) != 1: continue
    left = line[:idx2].strip()
    m = re.match(r"U\+[0-9A-Fa-f]+:\s*(.*)$", left)
    if not m: continue
    codes = m.group(1).split()
    kmand[ch] = codes
print(f"kMandarin 单字: {len(kmand)}")

# 3. luna char -> codes
luna = {}
for line in open(LUNA, encoding="utf-8"):
    line = line.rstrip("\n")
    if not line or line.startswith("#") or line.startswith("-"):
        continue
    p = line.split("\t")
    if len(p) < 2: continue
    w = p[0]
    if len(w.encode("utf-8")) != 3: continue
    codes = [c.strip() for c in p[1].split()]
    luna.setdefault(w, []).extend(codes)
print(f"luna 单字: {len(luna)}")

# 4. GB18030 Han
gb = set(chr(cp) for cp in range(0x4E00, 0x9FA6))

# 5. font glyph coverage
fb = open(FONT, "rb").read()
assert fb[:4] == b"PJFN"
cjk_off = struct.unpack_from("<I", fb, 18)[0]
bc = struct.unpack_from("<H", fb, cjk_off+10)[0]
blocks = struct.unpack_from(f"<{bc*3}I", fb, cjk_off+12)
ranges = [(blocks[i], blocks[i+1]) for i in range(0, len(blocks), 3)]
def has_glyph(cp):
    for a, b in ranges:
        if a <= cp <= b: return True
    return False

# ---------- target set ----------
existing_chars = {r[1] for r in recs}
target = (trad | (gb & set(kmand))) - existing_chars
print(f"目标集(未过滤字体): {len(target)}")
noglyph = sorted(c for c in target if not has_glyph(ord(c)))
target = set(c for c in target if has_glyph(ord(c)))
print(f"字体缺失过滤: {len(noglyph)} 字 例: {''.join(noglyph[:40])}")
print(f"目标集(最终): {len(target)}")

# ---------- pinyin for each char ----------
TONE = {'ā':'a','á':'a','ǎ':'a','à':'a','ē':'e','é':'e','ě':'e','è':'e',
        'ī':'i','í':'i','ǐ':'i','ì':'i','ō':'o','ó':'o','ǒ':'o','ò':'o',
        'ū':'u','ú':'u','ǔ':'u','ù':'u','ǖ':'ü','ǘ':'ü','ǚ':'ü','ǜ':'ü',
        'ń':'n','ň':'n','ḿ':'m'}
def normalize(code):
    out = code.lower()
    out = ''.join(TONE.get(c, c) for c in out)
    out = out.replace('ü', 'v')
    out = out.replace('lve', 'lue').replace('nve', 'nue')
    return out

CODE_OK = re.compile(r'^[a-z]{1,6}$')
def good(code):
    return bool(CODE_OK.match(code))

new_recs = []
skipped_nopinyin = []
skipped_badcode = []
used_src = Counter()
for ch in sorted(target):
    src = None
    codes = []
    if ch in luna:
        codes = [c for c in luna[ch] if good(normalize(c))]
        if codes: src = "luna"
    if not codes and ch in kmand:
        codes = [normalize(c) for c in kmand[ch] if good(normalize(c))]
        if codes: src = "kmand"
    if not codes:
        skipped_nopinyin.append(ch); continue
    # dedup readings, keep original order
    seen = set(); uniq = []
    for c in codes:
        if c not in seen:
            seen.add(c); uniq.append(c)
    used_src[src] += 1
    for c in uniq:
        new_recs.append((c, ch, 0))

print(f"拼音来源: {dict(used_src)}")
print(f"无拼音跳过: {len(skipped_nopinyin)} 例: {''.join(skipped_nopinyin[:40])}")
print(f"新增单字记录: {len(new_recs)}  (含多音, 字数 {len({r[1] for r in new_recs})})")

# ---------- merge & sort ----------
all_recs = list(recs) + new_recs
# dedup (code, hanzi, flag)
seen = set(); merged = []
for r in all_recs:
    k = (r[0], r[1])
    if k in seen: continue
    seen.add(k); merged.append(r)
# stable sort by (rank, code)
merged.sort(key=lambda r: (rank(r[0]), r[0]))
print(f"合并后总记录: {len(merged)}  (原 {len(recs)} + 新增 {len(merged)-len(recs)})")

# ---------- rebuild ----------
new_count = len(merged)
hdr = b"IME3" + bytes([1, CODELEN, 0, 0]) + struct.pack("<I", new_count)
new_idx = build_index([r[0] for r in merged])
new_idx_bytes = b"".join(struct.pack("<I", v) for v in new_idx)
rec_bytes = b"".join(r[0].encode().ljust(CODELEN, b"\x00") + r[1].encode('utf-8') + bytes([r[2]]) for r in merged)

out = hdr + new_idx_bytes + rec_bytes + word_section
out += b"\xff" * (2097152 - len(out))
print(f"输出: {len(out)} 字节 (used {len(out)-len(out.lstrip(b'\\xff'))}? )")

open("/tmp/ime_table_pinyin_new.bin", "wb").write(out)
print("已写入 /tmp/ime_table_pinyin_new.bin")
