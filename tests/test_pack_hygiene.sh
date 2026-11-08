#!/bin/bash
# beta.9 回归（测试台第三）：运行产物不许进包。
# 规则源：tools/check-pack-hygiene.py（打包前 --clean、打包端 refuse、dist-check 同一套）。
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
# 第一个参数：make 传来的 python（可选）；没传就自己找
if [ -n "${1:-}" ]; then PYTHON=$1; fi
if [ -z "${PYTHON:-}" ]; then
    for c in python python3 py; do
        if command -v "$c" >/dev/null 2>&1; then PYTHON=$c; break; fi
    done
fi
[ -n "${PYTHON:-}" ] || { echo "test_pack_hygiene: SKIP (python not found)"; exit 0; }
PY=$PYTHON
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }
ok()   { echo "  ok: $*"; }

scan(){ "$PY" "$ROOT/tools/check-pack-hygiene.py" "$1"; }

# ① 干净目录（只有正常交付物）→ PASS
mkdir -p "$WORK/clean/lang"
: > "$WORK/clean/SysRecover.exe"
: > "$WORK/clean/lang/en.lang"
scan "$WORK/clean" >/dev/null || fail "case1: clean dist rejected"
ok "clean dist passes"

# ② dist 根 restore-task.conf → FAIL 并点名
mkdir -p "$WORK/c2"
: > "$WORK/c2/restore-task.conf"
if scan "$WORK/c2" > "$WORK/c2.out"; then fail "case2: restore-task.conf accepted"; fi
grep -q "restore-task.conf" "$WORK/c2.out" || fail "case2: artifact not named in output"
ok "restore-task.conf rejected"

# ③ dist/x64/logs/ZJRESTORE-status.txt → FAIL（logs 目录 + 状态回执）
mkdir -p "$WORK/c3/x64/logs"
: > "$WORK/c3/x64/logs/ZJRESTORE-status.txt"
if scan "$WORK/c3" > "$WORK/c3.out"; then fail "case3: logs dir accepted"; fi
grep -q "x64/logs" "$WORK/c3.out" || fail "case3: logs dir not named"
ok "nested logs dir rejected"

# ④ _zjresy*.log → FAIL
mkdir -p "$WORK/c4"
: > "$WORK/c4/_zjresy1108.log"
if scan "$WORK/c4" > "$WORK/c4.out"; then fail "case4: _zjresy log accepted"; fi
ok "_zjresy*.log rejected"

# ⑤ --clean 清掉全部违规项，再扫通过
mkdir -p "$WORK/c5/x64/logs"
: > "$WORK/c5/restore-task.conf"
: > "$WORK/c5/_zjresy1108.log"
: > "$WORK/c5/x64/logs/ZJRESTORE-status.txt"
"$PY" "$ROOT/tools/check-pack-hygiene.py" --clean "$WORK/c5" >/dev/null || fail "case5: clean failed"
scan "$WORK/c5" >/dev/null || fail "case5: still dirty after --clean"
[ ! -e "$WORK/c5/x64/logs" ] || fail "case5: logs dir survived"
ok "--clean removes artifacts"

# ⑥ 静态断言：打包端与 dist-check 都接了同一规则
grep -q "check-pack-hygiene.py" "$ROOT/tools/portable-pack.py" || fail "case6: portable-pack not wired"
grep -q "check-pack-hygiene.py" "$ROOT/tools/check-dist.py" || fail "case6: check-dist not wired"
grep -q "check-pack-hygiene.py --clean" "$ROOT/Makefile" || fail "case6: package not wired"
ok "packaging + dist-check wired to same rule"

echo "test_pack_hygiene: PASS (6 cases)"
