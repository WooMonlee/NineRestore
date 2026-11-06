#pragma once
// 路径小工具（纯逻辑，可单测；见 tests/unit_tests.cpp 的 pathutil_* 用例）。
// 起因：2026-09-28 用户 CLI 备份 `--source C:` 报错 —— Win32 里 `C:` 的语义是
// “该盘的**当前目录**”，既不等于根，也不带 wimlib 要求的尾斜杠。
#include <string>

namespace sysrecover {

// 是否是盘符根：`C:` / `C:\` / `C:/`（大小写不敏感，可带首尾空白）。
bool IsVolumeRoot(const std::wstring& p);

// 盘符根 → `X:/`（大写盘符 + 尾斜杠，wimlib 的整盘源路径写法）；
// 不是盘符根则原样返回（调用方自己的路径不替用户改）。
std::wstring NormalizeVolumeRoot(const std::wstring& p);

// 目录的父路径（最后一个 `\` / `/` 之前的部分；没有分隔符时返回空）。
// 例：`D:\backup\a.wim` → `D:\backup`；`D:\a.wim` → `D:\`（盘符根必须带分隔符，
// 否则 GetFileAttributesW/`MakeDirTree` 会拿到 `D:` 这种“当前目录”语义）。
std::wstring ParentDir(const std::wstring& path);

// 递归建目录（已存在且是目录 → 成功；已存在但是文件 → 失败）。返回是否就绪。
bool MakeDirTree(const std::wstring& dir);

// 「驱动与导出」根目录（PIT-206，用户 2026-11-01）：
//   安装了 → `<程序目录>\Drivers`；
//   单文件版（解压到 %LOCALAPPDATA%、但 stub 会设 ZJ_ORIGIN_DIR=单文件所在目录）
//   → `<单文件目录>\Drivers`，这样用户**看得到**导出的驱动，
//   而且异机还原的"自动调用"读的也是同一个目录。
// 判据（PIT-212 起）：**先看 AppDir()\Drivers**（单文件"加驱动"版把驱动打进了
// 包内，必须优先用它）；没有再看 ZJ_ORIGIN_DIR\Drivers；最后才退回 AppDir()。
std::wstring DriversRoot();

// 当前进程是不是"单文件（便携）运行"：stub 会设 ZJ_ORIGIN_DIR。
bool IsPortableRuntime();

// 单文件自身所在目录（便携运行时由 stub 设）；非便携 → 空串。
std::wstring PortableOriginDir();

// 引导资产根（其下必须有 bootfiles\）。装菜单 / 修引导 / 首启清理读随包救援资产
// **统一走这里**，不再各自拿 exeDir 拼：
//   安装版 / UI：AppDir()（exe 所在应用根，bootfiles 就在旁边）；
//   单文件版：stub 已把包解到 AppDir()（%LOCALAPPDATA%\SysRecover\app），优先用它；
//             万一该目录没有 bootfiles（解包不完整），再退回单文件所在目录
//             （ZJ_ORIGIN_DIR，支持外置 bootfiles）。
// 两处都没有时仍返回 AppDir()，由调用方按**具体文件名**报错，不在查找层吞掉原因。
std::wstring BootAssetDir();

// <BootAssetDir>\bootfiles\<relative>；relative 用反斜杠，不带开头的 bootfiles\。
std::wstring BootAssetPath(const wchar_t* relative);

// 纯决策（单测）：appHasBootfiles → appDir；否则 portable && originHasBootfiles →
// originDir；否则 appDir。
std::wstring PickBootAssetRoot(bool appHasBootfiles, bool portable,
                               bool originHasBootfiles,
                               const std::wstring& appDir,
                               const std::wstring& originDir);

// 当前登录用户名（文件命名用；失败返回 "User"）。
std::wstring CurrentUserName();

}  // namespace sysrecover
