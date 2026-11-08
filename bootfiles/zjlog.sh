#!/bin/sh
# zjlog.sh — 救援层统一诊断底座（P0-B，PIT-145）
#
# 背景：5 个救援脚本（init / zjrestore-lite.sh / zjswap.sh / zjswap-undo.sh /
# setup-swap.sh）各复制了一份 say()，落盘行为渐渐漂移：init/lite 的文件行无
# 前缀、undo 带前缀、swap/setup 只打控制台不落文件 —— 多脚本合并成的
# /tmp/zjrestore.log 里分不清谁说的、还缺行。统一到本文件单源实现。
#
# 用法（调用方在 source 前设前缀，前缀是断言/录像取证的契约，不许改）：
#   ZJPREFIX=SWAPUNDO
#   . /zjlog.sh
#
# 约定：
#   · 控制台行 = "<PREFIX>: msg"（QEMU 断言 grep 它，如 `SWAPUNDO:`/`ZJ:`）；
#   · 文件行   = "YYYY-mm-dd HH:MM:SS <PREFIX>: msg" 追加到
#     ${LOG:-/tmp/zjrestore.log} —— 5 个脚本合并成一条时间线（LOG 未导出
#     的子进程也能落同一份：兜底路径相同）；
#   · 文件写失败必须静默（日志面可能未就绪/只读）—— say 永远 return 0，
#     不能因落盘失败让调用方的 `if say` 判断走偏，也兼容 `set -u`。
#
# 只放 say。不放 run/die：现有调用点是 "say ERROR → umount → exit N" 的
# 多步形态，硬套 die 只会增加 diff 噪音 —— 需要时再加，别为统一而统一。
# 带 timeout 的 NTFS 挂载纪律（PIT-112"挂死不可静默" + PIT-144 FUSE 优先）
# 也收敛在这里：三个裸 mount 的脚本（zjswap/zjswap-undo/setup-swap）共用。

: "${ZJPREFIX:=ZJ}"

# ── 救援层中文字库（16 点阵，取模嵌入）────────────────────────────────
# 控制台是 Linux VT（vgacon/fbcon）：内核一行只认 8 像素宽的字符格，
# 一个汉字占两格，所以字库里每个汉字都存成"左半 8px + 右半 8px"两块，
# 各映射到一个私用区码位（U+E000 起），运行时用 sed 把汉字换成这两个
# 半字码位 —— 双宽/折行/错位都不经过内核的双宽逻辑（那是按 16px 整字
# 画第一格、第二格留空，8px 字格下会把汉字挤扁）。
# 只在本地 VT 上启用：串口控制台没有字库，转码只会把日志变成乱码。
# 加载结果必须**看得见**（beta.14，PIT-238）：旧的静默失败让真机"汉字变白底
# 问号"既没屏幕提示也没日志线索。本函数把 rc/原因写进 ZJ_FONT_RC/ZJ_FONT_WHY
# 和 /tmp/zj-font.status，由 init 打一行 `FONT:` 到屏幕与日志。
# rc=0 成功；1 缺字库；2 缺/不可写目标 VT；3 缺 setfont；4 setfont 失败；
#   5 缺汉字映射表；6 串口控制台（无 VT 字槽，跳过）；
#   7 vgacon 文本控制台（无 framebuffer，PUA 字形槽不可用 —— PIT-247）。
: "${ZJ_FONT_RC:=0}"
: "${ZJ_FONT_WHY:=}"

# 是否有 framebuffer 控制台（fbcon）。UEFI 走 efifb/simpledrm → 有 fb0；
# BIOS 传统 VGA 文本模式走 vgacon → 没有 fb0。ZJ_FBCON 仅供回归注入（1/0）。
zj_fbcon(){
    if [ -n "${ZJ_FBCON:-}" ]; then
        [ "$ZJ_FBCON" = 1 ] && return 0
        return 1
    fi
    [ -e /sys/class/graphics/fb0 ] || return 1
    return 0
}

zj_console_kind(){ # → vt | serial | unknown（ZJ_CONSOLE_KIND 仅供回归注入）
    if [ -n "${ZJ_CONSOLE_KIND:-}" ]; then
        printf '%s' "$ZJ_CONSOLE_KIND"
        return 0
    fi
    _zck=$(cat /sys/class/tty/console/active 2>/dev/null)
    case "$_zck" in
        *tty[0-9]*) printf 'vt' ;;
        *ttyS*|*ttyAMA*|*ttymxc*|*ttyUSB*|*hvc*) printf 'serial' ;;
        *tty*) printf 'vt' ;;
        *) printf 'unknown' ;;
    esac
}

zj_font_load(){
    ZJ_FONT_RC=0
    ZJ_FONT_WHY=""
    _zis=$(zj_console_kind)
    if [ "$_zis" = serial ]; then
        ZJ_FONT_RC=6
        ZJ_FONT_WHY="serial console (no VT font slot)"
        zj_font_status skip "serial console"
        return 1
    fi
    # PIT-247（2026-11-08 真机）：BIOS 走 vgacon（VGA 文本模式，8 bit 字符格 =
    # 最多 256 个字符码）。我们的 PSF1 mode 0x03 把汉字拆成左右两块、映射到
    # PUA(U+E000+) → 字形槽 256..511；setfont 在 vgacon 上可能返回 0（字模确实
    # 写进了 VGA 存储），但 PUA 的 unicode 映射不生效 → 内核按"无字形"逐字画
    # 白底 '?'。UEFI 走 fbcon，PSF1 的 unicode 表直接生效，所以同一份载荷只有
    # BIOS 出问号。判据不能只看 setfont 返回码，必须看控制台类型：无 fb0 就
    # **不启用中文**，全部走英文/ASCII 兜底（PIT-238 纪律：宁英文，不问号）。
    if [ "$_zis" = vt ] && ! zj_fbcon; then
        ZJ_FONT_RC=7
        ZJ_FONT_WHY="vgacon (no framebuffer): PUA glyph slots unavailable; ASCII fallback"
        zj_font_status skip "$ZJ_FONT_WHY"
        return 1
    fi
    if [ -n "${ZJ_FONT_FORCE_FAIL:-}" ]; then
        ZJ_FONT_RC=4
        ZJ_FONT_WHY="injected failure (ZJ_FONT_FORCE_FAIL)"
        zj_font_status fail "$ZJ_FONT_WHY"
        return 1
    fi
    if [ ! -f /zjfont.psf ]; then
        ZJ_FONT_RC=1
        ZJ_FONT_WHY="missing /zjfont.psf"
        zj_font_status fail "$ZJ_FONT_WHY"
        return 1
    fi
    if [ ! -f /zjzhmap.sed ]; then
        ZJ_FONT_RC=5
        ZJ_FONT_WHY="missing /zjzhmap.sed (CJK code->glyph map)"
        zj_font_status fail "$ZJ_FONT_WHY"
        return 1
    fi
    if ! command -v setfont >/dev/null 2>&1; then
        ZJ_FONT_RC=3
        ZJ_FONT_WHY="setfont applet not available"
        zj_font_status fail "$ZJ_FONT_WHY"
        return 1
    fi
    : > /tmp/zjlog.err
    # 两条落点：/dev/tty0（当前 VT）优先；再退 /dev/console（console= 指向的活动
    # 控制台）。任一成功即视为装上 —— 真机上两者可能不是同一个设备。
    for _zt in /dev/tty0 /dev/console; do
        [ -w "$_zt" ] || continue
        setfont -C "$_zt" /zjfont.psf 2>>/tmp/zjlog.err
        _zrc=$?
        if [ "$_zrc" -eq 0 ]; then
            : > /tmp/zj-cjk-font.ok
            zj_font_status ok "tty=$_zt"
            return 0
        fi
        ZJ_FONT_RC=4
        ZJ_FONT_WHY="setfont rc=$_zrc on $_zt: $(head -1 /tmp/zjlog.err 2>/dev/null)"
    done
    if [ "$ZJ_FONT_RC" -eq 0 ]; then
        ZJ_FONT_RC=2
        ZJ_FONT_WHY="no writable VT device (/dev/tty0, /dev/console)"
    fi
    zj_font_status fail "$ZJ_FONT_WHY"
    return 1
}

zj_font_status(){ # $1=state $2=reason；供支持人员一眼核对
    _zmap=off
    _zgl=0
    if [ -f /zjzhmap.sed ]; then
        _zmap=on
        _zgl=$(wc -l < /zjzhmap.sed 2>/dev/null | tr -d ' \n')
    fi
    printf 'state=%s rc=%s console=%s map=%s map_entries=%s reason=%s\n' \
        "$1" "${ZJ_FONT_RC:-0}" "$(zj_console_kind)" "$_zmap" \
        "${_zgl:-0}" "$2" > /tmp/zj-font.status 2>/dev/null
    return 0
}

zj_console_cjk(){
    [ -f /tmp/zj-cjk-font.ok ] || return 1
    [ -f /zjzhmap.sed ] || return 1
    # PIT-247：vgacon 上一律不送 PUA（见 zj_font_load 注释）。双保险：即便旧版
    # 留下的 /tmp/zj-cjk-font.ok 还在，没有 fbcon 也走 ASCII 兜底。
    zj_fbcon || return 1
    [ -r /sys/class/tty/console/active ] || return 1
    case "$(cat /sys/class/tty/console/active)" in
        *tty[0-9]*) return 0 ;;
    esac
    return 1
}

# 动态文本（镜像文件名/路径、卷标）压成纯 ASCII：VT 字库只有 151 个自有汉字，
# 用户文本里的汉字没有字形 → 白底方块。这里把非 ASCII 串换成 `.`，保留可读的
# ASCII 部分；原文由调用方（say_dyn / logonly）完整落日志。
zj_ascii(){ # $1 = 文本；输出纯 ASCII（连续无字模字符 → 一个 `.`）
    # PIT-238（重做）：字库只覆盖 151 个自有汉字，用户文件名里的汉字（如
    # "忘/初/心"）没有字形 → VT 画白底方块。这里把**任何**非 ASCII 串压成 `.`
    #（不是问号、不是豆腐块、不是空白），ASCII 部分原样保留。
    printf '%s' "${1:-}" | LC_ALL=C sed 's/[^ -~][^ -~]*/./g'
}

# 判断"映射后仍残留字库外字符"：字库覆盖 = ASCII 可打印 + 151 个自有汉字，
# 后者已被 zjzhmap.sed 换成 PUA（U+E000..，UTF-8 前导字节只有 0xEE）。因此
# 只要出现 0xC2..0xED / 0xEF..0xF4 这些**其它**非 ASCII 前导字节，就是"该字
# 没有字形"——不管是汉字、假名、西里尔还是全角符号（PIT-238 重做）。
zj_has_unmapped(){
    _zub=$(printf '[\302-\355\357-\364]')
    printf '%s' "${1:-}" | LC_ALL=C grep -q "$_zub"
}

# ── 屏幕行宽纪律（beta.12，PIT-236）────────────────────────────────────
# 终端 80 列时 >80 会硬折行，续行从第 0 列起、没有前缀（beta.10 清点屏实测：
# dmi: / PROBE root=[...] 折 2–3 行、左边缘错乱）。控制台统一压到 **76 列**
# （与进度条同口径）：纯 ASCII 行按词/字段边界折行，续行重复 "<PREFIX>: "；
# 日志文件仍写完整原文（见 say 末尾，不被屏幕压缩吃掉）。
# 只折纯 ASCII 行：中文行由构建期 C8 门禁保证字面量 ≤78 列（实测最宽 63），
# 运行期中英混拼无法在 busybox 里按显示列宽稳定切分 —— 这是残留边界，
# docs/34 §20 已如实说明。
zj_fold(){ # stdin 逐行；$1 = 续行前缀（含 ": "）
    awk -v w=76 -v p="$1" '
    {
        s = $0
        while (length(s) > w) {
            cut = w
            for (i = w; i > length(p) + 8; i--) {
                c = substr(s, i, 1)
                if (c == " " || c == "," || c == "|" || c == "/" || c == ";" || c == "=") {
                    cut = i; break
                }
            }
            print substr(s, 1, cut)
            rest = substr(s, cut + 1)
            sub(/^ +/, "", rest)
            s = p rest
        }
        print s
    }'
}

say(){
    _zj_line="$ZJPREFIX: $*"
    case "$*" in
        *[!\ -~]*)
            # 含非 ASCII：落盘保持原文；控制台按环境三选一（beta.14，PIT-238）：
            #   ① VT + 字模已装 → 拆成半字码位（正常中文）；
            #   ② 串口 → 原样 UTF-8（终端自己会渲染）；
            #   ③ VT 但字模没装上 / 串里含字库外汉字 → **绝不原样送屏**
            #      （内核画白底方块），压成 ASCII（非 ASCII 串 → `.`）。
            # 模式写成 [!\ -~] 是实测过的：busybox ash 认它，[! -~] 带裸空格
            # 会在解析期报 "unexpected word"，[:print:] 在 ash 里把 UTF-8 当可打印。
            if zj_console_cjk; then
                _zjmap=$(printf '%s\n' "$_zj_line" | sed -f /zjzhmap.sed)
                if zj_has_unmapped "$_zjmap"; then
                    # 有字库外字符（典型：用户镜像名）→ `.` 兜底，绝不送白底方块。
                    # N = 映射后仍不在 PUA(0xEE) 里的字符数（逐字符计数见 tr 说明）。
                    _zjn=$(printf '%s' "$_zjmap" | LC_ALL=C tr -cd '\302-\355\357-\364' \
                        | wc -c | tr -d ' \n')
                    printf '%s\n' "$(zj_ascii "$_zj_line")" 1>&2
                    logonly "screen: omitted ${_zjn:-?} unmapped char(s); raw: $_zj_line"
                else
                    printf '%s\n' "$_zjmap" 1>&2
                fi
            elif [ "$(zj_console_kind)" = serial ]; then
                printf '%s\n' "$_zj_line" 1>&2
            else
                _zj_fb=$(zj_ascii "$_zj_line")
                if [ "${#_zj_fb}" -gt 76 ]; then
                    printf '%s\n' "$_zj_fb" | zj_fold "$ZJPREFIX: " 1>&2
                else
                    printf '%s\n' "$_zj_fb" 1>&2
                fi
            fi
            ;;
        *)
            # 纯 ASCII：>76 列按字段边界折行，续行带前缀（PIT-236）
            if [ "${#_zj_line}" -gt 76 ]; then
                printf '%s\n' "$_zj_line" | zj_fold "$ZJPREFIX: " 1>&2
            else
                printf '%s\n' "$_zj_line" 1>&2
            fi
            ;;
    esac
    echo "$(date '+%F %T') $_zj_line" >> "${LOG:-/tmp/zjrestore.log}" 2>/dev/null
    return 0
}

# say2 <中文> <English> —— 界面句双语版本（beta.14，PIT-238）。
# 有字模/VT 或串口时打中文；VT 但字模没装上时**只打英文**（不是把中文压成
# 残句），保证失败路径下用户看到的每一行都是可读英文。构建期 C8 门禁要求
# 所有含中文的界面句都走 say2，且英文版必须是纯 ASCII。
say2(){
    if zj_console_cjk || [ "$(zj_console_kind)" = serial ]; then
        say "$1"
    else
        say "$2"
    fi
}

# logonly —— 只落日志、不上屏（beta.14，PIT-242）。给"支持要看、用户不必看"
# 的内核/底层明细用（dmesg 80 行）：屏幕留给正常提示，日志仍是完整证据。
logonly(){
    echo "$(date '+%F %T') $ZJPREFIX: $*" >> "${LOG:-/tmp/zjrestore.log}" 2>/dev/null
    return 0
}

# say_dyn <ASCII 标签> <动态值> —— 动态值可能带中文（镜像文件名/路径/卷标）。
# 界面字样库只覆盖脚本里的固定汉字，**任意文件名里的汉字没有字形**：直接送屏
# 就是白底方块（beta.13 真机：镜像名"不忘初心"里「忘/初/心」三字无字形）。
# 这里把动态值里的非 ASCII 串压成 `.` 再上屏；产线日志仍追加一行原文，便于支持
# 人员核对真实路径。
say_dyn(){
    _zdl="$1$(zj_ascii "$2")"
    say "$_zdl"
    if [ "$(printf '%s' "$_zdl")" != "$(printf '%s' "$1$2")" ]; then
        echo "$(date '+%F %T') $ZJPREFIX: $1$2" >> "${LOG:-/tmp/zjrestore.log}" 2>/dev/null
    fi
    return 0
}

# say_img <file> <dev> <rem> —— 屏上只给"类型 + 大小 + 设备"，文件名只落日志
#（PIT-238 重做：屏幕用来做决定，不是用来看文件名；用户文件名汉字无字形）。
say_img(){
    _zisz=$(stat -c %s "$1" 2>/dev/null)
    _ziyz=$(printf '%s' "$1" | sed 's/.*\.//' | tr 'a-z' 'A-Z')
    say "found image: type=${_ziyz:-n/a(no-ext)} size=${_zisz:-n/a(no-stat)}B dev=$2 rem=${3:-0}"
    logonly "image file: $1"
    return 0
}

# mnt_ntfs <dev> <mnt> [ntfs3|fuse] —— 返回 0/1；错误进 /tmp/zjmnt.err
#   ntfs3（默认）：内核驱动优先，吞吐高（与 zjrestore-lite.sh::mnt_dev 同序）；
#   fuse：ntfs-3g 优先 —— **zjswap-undo.sh 必须用这个**（PIT-144：ntfs3 在
#         大规模 rename 风暴后会留幽灵索引项，rm -rf 必失败）。
# 每一步都带 timeout 60（PIT-112：ntfs3 挂坏设备会静默挂死 400s 级）；
# 成功打一行 `  mnt <dev> ok (<drv>)` —— "这次是 FUSE 还是内核驱动"是
# PIT-144 类问题的定罪要素，必须留痕。
mnt_ntfs(){
    : > /tmp/zjmnt.err
    if [ "${3:-ntfs3}" = fuse ]; then
        timeout 60 ntfs-3g -o force "$1" "$2" 2>>/tmp/zjmnt.err && { say "  mnt $1 ok (fuse)"; return 0; }
        timeout 60 mount -t ntfs3 "$1" "$2" 2>>/tmp/zjmnt.err && { say "  mnt $1 ok (ntfs3)"; return 0; }
    else
        timeout 60 mount -t ntfs3 "$1" "$2" 2>>/tmp/zjmnt.err && { say "  mnt $1 ok (ntfs3)"; return 0; }
        timeout 60 ntfs-3g -o force "$1" "$2" 2>>/tmp/zjmnt.err && { say "  mnt $1 ok (fuse)"; return 0; }
    fi
    timeout 60 mount -t ntfs-3g "$1" "$2" 2>>/tmp/zjmnt.err && { say "  mnt $1 ok (fuse)"; return 0; }
    return 1
}
