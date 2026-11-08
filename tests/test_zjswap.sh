#!/bin/sh
# H1 回归（P0 数据安全）：zjswap.sh 交换失败必须 fail-closed。
#
# 做法：不接触真实磁盘 —— 用 ZJ_SWAP_TEST_ROOT 让脚本在临时目录上演练，
#      用 ZJ_SWAP_FAIL_OLD 注入「某个旧项 move 必失败」（脚本内建的测试钩子）。
#
# 断言（任一不过即非零退出）：
#   A 注入旧项 move 失败 → 退出码 4；~new 保留；已搬走的旧项被回滚回根；无 ~old 残留
#   B 全部成功 → 退出码 0；~new 被删除；~old 保留（H4.5 可回退）
#   C 缺 .zj-done → 退出码 2；旧树一根汗毛都没动
#   D 新树缺 winload → 退出码 5；~new 保留（不删现场）
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
SCRIPT=${ZJ_SWAP_SCRIPT:-$ROOT/bootfiles/zjswap.sh}
LOG_SH=$ROOT/bootfiles/zjlog.sh
FAILED=0

fail() {
    echo "FAIL: $*"
    FAILED=1
}

# 造一个最小旧树 + 新树；回显测试根目录
mkcase() {
    _t=$(mktemp -d)
    _m=$_t/disk
    mkdir -p "$_m/Windows/System32" "$_m/Users"
    : > "$_m/Windows/System32/ntoskrnl.exe"
    : > "$_m/Users/a.txt"
    mkdir -p "$_m/~new/Windows/System32"
    : > "$_m/~new/Windows/System32/winload.exe"
    printf '12345' > "$_m/~new/.zj-done"
    echo "$_t"
}

# 跑一次交换；第 1 个参数是测试根，第 2 个是注入失败的旧项名（可空）
run_swap() {
    ZJ_SWAP_TEST_ROOT="$1/disk" \
    ZJ_LOG_SH="$LOG_SH" \
    ZJ_SWAP_FAIL_OLD="${2:-}" \
        sh "$SCRIPT" /dev/null "$1/disk" >"$1/out.log" 2>&1
    echo $?
}

# ── A：旧项 move 失败 → 必须非零退出 + 回滚 + 保留 ~new ──
TA=$(mkcase); MA=$TA/disk
rcA=$(run_swap "$TA" Users)
[ "$rcA" = "4" ] || fail "A: exit=$rcA (want 4)"; 
[ -e "$MA/~new/.zj-done" ] || fail "A: ~new was removed (must be kept)"
[ -e "$MA/Windows" ] || fail "A: rollback did not restore Windows to root"
[ -e "$MA/Users" ] || fail "A: Users vanished"
[ -e "$MA/~old" ] && fail "A: ~old left behind after rollback"
[ -e "$MA/Windows/System32/winload.exe" ] && fail "A: new tree leaked into root"

# ── B：正常交换 → 退出 0，~new 删除，~old 保留 ──
TB=$(mkcase); MB=$TB/disk
rcB=$(run_swap "$TB" "")
[ "$rcB" = "0" ] || fail "B: exit=$rcB (want 0)
$(cat "$TB/out.log" 2>/dev/null)"
[ -e "$MB/~new" ] && fail "B: ~new not removed after success"
[ -e "$MB/Windows/System32/winload.exe" ] || fail "B: new winload missing at root"
[ -e "$MB/~old/Windows" ] || fail "B: old system not preserved in ~old (H4.5)"

# ── C：缺完成标记 → 退出 2，旧树原样 ──
TC=$(mkcase); MC=$TC/disk
rm -f "$MC/~new/.zj-done"
rcC=$(run_swap "$TC" "")
[ "$rcC" = "2" ] || fail "C: exit=$rcC (want 2)"
[ -e "$MC/Windows/System32/ntoskrnl.exe" ] || fail "C: old tree was touched"

# ── D：交换后校验失败（新树缺 winload）→ 退出 5，~new 保留 ──
TD=$(mkcase); MD=$TD/disk
rm -f "$MD/~new/Windows/System32/winload.exe"
rcD=$(run_swap "$TD" "")
[ "$rcD" = "5" ] || fail "D: exit=$rcD (want 5)
$(cat "$TD/out.log" 2>/dev/null)"
[ -e "$MD/~new" ] || fail "D: ~new removed although verify failed"

# ── E：根上已有 ~old（上次中断/早期退出）→ 退出 3，旧树一根汗毛都没动 ──
# 审查用例：lite 重跑在 `~old` 无 Windows 的早期中断现场必须被拒绝，
# 不许把半截备份再换一遍（门在 zjswap.sh L1，不在 init）。
TE=$(mkcase); ME=$TE/disk
mkdir -p "$ME/~old"
rcE=$(run_swap "$TE" "")
[ "$rcE" = "3" ] || fail "E: exit=$rcE (want 3)"
[ -e "$ME/Windows/System32/ntoskrnl.exe" ] || fail "E: old tree was touched"
[ -e "$ME/~new/.zj-done" ] || fail "E: ~new was removed"
grep -q 'leftover ~old present' "$TE/out.log" || fail "E: refusal reason missing"

for _t in "$TA" "$TB" "$TC" "$TD" "$TE"; do rm -rf "$_t"; done

if [ "$FAILED" -ne 0 ]; then
    echo "test_zjswap: FAIL"
    exit 1
fi
echo "test_zjswap: PASS (A fail-closed/rollback, B happy path, C marker gate, D post-verify, E leftover ~old)"
exit 0
