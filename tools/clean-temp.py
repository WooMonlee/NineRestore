#!/usr/bin/env python3
"""temp/scratch 清理：默认 dry-run，只扫可再生成区。

三层保护：
1) bench / evidence 整目录豁免（显式跳过，不靠扩展名）；
2) VM / 本程序进程在跑时整体跳过；
3) 被仓库文档/脚本引用、被占用、或带 VM/镜像扩展名的文件一律不删。
"""
import argparse
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
TEMP = REPO / "temp"
SCRATCH = TEMP / "scratch"
EXEMPT_DIRS = ("bench", "evidence")
ARCHIVE_EXT = {".zip", ".7z", ".rar", ".esd", ".iso", ".tar", ".gz", ".xz", ".wim"}
VM_EXT = {".vmdk", ".vmsn", ".vmem", ".nvram", ".vmx", ".vmsd", ".vmxf"}
KEEP = {".gitkeep"}
DAY = 86400
REF_SUFFIX = {".md", ".py", ".ps1", ".bat", ".cmd", ".txt", ".mk", ".json"}
VM_PROCS = ("vmware-vmx", "vmrun", "vmware.exe", "sysrecover")


def vm_running():
    try:
        out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True,
                             text=True, timeout=20).stdout.lower()
    except Exception:
        return ["tasklist-failed"]  # 无法核实 -> 按“在跑”处理
    return [n for n in VM_PROCS if n in out]


def ref_text():
    parts = []
    files = [REPO / "Makefile"] + sorted(REPO.glob("*.md"))
    for sub in ("docs", "tools", "tests"):
        for p in (REPO / sub).rglob("*"):
            if p.is_file() and p.suffix.lower() in REF_SUFFIX and p.stat().st_size < 2_000_000:
                files.append(p)
    for p in files:
        try:
            parts.append(p.read_text(encoding="utf-8", errors="ignore"))
        except OSError:
            pass
    return "\n".join(parts)


def locked(p):
    try:
        with open(p, "r+b"):
            return False
    except OSError:
        return True


def human(n):
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return f"{n:.1f}{unit}"
        n /= 1024
    return f"{n:.1f}GB"


def size_of(items):
    total = 0
    for p in items:
        try:
            total += p.stat().st_size
        except OSError:
            pass
    return total


def main():
    ap = argparse.ArgumentParser(description="clean temp/scratch (dry-run by default)")
    ap.add_argument("--apply", action="store_true", help="actually delete the READY list")
    ap.add_argument("--days", type=int, default=7, help="age threshold for plain files")
    ap.add_argument("--archive-days", type=int, default=14, help="age threshold for archives")
    args = ap.parse_args()

    print(f"[mode] {'APPLY' if args.apply else 'DRY-RUN'}  root={SCRATCH}")
    if not SCRATCH.is_dir():
        print("[abort] scratch dir not found")
        return 2

    running = vm_running()
    if running:
        print(f"[abort] VM/app process running: {', '.join(running)}; nothing will be touched")
        return 0

    # 第 1 层保护：豁免目录必须存在且不被扫描（这里只做显式断言）
    for name in EXEMPT_DIRS:
        p = TEMP / name
        if p.is_dir():
            print(f"[exempt] whole dir skipped: {p}")

    refs = ref_text()
    now = time.time()
    ready, waiting, protected = [], [], []

    for p in sorted(SCRATCH.rglob("*")):
        if p.is_dir():
            continue
        rel = p.relative_to(SCRATCH).as_posix()
        if p.is_symlink():
            protected.append((p, "symlink"))
            continue
        if p.name in KEEP:
            protected.append((p, "keep-file"))
            continue
        if p.suffix.lower() in VM_EXT:
            protected.append((p, "vm-disk"))
            continue
        if p.name in refs:
            protected.append((p, "referenced"))
            continue
        if locked(p):
            protected.append((p, "locked"))
            continue
        age = (now - p.stat().st_mtime) / DAY
        limit = args.archive_days if p.suffix.lower() in ARCHIVE_EXT else args.days
        (ready if age >= limit else waiting).append((p, age, limit, rel))

    print("\n[ready] would delete now")
    for p, age, limit, rel in ready:
        print(f"  {human(p.stat().st_size):>9}  {age:5.1f}d/{limit}d  {rel}")
    if not ready:
        print("  (none)")

    print("\n[waiting] not old enough yet")
    for p, age, limit, rel in sorted(waiting, key=lambda x: x[2] - x[1]):
        print(f"  {human(p.stat().st_size):>9}  {age:5.1f}d/{limit}d  {rel}")
    if not waiting:
        print("  (none)")

    print("\n[protected] never touched")
    for p, why in protected:
        print(f"  {why:<10} {p.relative_to(SCRATCH).as_posix()}")
    if not protected:
        print("  (none)")

    print(f"\n[summary] ready={len(ready)} ({human(size_of([x[0] for x in ready]))})"
          f"  waiting={len(waiting)} ({human(size_of([x[0] for x in waiting]))})"
          f"  protected={len(protected)}")

    if args.apply and ready:
        removed, freed = 0, 0
        for p, _, _, _ in ready:
            try:
                freed += p.stat().st_size
                p.unlink()
                removed += 1
            except OSError as e:
                print(f"[warn] cannot delete {p}: {e}")
        TEMP.mkdir(exist_ok=True)
        log = TEMP / f"cleanup-{time.strftime('%Y%m%d-%H%M%S')}.txt"
        log.write_text(f"removed={removed} freed={freed}\n", encoding="utf-8")
        print(f"[done] removed={removed} freed={human(freed)} log={log}")
    elif args.apply:
        print("[done] nothing to delete")
    else:
        print("[dry-run] no file deleted")
    return 0


if __name__ == "__main__":
    sys.exit(main())
