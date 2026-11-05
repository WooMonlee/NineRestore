// 九转还原：ops_format（阶段 2 从 src/app/ops.cpp 机械拆分而来，逻辑未改）。
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


// ── PIT-211：格式化目标分区（A 引擎"先格式化再解压"用）──────────────
bool FormatTargetVolume(wchar_t letter, std::string& log) {
    std::string out;
    const int rc = RunProcess(SysToolPath(L"format.com"),
                              std::wstring(1, letter) + L": /FS:NTFS /Q /Y",
                              out);
    log = "format.com rc=" + std::to_string(rc) + " / " + out;
    return rc == 0;
}

// PIT-214：「不格式化」= 保留文件系统/卷标，只清空根目录内容（wimlib 卷模式
// 要求空卷；这也正是救援层 target_mode=keep 的语义）。
// 返回 0 = 干净；>0 = 有 N 项删不掉（SVI/$RECYCLE.BIN 正常，不阻断）。
int WipeVolumeContents(const std::wstring& root, std::string& log) {
    int failed = 0, removed = 0;
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW((root + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        log = "wipe: cannot list " + W2U(root) + " err=" +
              std::to_string(GetLastError());
        return -1;
    }
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        const std::wstring p = root + fd.cFileName;
        DeleteTreeRec(p);
        if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES)
            ++removed;
        else
            ++failed;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    log = "wipe: removed=" + std::to_string(removed) +
          " left=" + std::to_string(failed);
    return failed;
}

}  // namespace sysrecover
