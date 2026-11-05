// 九转还原：ops_preserve（阶段 2 从 src/app/ops.cpp 机械拆分而来，逻辑未改）。
// 拆分动机：0.9.96 的 ops.cpp 约 4700 行，备份/暂存/就地/引导/卷操作全部
// 混在一个翻译单元里，无法按职责 review 或单测。本文件只搬代码，不改行为。
// 备份/还原共享操作层实现。语义对齐 CLI（Phase 2-4 已验证）与旧 C# 编排器。
#include "../common/i18n.h"
#include "../common/xpress.h"  // PIT-213：单文件载荷压缩
#include "ops.h"
#include "shortcut.h"
#include "swapleft.h"
#include "universal.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <vector>

#include "../boot/bcd.h"
#include "../boot/bootfix.h"
#include "../boot/bootpath.h"
#include "../boot/bootself.h"
#include "../boot/grub.h"
#include "../boot/task.h"
#include "../boot/uefi.h"
#include "../common/ea.h"
#include "../common/crypt.h"
#include "../common/logger.h"
#include "../common/pathutil.h"
#include "../common/cpucap.h"
#include "../common/process.h"
#include "../common/progress.h"
#include "../common/refscan.h"
#include "../common/relocate.h"
#include "../common/selfarch.h"
#include "../common/singleton.h"
#include "../common/vss.h"
#include "../common/version.h"
#include "../disk/disk.h"
#include "../disk/mbrcode.h"
#include "../wim/exclude.h"
#include "safety.h"
#include "selfdiag.h"
#include "imgsrc.h"

#include "ops_internal.h"

namespace sysrecover {


// PIT-216：把目标分区的**现有内容整体移到 <root>~old**（旧系统保留，可回退）。
// 同卷改名，秒级完成；返回移动的顶层条目数（<0 = 失败，未动数据）。
int PreserveOldSystem(const std::wstring& root, std::string& log) {
    const std::wstring oldDir = root + L"~old";
    if (GetFileAttributesW(oldDir.c_str()) != INVALID_FILE_ATTRIBUTES) {
        log = "~old already exists (returnold or delete it first)";
        return -2;
    }
    if (!CreateDirectoryW(oldDir.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        log = "create ~old failed err=" + std::to_string(GetLastError());
        return -1;
    }
    int moved = 0, failed = 0;
    bool winMoved = false;
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW((root + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        log = "cannot list target root";
        return -1;
    }
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        if (_wcsicmp(fd.cFileName, L"~old") == 0) continue;
        const std::wstring src = root + fd.cFileName;
        const std::wstring dst = oldDir + L"\\" + fd.cFileName;
        if (MoveFileExW(src.c_str(), dst.c_str(), 0)) {
            ++moved;
            if (_wcsicmp(fd.cFileName, L"Windows") == 0) winMoved = true;
        } else {
            ++failed;
            LogWarn("preserve-old: cannot move " + W2U(fd.cFileName) +
                    " err=" + std::to_string(GetLastError()));
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    log = "preserve-old: moved=" + std::to_string(moved) +
          " failed=" + std::to_string(failed) +
          " windows=" + std::to_string((int)winMoved);
    if (!winMoved) {
        log += " (Windows did not move -> abort)";
        return -1;
    }
    return moved;
}

// 反向：把 ~old 里的条目搬回根（apply 失败时回滚）。
void RollbackPreservedOld(const std::wstring& root, std::string& log) {
    const std::wstring oldDir = root + L"~old";
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW((oldDir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    int back = 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        const std::wstring src = oldDir + L"\\" + fd.cFileName;
        const std::wstring dst = root + fd.cFileName;
        if (GetFileAttributesW(dst.c_str()) == INVALID_FILE_ATTRIBUTES &&
            MoveFileExW(src.c_str(), dst.c_str(), 0))
            ++back;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    RemoveDirectoryW(oldDir.c_str());
    log = "rollback: moved back " + std::to_string(back);
}

}  // namespace sysrecover
