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
    # 载荷自报版本（N18，2026-11-07）：与 version.json / initramfs 同源
    "bootfiles/rescue-build.txt",
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


# dist/bootfiles 里这些资产必须与仓库 bootfiles/ 逐字节一致。
# 背景（N10，2026-11-06 实机事故）：`make all` 只重建 exe，不刷新 dist 资源，
# 于是 dist 里装的是 11-02 的旧救援载荷（zjswap.sh 旧措辞 + 旧 initramfs），
# 实机“解压并安装”因此复现 SetupCl 0x8007139F 死循环。只跑 all 后直接装机 = 拿旧代码验新版。
MIRRORED_FROM_BOOTFILES = [
    "bootfiles/vmlinuz-zjrestore",
    "bootfiles/initramfs-zjrestore.cpio.gz",
    "bootfiles/zjrestore-lite.sh",
    "bootfiles/zjlog.sh",
    "bootfiles/zj-ea-apply.exe",
    "bootfiles/zj-restore-boot.exe",
    "bootfiles/mbr-boot-code.bin",
    "bootfiles/ntfs-boot-code.bin",
    "bootfiles/ntfs-boot-cont.bin",
    "bootfiles/bcd/bios.bcd",
    "bootfiles/bcd/uefi.bcd",
    "bootfiles/grldr",
    "bootfiles/grldr.mbr",
]


def sha256(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def _cpio_member(path: Path, name: str) -> str | None:
    """从 gzip 的 newc cpio 里取一个成员（只用于读 zjrescue-version 小文件）。"""
    import gzip
    data = gzip.decompress(path.read_bytes())
    off = 0
    while off + 110 <= len(data):
        hdr = data[off:off + 110]
        if hdr[:6] != b"070701":
            break
        def field(i: int) -> int:
            return int(hdr[6 + 8 * i:14 + 8 * i], 16)
        size = field(6)
        namesize = field(11)
        nstart = off + 110
        fname = data[nstart:nstart + namesize].split(b"\0")[0].decode(
            "utf-8", "replace")
        # newc：header(110)+name 一起按 4 字节对齐；数据后再按 4 对齐
        dstart = nstart + namesize + (-(110 + namesize)) % 4
        if fname == name:
            return data[dstart:dstart + size].decode("utf-8", "replace")
        off = dstart + size + (-size % 4)
    return None


def main() -> int:
    dist = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "dist"
    bad = []
    if not dist.is_dir():
        print("check-dist: FAIL missing %s" % dist)
        return 1
    for rel in REQUIRED:
        if not (dist / rel).is_file():
            bad.append("missing " + rel)
    # C6：dist 救援资产必须与仓库源逐字节一致（否则装机验的是旧代码）
    for rel in MIRRORED_FROM_BOOTFILES:
        src = ROOT / rel
        dst = dist / rel
        if not src.is_file() or not dst.is_file():
            continue
        if sha256(dst) != sha256(src):
            bad.append("stale dist payload (need `make package`, not `make all`): " + rel)
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
    # N18：三条版本标识必须同源 —— version.json / dist 载荷自报 / initramfs 内戳。
    # 背景：0.9.92 旧载荷留在 ESP，而程序一直"自称"新版；只比 exe 版本拦不住。
    rb = dist / "bootfiles" / "rescue-build.txt"
    if rb.is_file():
        got_rb = rb.read_text(encoding="utf-8", errors="replace").strip()
        if want and got_rb != want:
            bad.append("bootfiles/rescue-build.txt=%r != version.h=%r"
                       % (got_rb, want))
    else:
        bad.append("missing bootfiles/rescue-build.txt (package 未写载荷自报版本)")
    icp = dist / "bootfiles" / "initramfs-zjrestore.cpio.gz"
    if want and icp.is_file():
        try:
            stamp = _cpio_member(icp, "zjrescue-version")
            if stamp is None:
                bad.append("initramfs missing zjrescue-version (旧载荷/非本次构建)")
            elif stamp.strip() != want:
                bad.append("initramfs zjrescue-version=%r != version.h=%r"
                           % (stamp.strip(), want))
        except Exception as e:
            bad.append("initramfs zjrescue-version 读取失败: %s" % e)
    # beta.9（测试台第三）：运行产物门禁 —— 与打包端同源（check-pack-hygiene.py），
    # 防止 restore-task.conf / _zjresy*.log / ZJRESTORE-status.txt / logs 进包。
    try:
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            "check_pack_hygiene", str(ROOT / "tools" / "check-pack-hygiene.py"))
        hyg = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(hyg)
        for x in hyg.scan(dist):
            bad.append("pack hygiene: " + x)
    except Exception as e:
        bad.append("pack hygiene 校验异常: %s" % e)
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
