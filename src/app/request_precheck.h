// 备份/还原请求的**参数前置校验**（B9 下沉切片）。
//
// 现状盘点（为什么不搬更多）：
//   · 目标分区/空间/文件系统判据已经在 app 层 —— `CanRestoreInPlace`、
//     `CheckRestoreSpace`、`QuerySwapSpace`（ops_space.cpp）与备份侧的
//     FAT32/4GB/空间预检（ops_backup.cpp）。GUI 不再重复实现，无需搬家。
//   · 真正散在 GUI 的只剩"镜像路径是否像镜像、是否值得尝试打开"这一条，
//     即本文件的两个纯函数。它们不依赖任何 GUI 状态，供界面与后续 CLI 共用。
#pragma once

#include <string>

namespace sysrecover {

// 路径是否"像镜像文件"（以 .wim/.esd/.swm/.zje 结尾，忽略大小写）。
// 纯字符串判定：手写比较，不引 locale（避免 <cwctype> 依赖）。
bool LooksLikeImagePath(const std::wstring& path);

// 镜像路径是否"值得尝试打开"。
// ⚠️ 不要只用 GetFileAttributesW 预筛：GUI 带 requireAdministrator 清单（提权进程），
// 受 UAC 会话隔离影响，提权后的登录会话对 UNC 路径（\\server\share\...）常拿不到属性
// → 返回 INVALID → 以前直接跳过 LoadWimImages → 主按钮一直灰且无任何报错。
// 规则：能拿到属性时，目录一律不算；拿不到属性时，只要"像镜像"就交给 wimlib 去开。
bool ImagePathReady(const std::wstring& path);

}  // namespace sysrecover
