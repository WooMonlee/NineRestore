#!/bin/bash
# P0（2026-11-08 真机）回归：fallback 候选的身份判据（GUID 优先 + 序列号降级）。
#
# 背景：序列号两侧呈现方式不受我们控制（同一台机器的另一块盘 Windows 记
# `VMWare NVME_0000_00000001.`、Linux 侧长度都不同），去空白+转小写救不了。
# 判据因此改成：
#   ① GUID 双方都可取 → GUID 相等即认，序列号不一致只记警告；
#   ② GUID 取不到 → 序列号 + 位置（offset/size），序列号不一致则拒绝；
#   ③ 任务文件两项都空 → 只能按位置，日志明写"身份不可核对"。
#
# 做法：从 bootfiles/alpine/init 抽取纯函数 identity_verdict()，直接喂四象限；
# 再用静态断言把"拒绝 → _task_unreadable / _ran=0"的接线钉死。
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
INIT="$ROOT/bootfiles/alpine/init"

fail() { echo "FAIL: $*" >&2; exit 1; }
ok() { echo "  ok: $*"; }

eval "$(sed -n '/^identity_verdict(){/,/^}/p' "$INIT")"
type identity_verdict >/dev/null 2>&1 || fail "cannot extract identity_verdict() from $INIT"

# $1=want_serial $2=have_serial $3=want_guid $4=have_guid；$5=期望
expect_verdict() {
    _got=$(identity_verdict "$1" "$2" "$3" "$4")
    [ "$_got" = "$5" ] || fail "verdict(w=$1,h=$2,wg=$3,hg=$4) = $_got (want $5)"
    ok "$5"
}

# ── 四象限：GUID 有/无 × 序列号一致/不一致 ──
# Q1 GUID 有 + 序列号一致 → 认
expect_verdict 9fc4abcd 9fc4abcd 1111 1111 ok-guid
# Q2 GUID 有 + 序列号不一致 → 认，只记警告（序列号不再是判据）
expect_verdict vmware nvme_0000_00000001. 1111 1111 ok-guid-serial-diff
# Q3 GUID 无 + 序列号一致 → 降级认
expect_verdict vmware vmware "" "" ok-serial
# Q4 GUID 无 + 序列号不一致 → 拒绝
expect_verdict vmware nvme_0000_00000001.  "" "" reject-serial

# ── 审查用例 1：serial/guid 都不匹配 → 拒绝（→ 任务读不到）──
expect_verdict vmware other 1111 2222 reject-guid
# 任务带 guid、候选侧取不到 GUID 且序列号也不匹配 → 序列号分支拒绝
expect_verdict vmware other 1111 "" reject-serial
# 任务带身份、候选侧两项都取不到 → 拒绝（不许伪装成弱接受）
expect_verdict vmware "" 1111 "" unverifiable-cand

# ── 审查用例 2：serial 匹配、guid 缺失 → 认任务 ──
expect_verdict vmware vmware 1111 "" ok-serial

# ── 任务文件两项都空（真机确实取不到）→ 身份不可核对 ──
expect_verdict "" "" "" "" unverifiable

# ── 归一化口径必须在两侧同一套（去空白 + 转小写）：判据本身不做归一化，
#    以免测试台与真机的输入口径漂移；这里钉住"调用方已归一化"的契约。──
expect_verdict "vmware nvme_0000" "vmware nvme_0000" "" "" ok-serial
expect_verdict "vmware nvme_0000" "VMWARE NVME_0000" "" "" reject-serial

# ── 静态接线：fallback 拒绝 → _task_unreadable / _ran=0（不走 triage、不是失败菜单）──
grep -Fq 'case "$(identity_verdict "$_want_serial" "$_have_serial" "$_want_guid" "$_have_guid")"' "$INIT" \
    || fail "fallback 分支没有调用 identity_verdict()"
n=$(grep -c 'identity_verdict "$_want_serial"' "$INIT")
[ "$n" -ge 2 ] || fail "identity_verdict() 调用点只有 $n 处（主匹配 + fallback 应各一处）"
grep -Fq '_fb_rejects=$((_fb_rejects + 1))' "$INIT" \
    || fail "fallback 拒绝没有累计 _fb_rejects"
grep -Fq 'ok-guid|ok-guid-serial-diff|ok-serial) _fb_id_ok=1' "$INIT" \
    || fail "fallback 没有把 ok-serial（serial 匹配、guid 缺失）当成认任务"
grep -Fq 'if [ "${_fb_rejects:-0}" -gt 0 ] && [ -z "$_tmp_task_log" ]; then' "$INIT" \
    || fail "_fb_rejects>0 没有和 /tmp 可读日志联判"
grep -Fq '_task_rejected=1' "$INIT" || fail "_task_rejected 未置位"
grep -Fq 'if [ "$_ran" = "0" ] && { [ "$_task_seen" = "1" ] || [ "$_conf_seen" = "1" ]; }; then' "$INIT" \
    || fail "拒绝后没有按 '任务见过但不可读' 置 _task_unreadable"
grep -Fq '_why=task-unreadable' "$INIT" || fail "黑匣子没有把拒绝任务记成 task-unreadable"
grep -Fq '[ "$_zb_hit" = "1" ] && [ "$_task_rejected" = "0" ]' "$INIT" \
    || fail "被拒绝的任务没有从 triage 三选一里排除"
ok "wiring: reject -> _task_rejected=1 -> _ran=0 -> task-unreadable, no triage"

echo "test_fallback_identity: PASS (verdict quadrants + wiring)"
