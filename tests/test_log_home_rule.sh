#!/bin/bash
# beta.8 回归（测试台复验问题 2）：兜底日志窝的**确定性落点**。
# 现象：同一版本 1.0.0.7，兜底窝先后出现在 C:\ZJRESTORE-logs、一块 2GB 小盘、Data 卷。
# 规则（与分区枚举顺序无关）：内置优先 → 容量≥2GiB → 容量降序 → 设备名升序。
# 做法：抽取 pick_bb_home_devs()，每个用例用独立 stub 跑一遍。
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
LITE="$ROOT/bootfiles/zjrestore-lite.sh"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }
ok()   { echo "  ok: $*"; }

eval "$(sed -n '/^pick_bb_home_devs(){/,/^}/p' "$LITE")"
type pick_bb_home_devs >/dev/null 2>&1 || fail "cannot extract pick_bb_home_devs()"

# 每个用例在子 shell 里重建 stub：stub 用 case 硬编码，避免数组/枚举副作用。
pick_case(){ # $1=parts $2=target $3=size 映射 "dev:size;..." $4=removable "dev:1;..."
    _p=$1; _t=$2; _SIZEMAP=$3; _REMMAP=$4
    (
        PARTS=$_p; TARGET_DEV=$_t
        list_parts(){ printf '%s\n' $PARTS; }
        is_part_dev(){ return 0; }
        is_mounted(){ return 1; }
        dev_size(){
            _rest=$_SIZEMAP; _v=0
            while [ -n "$_rest" ]; do
                _it=${_rest%%;*}; _rest=${_rest#*;}
                [ "$_rest" = "$_it" ] && _rest=""
                case "$_it" in
                    "$1:"*) _v=${_it#*:}; break ;;
                esac
            done
            echo "$_v"
        }
        bb_part_removable(){
            _rest=$_REMMAP; _v=0
            while [ -n "$_rest" ]; do
                _it=${_rest%%;*}; _rest=${_rest#*;}
                [ "$_rest" = "$_it" ] && _rest=""
                case "$_it" in
                    "$1:"*) _v=${_it#*:}; break ;;
                esac
            done
            echo "$_v"
        }
        pick_bb_home_devs | head -1
    )
}

# ① 内置 400GB 优先于可移动 160GB/800GB；<2GiB 与目标盘不参选
out=$(pick_case "/dev/sda1 /dev/sda2 /dev/sdb1 /dev/sdc1 /dev/sdd1 /dev/sde1" "/dev/sda1" \
    "/dev/sda1:3000000000;/dev/sda2:400000000000;/dev/sdb1:160000000000;/dev/sdc1:2147483648;/dev/sdd1:1000000000;/dev/sde1:800000000000" \
    "/dev/sda2:0;/dev/sdb1:1;/dev/sdc1:0;/dev/sdd1:0;/dev/sde1:1")
[ "$out" = "/dev/sda2" ] || fail "case1: expected /dev/sda2, got '$out'"
ok "internal largest first (removable ignored)"

# ② 同等条件下按容量降序
out=$(pick_case "/dev/sda1 /dev/sdb1 /dev/sdc1" "" \
    "/dev/sda1:4000000000;/dev/sdb1:10000000000;/dev/sdc1:6000000000" "")
[ "$out" = "/dev/sdb1" ] || fail "case2: expected /dev/sdb1, got '$out'"
ok "size desc"

# ③ 容量相同 → 设备名升序（与枚举顺序无关）
out=$(pick_case "/dev/sdc1 /dev/sdb1 /dev/sda1" "" \
    "/dev/sda1:3000000000;/dev/sdb1:3000000000;/dev/sdc1:3000000000" "")
[ "$out" = "/dev/sda1" ] || fail "case3: expected /dev/sda1, got '$out'"
ok "tie-break by device name"

# ④ 目标盘被跳过
out=$(pick_case "/dev/sda1 /dev/sdb1" "/dev/sda1" \
    "/dev/sda1:500000000000;/dev/sdb1:3000000000" "")
[ "$out" = "/dev/sdb1" ] || fail "case4: expected /dev/sdb1, got '$out'"
ok "target excluded"

# ⑤ 全是小盘 → 无候选（不认领 2GB U 盘）
out=$(pick_case "/dev/sda1 /dev/sdb1" "" \
    "/dev/sda1:2000000000;/dev/sdb1:1000000000" "")
[ -z "$out" ] || fail "case5: expected no candidate, got '$out'"
ok "tiny volumes rejected"

# ⑥ 静态断言：两处 claim 都用确定性候选（不许退回 list_parts 枚举）
n=$(grep -c '$(pick_bb_home_devs)' "$LITE")
[ "$n" -ge 2 ] || fail "case6: expected >=2 call sites, got $n"
ok "claim + late-drop both use picker"

# ⑦ 静态断言：returnold 与 staged 同规则——程序在目标盘时 software_dir 落到数据盘
for f in ops_staged.cpp ops_returnold.cpp; do
    s="$ROOT/src/app/$f"
    grep -q 'onTarget' "$s" || fail "case7: $f missing onTarget rule"
    grep -Fq 'SameDrive(exLetter, tgtLetter)' "$s" || fail "case7: $f missing SameDrive check"
    grep -Fq 'FindDataDrive' "$s" || fail "case7: $f missing FindDataDrive fallback"
done
grep -Fq 'software dir unusable for logs (on target/removable)' "$ROOT/src/app/ops_returnold.cpp" || \
    fail "case7: ops_returnold missing degraded log"
ok "staged + returnold share software-dir rule"

echo "test_log_home_rule: PASS (7 cases)"
