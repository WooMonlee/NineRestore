#!/usr/bin/env python3
"""发布输入卫生门禁（beta.9，测试台第三条）。

测试台在 beta.8 的单文件包/解包目录里看到 08:36 的陈旧 `restore-task.conf`。
运行期产物（契约副件、救援日志、状态回执）一律不许进入发布输入：

  restore-task.conf / restore-task.json     暂存契约副件
  _zjresy*.log                              救援层镜像日志
  ZJRESTORE-*.txt / ZJRESTORE-*.log         状态回执、探针、apply 输出
  pending-restore.txt                       待执行标记
  logs/ 目录                                任何层级的运行日志目录
  *.tmp / *.dmp                             崩溃转储与临时文件

用法：
  python tools/check-pack-hygiene.py dist            # 只检查（失败返回 1）
  python tools/check-pack-hygiene.py --clean dist    # 清理（打包前由 Makefile 调用）

`make package` 会先 `--clean`，`portable-pack.py` 与 `check-dist.py` 都会再扫一遍，
所以即使有人手工往 dist 里丢运行产物，打包也不会产出脏包。
"""
import fnmatch
import os
import shutil
import sys
from pathlib import Path

FILE_PATTERNS = (
    "restore-task.conf",
    "restore-task.json",
    "_zjresy*.log",
    "ZJRESTORE-status.txt",
    "ZJRESTORE-last.log",
    "ZJRESTORE-probe.txt",
    "ZJRESTORE-apply.out",
    "ZJRESTORE-mkntfs.out",
    "ZJRESTORE-esp.out",
    "pending-restore.txt",
    "*.tmp",
    "*.dmp",
)


def scan(root):
    """返回违规项列表（相对路径 + 说明）；空列表 = 干净。"""
    root = Path(root)
    if not root.is_dir():
        return ["missing dist directory: %s" % root]
    bad = []
    for p in sorted(root.rglob("*")):
        rel = p.relative_to(root).as_posix()
        if p.is_dir():
            if p.name.lower() == "logs":
                bad.append(rel + "/ (runtime log directory)")
            continue
        for pat in FILE_PATTERNS:
            if fnmatch.fnmatchcase(p.name, pat):
                bad.append(rel + " (runtime artifact)")
                break
    return bad


def clean(root):
    """删除所有运行产物；返回删除数量。"""
    root = Path(root)
    removed = 0
    dirs = []
    for p in sorted(root.rglob("*")):
        if p.is_dir():
            if p.name.lower() == "logs":
                dirs.append(p)
            continue
        if any(fnmatch.fnmatchcase(p.name, pat) for pat in FILE_PATTERNS):
            try:
                os.chmod(p, 0o666)
                p.unlink()
                removed += 1
            except OSError:
                pass
    for d in sorted(dirs, key=lambda x: len(x.parts), reverse=True):
        try:
            shutil.rmtree(d)
            removed += 1
        except OSError:
            pass
    return removed


def main():
    args = sys.argv[1:]
    do_clean = False
    if args and args[0] == "--clean":
        do_clean = True
        args = args[1:]
    root = Path(args[0]) if args else Path("dist")
    if do_clean:
        n = clean(root)
        print("check-pack-hygiene: cleaned %d runtime artifact(s) under %s"
              % (n, root))
        return 0
    bad = scan(root)
    if bad:
        print("check-pack-hygiene: FAIL (%s)" % root)
        for x in bad:
            print("  - " + x)
        print("  hint: python tools/check-pack-hygiene.py --clean %s" % root)
        return 1
    print("check-pack-hygiene: OK (%s)" % root)
    return 0


if __name__ == "__main__":
    sys.exit(main())
