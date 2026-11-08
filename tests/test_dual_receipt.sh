#!/bin/bash
# beta.9 回归（测试台 A1）：成功回执必须**两处**真实存在。
# 现象：returnold 后软件目录 F:\ZJ8\logs\ZJRESTORE-status.txt 有，D:\ZJRESTORE-logs\
# 完全不存在，救援日志也没有 "fallback log home" —— 根因是 claim_bb_home 一旦
# find_soft_dir 成功就 return 0，BB_HOME 恒为空。
# 做法：抽取 bb_home_claim_on() + claim_bb_home()，用 stub 跑四种场景。
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
LITE="$ROOT/bootfiles/zjrestore-lite.sh"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }
ok()   { echo "  ok: $*"; }

eval "$(sed -n '/^bb_home_claim_on(){/,/^}/p' "$LITE")"
type bb_home_claim_on >/dev/null 2>&1 || fail "cannot extract bb_home_claim_on()"
eval "$(sed -n '/^claim_bb_home(){/,/^}/p' "$LITE")"
type claim_bb_home >/dev/null 2>&1 || fail "cannot extract claim_bb_home()"

# 每个用例在子 shell 里重建 stub；$1=soft_ok(0/1) $2=候选设备（空格分隔） $3=SD_REL
run_claim(){ # $1=soft_ok $2=devs $3=SD_REL → 输出 "BB_HOME|日志"
    _soft=$1; _devs=$2; _sd=$3
    (
        BB_HOME=""; SOFT_MNT=""; SD_REL="$_sd"; MIRROR_STARTED=0
        _SAY="$WORK/say.$RANDOM"
        : > "$_SAY"
        say(){ echo "$*" >> "$_SAY"; }
        start_log_mirror(){ :; }
        save_diag(){ :; }
        find_soft_dir(){
            [ "$_soft" = "1" ] || return 1
            SOFT_MNT=/tmp/zj_soft
            return 0
        }
        pick_bb_home_devs(){ printf '%s\n' $_devs; }
        mnt_dev(){ mkdir -p "$2"; return 0; }
        umount(){ :; }
        claim_bb_home
        echo "$BB_HOME|$(tr '\n' ';' < "$_SAY")"
        rm -f "$_SAY"
    )
}

# ① 软件目录可用 + 另一块内置盘 → 兜底窝落在另一块盘（A1 现场 F: 软目录 + D: 兜底）
out=$(run_claim 1 "/dev/sdb1" "ZJ8")
home=${out%%|*}; log=${out#*|}
[ "$home" = "/tmp/zj_home/ZJRESTORE-logs" ] || fail "case1: BB_HOME='$home'"
case "$log" in *"fallback log home: /tmp/zj_home -> ZJRESTORE-logs/ (separate volume"*) ;; *) fail "case1: missing fallback log line: $log" ;; esac
ok "software dir + second volume -> fallback claimed"

# ② 软件目录可用但只有这一块盘（单盘整盘为目标）→ 同卷不同路径仍认领
out=$(run_claim 1 "" "ZJ8")
home=${out%%|*}; log=${out#*|}
[ "$home" = "/tmp/zj_soft/ZJRESTORE-logs" ] || fail "case2: BB_HOME='$home'"
case "$log" in *"same volume as software dir, different path"*) ;; *) fail "case2: missing same-volume log: $log" ;; esac
ok "single volume -> same-volume different-path fallback"

# ③ 同卷同路径（software_dir 恰好叫 ZJRESTORE-logs）→ 显式去重并记日志
out=$(run_claim 1 "" "ZJRESTORE-logs")
home=${out%%|*}; log=${out#*|}
[ -z "$home" ] || fail "case3: expected dedupe (no BB_HOME), got '$home'"
case "$log" in *"skipped (same volume+path as software dir"*) ;; *) fail "case3: missing dedupe log: $log" ;; esac
ok "same volume+path -> dedupe with log"

# ④ 软件目录找不到（原有行为不变）→ 兜底窝照旧认领
out=$(run_claim 0 "/dev/sdb1" "ZJ8")
home=${out%%|*}; log=${out#*|}
[ "$home" = "/tmp/zj_home/ZJRESTORE-logs" ] || fail "case4: BB_HOME='$home'"
case "$log" in *"software dir not found"*) ;; *) fail "case4: missing soft-not-found log: $log" ;; esac
ok "software dir absent -> fallback unchanged"

# ⑤ 静态断言：成功回执写两处（软目录 logs/ + BB_HOME 根）
body=$(sed -n '/^write_success_receipt(){/,/^}/p' "$LITE")
[ "$(printf '%s' "$body" | grep -c 'ZJRESTORE-status.txt')" -ge 2 ] || \
    fail "case5: write_success_receipt must write two receipts"
printf '%s' "$body" | grep -Fq 'WARN: status receipt $_rcpt/2 written' || \
    fail "case5: missing partial-receipt warning"
ok "success receipt writes soft-dir + fallback"

# ⑥ 静态断言：claim 不再"找到软件目录就 return"（A1 根因）
printf '%s' "$(sed -n '/^claim_bb_home(){/,/^}/p' "$LITE")" | \
    grep -A2 'if find_soft_dir; then' | grep -q 'return 0' && \
    fail "case6: claim_bb_home still returns early after find_soft_dir"
ok "claim does not early-return on software dir"

echo "test_dual_receipt: PASS (6 cases)"
