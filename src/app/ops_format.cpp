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


// ── M8：格式化工具预检与 diskpart 回退 ──────────────────────────────
// 0.9.96 直接 RunProcess(SysToolPath(L"format.com"))，极简 PE 里该工具可能
// 不存在，结果只是一个笼统的 rc!=0。这里先探测文件是否存在：
//   · 有 format.com → 用它（沿用 /Q /Y，不传 /V，保持原卷标语义）；
//   · 没有 → 回退 diskpart（select volume <盘符> + format fs=ntfs quick）；
//   · 两者都没有 → 返回 -2，让上层能区分"工具缺失"与"格式化失败"。
static bool ToolFileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int FormatWithDiskpart(wchar_t letter, std::string& out) {
    wchar_t tmpDir[MAX_PATH] = {};
    if (!GetTempPathW(MAX_PATH, tmpDir))
        return -1;
    wchar_t scriptPath[MAX_PATH] = {};
    swprintf_s(scriptPath, L"%szj-format-%lu.txt", tmpDir,
               GetCurrentProcessId());
    // diskpart 脚本用纯 ASCII：盘符 + 快速格式化 NTFS。
    std::string script = "select volume ";
    script += static_cast<char>(letter);
    script += "\nformat fs=ntfs quick\nexit\n";
    HANDLE h = CreateFileW(scriptPath, GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    DWORD wrote = 0;
    const bool ok = WriteFile(h, script.data(), (DWORD)script.size(), &wrote,
                              nullptr) != 0;
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(scriptPath);
        return -1;
    }
    const int rc = RunProcess(SysToolPath(L"diskpart.exe"),
                              std::wstring(L"/s \"") + scriptPath + L"\"", out);
    DeleteFileW(scriptPath);
    return rc;
}

// PIT-211：格式化目标分区（A 引擎"先格式化再解压"用）。
// 返回 0=成功；-1=格式化失败；-2=format.com 与 diskpart 都不可用。
static int FormatTargetVolume(wchar_t letter, std::string& log,
                              std::string* rawOut) {
    const std::wstring fmt = SysToolPath(L"format.com");
    if (ToolFileExists(fmt)) {
        std::string out;
        const int rc = RunProcess(fmt,
                                  std::wstring(1, letter) + L": /FS:NTFS /Q /Y",
                                  out);
        log = "format.com rc=" + std::to_string(rc) + " / " + out;
        if (rawOut) *rawOut = out;
        return rc == 0 ? 0 : -1;
    }
    const std::wstring dp = SysToolPath(L"diskpart.exe");
    if (!ToolFileExists(dp)) {
        log = "format tool missing: neither format.com nor diskpart.exe";
        if (rawOut) *rawOut = log;
        LogWarn("format: no format.com and no diskpart.exe -> abort");
        return -2;
    }
    std::string out;
    const int rc = FormatWithDiskpart(letter, out);
    log = "format.com missing -> diskpart fallback rc=" +
          std::to_string(rc) + " / " + out;
    if (rawOut) *rawOut = out;
    LogInfo("format: format.com missing, used diskpart fallback");
    return rc == 0 ? 0 : -1;
}

// PIT-214：「不格式化」= 保留文件系统/卷标，只清空根目录内容（wimlib 卷模式
// 要求空卷；这也正是救援层 target_mode=keep 的语义）。
// 返回 0 = 干净；>0 = 有 N 项删不掉（SVI/$RECYCLE.BIN 正常，不阻断）。
// M9：枚举与回查都走 LongPath，深路径也能删到。
static int WipeVolumeContents(const std::wstring& root, std::string& log) {
    int failed = 0, removed = 0;
    const std::wstring lp = LongPath(root);
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW((lp + L"*").c_str(), &fd);
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
        if (GetFileAttributesW(LongPath(p).c_str()) == INVALID_FILE_ATTRIBUTES)
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
        // M8：返回值沿用旧契约（0 成功 / -1 失败）；-2（工具缺失）也归到 -1，
        // 但 log 中会明确写出 "format tool missing"，上层提示可区分。
        const int rc = FormatTargetVolume(letter, log, rawOut);
        return rc == 0 ? 0 : -1;
    }
    const int left = WipeVolumeContents(root, log);
    if (rawOut) *rawOut = log;
    return left;
}

}  // namespace sysrecover
