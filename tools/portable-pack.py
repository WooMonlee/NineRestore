r"""把 dist/ 打包成单文件（PIT-201 / PIT-213）。

**从 0.9.92 起，真正的打包由 exe 自己做**（`SysRecover.exe pack-self`）：
容器载荷用 ntdll 的 XPRESS-Huffman 压缩（约 50MB → 38MB），Python 侧不再重复
实现一份压缩器 —— 两套实现必然漂移。

用法（Makefile 不变）：
  python tools/portable-pack.py <stub.exe> <out.exe> <distDir> [releaseDir]

  stub.exe  : 仅为兼容旧命令行保留（实际用 <distDir>\bootfiles\portable-stub.exe）
  out.exe   : 输出路径
  distDir   : 要打包的目录（程序的 AppDir）
  releaseDir: 可选的发布目录（按版本号复制一份，如 D:\九转大还原1.0-beta.6.exe）

历史版本不可变（beta.6，测试台记账）：
  打包/发布一律**先写临时文件、再原子改名**，绝不在已存在的输出文件上原地重写。
  否则输出若被外部做成硬链接（例如 share\b3.exe → dist\SysRecover-Portable.exe），
  一次重建就会把共享端的历史原件一起改掉（1.0.0.3 已因此丢失）。
"""
import importlib.util
import json
import os
import shutil
import subprocess
import sys


def _load_hygiene():
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        'check-pack-hygiene.py')
    spec = importlib.util.spec_from_file_location('check_pack_hygiene', path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _atomic_copy(src, dst):
    """复制到同目录临时名再原子改名：目标已存在时也不会原地截断旧 inode。"""
    tmp = dst + '.tmp-%d' % os.getpid()
    try:
        shutil.copyfile(src, tmp)
        os.replace(tmp, dst)
    finally:
        if os.path.exists(tmp):
            try:
                os.remove(tmp)
            except OSError:
                pass


def main():
    if len(sys.argv) not in (4, 5):
        print(__doc__)
        return 2
    stub, out, dist = sys.argv[1], sys.argv[2], sys.argv[3]
    release_dir = sys.argv[4] if len(sys.argv) == 5 else None

    exe = os.path.join(dist, 'x64', 'SysRecover.exe')
    if not os.path.exists(exe):
        exe = os.path.join(dist, 'SysRecover.exe')  # 32 位入口（无 x64 子目录时）
    if not os.path.exists(exe):
        print('portable-pack: SysRecover.exe not found under %s' % dist)
        return 1

    # beta.9（测试台第三）：运行产物不许进包。dist 里的 restore-task.conf /
    # _zjresy*.log / ZJRESTORE-status.txt / logs 都说明打包输入被运行过。
    bad = _load_hygiene().scan(dist)
    if bad:
        print('portable-pack: REFUSE: runtime artifact(s) in %s' % dist)
        for x in bad:
            print('  - ' + x)
        print('  hint: python tools/check-pack-hygiene.py --clean %s' % dist)
        return 1

    out_abs = os.path.abspath(out)
    # 先写临时文件：输出路径若与外部文件是硬链接，原子改名只会替换本条目录项，
    # 另一个链接仍指向旧 inode（历史版本字节不变）。
    tmp_out = out_abs + '.build-%d.exe' % os.getpid()
    try:
        r = subprocess.run([exe, 'pack-self', '--out', tmp_out, '--stub',
                            os.path.abspath(stub)])
        if r.returncode != 0 or not os.path.exists(tmp_out):
            print('portable-pack: pack-self failed (rc=%d)' % r.returncode)
            return 1
        os.replace(tmp_out, out_abs)
    finally:
        if os.path.exists(tmp_out):
            try:
                os.remove(tmp_out)
            except OSError:
                pass
    size = os.path.getsize(out_abs)
    print('portable-pack: %s (%.1f MB)' % (out, size / 1048576.0))

    if release_dir:
        ver = '0.0.0.0'
        vj = os.path.join(dist, 'version.json')
        if os.path.exists(vj):
            with open(vj, 'r', encoding='utf-8') as f:
                ver = json.load(f).get('version', ver)
        # 2026-11-08 命名规范（RC 渠道）：文件名统一由 tools/version.py 派生，
        # 避免这里再抄一遍规则（对外 RC 号与内部四段号是两套独立计数）。
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import version as vmod
        zh = vmod.derive(vmod.read_macros())['filename']
        dst_zh = os.path.join(release_dir, zh)
        # 同版本号不覆盖旧包：撞名说明忘了递增 beta 号。
        if os.path.exists(dst_zh):
            print('portable-pack: REFUSE: %s already exists; bump the beta '
                  'number first (tools/version.py --bump)' % dst_zh)
            return 1
        try:
            _atomic_copy(out_abs, dst_zh)
            print('portable-pack: released -> %s' % dst_zh)
        except OSError as e:
            print('portable-pack: release copy FAILED: %s' % e)
            return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
