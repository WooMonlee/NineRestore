#pragma once
// 引导条目的显示名（ASCII）：
//   Menu = 用户显式「安装菜单」的常驻条目；
//   Temporary = 日常还原的一次性条目，首启由“恢复引导菜单”清理。
// 两者必须使用不同名称——清理只认 Temporary，绝不能误删常驻菜单。
namespace sysrecover {

// 品牌统一为 NineRestore（PIT-239/PIT-246）。刻意用 ASCII 而非「九转还原」：
//   · GRUB4DOS 菜单标题写中文需随包发 G4D 字体+GBK 码表，否则乱码（PIT-036）；
//   · BCD 的 description 在部分 BIOS 机器的 bootmgr 里中文显示成方框（PIT-054）。
// PIT-246（2026-11-08 真机）：UEFI 项名 / BCD 描述 / GRUB title 三处必须同一名字。
// 旧版 BCD 描述是 "NineRestore Recovery Environment"（真机 bootmgr 显示成
// "NineRestore Recover Ev.."），已统一为 "NineRestore"。
inline constexpr wchar_t kMenuEntryDesc[] = L"NineRestore";
inline constexpr wchar_t kMenuBcdDesc[] = L"NineRestore";
inline constexpr wchar_t kTempEntryDesc[] = L"NineRestore Temporary Restore";

// GRUB4DOS menu.lst 的 title（纯 ASCII）。必须与 kMenuEntryDesc 逐字符一致，
// 由 tools/check-brand.py 门禁校验；grub.cpp 只引用这里，不再各写一套。
inline constexpr char kMenuEntryDescAnsi[] = "NineRestore";

// 历史品牌名（旧版本生成过）：**不再生成**，只用于升级时识别、更新或清理
// 残留条目（否则旧 SysRecover 与新 NineRestore 会并存、指向同一份载荷）。
// 这些字面量允许留在代码里（LEGACY 标记），但不得出现在对外文案/菜单标题；
// tools/check-brand.py 检查运行时文件里除本文件外的 LEGACY 引用。
inline constexpr const wchar_t* const kLegacyPermanentDescs[] = {
    L"SysRecover",
    L"SysRecovery Environment",
    L"NineRestore Recovery Environment",
};
inline constexpr const wchar_t* const kLegacyTempDescs[] = {
    L"SysRecover Temporary Restore",
};

}  // namespace sysrecover
