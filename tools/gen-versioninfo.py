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
    "gui":            ("SysRecoverUI",           "NineRestore"),
    "cli":            ("SysRecover",             "NineRestore CLI"),
    "check":          ("SysRecoverCheck",        "NineRestore Image Check"),
    "launcher-ui":    ("SysRecoverLauncherUI",   "NineRestore Launcher"),
    "launcher-cli":   ("SysRecoverLauncherCLI",  "NineRestore CLI Launcher"),
    "launcher-check": ("SysRecoverLauncherCheck", "NineRestore Check Entry"),
    "ea":             ("zj-ea-apply",            "NineRestore EA Writer"),
    "boot":           ("zj-restore-boot",        "NineRestore Boot Entry Cleanup"),
    "pstub":          ("portable-stub",          "NineRestore Portable (CLI)"),
    "pstub-gui":      ("portable-stub-gui",      "NineRestore Portable"),
}


def grab(pattern: str, vh: str, name: str) -> str:
    m = re.search(pattern, vh)
    if not m:
        sys.exit("gen-versioninfo: version.h 里找不到 %s" % name)
    return m.group(1)


def numeric(major: int, minor: int, patch: int, beta: int) -> tuple[int, int, int, int]:
    """内部四段：beta 1.0.0.2；正式 1.0.0.0；正式之后 1.0.1.0。"""
    return (major, minor, patch, beta)


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
    major = int(grab(r'#define\s+SYSRECOVER_MAJOR\s+(\d+)', vh, "MAJOR"))
    minor = int(grab(r'#define\s+SYSRECOVER_MINOR\s+(\d+)', vh, "MINOR"))
    patch = int(grab(r'#define\s+SYSRECOVER_PATCH\s+(\d+)', vh, "PATCH"))
    beta = int(grab(r'#define\s+SYSRECOVER_BETA\s+(\d+)', vh, "BETA"))
    m_rc = re.search(r'#define\s+SYSRECOVER_RC\s+(\d+)', vh)
    rc = int(m_rc.group(1)) if m_rc else 0
    ver = grab(r'SYSRECOVER_VERSION\s+"([^"]+)"', vh, "SYSRECOVER_VERSION")
    display = grab(r'SYSRECOVER_DISPLAY\s+"([^"]+)"', vh, "SYSRECOVER_DISPLAY")
    company = grab(r'SYSRECOVER_COMPANY\s+"([^"]*)"', vh, "SYSRECOVER_COMPANY")
    copyright_ = grab(r'SYSRECOVER_COPYRIGHT\s+"([^"]*)"', vh, "SYSRECOVER_COPYRIGHT")
    product = grab(r'SYSRECOVER_PRODUCT\s+"([^"]*)"', vh, "SYSRECOVER_PRODUCT")
    n = numeric(major, minor, patch, beta)
    # 属性页字符串用显示串（beta 带 beta 字样）；数字字段是纯四段。
    # 文件描述带上渠道文字，方便用户一眼区分测试版/正式版。
    if rc:
        desc = desc + " (RC%d)" % rc
    elif beta:
        desc = desc + " (Beta)"

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
