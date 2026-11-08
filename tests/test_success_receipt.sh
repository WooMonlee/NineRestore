#!/bin/bash
# P3a（beta.7）回归：returnold 成功必须留"成功回执"。
# 测试台实测（beta.6）：returnold 成功后 C:/D:/F: 根均无 ZJRESTORE-status.txt，
# 状态只活在内存里（returnold 成功块只 persist_log 就 exit，EXIT trap 的 OK 分支
# 只写软件目录 + ESP；BIOS 无 ESP）。
# 做法：抽取 write_success_receipt() + bb_status_text()，断言两处直写点、全失败 WARN，
# 并静态断言 returnold 成功块确实调用了该 helper 后才 exit 0。
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
LITE="$ROOT/bootfiles/zjrestore-lite.sh"
WORK=$(mktemp -d)
OUT="$WORK/out"
: > "$OUT"
trap 'rm -rf "$WORK"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }
ok()   { echo "  ok: $*"; }
say()  { printf '%s\n' "$*" >> "$OUT"; }

# bb_status_text 依赖的全局量（set -u 下必须全部就位）
RESULT=OK
STEP=done
ZJENTRY=""
BOOTFIX_OK=""
ESP_OK=""
TARGET_DEV=""
IMAGE_PATH=""

eval "$(sed -n '/^bb_status_text(){/,/^}/p' "$LITE")"
eval "$(sed -n '/^write_success_receipt(){/,/^}/p' "$LITE")"
type bb_status_text >/dev/null 2>&1 || fail "cannot extract bb_status_text() from $LITE"
type write_success_receipt >/dev/null 2>&1 || fail "cannot extract write_success_receipt() from $LITE"

# ① 软件目录 + 兜底窝都可写 → 两处都落盘，内容含 result=OK / step=done，无 WARN
SD_REL="ZJ"
SOFT_MNT="$WORK/disk"; mkdir -p "$WORK/disk/ZJ/logs"
BB_HOME="$WORK/home"; mkdir -p "$BB_HOME"
: > "$OUT"
write_success_receipt
[ -f "$WORK/disk/ZJ/logs/ZJRESTORE-status.txt" ] || fail "case1: soft-dir status missing"
[ -f "$WORK/home/ZJRESTORE-status.txt" ] || fail "case1: bb-home status missing"
grep -q '^result=OK$' "$WORK/disk/ZJ/logs/ZJRESTORE-status.txt" || fail "case1: result!=OK"
grep -q '^step=done$' "$WORK/disk/ZJ/logs/ZJRESTORE-status.txt" || fail "case1: step!=done"
grep -q 'WARN: status receipt NOT written' "$OUT" && fail "case1: unexpected WARN"
ok "both surfaces written"

# ② 软件目录不可用 → 兜底窝仍要落盘，且不应 WARN
SOFT_MNT=""
BB_HOME="$WORK/home2"; mkdir -p "$BB_HOME"
: > "$OUT"
write_success_receipt
[ -f "$WORK/home2/ZJRESTORE-status.txt" ] || fail "case2: bb-home status missing"
grep -q 'WARN: status receipt NOT written' "$OUT" && fail "case2: WARN despite bb-home success"
ok "bb-home only"

# ③ 两处都不可用 → 必须响亮 WARN（不能静默）
SOFT_MNT=""
BB_HOME=""
: > "$OUT"
write_success_receipt || fail "case3: helper returned non-zero"
grep -q 'WARN: status receipt NOT written' "$OUT" || fail "case3: missing WARN"
ok "all-failed WARN"

# ④ 静态断言：returnold 成功块（RETURN-OLD DONE 之后）必须先写回执再 exit 0
blk=$(sed -n '/RETURN-OLD DONE/,/^    exit 0$/p' "$LITE")
[ -n "$blk" ] || fail "case4: cannot locate returnold success block"
printf '%s\n' "$blk" | grep -q 'write_success_receipt' || \
    fail "case4: returnold success does not call write_success_receipt"
printf '%s\n' "$blk" | grep -q 'bb_esp_drop' || \
    fail "case4: returnold success does not write ESP"
ok "returnold success uses shared receipt"

# ⑤ 静态断言：apply 成功路径也用同一 helper（口径一致，不许两套实现）
n=$(grep -c '^[[:space:]]*write_success_receipt[[:space:]]*$' "$LITE")
[ "$n" -ge 2 ] || fail "case5: expected >=2 call sites, got $n"
ok "apply + returnold share helper"

echo "test_success_receipt: PASS (5 cases)"
