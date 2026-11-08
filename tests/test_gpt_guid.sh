#!/bin/bash
# P0（2026-11-08 真机）：GPT 表项直读 GUID 的回归。
#
# 背景：客户盘上 rescue 层 blkid 取不到 PART_ENTRY_UUID（原样 n/a(no-blkid-uuid)），
# "GUID 优先"判据等于不存在，只能退回序列号；而序列号两侧长度都不同 → 正确目标
# 被拒。改成从 LBA1 的 GPT 头 + 分区表项按 offset 匹配取 Unique Partition GUID。
#
# 做法：用 python 合成最小 GPT 镜像（头 CRC/表项 CRC 都按规范算好），
# 再从 bootfiles/alpine/init 抽取纯函数直接喂镜像，覆盖：
#   ① 按 offset 取到正确表项 GUID（表项顺序打乱，证明不是"取第一条"）；
#   ② 相邻分区/错 size 不会串（size 闸门生效）；
#   ③ 头 CRC 坏 / 无 GPT 签名（MBR）/ 表项 GUID 全零 → 干净降级（输出空）。
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
INIT="$ROOT/bootfiles/alpine/init"
PY=${1:-${PYTHON:-python}}
TMP=$(mktemp -d 2>/dev/null || echo "${TEMP:-/tmp}/zjgpt.$$")
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }
ok() { echo "  ok: $*"; }

# ── 抽取纯函数（与 test_fallback_identity.sh 同一手法）──
GPT_ERR="$TMP/gpt.err"
export GPT_ERR
for fn in guid_norm gpt_hdr_fields gpt_entry_find gpt_part_guid; do
    eval "$(sed -n "/^${fn}(){/,/^}/p" "$INIT")"
    type "$fn" >/dev/null 2>&1 || fail "cannot extract ${fn}() from $INIT"
done
ok "extracted guid_norm + gpt_hdr_fields + gpt_entry_find + gpt_part_guid"

# ── 合成 GPT 镜像 ──
# A: 起始 LBA 2048（1MiB），1MiB 大小；B: 起始 LBA 4096（2MiB），1MiB 大小。
# 表项里 **B 排在 A 前面**：按 offset 匹配就必须拿到 A，证明不是"取第一条"。
"$PY" - "$TMP" <<'PY' || fail "cannot build synthetic GPT images"
import struct, sys, zlib, os, shutil

tmp = sys.argv[1]
TOTAL = 32768              # 16MiB
ELBA, NUM, ESZ = 2, 128, 128

GA = bytes.fromhex('a1b2c3d4e5f60718293a4b5c6d7e8f90')   # 目标分区 A
GB = bytes.fromhex('0f1e2d3c4b5a69788796a5b4c3d2e1f0')   # 相邻分区 B
GC = bytes.fromhex('11112222333344445555666677778888')   # 对照（表项顺序打乱用）
TYPE = bytes.fromhex('ebd0a0a2b9e5443387ea11d5e6a1b1f0')

parts = [
    # (type, unique, first_lba, last_lba)  —— B 在前，A 在后
    (TYPE, GB, 4096, 6143),
    (TYPE, GA, 2048, 4095),
    (TYPE, GC, 8192, 10239),
]

def build(path, corrupt_crc=False, no_sig=False, zero_unique=False):
    with open(path, 'wb') as f:
        f.truncate(TOTAL * 512)
    ent = bytearray(NUM * ESZ)
    for i, (t, u, first, last) in enumerate(parts):
        o = i * ESZ
        ent[o:o + 16] = t
        if zero_unique and u == GA:
            u = bytes(16)
        ent[o + 16:o + 32] = u
        ent[o + 32:o + 40] = struct.pack('<Q', first)
        ent[o + 40:o + 48] = struct.pack('<Q', last)
    ecrc = zlib.crc32(bytes(ent)) & 0xffffffff
    hdr = bytearray(92)
    hdr[0:8] = b'EFI PART'
    struct.pack_into('<I', hdr, 8, 0x00010000)
    struct.pack_into('<I', hdr, 12, 92)
    struct.pack_into('<I', hdr, 16, 0)                     # CRC 占位
    struct.pack_into('<Q', hdr, 24, 1)                     # my LBA
    struct.pack_into('<Q', hdr, 32, TOTAL - 1)             # backup LBA
    struct.pack_into('<Q', hdr, 40, 34)                    # first usable
    struct.pack_into('<Q', hdr, 48, TOTAL - 34)            # last usable
    hdr[56:72] = bytes.fromhex('00112233445566778899aabbccddeeff')
    struct.pack_into('<Q', hdr, 72, ELBA)
    struct.pack_into('<I', hdr, 80, NUM)
    struct.pack_into('<I', hdr, 84, ESZ)
    struct.pack_into('<I', hdr, 88, ecrc)
    crc = zlib.crc32(bytes(hdr)) & 0xffffffff
    struct.pack_into('<I', hdr, 16, crc)
    if corrupt_crc:
        hdr[56] ^= 0xff                                    # 改磁盘 GUID，不更新头 CRC
    with open(path, 'r+b') as f:
        if no_sig:
            f.seek(512)                                    # LBA1 全零 = MBR 盘语义
            f.write(bytes(512))
        else:
            f.seek(512)
            f.write(bytes(hdr))
        f.seek(ELBA * 512)
        f.write(bytes(ent))

build(os.path.join(tmp, 'good.img'))
build(os.path.join(tmp, 'badcrc.img'), corrupt_crc=True)
build(os.path.join(tmp, 'nosig.img'), no_sig=True)
build(os.path.join(tmp, 'nouuid.img'), zero_unique=True)
PY
ok "synthetic GPT images built"

# ── 正例：按 offset 精确取表项 GUID ──
_got=$(gpt_part_guid "$TMP/good.img" 1048576 1048576)      # A @1MiB, 1MiB
[ "$_got" = "ok a1b2c3d4e5f60718293a4b5c6d7e8f90" ] || fail "A @1MiB -> '$_got'"
ok "A @1MiB -> 表项 Unique GUID（表项排在 B 之后仍取对）"

_got=$(gpt_part_guid "$TMP/good.img" 2097152 "")           # B @2MiB（相邻分区）
[ "$_got" = "ok 0f1e2d3c4b5a69788796a5b4c3d2e1f0" ] || fail "B @2MiB -> '$_got'"
ok "B @2MiB -> 相邻分区 GUID（没有串到 A）"

_got=$(gpt_part_guid "$TMP/good.img" 4194304 "")           # C @4MiB
[ "$_got" = "ok 11112222333344445555666677778888" ] || fail "C @4MiB -> '$_got'"
ok "C @4MiB -> 第三条表项 GUID"

# size 闸门：offset 对但 size 不符 → 拒绝该表项（不拿 A 的 GUID 硬凑）
_got=$(gpt_part_guid "$TMP/good.img" 1048576 2097152)
case "$_got" in fail\ size-mismatch*) ok "错 size -> $_got" ;; *) fail "错 size -> '$_got'" ;; esac

# 不存在的 offset → 干净失败（不误认到相邻分区）
_got=$(gpt_part_guid "$TMP/good.img" 3145728 "")
case "$_got" in fail\ no-entry-at-offset*) ok "空档 offset -> $_got" ;; *) fail "空档 offset -> '$_got'" ;; esac

# ── 降级：表损坏/非 GPT/表项无 GUID 一律输出 fail（调用方回退序列号）──
for case in badcrc:header-crc-bad nosig:no-gpt-signature nouuid:no-entry-at-offset; do
    img=${case%%:*}; want=${case#*:}
    _got=$(gpt_part_guid "$TMP/$img.img" 1048576 1048576)
    case "$_got" in fail\ "$want"*) ok "$img -> $_got" ;; *) fail "$img -> '$_got' (want fail $want)" ;; esac
done

# ── 归一化：Windows 花括号/连字符 / blkid 大小写 → 同一种写法 ──
_n=$(guid_norm '{A1B2C3D4-E5F6-0718-293A-4B5C6D7E8F90}')
[ "$_n" = "a1b2c3d4e5f60718293a4b5c6d7e8f90" ] || fail "guid_norm 花括号/连字符 -> '$_n'"
_n=$(guid_norm ' a1 b2 c3 ')
[ "$_n" = "a1b2c3" ] || fail "guid_norm 空白 -> '$_n'"
ok "guid_norm 统一口径"

# ── 静态接线：两条路径共用同一个候选 GUID，且日志写明来源 ──
grep -Fq '_have_guid=$_guid_val' "$INIT" || fail "主匹配/fallback 没有共用 _guid_val"
grep -Fq '_guid_src=gpt-table' "$INIT" || fail "GPT 来源没有标记 gpt-table"
grep -Fq 'logonly "guid: dev=$_d src=$_guid_src' "$INIT" || fail "日志没有写 GUID 来源"
grep -Fq '_gr=$(gpt_part_guid "/dev/$_disk" "$_off" "$_szb")' "$INIT" || fail "scan 没有按 offset/size 调用 gpt_part_guid"
n=$(grep -c 'identity_verdict "$_want_serial"' "$INIT")
[ "$n" -ge 2 ] || fail "identity_verdict() 调用点只有 $n 处"
ok "wiring: blkid -> gpt-table -> none, 两条路径共用"

echo "test_gpt_guid: PASS (offset match + size gate + degradation + normalization)"
