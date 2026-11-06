#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen-versioninfo.py -- 从 src/common/version.h 生成 Windows VERSIONINFO 资源脚本。

**单一来源**：版本串、公司名、版权行都从 version.h 读，不在 .rc 里手写数字，
否则以后必然漂移。生成后的 .rc 交给各架构的 windres 编译，再链进对应 exe。

用法：
  python tools/gen-versioninfo.py --out build/versioninfo/SysRecover.rc \
      --profile cli --original SysRecover.exe

  --profile 选择「内部名 + 文件描述」的组合（描述里的中文写在本脚本里，走 UTF-8
  文件而不是命令行参数 —— PowerShell 5.1 传非 ASCII 参数会按 ANSI 转码，实测
  会把「九转还原 镜像检查」变成乱码；ProductName 直接读 version.h 所以没事）。
  --original 是实际输出文件名（x86 版是 SysRecoverUI_x86.exe 这种，随架构变）。

字段（用户 2026-11-06 规格，逐项都在）：
  FileDescription / FileVersion / ProductName（九转还原）/ ProductVersion /
  CompanyName / LegalCopyright / OriginalFilename / InternalName
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VERSION_H = ROOT / "src" / "common" / "version.h"

# profile -> (InternalName, FileDescription)
PROFILES = {
    "gui":            ("SysRecoverUI",           "九转还原"),
    "cli":            ("SysRecover",             "九转还原 命令行"),
    "check":          ("SysRecoverCheck",        "九转还原 镜像检查"),
    "launcher-ui":    ("SysRecoverLauncherUI",   "九转还原 启动器"),
    "launcher-cli":   ("SysRecoverLauncherCLI",  "九转还原 命令行启动器"),
    "launcher-check": ("SysRecoverLauncherCheck", "九转还原 检查入口"),
    "ea":             ("zj-ea-apply",            "九转还原 EA 补写器"),
    "boot":           ("zj-restore-boot",        "九转还原 启动项清理"),
    "pstub":          ("portable-stub",          "九转还原 单文件版（命令行）"),
    "pstub-gui":      ("portable-stub-gui",      "九转还原 单文件版"),
}


def grab(pattern: str, vh: str, name: str) -> str:
    m = re.search(pattern, vh)
    if not m:
        sys.exit("gen-versioninfo: version.h 里找不到 %s" % name)
    return m.group(1)


def numeric(version: str) -> tuple[int, int, int, int]:
    """1.0.0-beta.1 → (1,0,0,0)。预发布后缀无法放进 16 位数字段，只保留主.次.修订。"""
    base = version.split("-", 1)[0]
    parts = [int(x) for x in base.split(".")]
    while len(parts) < 3:
        parts.append(0)
    return (parts[0], parts[1], parts[2], 0)


def main() -> int:
    args = sys.argv[1:]
    out = None
    profile = None
    original = None
    i = 0
    while i < len(args):
        if args[i] == "--out" and i + 1 < len(args):
            out = args[i + 1]; i += 2
        elif args[i] == "--profile" and i + 1 < len(args):
            profile = args[i + 1]; i += 2
        elif args[i] == "--original" and i + 1 < len(args):
            original = args[i + 1]; i += 2
        else:
            sys.exit("gen-versioninfo: 未知参数 %r" % args[i])
    if not (out and profile and original):
        sys.exit("gen-versioninfo: 需要 --out / --profile / --original")
    if profile not in PROFILES:
        sys.exit("gen-versioninfo: 未知 profile %r（可选：%s）"
                 % (profile, ", ".join(sorted(PROFILES))))
    internal, desc = PROFILES[profile]

    vh = VERSION_H.read_text(encoding="utf-8")
    ver = grab(r'SYSRECOVER_VERSION\s+"([^"]+)"', vh, "SYSRECOVER_VERSION")
    company = grab(r'SYSRECOVER_COMPANY\s+"([^"]*)"', vh, "SYSRECOVER_COMPANY")
    copyright_ = grab(r'SYSRECOVER_COPYRIGHT\s+"([^"]*)"', vh, "SYSRECOVER_COPYRIGHT")
    product = grab(r'SYSRECOVER_PRODUCT\s+"([^"]*)"', vh, "SYSRECOVER_PRODUCT")
    n = numeric(ver)

    def esc(s: str) -> str:
        return s.replace("\\", "\\\\").replace('"', '\\"')

    body = f"""// 本文件由 tools/gen-versioninfo.py 自动生成，请勿手改（改 version.h）。
#include <windows.h>
#pragma code_page(65001)

1 VERSIONINFO
 FILEVERSION {n[0]},{n[1]},{n[2]},{n[3]}
 PRODUCTVERSION {n[0]},{n[1]},{n[2]},{n[3]}
 FILEFLAGSMASK 0x3fL
 FILEFLAGS 0x0L
 FILEOS 0x40004L
 FILETYPE 0x1L
 FILESUBTYPE 0x0L
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904B0"
    BEGIN
      VALUE "CompanyName", "{esc(company)}"
      VALUE "FileDescription", "{esc(desc)}"
      VALUE "FileVersion", "{esc(ver)}"
      VALUE "InternalName", "{esc(internal)}"
      VALUE "LegalCopyright", "{esc(copyright_)}"
      VALUE "OriginalFilename", "{esc(original)}"
      VALUE "ProductName", "{esc(product)}"
      VALUE "ProductVersion", "{esc(ver)}"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x409, 1200
  END
END
"""
    outp = Path(out)
    outp.parent.mkdir(parents=True, exist_ok=True)
    # 用 UTF-8（无 BOM）写；BOM 会让 windres 把首行当垃圾。
    outp.write_text(body, encoding="utf-8", newline="\n")
    print("gen-versioninfo: %s (version=%s)" % (outp, ver))
    return 0


if __name__ == "__main__":
    sys.exit(main())
