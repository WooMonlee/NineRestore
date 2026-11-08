#!/bin/sh
# zjlog.sh — 救援层统一诊断底座（P0-B，PIT-145）
#
# 背景：5 个救援脚本（init / zjrestore-lite.sh / zjswap.sh / zjswap-undo.sh /
# setup-swap.sh）各复制了一份 say()，落盘行为渐渐漂移：init/lite 的文件行无
# 前缀、undo 带前缀、swap/setup 只打控制台不落文件 —— 多脚本合并成的
# /tmp/zjrestore.log 里分不清谁说的、还缺行。统一到本文件单源实现。
#
# 用法（调用方在 source 前设前缀，前缀是断言/录像取证的契约，不许改）：
#   ZJPREFIX=SWAPUNDO
#   . /zjlog.sh
#
# 约定：
#   · 控制台行 = "<PREFIX>: msg"（QEMU 断言 grep 它，如 `SWAPUNDO:`/`ZJ:`）；
#   · 文件行   = "YYYY-mm-dd HH:MM:SS <PREFIX>: msg" 追加到
#     ${LOG:-/tmp/zjrestore.log} —— 5 个脚本合并成一条时间线（LOG 未导出
#     的子进程也能落同一份：兜底路径相同）；
#   · 文件写失败必须静默（日志面可能未就绪/只读）—— say 永远 return 0，
#     不能因落盘失败让调用方的 `if say` 判断走偏，也兼容 `set -u`。
#
# 只放 say。不放 run/die：现有调用点是 "say ERROR → umount → exit N" 的
# 多步形态，硬套 die 只会增加 diff 噪音 —— 需要时再加，别为统一而统一。
# 带 timeout 的 NTFS 挂载纪律（PIT-112"挂死不可静默" + PIT-144 FUSE 优先）
# 也收敛在这里：三个裸 mount 的脚本（zjswap/zjswap-undo/setup-swap）共用。

: "${ZJPREFIX:=ZJ}"

# ── 救援层中文字库（16 点阵，取模嵌入）────────────────────────────────
# 控制台是 Linux VT（vgacon/fbcon）：内核一行只认 8 像素宽的字符格，
# 一个汉字占两格，所以字库里每个汉字都存成"左半 8px + 右半 8px"两块，
# 各映射到一个私用区码位（U+E000 起），运行时用 sed 把汉字换成这两个
# 半字码位 —— 双宽/折行/错位都不经过内核的双宽逻辑（那是按 16px 整字
# 画第一格、第二格留空，8px 字格下会把汉字挤扁）。
# 只在本地 VT 上启用：串口控制台没有字库，转码只会把日志变成乱码。
zj_font_load(){
    [ -f /zjfont.psf ] || return 1
    [ -w /dev/tty0 ] || return 1
    if setfont -C /dev/tty0 /zjfont.psf 2>>/tmp/zjlog.err; then
        : > /tmp/zj-cjk-font.ok
        return 0
    fi
    return 1
}

zj_console_cjk(){
    [ -f /tmp/zj-cjk-font.ok ] || return 1
    [ -f /zjzhmap.sed ] || return 1
    [ -r /sys/class/tty/console/active ] || return 1
    case "$(cat /sys/class/tty/console/active)" in
        *tty[0-9]*) return 0 ;;
    esac
    return 1
}

say(){
    _zj_line="$ZJPREFIX: $*"
    case "$*" in
        *[!\ -~]*)
            # 含非 ASCII（中文界面句）：控制台拆成半字码位，落盘保持原文。
            # 模式写成 [!\ -~] 是实测过的：busybox ash 认它，[! -~] 带裸空格
            # 会在解析期报 "unexpected word"，[:print:] 在 ash 里把 UTF-8 当可打印。
            if zj_console_cjk; then
                printf '%s\n' "$_zj_line" | sed -f /zjzhmap.sed 1>&2
            else
                printf '%s\n' "$_zj_line" 1>&2
            fi
            ;;
        *)
            printf '%s\n' "$_zj_line" 1>&2
            ;;
    esac
    echo "$(date '+%F %T') $_zj_line" >> "${LOG:-/tmp/zjrestore.log}" 2>/dev/null
    return 0
}

# mnt_ntfs <dev> <mnt> [ntfs3|fuse] —— 返回 0/1；错误进 /tmp/zjmnt.err
#   ntfs3（默认）：内核驱动优先，吞吐高（与 zjrestore-lite.sh::mnt_dev 同序）；
#   fuse：ntfs-3g 优先 —— **zjswap-undo.sh 必须用这个**（PIT-144：ntfs3 在
#         大规模 rename 风暴后会留幽灵索引项，rm -rf 必失败）。
# 每一步都带 timeout 60（PIT-112：ntfs3 挂坏设备会静默挂死 400s 级）；
# 成功打一行 `  mnt <dev> ok (<drv>)` —— "这次是 FUSE 还是内核驱动"是
# PIT-144 类问题的定罪要素，必须留痕。
mnt_ntfs(){
    : > /tmp/zjmnt.err
    if [ "${3:-ntfs3}" = fuse ]; then
        timeout 60 ntfs-3g -o force "$1" "$2" 2>>/tmp/zjmnt.err && { say "  mnt $1 ok (fuse)"; return 0; }
        timeout 60 mount -t ntfs3 "$1" "$2" 2>>/tmp/zjmnt.err && { say "  mnt $1 ok (ntfs3)"; return 0; }
    else
        timeout 60 mount -t ntfs3 "$1" "$2" 2>>/tmp/zjmnt.err && { say "  mnt $1 ok (ntfs3)"; return 0; }
        timeout 60 ntfs-3g -o force "$1" "$2" 2>>/tmp/zjmnt.err && { say "  mnt $1 ok (fuse)"; return 0; }
    fi
    timeout 60 mount -t ntfs-3g "$1" "$2" 2>>/tmp/zjmnt.err && { say "  mnt $1 ok (fuse)"; return 0; }
    return 1
}
