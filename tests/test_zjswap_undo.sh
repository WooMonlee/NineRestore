#!/bin/sh
# PIT-249 回归：zjswap-undo.sh 必须能收场「正向交换 zjswap.sh 做到一半断电」。
#
# 现场特征：~new 是待换入的新树（有 .zj-done、没有 .zj-undo-phase），根上已没有
# Windows（被 zjswap 搬进了 ~old）。旧实现会 exit 3（把它当成"暂存后重启"），
# 于是救援层三选一里唯一能救回旧系统的 [1] 回滚选项实际不可用。
#
# 做法：不碰真实块设备 —— 用测试 stub 提供 say/mnt_ntfs，在临时目录造盘面。
# 断言：
#   A 正常「返回旧系统」→ rc 0，根上是旧系统，~new/~old 清空；
#   B 正向交换中断（根无 Windows）→ rc 0，~old 回到根，~new 删除；
#   C 根仍有 Windows（只是暂存了新树）→ rc 3，**一个文件都不动**（绝不误删现系统）；
#   D 反向交换续做（phase=2）仍然可用 → rc 0。
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
SCRIPT=$ROOT/bootfiles/zjswap-undo.sh
WORK=$(mktemp -d 2>/dev/null || mktemp -d -t zjundo)
trap 'rm -rf "$WORK"' EXIT

STUB="$WORK/zjlog-stub.sh"
{
    echo 'say(){ printf "%s\n" "$*" >> "$ZJ_TEST_OUT"; }'
    echo 'logonly(){ :; }'
    echo 'zj_ascii(){ printf "%s" "$1"; }'
    echo 'mnt_ntfs(){ return 0; }'
} > "$STUB"

FAILED=0
fail(){ echo "FAIL: $*"; FAILED=1; }

# 造一个"根上系统 + ~old 旧系统"的最小盘面；$1=case 名，$2=root-has-windows(1/0)
mkcase(){
    _t="$WORK/$1"
    _m="$_t/disk"
    mkdir -p "$_m/Windows/System32"
    : > "$_m/Windows/System32/new-system.txt"
    mkdir -p "$_m/~old/Windows/System32"
    : > "$_m/~old/Windows/System32/old-system.txt"
    if [ "$2" = "0" ]; then
        # 正向交换已经把根上的 Windows 搬走
        rm -rf "$_m/Windows"
        mkdir -p "$_m/Users"
        : > "$_m/Users/leftover.txt"
    fi
    echo "$_m"
}

run_undo(){
    ZJ_TEST_OUT="$1/out.log" ZJ_LOG_SH="$STUB" \
        sh "$SCRIPT" /dev/null "$1" >"$1/run.log" 2>&1
    echo $?
}

# ── A：正常返回旧系统 ──
MA=$(mkcase A 1)
rc=$(run_undo "$MA")
[ "$rc" = "0" ] || fail "A: exit=$rc (want 0)"
[ -f "$MA/Windows/System32/old-system.txt" ] || fail "A: old system not restored at root"
[ -f "$MA/Windows/System32/new-system.txt" ] && fail "A: new system still at root"
[ -e "$MA/~old" ] && fail "A: ~old left behind"
[ -e "$MA/~new" ] && fail "A: ~new left behind"

# ── B：正向交换中途断电（根无 Windows，~new 带 .zj-done）──
MB=$(mkcase B 0)
mkdir -p "$MB/~new/Windows/System32"
: > "$MB/~new/Windows/System32/winload.exe"
printf '12345' > "$MB/~new/.zj-done"
rc=$(run_undo "$MB")
[ "$rc" = "0" ] || fail "B: exit=$rc (want 0; interrupted forward swap must be recoverable)"
[ -f "$MB/Windows/System32/old-system.txt" ] || fail "B: ~old was not restored to root"
[ -e "$MB/~new" ] && fail "B: ~new (staged new tree) was not discarded"
[ -e "$MB/~old" ] && fail "B: ~old left behind"
[ -f "$MB/Users/leftover.txt" ] || fail "B: unrelated root entry was lost"

# ── C：根上系统完好、只是暂存了新树 → 必须 refuse（一个文件都不动）──
MC=$(mkcase C 1)
mkdir -p "$MC/~new/Windows/System32"
: > "$MC/~new/Windows/System32/winload.exe"
printf '12345' > "$MC/~new/.zj-done"
rc=$(run_undo "$MC")
[ "$rc" = "3" ] || fail "C: exit=$rc (want 3; healthy root with staged ~new must be refused)"
[ -f "$MC/Windows/System32/new-system.txt" ] || fail "C: current system was modified"
[ -f "$MC/~old/Windows/System32/old-system.txt" ] || fail "C: ~old was modified"
[ -f "$MC/~new/.zj-done" ] || fail "C: staged ~new was modified"
grep -q "leftover ~new present" "$MC/out.log" || fail "C: refusal reason not logged"

# ── D：反向交换续做（phase=2）→ 仍按 L6/L7 收场 ──
MD=$(mkcase D 0)
mkdir -p "$MD/~new"
printf 'phase=2\n' > "$MD/~new/.zj-undo-phase"
rc=$(run_undo "$MD")
[ "$rc" = "0" ] || fail "D: exit=$rc (want 0; phase=2 resume must finish)"
[ -f "$MD/Windows/System32/old-system.txt" ] || fail "D: phase=2 resume did not restore ~old"
[ -e "$MD/~new" ] && fail "D: ~new left behind after phase=2 resume"

if [ "$FAILED" -ne 0 ]; then
    echo "test_zjswap_undo: FAIL"
    exit 1
fi
echo "test_zjswap_undo: PASS (4 cases)"
