// 九转还原：ops_space（阶段 2 从 src/app/ops.cpp 机械拆分而来，逻辑未改）。
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

// 目标分区是否**未**被占用（可以就地写）？—— 对外可见（ops.h），因为 GUI/CLI 要用它
// 决定提示文案（"需不需要重启"）；StageRestore 内部用的是同一个函数，不会漂移。
// 判据（用户规格）：不是正在运行的系统盘，且能对该卷加独占锁 ——
// FSCTL_LOCK_VOLUME 会因"卷上有打开的文件/句柄"而失败。
// 能就地写 → 直接还原（PE 里 / 还原到非系统盘）；否则走暂存 + 重启 + Linux。
bool CanRestoreInPlace(const PartitionInfo& t, std::string& why,
                       InPlaceReason* reason) {
    // reason：给调用方一个**稳定码**来选显示文案（GUI 确认框原来靠
    // `why.find("系统盘")` 匹配中文子串，消息一翻译就失效 —— PLAN §14 M1）
    auto setr = [&](InPlaceReason r) { if (reason) *reason = r; };
    if (t.letter.empty()) {
        setr(INPLACE_NO_LETTER);
        why = Tr("目标分区没有盘符（无法就地写）");
        return false;
    }
    wchar_t winDir[MAX_PATH] = {};
    if (GetWindowsDirectoryW(winDir, MAX_PATH) &&
        SameDrive(winDir[0], t.letter[0])) {
        setr(INPLACE_SYSTEM_DISK);
        why = Tr("目标是正在运行的系统盘（必须重启后脱机还原）");
        return false;
    }
    // 用户规格（2026-09-23）：**PE 里只要目标不是正在运行的系统盘，就应该就地还原**，
    // 不重启也不提示重启。PE 下目标分区通常并没被真正使用，但 FSCTL_LOCK_VOLUME 仍
    // 可能因各种句柄失败 → 会误判成"必须重启"（用户实测踩到）。所以 PE 下直接放行；
    // 正常 Windows 仍用卷锁判定，只有真的锁不上（分区被占用、无法就地还原）才提示重启。
    if (IsWinPE()) {
        why = Tr("在 PE 中运行（目标非运行系统盘）→ 就地还原");
        // PE 下 FSCTL_LOCK_VOLUME 常因各种句柄失败 → 不能据此判"必须重启"（会把 PE
        // 误判成"要重启"，用户实测踩到）。所以**放行**，但仍**试一下锁**：锁不上只记
        // warn 继续（而不是拒绝）——这样格式化/wimlib 真失败时日志里有线索（问题清单 M-04）。
        std::wstring peDev = L"\\\\.\\" + t.letter + L":";
        HANDLE peH = CreateFileW(peDev.c_str(), GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, 0, nullptr);
        if (peH == INVALID_HANDLE_VALUE) {
            LogWarn("PE: 打开目标卷失败 err=" + std::to_string(GetLastError()) +
                    "，仍尝试就地还原");
        } else {
            DWORD peRet = 0;
            if (DeviceIoControl(peH, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0,
                                &peRet, nullptr)) {
                DeviceIoControl(peH, FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0,
                                &peRet, nullptr);
            } else {
                LogWarn("PE: 目标卷加锁失败（可能有句柄占用）err=" +
                        std::to_string(GetLastError()) +
                        "，仍尝试就地还原；若格式化/写入失败请先排除占用");
            }
            CloseHandle(peH);
        }
        setr(INPLACE_PE);
        return true;
    }
    std::wstring dev = L"\\\\.\\" + t.letter + L":";
    HANDLE h = CreateFileW(dev.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        setr(INPLACE_OPEN_FAIL);
        why = Tr("打开目标卷失败 err=") + std::to_string(GetLastError());
        return false;
    }
    DWORD ret = 0;
    BOOL locked = DeviceIoControl(h, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0,
                                  &ret, nullptr);
    if (locked)
        DeviceIoControl(h, FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0, &ret,
                        nullptr);
    CloseHandle(h);
    setr(locked ? INPLACE_OK : INPLACE_LOCKED);
    why = locked ? Tr("目标分区未被占用") : Tr("目标分区正被使用（有打开的文件或句柄）");
    return locked != FALSE;
}
// 还原前空间预检（P1）—— 见 ops.h。**必须在格式化之前**调用。
int CheckRestoreSpace(const std::wstring& imagePath, int index,
                      const PartitionInfo& target, std::string& err) {
    unsigned long long bytes = 0;
    // 加密镜像走容器 meta（不解密正文）；普通镜像走 wimlib。两者同一口径。
    int rc = ImageContentSize(imagePath, index < 1 ? 1 : index, &bytes);
    if (rc != 0 || bytes == 0) {
        LogError("space precheck: cannot read image size (rc=" +
                 std::to_string(rc) + ") -> skip");
        return 0;  // 读不到大小 → 不阻断
    }
    // 内容量（已扣掉硬链接重复部分）+ 10% + 300MB（NTFS 元数据/目录索引/$LogFile 等开销）
    unsigned long long need = bytes + bytes / 10 + (300ull << 20);
    unsigned long long have = target.sizeBytes;
    char buf[320];
    snprintf(buf, sizeof(buf),
             "space precheck: content %.1f GB, need ~%.1f GB, target %.1f GB",
             bytes / 1073741824.0, need / 1073741824.0, have / 1073741824.0);
    LogInfo(buf);
    if (have && need > have) {
        // L1：数值先格式化，再用固定文本拼接（译文不再当格式串）。
        char needG[32], haveG[32];
        snprintf(needG, sizeof(needG), "%.1f", need / 1073741824.0);
        snprintf(haveG, sizeof(haveG), "%.1f", have / 1073741824.0);
        err = Tr("目标分区空间不足：镜像解压后约需 ");
        err += needG;
        err += Tr(" GB（已含余量），但目标分区只有 ");
        err += haveG;
        err += Tr(" GB。\n请换更大的目标分区，或改用更小的镜像。");
        return 1;
    }
    return 0;
}

// ── Q1「先落盘后搬家」（用户 2026-10-18）────────────────────────────────────
// 还原确认后、写契约/格式化之前调用：程序家目录所在物理盘 == 目标盘时，
// 静默整包复制到安全分区 + 日志跟过去（不 re-exec：当前进程照旧跑完本次操作，
// 副本供还原后的下次使用）。触发范围仅还原/返回旧系统；备份不搬。
// 静态变量：同一进程内只判一次（RetryStagedRestore 再次调用时直接返回上次结果）。
namespace {
SettleResult g_settleResult;
}

SettleResult SettleHome(int targetDiskIndex, int exeDiskIndex) {
    if (g_settleResult.checked)
        return g_settleResult;

    g_settleResult.checked = true;

    // 程序目录不在固定盘（PE 的 X:\ RAM 盘 / 光盘）→ 无需搬迁
    std::wstring exeDir = ExeDir();
    if (exeDir.empty() || exeDir.size() < 2 || exeDir[1] != L':') {
        g_settleResult.detail = "exe dir not on a fixed drive -> no settle";
        return g_settleResult;
    }
    wchar_t exeLetter = (wchar_t)towupper(exeDir[0]);

    // 取不到程序目录盘号 → 不搬（fail-open，不阻断还原）
    if (exeDiskIndex < 0) {
        g_settleResult.detail = "cannot resolve exe disk index -> no settle";
        return g_settleResult;
    }

    // 程序目录不在目标盘 → 无需搬迁
    if (targetDiskIndex < 0 || exeDiskIndex != targetDiskIndex) {
        g_settleResult.detail = "exe dir not on target disk -> no settle";
        return g_settleResult;
    }

    // 程序目录在目标盘上 → 找安全分区（排除目标盘 + 系统盘优先非系统）
    DWORD mask = GetLogicalDrives();
    std::vector<wchar_t> candidates;
    wchar_t winDir[MAX_PATH] = {};
    GetWindowsDirectoryW(winDir, MAX_PATH);
    wchar_t sysLetter = winDir[0];
    // 构建候选列表：所有可写固定盘，排除目标盘符
    std::vector<std::wstring> probeDirs;  // 记录探针建的目录，选完后清理未选中的
    for (wchar_t c = L'A'; c <= L'Z'; ++c) {
        if (!(mask & (1u << (c - L'A'))))
            continue;
        if (c == exeLetter)  // 目标盘（= 程序当前所在盘）
            continue;
        wchar_t root[4] = {c, L':', L'\\', 0};
        if (GetDriveTypeW(root) != DRIVE_FIXED)
            continue;
        // 快速可写探针
        std::wstring probeDir = std::wstring(root) + L"ZJRESTORE";
        CreateDirectoryW(probeDir.c_str(), nullptr);
        probeDirs.push_back(probeDir);
        wchar_t probeFile[260];
        swprintf(probeFile, 260, L"%s\\.zj-probe.tmp", probeDir.c_str());
        HANDLE ph = CreateFileW(probeFile, GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY |
                                                    FILE_FLAG_DELETE_ON_CLOSE,
                                nullptr);
        if (ph != INVALID_HANDLE_VALUE) {
            CloseHandle(ph);
            candidates.push_back(c);
        }
    }

    wchar_t chosen = SelectRelocateDrive(candidates, sysLetter, exeLetter);

    // 清理未选中候选盘上的空探针目录（避免在无关盘上留 ZJRESTORE 残迹）
    for (const auto& pd : probeDirs) {
        wchar_t pdLetter = pd[0];
        if (chosen != 0 && pdLetter == chosen)
            continue;  // 选中的保留（后续还要用它放整包）
        RemoveDirectoryW(pd.c_str());  // 只删空目录；有内容时静默失败
    }
    if (chosen == 0) {
        g_settleResult.noSafeDrive = true;
        g_settleResult.detail = "on target disk but no safe drive found";
        LogWarn("settle: on target disk but no safe drive found (exe dir will "
                "be lost with target)");
        return g_settleResult;
    }

    // 复制整包 + 日志 到 <chosen>:\ZJRESTORE\（注意行尾不能有 \，会吞下一行）
    std::wstring destRoot = std::wstring(1, chosen) + L":\\" + kRecoveryDir;
    CreateDirectoryW(destRoot.c_str(), nullptr);

    // 空间检查：整包大小 + 300MB 余量
    unsigned long long needBytes = AppTreeSize() + (300ull << 20);
    ULARGE_INTEGER avail = {};
    std::wstring availPath = std::wstring(1, chosen) + L":\\";
    if (GetDiskFreeSpaceExW(availPath.c_str(), &avail, nullptr, nullptr) &&
        avail.QuadPart < needBytes) {
        g_settleResult.noSafeDrive = true;
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "settle: not enough space on %c: need=%.1fGB avail=%.1fGB",
                 (char)chosen, needBytes / 1073741824.0,
                 avail.QuadPart / 1073741824.0);
        g_settleResult.detail = buf;
        LogWarn(buf);
        return g_settleResult;
    }

    LogInfo("settle: copying app tree + logs to " + W2U(destRoot));
    bool copyOk = CopyDir(exeDir, destRoot);
    if (!copyOk) {
        g_settleResult.noSafeDrive = true;
        g_settleResult.detail = "copy to " + W2U(destRoot) + " failed";
        LogWarn("settle: CopyDir to " + W2U(destRoot) + " failed");
        return g_settleResult;
    }

    // 日志也跟过去（当前进程继续写原位置，但 LogBaseDir 从此返回新家）
    std::wstring srcLogs = LogBaseDir() + L"\\logs";
    std::wstring dstLogs = destRoot + L"\\logs";
    CreateDirectoryW(dstLogs.c_str(), nullptr);
    CopyDir(srcLogs, dstLogs);

    // 设置日志根覆盖 + 返回结果
    SetLogHomeOverride(destRoot);
    g_settleResult.moved = true;
    g_settleResult.newHome = destRoot;
    g_settleResult.detail = "copied to " + W2U(destRoot);
    LogInfo("settle: done, new home = " + W2U(destRoot));
    return g_settleResult;
}

const SettleResult& LastSettleResult() { return g_settleResult; }

// ── A 引擎空间预检（用户 2026-10-09 规格，docs/22 §1.1）────────────────
bool EstimateSwapSpace(const PartitionInfo& target,
                       unsigned long long imageContentBytes,
                       SwapSpaceEstimate& out) {
    out = SwapSpaceEstimate{};
    out.content = imageContentBytes;
    if (target.letter.empty())
        return false;
    std::wstring root = target.letter + L":\\";
    auto fsz = [&](const wchar_t* name) -> unsigned long long {
        WIN32_FILE_ATTRIBUTE_DATA fad = {};
        if (!GetFileAttributesExW((root + name).c_str(), GetFileExInfoStandard,
                                  &fad))
            return 0;
        ULARGE_INTEGER u;
        u.HighPart = fad.nFileSizeHigh;
        u.LowPart = fad.nFileSizeLow;
        return u.QuadPart;
    };
    out.pagefile = fsz(L"pagefile.sys");
    out.swapfile = fsz(L"swapfile.sys");
    out.hiberfil = fsz(L"hiberfil.sys");
    out.slack = imageContentBytes / 10 + (2ull << 30);  // 内容×10% + 2GB
    out.required =
        out.content + out.pagefile + out.swapfile + out.hiberfil + out.slack;
    ULARGE_INTEGER freeAvail = {}, total = {}, totalFree = {};
    if (GetDiskFreeSpaceExW(root.c_str(), &freeAvail, &total, &totalFree))
        out.freeBytes = freeAvail.QuadPart;
    else
        out.certain = false;
    out.enough = out.freeBytes >= out.required;
    // 不够（或读不到空闲）→ 不确定：上层弹一次让用户确认（用户规格：
    // "够 → 直接开始；不确定的让用户自己确认"）。
    if (!out.enough)
        out.certain = false;
    return true;
}

// 供 GUI 在确认框之前预判 A 引擎空间（用户 2026-10-20 规格）：
// "开始前直接显示 需要 X GB / 可用 Y GB；够就不提示，不够才禁用 A 引擎按钮"。
bool QuerySwapSpace(const std::wstring& imagePath, int index,
                    const PartitionInfo& target, SwapSpaceEstimate& out) {
    out = SwapSpaceEstimate{};
    unsigned long long bytes = 0;
    int rc = ImageContentSize(imagePath, index < 1 ? 1 : index, &bytes);
    if (rc != 0 || bytes == 0)
        return false;  // 读不到大小 → 无法判定（按"够"继续，与原行为一致）
    return EstimateSwapSpace(target, bytes, out);
}

}  // namespace sysrecover
