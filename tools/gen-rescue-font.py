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

字源格式（与 GNU Unifont 的 .hex 相同，便于换源）：
  每行 `码位:点阵`，点阵是 16 行、每行 8 列（32 个 hex 字符）或 16 列（64 个）。
  `#` 开头为注释。入库的子集在 tools/fonts/rescue-16px.hex，文件头写明上游
  来源与内容 SHA-256 —— 换源时用 --from-unifont 从上游整库导出即可。

用法：
  python tools/gen-rescue-font.py            # 重新生成 bootfiles/zjfont.psf
                                             #       bootfiles/zjzhmap.sed
  python tools/gen-rescue-font.py --check    # 只校验：字模/映射是否覆盖脚本用字、
                                             #   每条是否 ≤78 列（make check 会跑，不写文件）
  python tools/gen-rescue-font.py --verify   # 重新生成并与入库文件逐字节比对
  python tools/gen-rescue-font.py --from-unifont <上游 .hex|.hex.gz>
                                             # 只导出 tools/fonts/rescue-16px.hex
                                             #   （ASCII 0x20..0x7E + 脚本用到的汉字），
                                             #   不写 zjfont.psf / zjzhmap.sed

依赖：**无第三方库**。纯文本点阵解析，不需要 freetype，也不需要系统字体，
因此任何人拿到仓库都能复现出同一份字节（这一点是换开源字源的主要目的之一）。
改动提示语后必须重跑本工具，否则 check-shell.py 的 C8 会报"字模未覆盖"。
"""
import argparse
import gzip
import hashlib
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT_HEX = os.path.join(ROOT, "tools", "fonts", "rescue-16px.hex")
CELL_W, CELL_H = 8, 16
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
SAY_RE = re.compile(r'\bsay2?\s+"([^"]*)"')
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


def read_text(path):
    """读 .hex（可 gzip），返回文本。"""
    if path.endswith(".gz"):
        with gzip.open(path, "rb") as fh:
            return fh.read().decode("utf-8")
    with open(path, encoding="utf-8") as fh:
        return fh.read()


def parse_hex(text, where="<hex>"):
    """解析 `码位:点阵`，返回 {码位: rows}。rows 是 16 行、每行 8 或 16 个 0/1。"""
    glyphs = {}
    for ln, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if ":" not in line:
            raise SystemExit("%s:%d 不是 `码位:点阵` 格式: %r" % (where, ln, line))
        cp_s, bits = line.split(":", 1)
        try:
            cp = int(cp_s, 16)
        except ValueError:
            raise SystemExit("%s:%d 码位不是十六进制: %r" % (where, ln, cp_s))
        if len(bits) not in (32, 64):
            raise SystemExit("%s:%d 点阵应为 32（8 列）或 64（16 列）个 hex 字符，"
                             "实为 %d 个" % (where, ln, len(bits)))
        cols = 8 if len(bits) == 32 else 16
        step = cols // 4
        rows = []
        for i in range(0, len(bits), step):
            v = int(bits[i:i + step], 16)
            rows.append([(v >> (cols - 1 - b)) & 1 for b in range(cols)])
        if len(rows) != CELL_H:
            raise SystemExit("%s:%d 应为 %d 行，实为 %d 行"
                             % (where, ln, CELL_H, len(rows)))
        glyphs[cp] = rows
    return glyphs


def load_font_hex():
    if not os.path.isfile(FONT_HEX):
        raise SystemExit("缺少字源 %s；先用 --from-unifont <上游 .hex> 导出子集，"
                         "或从仓库恢复该文件" % os.path.relpath(FONT_HEX, ROOT))
    return parse_hex(read_text(FONT_HEX), os.path.relpath(FONT_HEX, ROOT))


def pack_rows(rows, cols=CELL_W):
    """rows（每行 cols 个 0/1）→ 每行 1 字节（cols=8）。"""
    out = bytearray()
    for row in rows:
        b = 0
        for i in range(cols):
            if row[i]:
                b |= 0x80 >> i
        out.append(b)
    return bytes(out)


def build(hanzi):
    glyphs = load_font_hex()
    warned = []

    index = {}
    slot_bytes = [None] * MAX_GLYPHS
    slot_maps = [[] for _ in range(MAX_GLYPHS)]

    for cp in range(ASCII_START, ASCII_END + 1):
        rows = glyphs.get(cp)
        if rows is None:
            raise SystemExit("字源缺少 ASCII U+%04X 的字形（0x20..0x7E 必须齐全）" % cp)
        if len(rows[0]) != CELL_W:
            # 字源若把 ASCII 画成 16 列，取左 8 列（控制台一格只有 8px）
            warned.append(cp)
            rows = [r[:CELL_W] for r in rows]
        slot_bytes[cp] = pack_rows(rows)
        slot_maps[cp] = [cp]
    if warned:
        print("注意：字源里 %d 个 ASCII 字形是 16 列，已取左 8 列（U+%04X 起）"
              % (len(warned), warned[0]))

    missing = [ch for ch in hanzi if ord(ch) not in glyphs]
    if missing:
        raise SystemExit("字源缺 %d 个汉字: %s；请用 --from-unifont 重新导出子集"
                         % (len(missing), "".join(missing)))

    free = [s for s in (list(range(0x80, MAX_GLYPHS)) + list(range(0x00, ASCII_START))
                        ) if slot_bytes[s] is None]
    if 2 * len(hanzi) > len(free):
        raise SystemExit("字模预算超了：需要 %d 槽，只剩 %d"
                         % (2 * len(hanzi), len(free)))
    for i, ch in enumerate(hanzi):
        rows = glyphs[ord(ch)]
        if len(rows[0]) != 16:
            raise SystemExit("汉字 %r 在字源里只有 %d 列，16 点阵需要 16 列"
                             % (ch, len(rows[0])))
        lslot, rslot = free[2 * i], free[2 * i + 1]
        slot_bytes[lslot] = pack_rows([r[:8] for r in rows])
        slot_bytes[rslot] = pack_rows([r[8:16] for r in rows])
        slot_maps[lslot] = [PUA_BASE + 2 * i]
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
    return psf, index, used


def sed_text(index, hanzi):
    out = []
    for ch in hanzi:
        out.append("s/%s/%s%s/g" % (ch, chr(index[ch][2]), chr(index[ch][3])))
    return "\n".join(out) + "\n"


def export_subset(src_path, hanzi):
    """从上游整库 .hex 导出本脚本用到的子集（ASCII + 汉字），写 FONT_HEX。"""
    text = read_text(src_path)
    digest = hashlib.sha256(text.encode("utf-8")).hexdigest()
    glyphs = parse_hex(text, src_path)
    want = list(range(ASCII_START, ASCII_END + 1)) + [ord(ch) for ch in hanzi]
    missing = [cp for cp in want if cp not in glyphs]
    if missing:
        raise SystemExit("上游缺 %d 个字形: %s"
                         % (len(missing), ", ".join("U+%04X" % cp for cp in missing[:12])))

    lines = [
        "# 救援层控制台点阵子集（%d ASCII + %d 汉字，16 行；8 或 16 列）" % (95, len(hanzi)),
        "# 由 tools/gen-rescue-font.py --from-unifont 从上游整库导出，勿手改。",
        "# 上游文件：%s" % os.path.basename(src_path),
        "# 上游内容 SHA-256（解压后文本）：%s" % digest,
        "# 说明：本文件只含脚本当前用到的字；改提示语后请重跑 --from-unifont。",
        "",
    ]
    for cp in want:
        rows = glyphs[cp]
        stride = len(rows[0]) // 4
        bits = ""
        for row in rows:
            v = 0
            for b in row:
                v = (v << 1) | b
            bits += ("%0*x" % (stride, v))
        lines.append("%04X:%s" % (cp, bits))
    os.makedirs(os.path.dirname(FONT_HEX), exist_ok=True)
    with open(FONT_HEX, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")
    print("导出 %s：%d 个字形（ASCII 95 + 汉字 %d），%d B"
          % (os.path.relpath(FONT_HEX, ROOT), len(want), len(hanzi),
             os.path.getsize(FONT_HEX)))


SED_LINE_RE = re.compile(r"s/(.)/(..)/g$")
PREFIX_COLS = 4          # "ZJ: " / "SR: "
CONSOLE_COLS = 78        # 80 列控制台，留 2 列余量
ZH_FONT = "bootfiles/zjfont.psf"
ZH_MAP = "bootfiles/zjzhmap.sed"


def load_map(path):
    """读已入库的 zjzhmap.sed，返回 {汉字: (左PUA, 右PUA)}。"""
    out = {}
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            m = SED_LINE_RE.match(line.rstrip("\n"))
            if m and len(m.group(2)) == 2:
                out[m.group(1)] = (m.group(2)[0], m.group(2)[1])
    return out


def load_font_pua(path):
    """读已入库的 zjfont.psf（PSF1），返回 unicode 表里的码位集合。"""
    data = open(path, "rb").read()
    if len(data) < 4:
        raise SystemExit("%s 太短，不是 PSF1" % path)
    m0, m1, mode, charsize = struct.unpack("<BBBB", data[:4])
    if (m0, m1) != (0x36, 0x04):
        raise SystemExit("%s 不是 PSF1（头 %02x %02x；PSF2 本镜像 busybox 不支持）"
                         % (path, m0, m1))
    if mode > 0x05:
        raise SystemExit("%s PSF1 mode=0x%02x 不受支持" % (path, mode))
    count = 512 if mode & 0x01 else 256
    table = data[4 + count * charsize:]
    pua = set()
    for i in range(0, len(table) - 1, 2):
        cp = struct.unpack("<H", table[i:i + 2])[0]
        if cp != 0xffff:
            pua.add(cp)
    return pua


def check_layers(msgs, zh_map, pua):
    """C8 同类校验：映射覆盖 + 字模覆盖 + 单行宽度。错误信息带 文件:行号。"""
    bad = 0
    for rel, ln, s in msgs:
        cols = PREFIX_COLS + width(s)
        if cols > CONSOLE_COLS:
            print("  %s:%d %d 列 > %d 列（含 %d 列前缀），必须拆行: %s"
                  % (rel, ln, cols, CONSOLE_COLS, PREFIX_COLS, s))
            bad += 1
        for ch in s:
            if not is_cjk(ch):
                continue
            if ch not in zh_map:
                print("  %s:%d 汉字 %r 没有映射（%s 未覆盖；请重跑 "
                      "python tools/gen-rescue-font.py）" % (rel, ln, ch, ZH_MAP))
                bad += 1
                continue
            left, right = zh_map[ch]
            if ord(left) not in pua or ord(right) not in pua:
                print("  %s:%d 汉字 %r 的字形缺失（%s 里没有 U+%04X/U+%04X；"
                      "请重跑生成器）"
                      % (rel, ln, ch, ZH_FONT, ord(left), ord(right)))
                bad += 1
    return bad


def runtime_screen(s, zh_map, pua):
    """模拟运行期 zj_ascii（bootfiles/zjlog.sh）：ASCII 保留；字模内汉字保留
    （运行时会被 zjzhmap.sed 换成 PUA 半字）；**其余字符连续串压成一个 `.`**。"""
    out, in_run = [], False
    for ch in s:
        if ASCII_START <= ord(ch) <= ASCII_END:
            out.append(ch)
            in_run = False
        elif ch in zh_map and all(ord(p) in pua for p in zh_map[ch]):
            out.append(ch)
            in_run = False
        else:
            if not in_run:
                out.append(".")
            in_run = True
    return "".join(out)


def check_dynamic_samples(zh_map, pua):
    """PIT-238（重做）回归：用户文本（镜像名/卷标/路径）里的**库外汉字**必须
    被替换成 `.`，绝不允许原样送屏（真机：白底方块/问号）。"""
    log_sh = os.path.join(ROOT, "bootfiles", "zjlog.sh")
    with open(log_sh, encoding="utf-8") as fh:
        shell_src = fh.read()
    if "s/[^ -~][^ -~]*/./g" not in shell_src:
        print("  bootfiles/zjlog.sh 的 zj_ascii 占位符不是 `.`（运行期与校验器不一致）")
        return 1
    bad = 0
    # 库外汉字样例（真机实测串）：忘/初/心 三个都没有字形。
    sample = "C:\\images\\不忘初心\\win10.wim"
    got = runtime_screen(sample, zh_map, pua)
    if any(is_cjk(c) and c not in zh_map for c in got):
        print("  样例仍有库外汉字未替换: %r" % got)
        bad += 1
    # 库里有的「不」必须保留（这正是真机"部分正常"的来源）。
    if "不" in zh_map and "不" not in got:
        print("  样例把字模内的「不」也替换掉了: %r" % got)
        bad += 1
    # 纯库外串必须只剩 ASCII + `.`，且连续串只留一个点。
    plain = "D:\\忘初心\\win10.wim"
    want = "D:\\.\\win10.wim"
    got = runtime_screen(plain, zh_map, pua)
    if got != want:
        print("  库外汉字替换结果不对: %r（应为 %r）" % (got, want))
        bad += 1
    # 字模内的字必须原样保留（不许误伤自有提示文案）。
    covered = "".join(list(zh_map)[:4])
    if zh_map and runtime_screen(covered, zh_map, pua) != covered:
        print("  字模内汉字被误替换: %r" % covered)
        bad += 1
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="只校验：字模/映射是否覆盖脚本用字、每行是否 ≤78 列（不写文件）")
    ap.add_argument("--verify", action="store_true",
                    help="用 tools/fonts/rescue-16px.hex 重新生成并与仓库内文件逐字节比对")
    ap.add_argument("--from-unifont", metavar="UPSTREAM_HEX",
                    help="从上游整库 .hex（可 gzip）导出子集，不生成资产")
    args = ap.parse_args()

    msgs = scan_messages()
    hanzi, bad = collect_hanzi(msgs)
    if bad:
        for s, c in bad:
            print("非汉字/非 ASCII 字符 %r (U+%04X) in: %s" % (c, ord(c), s))
        raise SystemExit("提示语只允许汉字 + 半角 ASCII（全角标点请换成半角）")

    print("中文字面量 %d 条，不同汉字 %d 个，字形预算 %d/512"
          % (len(msgs), len(hanzi), 95 + 2 * len(hanzi)))

    if args.from_unifont:
        export_subset(args.from_unifont, hanzi)
        print("（未生成 zjfont.psf / zjzhmap.sed；确认字源后直接运行本工具生成）")
        return 0

    fp = os.path.join(ROOT, "bootfiles", "zjfont.psf")
    sp = os.path.join(ROOT, "bootfiles", "zjzhmap.sed")

    if args.check:
        maxc = max(PREFIX_COLS + width(s) for _, _, s in msgs)
        print("列宽上限 %d（%d 条中最宽 %d 列），字模 %s，映射 %s"
              % (CONSOLE_COLS, len(msgs), maxc, ZH_FONT, ZH_MAP))
        if not os.path.isfile(fp) or not os.path.isfile(sp):
            raise SystemExit("缺 %s / %s；请先跑 python tools/gen-rescue-font.py"
                             % (ZH_FONT, ZH_MAP))
        bad = check_layers(msgs, load_map(sp), load_font_pua(fp))
        if bad:
            raise SystemExit("字模/映射/宽度校验失败 %d 处（见上）" % bad)
        dyn_bad = check_dynamic_samples(load_map(sp), load_font_pua(fp))
        if dyn_bad:
            raise SystemExit("用户文本替换校验失败 %d 处（见上）" % dyn_bad)
        print("动态样例校验：库外汉字 → `.`，字模内汉字保留（不忘初心 / D:\\忘初心）")
        print("(check only, nothing written)")
        return 0

    psf, index, used = build(hanzi)

    if args.verify:
        ok = True
        if not os.path.isfile(fp) or open(fp, "rb").read() != psf:
            print("不一致: %s（重新生成的 %d B）" % (fp, len(psf)))
            ok = False
        want = sed_text(index, hanzi)
        have = open(sp, encoding="utf-8").read() if os.path.isfile(sp) else ""
        if have != want:
            print("不一致: %s（重新生成 %d 行）" % (sp, len(want.splitlines())))
            ok = False
        print("verify: %s" % ("byte-identical" if ok else "MISMATCH"))
        return 0 if ok else 1

    with open(fp, "wb") as fh:
        fh.write(psf)
    with open(sp, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(sed_text(index, hanzi))

    print("wrote %s (%d bytes), %s (%d bytes); glyphs used %d/512"
          % (os.path.relpath(fp, ROOT), len(psf),
             os.path.relpath(sp, ROOT), os.path.getsize(sp), used))
    print("提示语列宽（含 %d 列前缀）：" % PREFIX_COLS)
    over = 0
    for rel, ln, s in msgs:
        cols = PREFIX_COLS + width(s)
        if cols > CONSOLE_COLS:
            over += 1
        print("  %-28s:%-4d %3d  %s" % (os.path.basename(rel), ln, cols, s))
    if over:
        raise SystemExit("有 %d 条提示超过 %d 列，必须拆行" % (over, CONSOLE_COLS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
