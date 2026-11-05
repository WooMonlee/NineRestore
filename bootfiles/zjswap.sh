#!/bin/sh
# zjswap.sh — A 引擎交换引擎：把 <目标>:\~new 换入原位，旧树移入 ~old。
#
# 用法: zjswap.sh <目标分区设备> <挂载点>
# 前置（Windows 侧保证）：~new 已由 `SysRecover.exe extract --all` 完整解压，
#   并在成功后写入完成标记 `~new/.zj-done`（内容：内容量字节 + 时间）。
#
# 失败安全（用户 2026-10-09 定案，docs/23 L1/D2；1.0 整改 H1）：
#   · 任何检查不过 → **报错退出、绝不碰旧树**（旧系统原样可启动）；
#   · 解压失败/中断（含断电）→ 标记缺失 → 拒绝交换；孤儿 ~new 由下次
#     Windows 启动检测并清理（提示用户换镜像重试）；**不做断点续解**（首版）。
#   · 交换自身中断 → 半新半旧（P0 记录、P1 事务化；见 docs/23 L3）。
#   · 目标上已有 ~old（上次交换中断/用户选了保留）→ 拒绝交换（exit 3），
#     由下次 Windows 启动的残留询问处理（docs/23 §5）。
#
# H1 fail-closed（1.0）：
#   · 旧项 move 失败 → 把已搬进 ~old 的项**回滚搬回根**，保留 ~new，exit 4；
#   · 新项 move 失败 → **不删 ~new**（保留现场给下一次启动诊断），exit 4；
#   · 删除根上冲突项之前，必须先确认同名旧项已在 ~old（或属于可丢弃的特殊项）；
#   · 交换完成后校验 Windows\System32\winload（.exe/.efi 至少一个存在），
#     不过则 exit 5，且不删 ~new；
#   · **只有全部成功**才删除 ~new 并 exit 0。
#
# H4.5 回退保障（1.0）：keep_old=0 也**不再当场删除 ~old** —— 旧系统保留到新系统
#   首次成功开机（Windows 侧 RunOnce 的 zj-swap-cleanup.cmd 检测到 ~new 不存在后
#   才删）。这样"换进去起不来"时旧系统仍在，可走「返回旧系统」。
#
# 特殊顶层项（docs/23 L2/D3）：回收站/系统卷信息/页文件删除旧（新树自带或
#   系统首启重建），不进 ~old。
set -u

T="${1:?target dev}"; M="${2:?mount point}"
# say/mnt_ntfs 单源（P0-B/PIT-145）：SWAP: 前缀是断言契约；挂载带 timeout 60
# （PIT-112 挂死纪律），错误落 /tmp/zjmnt.err 不再被 2>/dev/null 静默吞掉。
ZJPREFIX=SWAP
. /zjlog.sh

mkdir -p "$M"
mnt_ntfs "$T" "$M" || {
    say "mount FAILED ($T): $(head -1 /tmp/zjmnt.err 2>/dev/null)"
    exit 1
}

# 统一的失败退出：保留 ~new/现场，卸载后以非零码退出。
abort_swap() {
    _code="$1"; shift
    say "ERROR: $*"
    say "swap ABORTED (rc=$_code); ~new kept if present, old data recoverable"
    sync
    umount "$M" 2>/dev/null
    exit "$_code"
}

# ── L1：先验完成标记，再动任何东西（fail-closed）──
if [ ! -f "$M/~new/.zj-done" ]; then
    say "ERROR: ~new missing or incomplete (.zj-done absent) -> refuse; old system untouched"
    umount "$M" 2>/dev/null
    exit 2
fi
say "marker ok: $(cat "$M/~new/.zj-done" 2>/dev/null | tr -d '\r\n')"

# 异机还原失败标记（2026-10-30）：~new 是完整的，但驱动没注入成功 → 绝不能交换
# （换进去可能因为认不到硬盘而起不来）。失败标记由 Windows 侧重试成功后删除。
if [ -f "$M/~new/.zj-universal-failed" ]; then
    say "ERROR: universal restore preparation failed; refuse swap (old system untouched)"
    say "reason: $(cat "$M/~new/.zj-universal-failed" 2>/dev/null | tr -d '\r\n' | head -c 300)"
    umount "$M" 2>/dev/null
    exit 4
fi

# ── L1：目标上已有 ~old → 拒绝交换（上次中断/用户保留；交给 Windows 侧询问）──
if [ -d "$M/~old" ]; then
    say "ERROR: leftover ~old present -> refuse swap (exit 3); handle it on next Windows start"
    umount "$M" 2>/dev/null
    exit 3
fi

# ── 旧顶层项 → ~old（特殊项删旧）──
mkdir -p "$M/~old" || abort_swap 4 "mkdir ~old FAILED"
_old_fail=0
for d in "$M"/* "$M"/.[!.]*; do
    [ -e "$d" ] || continue
    b=$(basename "$d")
    case "$b" in ~new|~old|.|..) continue;; esac
    case "$b" in
        '$RECYCLE.BIN'|'System Volume Information'|pagefile.sys|swapfile.sys|hiberfil.sys)
            rm -rf "$d" 2>/dev/null || {
                say "ERROR: remove special old item $b failed"
                _old_fail=1
            }
            continue;;
    esac
    mv "$d" "$M/~old/" 2>/dev/null || {
        say "ERROR: move old $b -> ~old failed"
        _old_fail=1
    }
done
if [ "$_old_fail" -ne 0 ]; then
    # 回滚：把已搬进 ~old 的项搬回根，尽量恢复旧系统原状（特殊项已按契约丢弃）。
    say "rollback: moving items back from ~old to root"
    for d in "$M/~old"/* "$M/~old"/.[!.]*; do
        [ -e "$d" ] || continue
        b=$(basename "$d")
        mv "$d" "$M/$b" 2>/dev/null || say "ERROR: rollback $b failed (manual recovery needed)"
    done
    rmdir "$M/~old" 2>/dev/null
    abort_swap 4 "old tree partially moved; rollback attempted, ~new untouched"
fi

# ── ~new 顶层项 → 原位 ──
_new_fail=0
for d in "$M/~new"/* "$M/~new"/.[!.]*; do
    [ -e "$d" ] || continue
    b=$(basename "$d")
    case "$b" in .zj-done|.zj-universal-failed) continue;; esac
    # 跳过契约文件（Windows 侧写的 _zjresy*.log / restore-task.conf）
    # 这些文件不应该被移到根目录，swap 后应删除
    case "$b" in
        _zjresy*.log|restore-task.conf|restore-task.json)
            rm -f "$d" 2>/dev/null
            continue;;
    esac
    if [ -e "$M/$b" ]; then
        # H1：删冲突项前必须确认「旧项已安全落到 ~old」。此阶段根上非特殊项
        # 要么已搬进 ~old、要么是特殊项（删不掉会在上一阶段就中止）。
        case "$b" in
            '$RECYCLE.BIN'|'System Volume Information'|pagefile.sys|swapfile.sys|hiberfil.sys) ;;
            *)
                if [ ! -e "$M/~old/$b" ]; then
                    say "ERROR: conflict $b at root but old copy is NOT in ~old; refuse to delete"
                    _new_fail=1
                    continue
                fi;;
        esac
        rm -rf "${M:?}/$b" 2>/dev/null || {
            say "ERROR: clear conflict $b failed"
            _new_fail=1
            continue
        }
    fi
    mv "$d" "$M/$b" 2>/dev/null || {
        say "ERROR: move new $b -> root failed"
        _new_fail=1
    }
done
if [ "$_new_fail" -ne 0 ]; then
    abort_swap 4 "new tree swap incomplete; ~new kept for diagnosis (do NOT boot new system)"
fi

# ── 交换后校验（H1）：关键系统文件必须真的落到根上 ──
if [ ! -f "$M/Windows/System32/winload.exe" ] && \
   [ ! -f "$M/Windows/System32/winload.efi" ]; then
    abort_swap 5 "post-swap verify failed: Windows\\System32\\winload.exe/.efi missing"
fi
say "post-swap verify ok (Windows\\System32\\winload present)"

# 只有全部成功才删 ~new（含 .zj-done）。删不掉也按失败上报（残留会被下次启动识别
# 为"交换中断"，比误报成功安全）。
rm -rf "$M/~new" 2>/dev/null || abort_swap 6 "remove ~new failed; leftover will be treated as interrupted swap"
sync

# H4.5：keep_old=0 不再当场删 ~old。旧系统保留到新系统首次成功开机，由
# zj-swap-cleanup.cmd（RunOnce，见 ops_staged.cpp）确认 ~new 不存在后才删；
# 起不来时可「返回旧系统」。keep_old=1 由 Windows 侧菜单向用户确认删除。
if [ "${ZJ_KEEP_OLD:-1}" = "0" ]; then
    say "keep_old=0 -> ~old retained until new system first successful boot (RunOnce cleanup)"
else
    say "keep_old=1 -> ~old retained for 'return to old system'"
fi
say "swap done (old tree kept at ~old)"
umount "$M" 2>/dev/null
exit 0
