# -*- coding: utf-8 -*-
"""生成救援层控制台中文字模（16 点阵 / 取模嵌入）。

为什么要"左半 + 右半"两块：
  Linux VT（vgacon/fbcon）的最小字符格是 8 像素宽（80 列 × 8px）。内核对双宽
  字符的处理是"第一格画整字、第二格填空格"（drivers/tty/vt/vt.c），8px 字库
  下汉字会被挤成 8px。因此本工具把每个汉字切成 8×16 + 8×16 两块，各映射到一
  个私用区码位（U+E000 起），运行时由 /zjzhmap.sed 把汉字替换成这两个码位：
  汉字真的占 16×16、真的占 2 列，ASCII 仍是 8px / 1 列。

槽位布局（PSF1 最多 512 个字形，内核 max_font_glyphs 也是 512）：
  · 0x20..0x7e  ASCII —— **必须留在自己的码位上**：VT 的擦屏/滚屏是把 0x20
    直接写进显存的（绕过 unicode 映射），空格不在 0x20 会在清屏后满屏乱码。
  · 0x80..0x1ff + 0x00..0x1f 汉字左右半（这些槽不会被 8 位裸字符命中）。

用法：
  python tools/gen-rescue-font.py            # 重新生成 bootfiles/zjfont.psf
                                             #       bootfiles/zjzhmap.sed
  python tools/gen-rescue-font.py --check    # 只检查：字模是否覆盖脚本用字
依赖：freetype-py（pip install freetype-py）+ Windows 自带 simsun.ttc。
改动提示语后必须重跑本工具，否则 check-shell.py 的 C8 会报"字模未覆盖"。
"""
import argparse
import os
import re
import struct
import sys

import freetype

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT_SRC = r"C:\Windows\Fonts\simsun.ttc"
FONT_INDEX = 0
PX = 16
CELL_W, CELL_H = 8, 16
BASELINE = 14
PUA_BASE = 0xE000
ASCII_START, ASCII_END = 0x20, 0x7E
MAX_GLYPHS = 512

PRODUCT = [
    "bootfiles/zjrestore-lite.sh",
    "bootfiles/zjswap.sh",
    "bootfiles/zjswap-undo.sh",
    "bootfiles/setup-swap.sh",
    "bootfiles/zjlog.sh",
    "bootfiles/alpine/init",
]
SAY_RE = re.compile(r'\bsay\s+"([^"]*)"')
CJK_RE = re.compile('[\u4e00-\u9fff]')
FULLWIDTH_RE = re.compile('[\u3000-\u303f\uff00-\uffef]')


def is_cjk(ch):
    o = ord(ch)
    return 0x4E00 <= o <= 0x9FFF or 0x3400 <= o <= 0x4DBF


def width(s):
    return sum(2 if is_cjk(c) else 1 for c in s)


def scan_messages():
    """收集所有带中文的 say 字面量：(相对路径, 行号, 文本)。"""
    out = []
    for rel in PRODUCT:
        path = os.path.join(ROOT, rel)
        with open(path, encoding="utf-8") as fh:
            for i, line in enumerate(fh, 1):
                if line.lstrip().startswith("#"):
                    continue
                m = SAY_RE.search(line)
                if m and CJK_RE.search(m.group(1)):
                    out.append((rel, i, m.group(1)))
    return out


def collect_hanzi(msgs):
    chars = sorted({c for _, _, s in msgs for c in s if is_cjk(c)})
    bad = []
    for _, _, s in msgs:
        for c in s:
            if FULLWIDTH_RE.match(c):
                bad.append((s, c))
            elif not is_cjk(c) and not (ASCII_START <= ord(c) <= ASCII_END):
                bad.append((s, c))
    return chars, bad


def load_bitmap(face, ch):
    face.load_char(ch, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_MONO)
    g = face.glyph
    bm = g.bitmap
    assert bm.pixel_mode == freetype.FT_PIXEL_MODE_MONO, "not a mono bitmap"
    rows = []
    buf = bm.buffer
    for y in range(bm.rows):
        row = []
        for x in range(bm.width):
            byte = buf[y * bm.pitch + (x >> 3)]
            row.append(1 if (byte >> (7 - (x & 7))) & 1 else 0)
        rows.append(row)
    return rows, g.bitmap_left, g.bitmap_top


def place(face, ch, cw, chh, clip_report):
    rows, left, top = load_bitmap(face, ch)
    wsrc = len(rows[0]) if rows else 0
    if wsrc > cw:
        scaled = []
        for row in rows:
            out = []
            for x in range(cw):
                a = (x * wsrc) // cw
                b = ((x + 1) * wsrc) // cw
                out.append(1 if any(row[i] for i in range(a, max(a + 1, b))) else 0)
            scaled.append(out)
        clip_report.append((ch, "squeezed %d->%d" % (wsrc, cw)))
        rows, wsrc = scaled, cw
    canvas = [[0] * cw for _ in range(chh)]
    x0 = left
    y0 = BASELINE - top
    if x0 < 0 or y0 < 0 or x0 + wsrc > cw or y0 + len(rows) > chh:
        clip_report.append((ch, left, top, wsrc, len(rows)))
    for y, row in enumerate(rows):
        yy = y0 + y
        if not (0 <= yy < chh):
            continue
        for x, bit in enumerate(row):
            xx = x0 + x
            if bit and 0 <= xx < cw:
                canvas[yy][xx] = 1
    return canvas


def canvas_to_bytes(cv):
    out = bytearray()
    for row in cv:
        b = 0
        for i, bit in enumerate(row):
            if bit:
                b |= 0x80 >> i
        out.append(b)
    return bytes(out)


def build(hanzi):
    face = freetype.Face(FONT_SRC, FONT_INDEX)
    face.set_pixel_sizes(0, PX)

    clip_ascii, clip_cjk = [], []
    index = {}
    slot_bytes = [None] * MAX_GLYPHS
    slot_maps = [[] for _ in range(MAX_GLYPHS)]

    for cp in range(ASCII_START, ASCII_END + 1):
        cv = place(face, chr(cp), CELL_W, CELL_H, clip_ascii)
        slot_bytes[cp] = canvas_to_bytes(cv)
        slot_maps[cp] = [cp]

    free = [s for s in (list(range(0x80, MAX_GLYPHS)) + list(range(0x00, ASCII_START))
                        ) if slot_bytes[s] is None]
    if 2 * len(hanzi) > len(free):
        raise SystemExit("字模预算超了：需要 %d 槽，只剩 %d"
                         % (2 * len(hanzi), len(free)))
    for i, ch in enumerate(hanzi):
        cv16 = place(face, ch, 16, 16, clip_cjk)
        lslot, rslot = free[2 * i], free[2 * i + 1]
        slot_bytes[lslot] = canvas_to_bytes([r[0:8] for r in cv16])
        slot_maps[lslot] = [PUA_BASE + 2 * i]
        slot_bytes[rslot] = canvas_to_bytes([r[8:16] for r in cv16])
        slot_maps[rslot] = [PUA_BASE + 2 * i + 1]
        index[ch] = (lslot, rslot, PUA_BASE + 2 * i, PUA_BASE + 2 * i + 1)

    charsize = CELL_H * ((CELL_W + 7) // 8)
    body = b"".join(b if b is not None else bytes(charsize) for b in slot_bytes)
    table = bytearray()
    for cps in slot_maps:
        for cp in cps:
            table += struct.pack("<H", cp)   # PSF1 的 unicode 表是 UTF-16LE
        table += b"\xff\xff"                 # 每个字形一条 0xFFFF 分隔
    psf = struct.pack("<BBBB", 0x36, 0x04, 0x03, charsize) + body + bytes(table)

    used = sum(1 for b in slot_bytes if b is not None)
    return psf, index, used, clip_ascii, clip_cjk


def sed_text(index, hanzi):
    out = []
    for ch in hanzi:
        out.append("s/%s/%s%s/g" % (ch, chr(index[ch][2]), chr(index[ch][3])))
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="只检查字模是否覆盖脚本用字，不写文件")
    args = ap.parse_args()

    msgs = scan_messages()
    hanzi, bad = collect_hanzi(msgs)
    if bad:
        for s, c in bad:
            print("非汉字/非 ASCII 字符 %r (U+%04X) in: %s" % (c, ord(c), s))
        raise SystemExit("提示语只允许汉字 + 半角 ASCII（全角标点请换成半角）")

    print("中文字面量 %d 条，不同汉字 %d 个，字形预算 %d/512"
          % (len(msgs), len(hanzi), 95 + 2 * len(hanzi)))

    if args.check:
        print("(check only, nothing written)")
        return 0

    if not os.path.isfile(FONT_SRC):
        raise SystemExit("缺少字体源 %s" % FONT_SRC)
    psf, index, used, clip_ascii, clip_cjk = build(hanzi)
    if clip_ascii:
        print("ASCII 裁剪: %r" % clip_ascii)
    if clip_cjk:
        print("汉字裁剪: %r" % clip_cjk)

    fp = os.path.join(ROOT, "bootfiles", "zjfont.psf")
    with open(fp, "wb") as fh:
        fh.write(psf)
    sp = os.path.join(ROOT, "bootfiles", "zjzhmap.sed")
    with open(sp, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(sed_text(index, hanzi))

    print("wrote %s (%d bytes), %s (%d bytes); glyphs used %d/512"
          % (os.path.relpath(fp, ROOT), len(psf),
             os.path.relpath(sp, ROOT), os.path.getsize(sp), used))
    print("提示语列宽（含 4 列前缀）：")
    over = 0
    for rel, ln, s in msgs:
        cols = 4 + width(s)
        if cols > 78:
            over += 1
        print("  %-28s:%-4d %3d  %s" % (os.path.basename(rel), ln, cols, s))
    if over:
        raise SystemExit("有 %d 条提示超过 78 列，必须拆行" % over)
    return 0


if __name__ == "__main__":
    sys.exit(main())
