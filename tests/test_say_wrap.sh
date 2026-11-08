#!/bin/bash
# beta.12 回归（PIT-236）：屏幕行宽 ≤76、续行带 "<PREFIX>: " 前缀；日志保留完整原文。
# 说明：本用例只覆盖**纯 ASCII** 的运行期拼串（这是清点屏 dmi/PROBE/block devs 的
# 实际形态，可用 zj_fold 兜住）。中文行由构建期 C8 门禁保证字面量 ≤78 列（实测最宽
# 63）；"中文 + 超长 ASCII 字段"的运行期混拼无法在 busybox 里按显示列宽稳定切分，
# 属残留边界，见 docs/34 §20，不在此假装覆盖。
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }
ok()   { echo "  ok: $*"; }

export ZJPREFIX=SR
export LOG="$WORK/zjrestore.log"
# 用例 ⑥ 验的是"串口控制台下原样输出 UTF-8"（VM -nographic 的形态）；
# 真机 VT 的字模失败路径见用例 ⑧⑨。
export ZJ_CONSOLE_KIND=serial
. "$ROOT/bootfiles/zjlog.sh" || fail "cannot source zjlog.sh"
type zj_fold >/dev/null 2>&1 || fail "zj_fold not defined"

# say 一次：控制台（stderr）落 $WORK/console，日志落 $LOG
say_cap(){ # $1 = message
    : > "$LOG"
    say "$1" 2> "$WORK/console"
}

width_ok(){ # $1 = file
    awk 'length($0) > 76 { bad = 1 } END { exit bad ? 1 : 0 }' "$1"
}
prefix_ok(){ # $1 = file
    awk 'substr($0, 1, 4) != "SR: " { bad = 1 } END { exit bad ? 1 : 0 }' "$1"
}

# ① PROBE 长行（清点屏同款）：折成多行、每行 ≤76、续行有前缀
long='PROBE root=[$RECYCLE.BIN,System Volume Information,Windows,Program Files,ProgramData,boot,bootmgr,EFI,images,old_userfile.txt,~new,~old,ZJRESTORE,restore-task.conf]'
say_cap "$long"
width_ok "$WORK/console" || fail "case1: line >76 columns"
prefix_ok "$WORK/console" || fail "case1: continuation missing prefix"
n=$(wc -l < "$WORK/console")
[ "$n" -ge 2 ] || fail "case1: expected wrapped line, got $n line(s)"
ok "PROBE long line wrapped, every line prefixed"

# ② block devs（多盘多分区）：同样逐行 ≤76 + 前缀
many='block devs: sda sda1 sda2 sda3 sdb sdb1 sdc sdc1 sdd sdd1 nvme0n1 nvme0n1p1 nvme0n1p2 nvme0n1p3 mmcblk0 mmcblk0p1 mmcblk0p2 sr0'
say_cap "$many"
width_ok "$WORK/console" || fail "case2: line >76 columns"
prefix_ok "$WORK/console" || fail "case2: continuation missing prefix"
ok "block devs list wrapped per line"

# ③ 无分隔符长 token：硬切，仍然 ≤76 且每行有前缀
token='AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA'
say_cap "$token"
width_ok "$WORK/console" || fail "case3: hard cut >76"
prefix_ok "$WORK/console" || fail "case3: continuation missing prefix"
ok "unbreakable token hard-cut, prefixed"

# ④ 短行不折、不加多余换行
say_cap 'image: /dev/sda3/images/test.wim'
n=$(wc -l < "$WORK/console")
[ "$n" = "1" ] || fail "case4: short line expected 1, got $n"
[ "$(< "$WORK/console")" = "SR: image: /dev/sda3/images/test.wim" ] || fail "case4: short line changed"
ok "short line unchanged (no extra newline)"

# ⑤ 日志保留完整原文（不被屏幕折行吃掉）
say_cap "$long"
grep -Fq "$long" "$LOG" || fail "case5: full original missing from log"
n=$(wc -l < "$LOG")
[ "$n" = "1" ] || fail "case5: log expected 1 line, got $n"
ok "log keeps full original on one line"

# ⑥ 中文行（≤78 列）不参与 ASCII 折行、不被拆坏
zh='救援层正在清点磁盘分区，请稍候，不要断电或拔掉硬盘'
say_cap "$zh"
n=$(wc -l < "$WORK/console")
[ "$n" = "1" ] || fail "case6: CJK line expected 1, got $n"
[ "$(< "$WORK/console")" = "SR: $zh" ] || fail "case6: CJK line changed"
ok "CJK line untouched"

# ⑦ 静态断言：口径是 76（与进度条一致），且日志行不带折行
grep -q 'awk -v w=76' "$ROOT/bootfiles/zjlog.sh" || fail "case7: wrap width is not 76"
grep -q "echo \"\$(date" "$ROOT/bootfiles/zjlog.sh" || fail "case7: log line missing"
ok "static: width 76, log path unchanged"

# ⑧ beta.14（PIT-238 重做）：VT + 字模没装上 → 绝不把非 ASCII 送屏；整行中文
# 压成 `.`（固定占位符），**不许出现问号/豆腐块/空白**
ZJ_CONSOLE_KIND=vt ZJ_FONT_FORCE_FAIL=1 say "$zh" 2> "$WORK/console"
if LC_ALL=C grep -q '[^ -~]' "$WORK/console"; then
    fail "case8: non-ASCII reached a VT console without a loaded font"
fi
grep -Fq '?' "$WORK/console" && fail "case8: question mark fallback is forbidden"
[ "$(< "$WORK/console")" = "SR: ." ] || fail "case8: expected 'SR: .', got '$(< "$WORK/console")'"
ok "VT without font: '.' placeholder, no '?'"

# ⑨ say2：字模失败时只打英文版，且英文版是纯 ASCII
ZJ_CONSOLE_KIND=vt ZJ_FONT_FORCE_FAIL=1 say2 "中文提示" "English fallback" 2> "$WORK/console"
[ "$(< "$WORK/console")" = "SR: English fallback" ] || fail "case9: say2 did not fall back to English"
if LC_ALL=C grep -q '[^ -~]' "$WORK/console"; then
    fail "case9: say2 fallback contains non-ASCII"
fi
ok "say2 English fallback on font failure"

# ⑩ 动态中文（镜像名"不忘初心"）：无字形串压成 `.`，日志留原文
: > "$LOG"
dyn=$(ZJ_CONSOLE_KIND=vt zj_ascii 'C:\images\不忘初心的win10.wim')
[ "$dyn" = 'C:\images\.win10.wim' ] || fail "case10: zj_ascii got '$dyn'"
say_dyn 'image=' 'C:\images\不忘初心的win10.wim' 2> "$WORK/console"
if LC_ALL=C grep -q '[^ -~]' "$WORK/console"; then
    fail "case10: dynamic CJK reached the screen"
fi
grep -Fq '?' "$WORK/console" && fail "case10: question mark fallback is forbidden"
[ "$(< "$WORK/console")" = 'SR: image=C:\images\.win10.wim' ] \
    || fail "case10: got '$(< "$WORK/console")'"
grep -Fq '不忘初心' "$LOG" || fail "case10: log lost the original dynamic value"
ok "dynamic CJK sanitized to '.', kept in log"

# ⑪ 覆盖判定：字库内汉字映射成 PUA(0xEE) 后不算"缺字形"；库外汉字必须命中
pua=$(printf '\356\200\200')          # U+E000（字模内汉字映射目标）
LC_ALL=C zj_has_unmapped "SR: ${pua}" && fail "case11: PUA flagged as unmapped"
LC_ALL=C zj_has_unmapped 'SR: 忘' || fail "case11: out-of-font CJK not flagged"
ok "coverage check: PUA covered, out-of-font CJK flagged"

# ⑫ PIT-247（2026-11-08 真机）：BIOS 走 vgacon（无 fbcon）时不许启用中文。
# 真机现象：UEFI 正常、BIOS 全成白底问号；根因是 setfont 的返回码不代表
# PUA 字形真的可寻址。这里钉住"没有 fb0 就不加载中文、rc=7、FONT 行可见"。
ZJ_CONSOLE_KIND=vt ZJ_FBCON=0 zj_font_load 2> "$WORK/console"
rc=$?
[ "$rc" != "0" ] || fail "case12: vgacon must not report font OK"
[ "$ZJ_FONT_RC" = "7" ] || fail "case12: expected rc=7, got $ZJ_FONT_RC"
grep -Fq 'state=skip rc=7' /tmp/zj-font.status 2>/dev/null \
    || fail "case12: font status not recorded (state=skip rc=7)"
ZJ_CONSOLE_KIND=vt ZJ_FBCON=0 say2 "中文提示" "English fallback" 2> "$WORK/console"
[ "$(< "$WORK/console")" = "SR: English fallback" ] \
    || fail "case12: vgacon say2 not English"
ok "vgacon: font skipped rc=7, status recorded, say2 English"

echo "test_say_wrap: PASS (12 cases)"
