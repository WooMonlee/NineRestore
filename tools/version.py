#!/usr/bin/env python3
"""SysRecover 版本号工具 —— `src/common/version.h` 是唯一来源，本脚本负责读写它。

规则（2026-11-07 用户定稿，见 docs/37-版本命名规范.md）：
  主版本.次版本 由人指定（当前 1.0）；
  测试版：beta 号在**第四段**，每发一版 +1，第三位固定 0；
          `1.0 beta.N` / 内部 `1.0.0.N` / 文件 `九转大还原1.0-beta.N.exe`
  正式版：beta=0 → `1.0.0` / `1.0.0.0` / `九转大还原1.0.0.exe`
  正式版之后：第三位递增 → `1.0.1` / `1.0.1.0` / `九转大还原1.0.1.exe`
版本判定按文件名与渠道，不靠数字大小比较（正式版四段会回落）。

用法：
  python tools/version.py              # 打印内部四段（Makefile 生成 version.json）
  python tools/version.py --display    # 打印界面显示串
  python tools/version.py --filename   # 打印发布文件名（不含目录）
  python tools/version.py --bump       # beta 号 +1（1.0 beta.2 → 1.0 beta.3）
  python tools/version.py --release    # 切正式版：beta=0（1.0.0 / 1.0.0.0）
  python tools/version.py --beta N     # 切回 beta 渠道并指定 beta 号
  python tools/version.py --patch N    # 正式版之后：第三位指定为 N（beta=0）
  python tools/version.py --check      # 只校验一致性
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VERSION_H = ROOT / "src" / "common" / "version.h"
PRODUCT_ZH = "九转大还原"  # 发布文件名前缀（用户 2026-11 指定，不带空格）

MACROS = {
    "SYSRECOVER_MAJOR": r"#define\s+SYSRECOVER_MAJOR\s+(\d+)",
    "SYSRECOVER_MINOR": r"#define\s+SYSRECOVER_MINOR\s+(\d+)",
    "SYSRECOVER_PATCH": r"#define\s+SYSRECOVER_PATCH\s+(\d+)",
    "SYSRECOVER_BETA": r"#define\s+SYSRECOVER_BETA\s+(\d+)",
    "SYSRECOVER_VERSION": r'#define\s+SYSRECOVER_VERSION\s+"([^"]+)"',
    "SYSRECOVER_DISPLAY": r'#define\s+SYSRECOVER_DISPLAY\s+"([^"]+)"',
}
NUM_KEYS = ("SYSRECOVER_MAJOR", "SYSRECOVER_MINOR", "SYSRECOVER_PATCH",
            "SYSRECOVER_BETA")


def read_macros(path: Path = VERSION_H) -> dict:
    text = Path(path).read_text(encoding="utf-8")
    out = {}
    for name, pat in MACROS.items():
        m = re.search(pat, text)
        if not m:
            sys.exit("ERROR: 在 %s 里找不到 %s" % (path, name))
        out[name] = int(m.group(1)) if name in NUM_KEYS else m.group(1)
    return out


def derive(m: dict) -> dict:
    """由四个字段派生 内部四段 / 显示串 / 文件名，并校验一致性。"""
    major = int(m["SYSRECOVER_MAJOR"])
    minor = int(m["SYSRECOVER_MINOR"])
    patch = int(m["SYSRECOVER_PATCH"])
    beta = int(m["SYSRECOVER_BETA"])
    internal = "%d.%d.%d.%d" % (major, minor, patch, beta)
    if beta:
        display = "%d.%d beta.%d" % (major, minor, beta)
        filename = "%s%d.%d-beta.%d.exe" % (PRODUCT_ZH, major, minor, beta)
    else:
        display = "%d.%d.%d" % (major, minor, patch)
        filename = "%s%d.%d.%d.exe" % (PRODUCT_ZH, major, minor, patch)
    return {"internal": internal, "display": display, "filename": filename,
            "beta": beta}


def write_macros(m: dict, path: Path = VERSION_H) -> None:
    path = Path(path)
    text = path.read_text(encoding="utf-8")
    for name in NUM_KEYS:
        text, n = re.subn(MACROS[name],
                          "#define %s %d" % (name, int(m[name])), text, count=1)
        if n != 1:
            sys.exit("ERROR: 写入 %s 失败（%s）" % (path, name))
    d = derive(m)
    for name, val in (("SYSRECOVER_VERSION", d["internal"]),
                      ("SYSRECOVER_DISPLAY", d["display"])):
        text, n = re.subn(MACROS[name],
                          '#define %s "%s"' % (name, val), text, count=1)
        if n != 1:
            sys.exit("ERROR: 写入 %s 失败（%s）" % (path, name))
    # 必须用 \n（C++ 源文件；Windows 文本模式会写成 \r\n）
    path.write_text(text, encoding="utf-8", newline="\n")


def check(m: dict) -> list[str]:
    d = derive(m)
    bad = []
    if m["SYSRECOVER_VERSION"] != d["internal"]:
        bad.append("SYSRECOVER_VERSION=%r != %r"
                   % (m["SYSRECOVER_VERSION"], d["internal"]))
    if m["SYSRECOVER_DISPLAY"] != d["display"]:
        bad.append("SYSRECOVER_DISPLAY=%r != %r"
                   % (m["SYSRECOVER_DISPLAY"], d["display"]))
    if int(m["SYSRECOVER_BETA"]) and int(m["SYSRECOVER_PATCH"]):
        bad.append("beta 期间第三位必须为 0（beta 号在第四段）")
    if int(m["SYSRECOVER_BETA"]) > 999:
        bad.append("beta 号必须在 0..999（能进 16 位数字段）")
    return bad


def main() -> None:
    ap = argparse.ArgumentParser(description="SysRecover 版本号工具")
    ap.add_argument("--bump", action="store_true", help="beta 号 +1")
    ap.add_argument("--release", action="store_true", help="切正式版（beta=0）")
    ap.add_argument("--beta", type=int, metavar="N", help="切回 beta 渠道并指定 beta 号")
    ap.add_argument("--patch", type=int, metavar="N",
                    help="正式版之后：第三位指定为 N（beta=0）")
    ap.add_argument("--display", action="store_true", help="打印界面显示串")
    ap.add_argument("--filename", action="store_true", help="打印发布文件名")
    ap.add_argument("--check", action="store_true", help="只校验一致性")
    a = ap.parse_args()

    m = read_macros()
    if a.check:
        bad = check(m)
        if bad:
            sys.exit("ERROR: 版本字段不一致:\n  - " + "\n  - ".join(bad))
        print("OK %s (%s)" % (m["SYSRECOVER_VERSION"], m["SYSRECOVER_DISPLAY"]))
        return
    if a.display:
        print(derive(m)["display"])
        return
    if a.filename:
        print(derive(m)["filename"])
        return
    if a.patch is not None:
        m["SYSRECOVER_PATCH"] = a.patch
        m["SYSRECOVER_BETA"] = 0
        write_macros(m)
        print(derive(m)["filename"])
        return
    if a.release:
        m["SYSRECOVER_BETA"] = 0
        write_macros(m)
        print(derive(m)["filename"])
        return
    if a.beta is not None:
        if int(m["SYSRECOVER_PATCH"]) != 0:
            sys.exit("ERROR: 切回 beta 前请先把第三位归零（--patch 0 或手工改）")
        m["SYSRECOVER_BETA"] = a.beta
        write_macros(m)
        print(derive(m)["filename"])
        return
    if a.bump:
        if not int(m["SYSRECOVER_BETA"]):
            sys.exit("ERROR: 当前是正式版渠道，beta 号不存在；先 --beta N")
        m["SYSRECOVER_BETA"] = int(m["SYSRECOVER_BETA"]) + 1
        write_macros(m)
        print(derive(m)["filename"])
        return

    bad = check(m)
    if bad:
        sys.exit("ERROR: 版本字段不一致:\n  - " + "\n  - ".join(bad))
    print(m["SYSRECOVER_VERSION"])


if __name__ == "__main__":
    main()
