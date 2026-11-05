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
static bool FormatTargetVolume(wchar_t letter, std::string& log) {
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
static int WipeVolumeContents(const std::wstring& root, std::string& log) {
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

// 目标卷准备的**唯一入口**（阶段 2 收敛；行为与 0.9.96 的三处调用完全一致）：
//   · formatIt=true  → format.com /FS:NTFS /Q /Y（不传 /V，保留原卷标语义）；
//   · formatIt=false → 保留文件系统、只清空根目录内容（救援层 target_mode=keep
//     同语义，PIT-214）。
// 返回值与旧函数保持一一对应：0 成功；-1 失败（format 失败或清空无法列目录）；
// >0 = 清空后仍有 N 项删不掉（SVI/$RECYCLE.BIN 常见，调用方按非致命告警处理）。
// rawOut（可选）：回传未加前缀的原始输出（format 模式为 format.com 输出，
// keep 模式同 log），供调用方拼错误文案时不丢细节。
// 注意：阶段 3 会在这里加「破坏性操作前置检查」；本提交只做结构收敛，不改行为。
int PrepareTargetVolume(const std::wstring& root, wchar_t letter, bool formatIt,
                        std::string& log, std::string* rawOut) {
    if (formatIt) {
        std::string out;
        const int rc = RunProcess(SysToolPath(L"format.com"),
                                  std::wstring(1, letter) + L": /FS:NTFS /Q /Y",
                                  out);
        log = "format.com rc=" + std::to_string(rc) + " / " + out;
        if (rawOut) *rawOut = out;
        return rc == 0 ? 0 : -1;
    }
    const int left = WipeVolumeContents(root, log);
    if (rawOut) *rawOut = log;
    return left;
}

}  // namespace sysrecover
