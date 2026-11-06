#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check-versioninfo.py -- exe 版本资源门禁（接 `make check` 与 `make dist-check`）。

用户规格（2026-11-06）：右键 exe → 属性 → 详细信息 必须能读到
产品名（九转还原）/ 版本 / 公司（DreamGrain）/ 版权。
**缺失资源或不一致即失败** —— 防止以后换构建方式时又把 VERSIONINFO 悄悄丢掉。

做法：用 PowerShell 的 `(Get-Item x).VersionInfo` 真读 exe（不解析 PE 字节，
与用户在资源管理器里看到的是同一份数据），与本文件同目录的 version.h 比对。

用法：
  python tools/check-versioninfo.py <exe> [<exe> ...]
"""
from __future__ import annotations

import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FIELDS = ["ProductName", "ProductVersion", "FileVersion", "CompanyName",
          "LegalCopyright", "FileDescription", "OriginalFilename", "InternalName"]


def expected() -> dict:
    vh = (ROOT / "src" / "common" / "version.h").read_text(encoding="utf-8")

    def grab(pat: str, name: str) -> str:
        m = re.search(pat, vh)
        if not m:
            sys.exit("check-versioninfo: version.h 里找不到 %s" % name)
        return m.group(1)

    return {
        "ProductName": grab(r'SYSRECOVER_PRODUCT\s+"([^"]*)"', "SYSRECOVER_PRODUCT"),
        "ProductVersion": grab(r'SYSRECOVER_VERSION\s+"([^"]+)"', "SYSRECOVER_VERSION"),
        "FileVersion": grab(r'SYSRECOVER_VERSION\s+"([^"]+)"', "SYSRECOVER_VERSION"),
        "CompanyName": grab(r'SYSRECOVER_COMPANY\s+"([^"]*)"', "SYSRECOVER_COMPANY"),
        "LegalCopyright": grab(r'SYSRECOVER_COPYRIGHT\s+"([^"]*)"', "SYSRECOVER_COPYRIGHT"),
    }


def read_version_info(paths: list[Path]) -> dict:
    """→ {path: {field: value}}；PS 不可用或读失败即抛 RuntimeError。"""
    if not paths:
        return {}
    tmp = Path(tempfile.mkdtemp(prefix="zjvi-")) / "vi.json"
    quoted = ",".join("'" + str(p.resolve()).replace("'", "''") + "'" for p in paths)
    script = (
        "$r=@();"
        "foreach($p in @(%s)){"
        "$v=(Get-Item -LiteralPath $p).VersionInfo;"
        "$r+=[PSCustomObject]@{path=$p;ProductName=$v.ProductName;"
        "ProductVersion=$v.ProductVersion;FileVersion=$v.FileVersion;"
        "CompanyName=$v.CompanyName;LegalCopyright=$v.LegalCopyright;"
        "FileDescription=$v.FileDescription;OriginalFilename=$v.OriginalFilename;"
        "InternalName=$v.InternalName}};"
        "$r | ConvertTo-Json -Compress | Set-Content -LiteralPath '%s' -Encoding UTF8"
        % (quoted, str(tmp).replace("'", "''"))
    )
    r = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                        "-Command", script],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if r.returncode != 0 or not tmp.is_file():
        raise RuntimeError("PowerShell 读取 VersionInfo 失败: %s"
                           % r.stderr.decode("utf-8", "replace")[:400])
    raw = json.loads(tmp.read_text(encoding="utf-8-sig"))
    if isinstance(raw, dict):
        raw = [raw]
    return {x["path"].lower(): x for x in raw}


def check(paths: list[Path], strict_name: bool = True) -> list[str]:
    exp = expected()
    info = read_version_info(paths)
    bad = []
    for p in paths:
        got = info.get(str(p.resolve()).lower())
        if not got:
            bad.append("%s: 读不到 VersionInfo" % p)
            continue
        for f, want in exp.items():
            if (got.get(f) or "").strip() != want:
                bad.append("%s: %s=%r != %r" % (p, f, got.get(f), want))
        # 描述/内部名/原始文件名不能空（有资源但字段缺，资源管理器同样显示空白）
        for f in ("FileDescription", "OriginalFilename", "InternalName"):
            if not (got.get(f) or "").strip():
                bad.append("%s: %s 为空" % (p, f))
        # 单文件外层是 stub + 追加载荷：OriginalFilename 天然是 stub 的名字，
        # 与最终的 SysRecover-Portable*.exe 不同 —— 这类只要求字段非空。
        if strict_name and (got.get("OriginalFilename") or "").strip() != p.name:
            bad.append("%s: OriginalFilename=%r 与文件名不符"
                       % (p, got.get("OriginalFilename")))
    return bad


def main() -> int:
    if len(sys.argv) < 2:
        print("check-versioninfo: 需要至少一个 exe 路径")
        return 1
    paths = [Path(x) for x in sys.argv[1:]]
    missing = [str(p) for p in paths if not p.is_file()]
    bad = ["missing " + x for x in missing]
    existing = [p for p in paths if p.is_file()]
    try:
        bad += check(existing)
    except RuntimeError as e:
        bad.append(str(e))
    if bad:
        print("check-versioninfo: FAIL")
        for x in bad:
            print("  - " + x)
        return 1
    print("check-versioninfo: PASS (%d exe, version=%s)"
          % (len(existing), expected()["ProductVersion"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
