#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check-dist.py -- 发布目录制品门禁（零依赖）。

检查 `make package` 后的 dist/：
  * 必需文件齐全（双架构 exe、救援资产、语言/皮肤/版本）；
  * dist/version.json == src/common/version.h；
  * 不应出现运行期 logs/、*.tmp、*.dmp；
  * 若存在 SHA256SUMS.txt，逐条重算校验。

用法：python tools/check-dist.py [dist]
"""
from __future__ import annotations

import hashlib
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REQUIRED = [
    "SysRecover.exe", "SysRecoverUI.exe", "SysRecoverCheck.exe",
    "x64/SysRecover.exe", "x64/SysRecoverUI.exe", "x64/SysRecoverCheck.exe",
    "SysRecover_x86.exe", "SysRecoverUI_x86.exe", "SysRecoverCheck_x86.exe",
    "bootfiles/vmlinuz-zjrestore",
    "bootfiles/initramfs-zjrestore.cpio.gz",
    "bootfiles/zjrestore-lite.sh",
    "bootfiles/zjlog.sh",
    "bootfiles/zj-ea-apply.exe",
    "bootfiles/zj-restore-boot.exe",
    # 自足引导资产（2026-10-27，用户反馈 2：极简 PE 无 bootsect/bcdboot/bcdedit）
    "bootfiles/mbr-boot-code.bin",
    "bootfiles/ntfs-boot-code.bin",
    "bootfiles/ntfs-boot-cont.bin",
    "bootfiles/bcd/bios.bcd",
    "bootfiles/bcd/uefi.bcd",
    "lang/en.lang", "skin/main.xml",
    "resources/themes/default/global.xml",
    "version.json",
]


def sha256(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def main() -> int:
    dist = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "dist"
    bad = []
    if not dist.is_dir():
        print("check-dist: FAIL missing %s" % dist)
        return 1
    for rel in REQUIRED:
        if not (dist / rel).is_file():
            bad.append("missing " + rel)
    vh = (ROOT / "src" / "common" / "version.h").read_text(encoding="utf-8")
    m = re.search(r'SYSRECOVER_VERSION\s+"([^"]+)"', vh)
    want = m.group(1) if m else ""
    vj = dist / "version.json"
    if vj.is_file():
        try:
            got = json.loads(vj.read_text(encoding="utf-8")).get("version")
        except Exception as e:
            got = None
            bad.append("version.json parse: %s" % e)
        if want and got != want:
            bad.append("version.json=%r != version.h=%r" % (got, want))
    if (dist / "logs").exists():
        bad.append("runtime directory present: logs/")
    # 版本资源（用户 2026-11-06）：每个**会被分发**的 exe 都要能在资源管理器
    # 「属性 → 详细信息」里读到 产品名/版本/公司/版权；单文件最外层 exe 也一样
    # （用户右键看到的就是 stub 这一层，内层 PE 运行时才暴露）。
    try:
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            "check_versioninfo", str(ROOT / "tools" / "check-versioninfo.py"))
        vi = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(vi)
        exes = [dist / rel for rel in REQUIRED if rel.lower().endswith(".exe")]
        bad += vi.check(exes)
        # 单文件外层（stub + 追加载荷）：同样要有完整字段，但 OriginalFilename
        # 天然是 stub 名，不做「与文件名一致」的严格比对。
        singles = [dist / rel for rel in ("SysRecover-Portable.exe",
                                          "SysRecover-Portable-CLI.exe")
                   if (dist / rel).is_file()]
        bad += vi.check(singles, strict_name=False)
        for rel in ("bootfiles/portable-stub.exe", "bootfiles/portable-stub-gui.exe"):
            if (dist / rel).is_file():
                bad += vi.check([dist / rel])
    except Exception as e:  # 导入失败也要算失败，不能静默跳过
        bad.append("VersionInfo 校验异常: %s" % e)
    for p in dist.rglob("*"):
        if p.is_file() and (p.name.endswith(".tmp") or p.suffix.lower() == ".dmp"):
            bad.append("runtime artifact: " + str(p.relative_to(dist)))
    sums = dist / "SHA256SUMS.txt"
    if sums.is_file():
        for line in sums.read_text(encoding="utf-8").splitlines():
            m = re.match(r"^([0-9a-fA-F]{64})  (.+)$", line)
            if not m:
                continue
            p = dist / m.group(2)
            if not p.is_file():
                bad.append("SHA256SUMS missing " + m.group(2))
            elif sha256(p) != m.group(1).lower():
                bad.append("SHA256 mismatch " + m.group(2))
    if bad:
        print("check-dist: FAIL")
        for x in bad:
            print("  - " + x)
        return 1
    print("check-dist: PASS (%s)" % dist)
    return 0


if __name__ == "__main__":
    sys.exit(main())
