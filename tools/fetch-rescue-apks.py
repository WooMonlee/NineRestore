#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""fetch-rescue-apks.py — 无人值守拉取救援层的 Alpine 用户态包。

背景（2026-11-07，N18）：`tools/build-debian-rescue.py` 组装 initramfs 时需要
`tools/vmtest/dl/alpine/*.apk`（.gitignore 忽略，不在仓库里）。缺缓存时构建链
只能报"missing xxx.apk"然后停住 —— 于是有人干脆跳过重建，dist 里就留了旧载荷
（N15/N10 事故）。

本脚本把"取包"变成构建链的一步：按**固定文件名**从 Alpine v3.20 官方 CDN
下载（先 main、再 community），已存在且非空则跳过。全程无交互、无 msys2 依赖，
只要 Python 3 + 网络。下载目标与 build-alpine-initramfs.py 的 DL 常量一致。

用法：
  python tools/fetch-rescue-apks.py            # 缺什么补什么
  python tools/fetch-rescue-apks.py --check    # 只检查缓存完整性，不下载
"""
import os
import sys
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DL = os.path.join(ROOT, 'tools', 'vmtest', 'dl', 'alpine')

BASE = 'https://dl-cdn.alpinelinux.org/alpine'
# 固定文件名优先在 v3.20 找；个别包在本分支已滚动到更早的修订（device-mapper
# 2.03.29 / openssl 3.3.7-r2 只存在于 v3.21），因此按分支顺序回退。都是同一
# 上游版本的 ABI 兼容构建，不改变内核/脚本。
BRANCHES = ('v3.20', 'v3.21')
REPOS = ('main', 'community')

# 与 tools/vmtest/build-alpine-initramfs.py 的 APKS 保持一致（固定版本号）。
APKS = [
    'busybox-static-1.36.1-r31.apk',
    'ntfs-3g-2026.2.25-r0.apk',
    'ntfs-3g-libs-2026.2.25-r0.apk',
    'ntfs-3g-progs-2026.2.25-r0.apk',
    'fuse3-libs-3.16.2-r0.apk',
    'musl-1.2.5-r3.apk',
    'blkid-2.40.1-r1.apk',
    'libblkid-2.40.1-r1.apk',
    'libuuid-2.40.1-r1.apk',
    'libeconf-0.6.3-r0.apk',
    'wimlib-1.14.4-r0.apk',
    'device-mapper-2.03.29-r1.apk',
    'device-mapper-libs-2.03.29-r1.apk',
    'openssl-3.3.7-r2.apk',
    'libcrypto3-3.3.7-r2.apk',
    'libssl3-3.3.7-r2.apk',
    'linux-firmware-adaptec-20240811-r0.apk',
    'linux-firmware-advansys-20240811-r0.apk',
    'linux-firmware-isci-20240811-r0.apk',
    'linux-firmware-microchip-20240811-r0.apk',
    'linux-firmware-qlogic-20240811-r0.apk',
]


def present(path):
    return os.path.isfile(path) and os.path.getsize(path) > 0


def fetch_one(name, check_only):
    dst = os.path.join(DL, name)
    if present(dst):
        return True
    if check_only:
        print('MISSING %s' % dst)
        return False
    last = None
    for branch in BRANCHES:
        for repo in REPOS:
            url = '%s/%s/%s/x86_64/%s' % (BASE, branch, repo, name)
            try:
                tmp = dst + '.part'
                print('fetch %s (%s/%s)' % (name, branch, repo), flush=True)
                with urllib.request.urlopen(url, timeout=60) as resp, \
                        open(tmp, 'wb') as out:
                    while True:
                        chunk = resp.read(1 << 20)
                        if not chunk:
                            break
                        out.write(chunk)
                if os.path.getsize(tmp) == 0:
                    raise RuntimeError('empty file')
                os.replace(tmp, dst)
                return True
            except (urllib.error.HTTPError, urllib.error.URLError,
                    RuntimeError) as e:
                last = e
                try:
                    os.remove(tmp)
                except OSError:
                    pass
                continue
    print('FAIL %s: %s' % (name, last))
    return False


def main():
    check_only = '--check' in sys.argv
    os.makedirs(DL, exist_ok=True)
    bad = [n for n in APKS if not fetch_one(n, check_only)]
    if bad:
        print('fetch-rescue-apks: FAIL (%d/%d missing)' % (len(bad), len(APKS)))
        print('  缓存目录：%s' % DL)
        print('  离线环境请手工放入上述文件后重试。')
        return 1
    print('fetch-rescue-apks: PASS (%d/%d in cache)' % (len(APKS), len(APKS)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
