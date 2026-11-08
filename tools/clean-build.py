#!/usr/bin/env python3
"""清构建产物，但保留**已入库**的构建清单（build/duilib.mk）。

事故背景（2026-11-08）：`make clean` 原先用 `del /S /Q build`，把已经入库、
且被 Makefile `include` 的 `build/duilib.mk`（duilib 源清单）一起删掉，
下一次构建直接 `No rule to make target 'build/duilib.mk'` 失败，只能手工从
git 还原。clean 的职责是清产物，不该碰入库文件。

用法：python tools/clean-build.py <目录> [目录...]
规则：目录根下 `*.mk` 一律保留，其余文件与子目录递归删除。
"""
import os
import shutil
import sys


def clean(path: str) -> None:
    if not os.path.isdir(path):
        return
    for name in sorted(os.listdir(path)):
        full = os.path.join(path, name)
        if name.endswith('.mk'):
            print('clean-build: keep %s (tracked manifest)' % full)
            continue
        try:
            if os.path.isdir(full):
                shutil.rmtree(full)
            else:
                os.remove(full)
        except OSError as exc:
            print('clean-build: skip %s (%s)' % (full, exc), file=sys.stderr)


def main() -> None:
    if len(sys.argv) < 2:
        sys.exit('usage: clean-build.py <dir> [dir...]')
    for d in sys.argv[1:]:
        clean(d)


if __name__ == '__main__':
    main()
