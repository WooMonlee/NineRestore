#!/usr/bin/env bash
# beta.13（PIT-237）：设备路径归一化 + 清点屏空值显式化。
#
# 背景（beta.12 测试台）：清点屏 `PROBE` 行出现 `/dev/dev/sdb1`（设备名已带前缀又
# 拼一次），`off=?`、`root=` 空白。本回归覆盖：
#   ① 两个救援脚本都只用 zj_dev() 产出设备路径，禁止再手拼 "/dev/$x"；
#   ② 不允许残留 `${x:-?}` 这种裸问号占位；
#   ③ zj_dev/zj_devname 对裸名、带前缀、误拼双前缀三种输入都归一到 /dev/<name>；
#   ④ 清点屏关键字段（off/root/size/fs/err）的空值必须带显式 n/a(原因)。
# 说明：运行期拼出的完整字符串无法静态穷举（docs/34 §19 已如实说明），这里只锁
# 死"归一化入口 + 占位符 + 已知文案"，其余靠 QEMU 抓屏复核。
set -u

fail(){ echo "FAIL: $*" >&2; exit 1; }
ok(){ echo "PASS: $*"; }

ROOT=$(cd "$(dirname "$0")/.." && pwd)
INIT="$ROOT/bootfiles/alpine/init"
LITE="$ROOT/bootfiles/zjrestore-lite.sh"
[ -f "$INIT" ] || fail "missing $INIT"
[ -f "$LITE" ] || fail "missing $LITE"

# ① 归一化入口存在，且 list_parts 不再手拼
for f in "$INIT" "$LITE"; do
    grep -q 'zj_devname()' "$f" || fail "$(basename "$f"): 缺 zj_devname()"
    grep -q 'zj_dev()' "$f"     || fail "$(basename "$f"): 缺 zj_dev()"
    grep -q 'zj_dev "$_b"' "$f" || fail "$(basename "$f"): list_parts 未用 zj_dev"
    if grep -n -F 'echo "/dev/$_b"' "$f" >/dev/null; then
        fail "$(basename "$f"): list_parts 还在手拼 /dev/\$_b"
    fi
done
ok "两个脚本都有 zj_dev()/zj_devname()，list_parts 已归一"

# ② 裸问号占位与字面双前缀必须绝迹
for f in "$INIT" "$LITE"; do
    if grep -n -F ':-?}' "$f" >/dev/null; then
        fail "$(basename "$f"): 仍有 \${x:-?} 占位"
    fi
    if grep -n -F 'dev=/dev/' "$f" >/dev/null; then
        fail "$(basename "$f"): 出现字面 dev=/dev/（拼前缀回归）"
    fi
done
ok "无 \${x:-?} 占位、无字面 dev=/dev/"

# ③ 函数级实测：init 的多行定义（lite 为单行，等价逻辑各测一遍）
eval "$(sed -n '/^zj_devname(){/,/^}/p' "$INIT")"
eval "$(sed -n '/^zj_dev(){/,/^}/p' "$INIT")"
check_norm(){
    [ "$(zj_dev sda1)" = "/dev/sda1" ]          || fail "$1: zj_dev sda1"
    [ "$(zj_dev /dev/sda1)" = "/dev/sda1" ]     || fail "$1: zj_dev /dev/sda1"
    [ "$(zj_dev /dev/dev/sda1)" = "/dev/sda1" ] || fail "$1: zj_dev /dev/dev/sda1"
    [ "$(zj_dev '')" = "" ]                     || fail "$1: zj_dev 空输入应为空"
    [ "$(zj_devname /dev/sda1)" = "sda1" ]      || fail "$1: zj_devname /dev/sda1"
    [ "$(zj_devname sda1)" = "sda1" ]           || fail "$1: zj_devname sda1"
}
check_norm init
ok "init: zj_dev/zj_devname 归一裸名/前缀/双前缀"

unset -f zj_dev zj_devname
eval "$(grep -m1 '^zj_devname(){' "$LITE")"
eval "$(grep -m1 '^zj_dev(){' "$LITE")"
check_norm lite
ok "lite: zj_dev/zj_devname 归一裸名/前缀/双前缀"

# ④ 清点屏关键字段的空值文案必须显式 n/a(原因)
for pat in 'n/a(no-sysfs-size)' 'n/a(no-signature)' 'n/a(no-stderr)'; do
    grep -q -F "$pat" "$INIT" || fail "init: 缺空值文案 $pat"
    grep -q -F "$pat" "$LITE" || fail "lite: 缺空值文案 $pat"
done
for pat in 'n/a(no-sysfs-start,no-blkid-offset)' 'n/a(empty-dir)'; do
    grep -q -F "$pat" "$INIT" || fail "init: 缺空值文案 $pat"
done
ok "空值文案 off/root/size/fs/err 显式带原因"

echo "PASS (4 cases)"
