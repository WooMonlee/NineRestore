#!/bin/bash
# P1（beta.6）回归：救援层「上次任务未确认完成」三选一不得把无人值守机器
# 卷进死循环（测试台 05:42/05:45/05:47 各一轮）。
#
# 做法：从 bootfiles/alpine/init 抽出 triage_previous() 本体，在隔离的 stub
# 环境里喂 EOF / 半截输入，断言：
#   ① 已暂存 action=returnold → 直接放行，不进三选一（沿既定动作继续）；
#   ② 无人值守 + 可续做任务 → 自动 [2]（继续），绝不自动 [1]（要人工 YES）；
#   ③ 无人值守 + 无任务 → 自动 [3]，只重启一次，函数返回（不重印菜单）；
#   ④ 人工选 [1] 后 YES 超时 → 最多重问一次，之后明确收场，不无限循环。
# 只跑 bash，不碰任何真实块设备。
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
INIT="$ROOT/bootfiles/alpine/init"
WORK=$(mktemp -d 2>/dev/null || mktemp -d -t zjtriage)
OUT="$WORK/out"
: > "$OUT"
trap 'rm -rf "$WORK"' EXIT

# ---- 被 triage_previous 依赖的最小 stub（磁盘一律不碰）----
say() { printf '%s\n' "$*" >> "$OUT"; }
say2() { printf '%s\n' "$1" >> "$OUT"; }
find_old_part() { echo /dev/fake; }
has_old_tree() { return 0; }
show_support() { :; }
clear_stale_task() { :; }
mark_pending_task() { :; }
sync() { :; }
sleep() { :; }
REBOOTS=0
reboot_now() { REBOOTS=$((REBOOTS + 1)); say "STUB reboot_now"; }
poweroff() { :; }
halt() { :; }

eval "$(sed -n '/^triage_previous(){/,/^}/p' "$INIT")" || {
    echo "FAIL: cannot extract triage_previous() from $INIT" >&2
    exit 1
}
type triage_previous >/dev/null 2>&1 || {
    echo "FAIL: triage_previous() not defined after extraction" >&2
    exit 1
}

# ---- PIT-249：抽 has_new_tree()，用 stub mount 造 /tmp/zj_new_chk 现场 ----
eval "$(sed -n '/^has_new_tree(){/,/^}/p' "$INIT")" || {
    echo "FAIL: cannot extract has_new_tree() from $INIT" >&2
    exit 1
}
type has_new_tree >/dev/null 2>&1 || {
    echo "FAIL: has_new_tree() not defined after extraction" >&2
    exit 1
}
# ---- 审查用例：~old 无 Windows（早期中断）不得被当成"上次任务未确认" ----
eval "$(sed -n '/^has_old_tree(){/,/^}/p' "$INIT")" || {
    echo "FAIL: cannot extract has_old_tree() from $INIT" >&2
    exit 1
}
type has_old_tree >/dev/null 2>&1 || {
    echo "FAIL: has_old_tree() not defined after extraction" >&2
    exit 1
}
MOUNT_NEW=0          # 1 = 挂载后造出 ~new/.zj-done
MOUNT_OLD=1          # 1 = ~old/Windows 在；2 = ~old 在但没有 Windows（早期中断）
mount() {
    case "$4" in
        /tmp/zj_new_chk)
            mkdir -p /tmp/zj_new_chk
            if [ "$MOUNT_NEW" = "1" ]; then
                mkdir -p /tmp/zj_new_chk/~new
                : > /tmp/zj_new_chk/~new/.zj-done
            fi
            return 0
            ;;
        /tmp/zj_old_chk)
            mkdir -p /tmp/zj_old_chk
            case "$MOUNT_OLD" in
                1) mkdir -p /tmp/zj_old_chk/~old/Windows ;;
                2) mkdir -p /tmp/zj_old_chk/~old ;;
            esac
            return 0
            ;;
    esac
    return 0
}
ntfs-3g() { return 1; }
umount() { :; }
timeout() { shift; "$@"; }

fail() { echo "FAIL: $*" >&2; exit 1; }
reset_case() { : > "$OUT"; REBOOTS=0; MOUNT_NEW=0; MOUNT_OLD=1; rm -rf /tmp/zj_new_chk /tmp/zj_old_chk; }

# ① 已暂存 returnold：放行，不打印三选一
reset_case
triage_previous /dev/fake 1 returnold >/dev/null 2>&1
[ "$?" -eq 0 ] || fail "case1: triage_previous did not return 0 for staged returnold"
grep -q "skip 'unconfirmed task' guard" "$OUT" || fail "case1: skip message missing"
grep -q "上次任务未确认" "$OUT" && fail "case1: guard menu was shown for returnold"
[ "$REBOOTS" -eq 0 ] || fail "case1: unexpected reboot"

# ② 无人值守 + 有可续做任务 → [2] 继续
reset_case
triage_previous /dev/fake 1 "" </dev/null >/dev/null 2>&1
[ "$REBOOTS" -eq 0 ] || fail "case2: unattended path rebooted instead of continuing"
grep -q "自动:120秒无输入,已选 2" "$OUT" || fail "case2: auto choice was not 2"
grep -q "继续上次未完成的任务" "$OUT" || fail "case2: did not continue task"
grep -q "输入大写YES确认" "$OUT" && fail "case2: unattended auto-selected rollback (needs YES)"
grep -q "自动选\[1\]" "$OUT" && fail "case2: prompt still advertises auto-[1]"

# ③ 无人值守 + 无任务 → 只重启一次，函数返回（不重印菜单）
reset_case
triage_previous /dev/fake 0 "" </dev/null >/dev/null 2>&1
[ "$REBOOTS" -eq 1 ] || fail "case3: expected exactly 1 reboot, got $REBOOTS"
n=$(grep -c "上次任务未确认" "$OUT")
[ "$n" -eq 1 ] || fail "case3: menu reprinted $n times (loop)"
grep -q "自动:120秒无输入,已选 3" "$OUT" || fail "case3: auto choice was not 3"

# ④ 人工选 [1] 但 YES 超时 → 最多问一次 YES，随后收场（不无限循环）
reset_case
printf '1\n' | triage_previous /dev/fake 1 "" >/dev/null 2>&1
[ "$?" -eq 0 ] || fail "case4: did not terminate"
n=$(grep -c "输入大写YES确认" "$OUT")
[ "$n" -eq 1 ] || fail "case4: YES asked $n times (expected 1)"
grep -q "回滚未确认 (1/2)" "$OUT" || fail "case4: denial not recorded"
grep -q "自动:120秒无输入,已选 2" "$OUT" || fail "case4: did not fall back to auto-[2]"

# ⑤ ~new/.zj-done 存在 → has_new_tree 判为"交换中断现场"
rm -rf /tmp/zj_new_chk; MOUNT_NEW=1
has_new_tree /dev/fake >/dev/null 2>&1 || fail "case5: ~new/.zj-done was not detected"
# ⑥ ~new 在但没有 .zj-done（未完成暂存）→ 不算中断现场
rm -rf /tmp/zj_new_chk; MOUNT_NEW=0
mkdir -p /tmp/zj_new_chk/~new
has_new_tree /dev/fake >/dev/null 2>&1 && fail "case6: ~new without .zj-done must not count"
# ⑦ ~old/Windows 存在 → has_old_tree 判为真（上次中断现场）
rm -rf /tmp/zj_old_chk; MOUNT_OLD=1
has_old_tree /dev/fake >/dev/null 2>&1 || fail "case7: ~old/Windows was not detected"
# ⑧ ~old 在但没有 Windows（早期中断，备份还没成形）→ 假：
#    不弹三选一；lite 重跑由 zjswap.sh L1 以 exit 3 拒绝（见 test_zjswap.sh E）
rm -rf /tmp/zj_old_chk; MOUNT_OLD=2
has_old_tree /dev/fake >/dev/null 2>&1 && fail "case8: ~old without Windows must not count"
# ⑨ 审查用例 4（假阳性回归）：~new 在但 .zj-done 已删 → has_new_tree 假 → _zb_hit=0
#    已有 ⑥ 覆盖同一现场；这里再钉住 _zb_hit 的接线（必须是"~old 命中且 ~new 完整"）
rm -rf /tmp/zj_new_chk /tmp/zj_old_chk; MOUNT_NEW=0; MOUNT_OLD=1
mkdir -p /tmp/zj_new_chk/~new
has_new_tree /dev/fake >/dev/null 2>&1 && fail "case9: ~new without .zj-done must not count"
grep -Fq 'if [ -n "$_zb_dev" ] && has_new_tree "$_zb_dev"; then _zb_hit=1; fi' "$INIT" \
    || fail "case9: _zb_hit is not gated by has_new_tree (false positive can come back)"
# ⑩ 挂载失败（两种驱动都失败）→ 保守判定为否，不得误报中断
rm -rf /tmp/zj_new_chk; MOUNT_NEW=0
mount() { return 1; }
has_new_tree /dev/fake >/dev/null 2>&1 && fail "case10: mount failure must be treated as no-new-tree"

echo "test_rescue_triage: PASS (10 cases)"
