#!/bin/bash
# P1-b（beta.7）回归：救援层契约可信度。
# 测试台实测（beta.6）：留 ~old、无暂存任务、无人值守 120s，init 自动选了 [2]
# 而不是 [3]。根因是 `find -maxdepth 4 | head -1` 抓到陈旧副本（先 ~old、后
# Program Files 软件目录的 logs），_ran=1 就判成"有任务"。
# 做法：从 bootfiles/alpine/init 抽取 conf_is_credible() 本体，喂伪造目录树，
# 断言只认"写者会写的位置"，残留副本一律拒绝。
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
INIT="$ROOT/bootfiles/alpine/init"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }
ok()   { echo "  ok: $*"; }

eval "$(sed -n '/^guid_norm(){/,/^}/p' "$INIT")"
type guid_norm >/dev/null 2>&1 || fail "cannot extract guid_norm() from $INIT"
eval "$(sed -n '/^conf_is_credible(){/,/^}/p' "$INIT")"
type conf_is_credible >/dev/null 2>&1 || fail "cannot extract conf_is_credible() from $INIT"

found_log=""
mk_conf(){
    # $1=目录 $2=action（默认 restore） $3=目标身份行（默认 target_offset=1048576）
    mkdir -p "$1"
    _act=${2:-restore}
    _tgt=${3-target_offset=1048576}
    {
        echo "action=$_act"
        [ -n "$_tgt" ] && echo "$_tgt"
    } > "$1/restore-task.conf"
}

# ① 交换残留 ~old：即使目录名是 ZJRESTORE、旁边还有 exe，也必须拒绝
c="$WORK/disk/~old/ZJRESTORE"
mk_conf "$c"; : > "$c/SysRecover.exe"
conf_is_credible "$c/restore-task.conf" && fail "case1: ~old stale copy was accepted"
ok "~old rejected"

# ② 交换残留 ~new：同上
c="$WORK/disk/~new/ZJRESTORE"
mk_conf "$c"; : > "$c/SysRecover.exe"
conf_is_credible "$c/restore-task.conf" && fail "case2: ~new stale copy was accepted"
ok "~new rejected"

# ③ 日志归档 logs：软件目录的 logs 子目录里即使有副本也必须拒绝
c="$WORK/disk/SysRecover"
mk_conf "$c"; : > "$c/SysRecover.exe"
mkdir -p "$c/logs"
cp "$c/restore-task.conf" "$c/logs/restore-task.conf"
conf_is_credible "$c/logs/restore-task.conf" && fail "case3: conf under logs/ was accepted"
ok "logs/ rejected"

# ④ 正常软件目录（conf 与 SysRecover.exe 同级）→ 接受
c="$WORK/disk/SysRecover"
: > "$c/SysRecover.exe"
conf_is_credible "$c/restore-task.conf" || fail "case4: software dir conf was rejected"
ok "software dir accepted"

# ⑤ 本产品备用写点 <数据盘>\ZJRESTORE → 接受（无 exe 也算）
c="$WORK/disk/ZJRESTORE"
mk_conf "$c"
conf_is_credible "$c/restore-task.conf" || fail "case5: ZJRESTORE dir conf was rejected"
ok "ZJRESTORE dir accepted"

# ⑥ 普通目录（无锚点）→ 拒绝（陈旧副本常落在这种地方）
c="$WORK/disk/Backup"
mk_conf "$c"
conf_is_credible "$c/restore-task.conf" && fail "case6: unanchored conf was accepted"
ok "unanchored rejected"

# ⑥b P0-3（2026-11-08 真机）：目标分区根 + conf 与 _zjresy*.log 同目录且
# target_offset 一致 → 接受（这是写者 task.cpp WriteRestoreLog 的合法布局，
# 旧规则只认 exe 旁边 / ZJRESTORE，把这份活契约误判成陈旧副本）。
c="$WORK/disk2"
mk_conf "$c"
{ echo "target_part_offset=1048576"; } > "$c/_zjresy-20261108.log"
conf_is_credible "$c/restore-task.conf" || fail "case6b: root conf beside matching task log was rejected"
ok "root conf beside matching log accepted"

# ⑥c 同目录有日志但 offset 对不上 → 仍然拒绝（陈旧/跨盘副本）
c="$WORK/disk3"
mk_conf "$c"
{ echo "target_part_offset=2097152"; } > "$c/_zjresy-stale.log"
conf_is_credible "$c/restore-task.conf" && fail "case6c: root conf with mismatched task log was accepted"
ok "root conf with mismatched log rejected"

# ⑦ action 非法 → 拒绝
c="$WORK/disk/SysRecover2"
mk_conf "$c" format; : > "$c/SysRecover.exe"
conf_is_credible "$c/restore-task.conf" && fail "case7: bad action was accepted"
ok "bad action rejected"

# ⑧ 缺 target 身份键（offset/guid 都没有）→ 拒绝
c="$WORK/disk/SysRecover3"
mk_conf "$c" restore ""
: > "$c/SysRecover.exe"
conf_is_credible "$c/restore-task.conf" && fail "case8: conf without target identity was accepted"
ok "missing target identity rejected"

# ⑨ 与主契约日志 target 身份不一致 → 拒绝（别的盘上的旧软件目录不能指错目标）
LOGB="/tmp/_zjresy-beta7-test-$$.log"
trap 'rm -rf "$WORK"; rm -f "$LOGB"' EXIT
{
    echo "target_part_offset=2097152"
    echo "target_guid=1111-2222"
} > "$LOGB"
found_log="$LOGB"
c="$WORK/disk/SysRecover4"
mk_conf "$c" restore "target_offset=1048576"; : > "$c/SysRecover.exe"
conf_is_credible "$c/restore-task.conf" && fail "case9: offset-mismatched conf was accepted"
ok "offset mismatch rejected"
c="$WORK/disk/SysRecover5"
mk_conf "$c" restore "target_offset=2097152
target_guid=3333-4444"; : > "$c/SysRecover.exe"
conf_is_credible "$c/restore-task.conf" && fail "case10: guid-mismatched conf was accepted"
ok "guid mismatch rejected"
c="$WORK/disk/SysRecover6"
mk_conf "$c" restore "target_offset=2097152
target_guid=1111-2222"; : > "$c/SysRecover.exe"
conf_is_credible "$c/restore-task.conf" || fail "case11: identity-matching conf was rejected"
ok "identity match accepted"

# ⑫ 静态断言：搜索逻辑必须"收集候选→逐个可信度过滤"，不能拿 find 第一个
grep -Fq 'find "$ZJ_M" -maxdepth 4 -name restore-task.conf > /tmp/zj-conf-cands' "$INIT" || \
    fail "case12: candidate collection missing in init"
grep -Fq 'if ! conf_is_credible "$_c"; then' "$INIT" || \
    fail "case12: candidate filter missing in init"
grep -Fq 'find "$ZJ_M" -maxdepth 4 -name restore-task.conf | head -1' "$INIT" && \
    fail "case12: old head -1 conf search still present in init"
ok "candidate loop in place"

echo "test_conf_credible: PASS (12 cases)"
