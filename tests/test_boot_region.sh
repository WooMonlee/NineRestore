#!/usr/bin/env bash
# 单测：BIOS 分区引导区判据 / 重写（zj_repair_boot_region）
#
# 背景：Linux 侧曾只用「扇区 1 有任意非零字节」判定引导区完好，代码段被写坏
# 也判为完好（日志还打 kept intact）→ 看着修好了、BIOS 实际起不来。
# 现在判据与 Windows 侧 EnsureNtfsBootRegion 等价：0x54 必须是 FA 33 C0 8E，
# 且扇区 1 起的续段非全零；任一不满足就重写。
#
# 这里把脚本里的函数块原样抽出来跑，不依赖真实磁盘，也不需要 root。
set -u
cd "$(dirname "$0")/.." || exit 1

SCRIPT=bootfiles/zjrestore-lite.sh
TMP=$(mktemp -d) || exit 1
trap 'rm -rf "$TMP"' EXIT

PASS=0
FAIL=0
ok()  { PASS=$((PASS + 1)); echo "  ok   $1"; }
bad() { FAIL=$((FAIL + 1)); echo "  FAIL $1"; }

LOGF="$TMP/say.log"
say() { printf '%s\n' "$*" >> "$LOGF"; }
# shellcheck disable=SC1090
eval "$(sed -n '/^    # >>> zj-boot-region/,/^    # <<< zj-boot-region/p' "$SCRIPT")"
if ! command -v zj_repair_boot_region >/dev/null 2>&1; then
    echo "  FAIL 无法从 $SCRIPT 抽出 zj_repair_boot_region"
    exit 1
fi

# blob：代码段 426B（首 4 字节 = fa33c08e），续段 4096B（非零）
{ printf '\xfa\x33\xc0\x8e'; head -c 422 /dev/zero | tr '\0' 'A'; } > "$TMP/code.bin"
head -c 4096 /dev/zero | tr '\0' 'B' > "$TMP/cont.bin"

newdev() { head -c 8192 /dev/zero > "$1"; }
put54()  { printf '\xfa\x33\xc0\x8e' | dd of="$1" bs=1 seek=84 conv=notrunc 2>/dev/null; }
put54x() { printf '\x13\x37\x00\x00' | dd of="$1" bs=1 seek=84 conv=notrunc 2>/dev/null; }
put1()   { printf 'X' | dd of="$1" bs=1 seek=512 conv=notrunc 2>/dev/null; }
g54()    { dd if="$1" bs=1 skip=84 count=4 2>/dev/null | od -An -tx1 | tr -d ' \n'; }
s1()     { dd if="$1" bs=512 skip=1 count=1 2>/dev/null | od -An -tx1 | tr -d ' \n'; }
h()      { od -An -tx1 "$1" 2>/dev/null | tr -d ' \n'; }

# ---- 用例 1：0x54 正确 + 续段非零 → 跳过（文件一个字节都不动）----
D="$TMP/a.bin"; newdev "$D"; put54 "$D"; put1 "$D"
B4=$(h "$D"); : > "$LOGF"
zj_repair_boot_region "$D" "$TMP/code.bin" "$TMP/cont.bin" 0 2>>"$TMP/dd.err"
RC=$?
[ "$RC" = "0" ] && ok "case1 返回 0" || bad "case1 返回 $RC"
grep -qF 'boot region ok (0x54=fa33c08e)' "$LOGF" && ok "case1 日志 ok 指纹" || bad "case1 缺 ok 日志"
[ "$(h "$D")" = "$B4" ] && ok "case1 未改写" || bad "case1 被改写"

# ---- 用例 2：0x54 错误（续段仍非零）→ 重写 ----
D="$TMP/b.bin"; newdev "$D"; put54x "$D"; put1 "$D"
: > "$LOGF"; B4=$(h "$D")
zj_repair_boot_region "$D" "$TMP/code.bin" "$TMP/cont.bin" 0 2>>"$TMP/dd.err"
RC=$?
[ "$RC" = "0" ] && ok "case2 返回 0" || bad "case2 返回 $RC"
grep -qF 'boot region rewritten' "$LOGF" && ok "case2 日志 rewritten" || bad "case2 缺 rewritten"
grep -qF 'kept intact' "$LOGF" && bad "case2 仍出现 kept intact" || ok "case2 无 kept intact"
[ "$(g54 "$D")" = "fa33c08e" ] && ok "case2 0x54 已修正" || bad "case2 0x54=$(g54 "$D")"
[ "$(h "$D")" != "$B4" ] && ok "case2 确有写盘" || bad "case2 没有改动"

# ---- 用例 3：0x54 正确但续段全零 → 重写 ----
D="$TMP/c.bin"; newdev "$D"; put54 "$D"
: > "$LOGF"
zj_repair_boot_region "$D" "$TMP/code.bin" "$TMP/cont.bin" 0 2>>"$TMP/dd.err"
RC=$?
[ "$RC" = "0" ] && ok "case3 返回 0" || bad "case3 返回 $RC"
grep -qF 'boot region rewritten' "$LOGF" && ok "case3 日志 rewritten" || bad "case3 缺 rewritten"
[ "$(s1 "$D")" = "$(s1 "$TMP/cont.bin")" ] && ok "case3 续段已写入" || bad "case3 续段未写入"

# ---- 用例 4：缺 blob → 返回 1 且明确报错 ----
D="$TMP/d.bin"; newdev "$D"
: > "$LOGF"
zj_repair_boot_region "$D" "$TMP/nope.bin" "$TMP/cont.bin" 0 2>>"$TMP/dd.err"
RC=$?
[ "$RC" = "1" ] && ok "case4 返回 1" || bad "case4 返回 $RC"
grep -qF 'ERROR: boot region bad' "$LOGF" && ok "case4 有 ERROR" || bad "case4 缺 ERROR"
[ "$(h "$D")" = "$(newdev "$TMP/d0.bin"; h "$TMP/d0.bin")" ] && ok "case4 未改写" || bad "case4 被改写"

# ---- 用例 5：给了总扇区数 → 卷末尾备份引导扇区同步更新 ----
D="$TMP/e.bin"; newdev "$D"; put54x "$D"
: > "$LOGF"
zj_repair_boot_region "$D" "$TMP/code.bin" "$TMP/cont.bin" 16 2>>"$TMP/dd.err"
RC=$?
[ "$RC" = "0" ] && ok "case5 返回 0" || bad "case5 返回 $RC"
TAIL=$(dd if="$D" bs=1 skip=$((15 * 512 + 84)) count=4 2>/dev/null | od -An -tx1 | tr -d ' \n')
[ "$TAIL" = "fa33c08e" ] && ok "case5 末尾备份扇区已更新" || bad "case5 末尾备份=$TAIL"

# ---- 用例 6：脚本里不再有含糊的 kept intact 单独成句 ----
if grep -q 'kept intact' "$SCRIPT"; then
    bad "脚本仍出现 kept intact"
else
    ok "脚本已无 kept intact"
fi

echo "boot-region: $((PASS + FAIL)) checks, $FAIL failures"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
