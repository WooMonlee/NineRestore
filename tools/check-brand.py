#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check-brand.py — 启动项品牌名门禁（PIT-246）

背景（2026-11-08 真机）：同一台机器先后出现过 3 个引导项名字：
  · SysRecover                —— 早期版本写入的旧品牌名（残留）
  · NineRestore               —— beta.14 起 UEFI 项名 / menu.lst title
  · NineRestore Recovery Environment —— BCD description（bootmgr 显示成
                                          "NineRestore Recover Ev.."）
原因是 UEFI 安装时 FindOurEntry 只按"当前描述名"匹配：升级后旧名字的条目
找不到 → 新写一条 → 新旧并存，且都指向同一份 ESP 载荷。清理时同理只删一条。

规则：
  B1  bootnames.h 是唯一名字来源：
        kMenuEntryDesc == kMenuBcdDesc == kMenuEntryDescAnsi == "NineRestore"
        临时项 kTempEntryDesc 必须以 "NineRestore" 开头（清理靠它区分常驻项）。
  B2  GRUB 写菜单必须引用 kMenuEntryDescAnsi，不得再手写 title。
  B3  对外可见面（C++ 字符串字面量 / bootfiles 非注释行 / lang / README）
      不得出现旧品牌名。历史名只允许留在 bootnames.h 的 LEGACY 列表，
      以及带 "LEGACY" 注释的行（升级识别/清理用，不生成、不显示）。
  B4  init 必须同时识别旧的一次性项名字（升级清理），并保留 ZJRESTORE 路径判据。

用法：python tools/check-brand.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

BRAND = "NineRestore"
FORBIDDEN = [
    "SysRecovery Environment",
    "SysRecover Temporary Restore",
    "NineRestore Recovery Environment",
    "title SysRecover",
    "`SysRecover`",  # README 等交付文档里的"引导项"写法（exe 名不算）
]
SRC_DIRS = ["src"]
BOOT_DIRS = ["bootfiles"]
EXTRA_FILES = ["README.md", "README.en.md", "resources/README.txt"]  # 不存在则跳过
LANG_DIR = "lang"


def read(rel):
    path = os.path.join(ROOT, rel)
    with open(path, "rb") as f:
        raw = f.read()
    if raw.startswith(b"\xef\xbb\xbf"):
        raw = raw[3:]
    return raw.decode("utf-8", errors="replace")


def walk(rel_dir, exts):
    out = []
    base = os.path.join(ROOT, rel_dir)
    for dirpath, _dirs, files in os.walk(base):
        for fn in files:
            if os.path.splitext(fn)[1].lower() in exts:
                out.append(os.path.relpath(os.path.join(dirpath, fn), ROOT))
    return out


def cpp_string_literals(text):
    """粗略提取 C/C++ 的双引号字符串（含 L"..."），跳过 // 与 /* */ 注释。"""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            i = text.find("\n", i)
            if i < 0:
                break
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
        elif c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            out.append(text[i + 1:j])
            i = j + 1
        else:
            i += 1
    return out


def check_names(fail):
    rel = "src/boot/bootnames.h"
    if not os.path.isfile(os.path.join(ROOT, rel)):
        fail.append("missing file: " + rel)
        return
    text = read(rel)

    def grab(name):
        m = re.search(name + r"\s*\[\]\s*=\s*L\"([^\"]+)\"", text)
        return m.group(1) if m else None

    entry = grab("kMenuEntryDesc")
    bcd = grab("kMenuBcdDesc")
    temp = grab("kTempEntryDesc")
    m = re.search(r"kMenuEntryDescAnsi\s*\[\]\s*=\s*\"([^\"]+)\"", text)
    ansi = m.group(1) if m else None
    if entry != BRAND:
        fail.append("%s: kMenuEntryDesc=%r，应统一为 %r" % (rel, entry, BRAND))
    if bcd != BRAND:
        fail.append("%s: kMenuBcdDesc=%r，BCD 描述也必须统一为 %r"
                    % (rel, bcd, BRAND))
    if ansi != BRAND:
        fail.append("%s: kMenuEntryDescAnsi=%r，GRUB title 也必须为 %r"
                    % (rel, ansi, BRAND))
    if not temp or not temp.startswith(BRAND):
        fail.append("%s: kTempEntryDesc=%r 必须以 %r 开头（清理靠它区分常驻项）"
                    % (rel, temp, BRAND))

    grub = read("src/boot/grub.cpp")
    if "kMenuEntryDescAnsi" not in grub:
        fail.append("src/boot/grub.cpp: 写 menu.lst 必须引用 kMenuEntryDescAnsi"
                    "（禁止再手写 title，BIOS/UEFI 不能各写一套）")


def scan_strings(fail, rel, strings):
    for s in strings:
        for bad in FORBIDDEN:
            if bad in s:
                fail.append("%s: 对外可见字符串仍含旧品牌名 %r（改用 bootnames.h 的 %r）"
                            % (rel, bad, BRAND))


def check_visible(fail):
    for rel in walk("src", {".cpp", ".h", ".hpp"}):
        if rel.replace("\\", "/") == "src/boot/bootnames.h":
            continue  # 唯一的名字来源；旧名字只允许留在这里的 LEGACY 列表
        scan_strings(fail, rel, cpp_string_literals(read(rel)))
    for rel in walk("bootfiles", {".sh", ".cfg", ".lst", ".txt", ".md"}):
        text = read(rel).splitlines()
        for i, line in enumerate(text, 1):
            if line.lstrip().startswith("#"):
                continue
            for bad in FORBIDDEN:
                if bad in line:
                    fail.append("%s:%d: 运行时文件仍写旧品牌名 %r（历史名只允许"
                                "带 LEGACY 注释用于识别残留）" % (rel, i, bad))
    base = os.path.join(ROOT, LANG_DIR)
    if os.path.isdir(base):
        for fn in os.listdir(base):
            if fn.endswith(".lang"):
                scan_strings(fail, LANG_DIR + "/" + fn, [read(LANG_DIR + "/" + fn)])
    for rel in EXTRA_FILES:
        if os.path.isfile(os.path.join(ROOT, rel)):
            for i, line in enumerate(read(rel).splitlines(), 1):
                for bad in FORBIDDEN:
                    if bad in line:
                        fail.append("%s:%d: 交付文档仍含旧品牌名 %r" % (rel, i, bad))


def check_init(fail):
    rel = "bootfiles/alpine/init"
    text = read(rel)
    if "NineRestore Temporary" not in text and "4e0069006e006500" not in text:
        fail.append("%s: 一次性项识别未覆盖 %r（升级后 disarm 会失败）"
                    % (rel, "NineRestore Temporary Restore"))
    if "5300790073005200650063006f007600650072" not in text:
        fail.append("%s: 一次性项识别必须保留旧名 LEGACY 匹配（升级清理）" % rel)


def main():
    fail = []
    check_names(fail)
    check_visible(fail)
    check_init(fail)
    if fail:
        print("check-brand: FAIL (%d)" % len(fail))
        for f in fail:
            print("  - " + f)
        return 1
    print("check-brand: PASS (name=%r; UEFI/BCD/GRUB 同源；无旧品牌可见名)" % BRAND)
    return 0


if __name__ == "__main__":
    sys.exit(main())
