#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check-shell.py — 救援层 shell 门禁（P2，AGENTS PIT-147）

五条规则（全部零依赖、纯 Python）：

  C1 语法   —— bash -n 过一遍全部产品脚本（+ 负例 harness）。
               语法错误必须死在开发机，不能死在客户机的 rescue shell 里。

  C2 棘轮   —— 每个产品文件里 `2>/dev/null` 的出现次数（纯注释行不计）
               不得超过 tools/shell-stderr-baseline.txt 里记录的基线。
               新增消音 = 必须显式改基线（diff 可见 = 人工分类审查过）；
               删减自然通过。防止"随手补一个 2>/dev/null"让静默失败回潮。

  C3 禁消音 —— 关键命令的 stderr 绝不允许 2>/dev/null（wimlib/mkntfs/...），
               这些命令失败必须原样吼出来（按"段首命令"判定：
               `command -v wimlib-imagex 2>/dev/null` 是探测，不算执行）。

  C4 入口标记 —— `bootfiles/alpine/init` 必须在调用 `zjrestore-lite.sh` 之前
               调用 `mark_entry_started`，且该函数只写 ESP 的
               `EFI/ZJRESTORE/entry-started.txt`、body 内无 `exit`（失败不得阻塞启动）。

  C5 载荷一致 —— `bootfiles/initramfs-zjrestore.cpio.gz` 里的救援脚本必须与
               `bootfiles/` 源码逐字节一致。该包曾经落后源码（docs/33 §5.4），
               而 `make all` 不刷新它，只能靠门禁拦住。

分类哲学见 docs/25-PIT坑位册.md PIT-147：
  A（保留）= 探针/尽力而为/有读回校验或响亮兜底的消音；
  B（修掉）= 消音会藏真实故障的 —— 一律改成 WARN/ERROR 或路由到证据文件。

用法：
  python tools/check-shell.py                # 跑门禁
  python tools/check-shell.py --update-baseline   # 分类审查后重设基线（慎用）
"""
import gzip
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 产品脚本（计入棘轮 + 禁消音 + 语法）
PRODUCT = [
    "bootfiles/zjrestore-lite.sh",
    "bootfiles/zjswap.sh",
    "bootfiles/zjswap-undo.sh",
    "bootfiles/setup-swap.sh",
    "bootfiles/zjlog.sh",
    "bootfiles/alpine/init",
]
# 语法检查额外覆盖（工具脚本，不计棘轮）
SYNTAX_ONLY = [
    "tools/vmtest/test-caps-failpath.sh",
    "tools/vmtest/test-reparse-awk.sh",
    "tools/vmtest/test-menu-loop.sh",
]
BASELINE_PATH = os.path.join(ROOT, "tools", "shell-stderr-baseline.txt")
TOKEN = "2>/dev/null"

# C3：这些命令失败必须看得见（未修复前不许进名单 —— 名单里的命令
# 当前必须零违例，否则门禁一上线就红）
DENY_CMDS = [
    "wimlib-imagex",
    "mkntfs",
    "ntfsfix",
    "ntfsresize",
    "wipefs",
    "sfdisk",
    "fdisk",
    "ntfs-3g",
]

# C4：init 首行标记（docs/33 §5.4）
INIT_PATH = "bootfiles/alpine/init"
MARKER_FILE = "EFI/ZJRESTORE/entry-started.txt"

# C5：initramfs 内成员 -> 仓库内源码（逐字节一致，文本换行/BOM 归一后比较）
INITRAMFS_PATH = "bootfiles/initramfs-zjrestore.cpio.gz"
INITRAMFS_MEMBERS = {
    "init": "bootfiles/alpine/init",
    "zjrestore-lite.sh": "bootfiles/zjrestore-lite.sh",
    "zjswap.sh": "bootfiles/zjswap.sh",
    "zjswap-undo.sh": "bootfiles/zjswap-undo.sh",
    "setup-swap.sh": "bootfiles/setup-swap.sh",
    "zjlog.sh": "bootfiles/zjlog.sh",
    # 二进制成员按原始字节比较（不做换行归一）
    "ntfs-boot-code.bin": "bootfiles/ntfs-boot-code.bin",
    "ntfs-boot-cont.bin": "bootfiles/ntfs-boot-cont.bin",
    "zj-restore-boot.exe": "bootfiles/zj-restore-boot.exe",
    # 中文字模与"汉字 -> 半字码位"映射（取模嵌入；见 tools/gen-rescue-font.py）
    "zjfont.psf": "bootfiles/zjfont.psf",
    "zjzhmap.sed": "bootfiles/zjzhmap.sed",
}
BINARY_MEMBERS = {"ntfs-boot-code.bin", "ntfs-boot-cont.bin",
                  "zj-restore-boot.exe", "zjfont.psf", "zjzhmap.sed"}

# C6：这些源文件只要比 initramfs 新，就说明载荷已过期（必须先重建）
# （vmlinuz 是同一构建的产物，不列入；alpine/ 下全部文件都算源）
PAYLOAD_SOURCES = PRODUCT + [
    "bootfiles/ntfs-boot-code.bin",
    "bootfiles/ntfs-boot-cont.bin",
    "bootfiles/zj-restore-boot.exe",
    "bootfiles/zjfont.psf",
    "bootfiles/zjzhmap.sed",
]

# C7：initramfs 里自带的构建版本戳必须与 version.h 同源
VERSION_H = "src/common/version.h"
VERSION_MEMBER = "zjrescue-version"


def find_bash():
    p = shutil.which("bash")
    if p:
        return p
    for cand in (
        r"C:\Program Files\Git\bin\bash.exe",
        r"C:\Program Files (x86)\Git\bin\bash.exe",
    ):
        if os.path.isfile(cand):
            return cand
    return None


def read_lines(rel):
    path = os.path.join(ROOT, rel)
    with open(path, "rb") as f:
        raw = f.read()
    if raw.startswith(b"\xef\xbb\xbf"):
        raw = raw[3:]
    return raw.decode("utf-8", errors="replace").splitlines()


def is_comment(line):
    return line.lstrip().startswith("#")


def count_token(rel):
    n = 0
    for line in read_lines(rel):
        if is_comment(line):
            continue
        n += line.count(TOKEN)
    return n


# 段首才是被消音的命令：行首/`|`/`&&`/`||`/`;`/`(` 之后（可带 timeout N / ! 前缀）
def _segment_re(cmd):
    return re.compile(
        r"(?:^|[|;&{(])\s*(?:!\s*)?(?:timeout\s+\d+\s+)?" + re.escape(cmd) + r"\b"
    )


def deny_violations(rel):
    """返回 (行号, 命令) 列表：该行消了 stderr 且段首是禁消音命令。"""
    out = []
    for i, line in enumerate(read_lines(rel), 1):
        if is_comment(line) or TOKEN not in line:
            continue
        for cmd in DENY_CMDS:
            if _segment_re(cmd).search(line):
                out.append((i, cmd))
    return out


# C8（取模中文化后反转）：控制台提示允许中文，但必须落在字模覆盖范围内，
# 且一条提示只占一行（80 列控制台、中文 2 列、上限 78 列）。内核的双宽折行
# 在 8px 字格下会把汉字挤扁/错位，所以宁可拆成多条 say，也不让内核折行。
# 纯 ASCII 的 say 不动（诊断行；QEMU 断言 grep 它们）。注释里的中文不受影响。
CJK_RE = re.compile('[\u4e00-\u9fff]')
FULLWIDTH_RE = re.compile('[\u3000-\u303f\uff00-\uffef]')
SAY_RE = re.compile(r'\bsay\s+"([^"]*)"')
ZH_MAP = "bootfiles/zjzhmap.sed"
ZH_FONT = "bootfiles/zjfont.psf"
CONSOLE_COLS = 78     # 80 列控制台，留 2 列余量
PREFIX_COLS = 4       # "SR: " / "ZJ: "


def _char_cols(ch):
    return 2 if CJK_RE.match(ch) else 1


def _load_zh_map():
    """读 zjzhmap.sed：汉字 -> 两个半字码位（PUA）。"""
    path = os.path.join(ROOT, ZH_MAP)
    if not os.path.isfile(path):
        return None
    out = {}
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            m = re.match("s/(.)/(..)/g$", line.rstrip("\n"))
            if m:
                out[m.group(1)] = m.group(2)
    return out


def _font_pua_set():
    """读 PSF1 的 unicode 表，返回字模实际提供的中文码位集合。"""
    path = os.path.join(ROOT, ZH_FONT)
    if not os.path.isfile(path):
        return None
    with open(path, "rb") as fh:
        raw = fh.read()
    if len(raw) < 4 or raw[0] != 0x36 or raw[1] != 0x04:
        return None
    mode, charsize = raw[2], raw[3]
    count = 512 if (mode & 0x01) else 256
    off = 4 + count * charsize
    vals = set()
    while off + 2 <= len(raw):
        v = raw[off] | (raw[off + 1] << 8)
        off += 2
        if v != 0xFFFF:
            vals.add(v)
    return vals


def console_ascii_violations():
    """C8：控制台提示的字符必须都在字模里，且一行放得下。

    规则：
      · 纯 ASCII 的 say 照旧（诊断行，日志断言依赖它们）。
      · 出现汉字 = 中文界面句，必须满足：
          - 每个汉字都在 bootfiles/zjzhmap.sed 里（字模覆盖，不相干的字会显示成 ?）
          - 不许全角标点（字模没有，而且宽度会超过 2 列）
          - 一行 ≤ 78 列（中文 2 列、ASCII 1 列；不许靠内核折行，双宽折行会错位）
          - 不接受"半中半英但英文不在 ASCII 区"的怪字符
    """
    out = []
    cmap = _load_zh_map()
    pua = _font_pua_set()
    if cmap is None:
        out.append("缺少 %s（取模中文化必须先跑 tools/gen-rescue-font.py）" % ZH_MAP)
    if pua is None:
        out.append("缺少 %s（取模中文化必须先跑 tools/gen-rescue-font.py）" % ZH_FONT)
    if cmap is None or pua is None:
        return out
    if set(ord(c) for pair in cmap.values() for c in pair) - pua:
        out.append("%s 与 %s 不同步：映射里的码位在字模里找不到" % (ZH_MAP, ZH_FONT))
    for rel in PRODUCT:
        for i, line in enumerate(read_lines(rel), 1):
            if is_comment(line):
                continue
            m = SAY_RE.search(line)
            if not m:
                continue
            msg = m.group(1)
            if not CJK_RE.search(msg):
                if any(not (0x20 <= ord(c) <= 0x7E) for c in msg):
                    out.append("%s:%d: 非 ASCII 但也不含汉字: %r" % (rel, i, msg[:40]))
                continue
            cols = PREFIX_COLS + sum(_char_cols(c) for c in msg)
            if cols > CONSOLE_COLS:
                out.append("%s:%d: 控制台提示 %d 列 > %d 列，必须拆成多行独立 say: %s"
                           % (rel, i, cols, CONSOLE_COLS, msg[:50]))
            for c in msg:
                if FULLWIDTH_RE.match(c):
                    out.append("%s:%d: 全角标点 %r 不在字模里，请换半角: %s"
                               % (rel, i, c, msg[:40]))
                elif CJK_RE.match(c) and c not in cmap:
                    out.append("%s:%d: 汉字 %r 未被字模覆盖（先跑 tools/gen-rescue-font.py）"
                               % (rel, i, c))
    return out


def _first_line(lines, pred):
    for i, line in enumerate(lines, 1):
        if pred(line):
            return i
    return 0


def _function_body(lines, def_line):
    """从 1-based 的 def_line 起按花括号配对，返回 (起始索引, 结束索引) 的 0-based 切片。"""
    depth = 0
    for i in range(def_line - 1, len(lines)):
        line = lines[i]
        depth += line.count("{") - line.count("}")
        if i > def_line - 1 and depth <= 0:
            return lines[def_line:i]
    return []


def init_marker_violations():
    """C4：入口标记必须早于首次调用 zjrestore-lite.sh，且失败不阻塞启动。"""
    out = []
    lines = read_lines(INIT_PATH)
    def_line = _first_line(lines, lambda l: re.match(r"^mark_entry_started\s*\(\s*\)\s*\{", l))
    call_line = _first_line(lines, lambda l: re.match(r"^mark_entry_started\s*$", l))
    if not def_line:
        out.append("%s: 未定义 mark_entry_started()" % INIT_PATH)
        return out
    if not call_line:
        out.append("%s: 缺少顶层调用 mark_entry_started（定义后必须执行一次）" % INIT_PATH)
    restore_line = _first_line(
        lines, lambda l: "zjrestore-lite.sh" in l and not is_comment(l)
    )
    if call_line and restore_line and call_line > restore_line:
        out.append(
            "%s:%d: mark_entry_started 必须在首次调用 zjrestore-lite.sh(%d) 之前执行"
            % (INIT_PATH, call_line, restore_line)
        )
    body = _function_body(lines, def_line)
    if not body:
        out.append("%s: mark_entry_started() 函数体为空或花括号不配" % INIT_PATH)
    if not any(MARKER_FILE in l for l in body):
        out.append("%s: mark_entry_started() 必须写 %s" % (INIT_PATH, MARKER_FILE))
    for line in body:
        if is_comment(line):
            continue
        if re.search(r"(?:^|[|;&{(\s])exit(?:\s|$)", line):
            out.append("%s: mark_entry_started() 不得调用 exit（标记失败不能阻塞启动）" % INIT_PATH)
            break
    return out


def _cpio_members(path):
    """解析 newc cpio（gzip 压缩），返回 名称 -> 内容。"""
    with gzip.open(path, "rb") as f:
        data = f.read()
    out = {}
    i = 0
    while i + 110 <= len(data):
        hdr = data[i:i + 110]
        if hdr[:6] != b"070701":
            break
        try:
            fields = [int(hdr[6 + 8 * k:14 + 8 * k], 16) for k in range(13)]
        except ValueError:
            break
        namesize, filesize = fields[11], fields[6]
        i += 110
        name = data[i:i + namesize].split(b"\x00", 1)[0].decode("utf-8", "replace")
        i += namesize
        i += (-i) % 4
        out[name] = data[i:i + filesize]
        i += filesize
        i += (-i) % 4
        if name == "TRAILER!!!":
            break
    return out


def _normalize(data):
    if data.startswith(b"\xef\xbb\xbf"):
        data = data[3:]
    return data.replace(b"\r\n", b"\n")


def initramfs_mismatches():
    """C5：initramfs 内救援脚本必须与源码一致（防止安装出旧救援层）。"""
    out = []
    path = os.path.join(ROOT, INITRAMFS_PATH)
    if not os.path.isfile(path):
        return ["missing file: %s" % INITRAMFS_PATH]
    members = _cpio_members(path)
    for name, rel in sorted(INITRAMFS_MEMBERS.items()):
        if name not in members:
            out.append("%s: 缺少成员 %s" % (INITRAMFS_PATH, name))
            continue
        if not os.path.isfile(os.path.join(ROOT, rel)):
            out.append("missing file: %s" % rel)
            continue
        with open(os.path.join(ROOT, rel), "rb") as f:
            raw = f.read()
        if name in BINARY_MEMBERS:
            same = members[name] == raw
        else:
            same = _normalize(members[name]) == _normalize(raw)
        if not same:
            out.append(
                "%s: 成员 %s 与 %s 不一致（先重新打包 initramfs 再提交）"
                % (INITRAMFS_PATH, name, rel)
            )
    return out


def _payload_sources():
    out = []
    for rel in PAYLOAD_SOURCES:
        if os.path.isfile(os.path.join(ROOT, rel)):
            out.append(rel)
    adir = os.path.join(ROOT, "bootfiles", "alpine")
    for dirpath, _dirs, files in os.walk(adir):
        for fn in files:
            full = os.path.join(dirpath, fn)
            out.append(os.path.relpath(full, ROOT).replace(os.sep, "/"))
    return out


def payload_freshness_violations():
    """C6：源文件 mtime 晚于 initramfs = 载荷过期，报出具体文件。"""
    cpio = os.path.join(ROOT, INITRAMFS_PATH)
    if not os.path.isfile(cpio):
        return []
    built = os.path.getmtime(cpio)
    stale = []
    for rel in _payload_sources():
        if os.path.getmtime(os.path.join(ROOT, rel)) > built:
            stale.append(rel)
    if not stale:
        return []
    return [
        "%s 早于以下源文件（救援载荷过期，必须重建：make rescue-payload）: %s"
        % (INITRAMFS_PATH, ", ".join(sorted(stale)))
    ]


def payload_version_violations():
    """C7：initramfs 自带 zjrescue-version 必须等于 version.h 的版本。"""
    cpio = os.path.join(ROOT, INITRAMFS_PATH)
    vh = os.path.join(ROOT, VERSION_H)
    if not os.path.isfile(cpio) or not os.path.isfile(vh):
        return []
    with open(vh, encoding="utf-8") as f:
        m = re.search(r'SYSRECOVER_VERSION\s+"([^"]+)"', f.read())
    want = m.group(1) if m else ""
    members = _cpio_members(cpio)
    got = members.get(VERSION_MEMBER, b"").decode("utf-8", "replace").strip()
    if want and got != want:
        return [
            "%s: %s=%r != version.h=%r（重建救援载荷）"
            % (INITRAMFS_PATH, VERSION_MEMBER, got, want)
        ]
    return []


def load_baseline():
    base = {}
    if not os.path.isfile(BASELINE_PATH):
        return base
    for line in open(BASELINE_PATH, encoding="utf-8"):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) >= 2 and parts[0].isdigit():
            base[" ".join(parts[1:])] = int(parts[0])
    return base


def write_baseline(counts):
    lines = [
        "# check-shell.py C2 棘轮基线（各文件 2>/dev/null 消音次数上限，纯注释行不计）",
        "# 只许降不许升；确需升级 = 先在 AGENTS/PIT 里完成 A/B 分类审查再更新本文件。",
        "# 格式：次数 路径",
        "",
    ]
    for rel in PRODUCT:
        lines.append("%d\t%s" % (counts[rel], rel))
    with open(BASELINE_PATH, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def main():
    update = "--update-baseline" in sys.argv
    # --payload-only：package 前置门禁用。跳过依赖 bash 的 C1–C4，只验
    # 载荷一致(C5)、载荷新鲜度(C6)、载荷版本戳(C7)。
    payload_only = "--payload-only" in sys.argv
    fail = []
    counts = {}

    bash = None
    if not payload_only:
        bash = find_bash()
        if not bash:
            print("check-shell: FAIL: bash not found (install Git for Windows)")
            return 1

    # C1 语法
    for rel in ([] if payload_only else PRODUCT + SYNTAX_ONLY):
        path = os.path.join(ROOT, rel)
        if not os.path.isfile(path):
            fail.append("missing file: %s" % rel)
            continue
        r = subprocess.run([bash, "-n", path], capture_output=True, text=True)
        if r.returncode != 0:
            fail.append("bash -n %s:\n%s%s" % (rel, r.stdout, r.stderr))

    if payload_only:
        fail.extend(initramfs_mismatches())
        fail.extend(payload_freshness_violations())
        fail.extend(payload_version_violations())
        fail.extend(console_ascii_violations())
        if fail:
            print("check-shell: FAIL (%d, payload-only)" % len(fail))
            for f in fail:
                print("  - " + f)
            return 1
        print("check-shell: PASS (payload-only; initramfs %d/%d; freshness ok; "
              "version stamp ok)" % (len(INITRAMFS_MEMBERS),
                                     len(INITRAMFS_MEMBERS)))
        return 0

    # C2 棘轮
    counts = {rel: count_token(rel) for rel in PRODUCT}
    if update:
        write_baseline(counts)
        print("check-shell: baseline updated -> tools/shell-stderr-baseline.txt")
    else:
        base = load_baseline()
        if not base:
            fail.append(
                "baseline missing/empty: run --update-baseline after classification review"
            )
        else:
            for rel in PRODUCT:
                cur = counts[rel]
                lim = base.get(rel)
                if lim is None:
                    fail.append("baseline has no entry for %s" % rel)
                elif cur > lim:
                    fail.append(
                        "%s: %d occurrences of %s > baseline %d "
                        "(new suppression must be classified: "
                        "document A/B rationale in a PIT, then --update-baseline)"
                        % (rel, cur, TOKEN, lim)
                    )

    # C3 禁消音
    for rel in PRODUCT:
        for lineno, cmd in deny_violations(rel):
            fail.append(
                "%s:%d: stderr of critical command '%s' must not be silenced (%s)"
                % (rel, lineno, cmd, TOKEN)
            )

    # C4 入口标记 / C5 载荷一致 / C6 新鲜度 / C7 版本戳 / C8 控制台 ASCII
    fail.extend(init_marker_violations())
    fail.extend(initramfs_mismatches())
    fail.extend(payload_freshness_violations())
    fail.extend(payload_version_violations())
    fail.extend(console_ascii_violations())

    if fail:
        print("check-shell: FAIL (%d)" % len(fail))
        for f in fail:
            print("  - " + f)
        return 1
    print(
        "check-shell: PASS (syntax %d ok; %s; deny-list %s clean; "
        "entry-marker ok; initramfs %d/%d)"
        % (
            len(PRODUCT) + len(SYNTAX_ONLY),
            ", ".join("%s=%d" % (os.path.basename(k), v) for k, v in counts.items()),
            len(DENY_CMDS),
            len(INITRAMFS_MEMBERS),
            len(INITRAMFS_MEMBERS),
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
