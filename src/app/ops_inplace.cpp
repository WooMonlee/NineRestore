// 九转还原：ops_inplace（阶段 2 从 src/app/ops.cpp 机械拆分而来，逻辑未改）。
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


// ── PIT-211：离线改计算机名（目标盘的 SYSTEM 库，不需要目标系统在运行）──
// 与在线 Rename-Computer 等价：ComputerName + ActiveComputerName +
// Tcpip\Parameters(Hostname / NV Hostname)；每个 ControlSet 都写。
bool SetComputerNameOffline(const std::wstring& winDir, const std::wstring& name,
                            std::string& log) {
    if (name.empty()) return true;
    const std::wstring hive = winDir + L"\\System32\\config\\SYSTEM";
    if (GetFileAttributesW(hive.c_str()) == INVALID_FILE_ATTRIBUTES) {
        log += "rename: SYSTEM hive missing (" + W2U(hive) + ")\n";
        return false;
    }
    auto enablePriv = [](const wchar_t* pn) {
        HANDLE tok = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(),
                              TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
            return;
        TOKEN_PRIVILEGES tp = {};
        tp.PrivilegeCount = 1;
        if (LookupPrivilegeValueW(nullptr, pn, &tp.Privileges[0].Luid)) {
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), nullptr, nullptr);
        }
        CloseHandle(tok);
    };
    enablePriv(L"SeBackupPrivilege");
    enablePriv(L"SeRestorePrivilege");
    const wchar_t* kTmp = L"ZJ_SYSNAME";
    RegUnLoadKeyW(HKEY_LOCAL_MACHINE, kTmp);  // 清上次异常退出的残留
    const LONG lr = RegLoadKeyW(HKEY_LOCAL_MACHINE, kTmp, hive.c_str());
    if (lr != ERROR_SUCCESS) {
        log += "rename: RegLoadKey failed rc=" + std::to_string(lr) + "\n";
        return false;
    }
    auto setSz = [&](const std::wstring& sub, const wchar_t* value,
                     const std::wstring& data) {
        HKEY k = nullptr;
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, sub.c_str(), 0, nullptr, 0,
                            KEY_SET_VALUE, nullptr, &k, nullptr) !=
            ERROR_SUCCESS)
            return;
        RegSetValueExW(k, value, 0, REG_SZ, (const BYTE*)data.c_str(),
                       (DWORD)((data.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
    };
    bool wrote = false;
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kTmp, 0, KEY_READ, &root) ==
        ERROR_SUCCESS) {
        wchar_t sub[256];
        for (DWORD i = 0;; ++i) {
            DWORD len = 256;
            if (RegEnumKeyExW(root, i, sub, &len, nullptr, nullptr, nullptr,
                              nullptr) != ERROR_SUCCESS)
                break;
            const std::wstring cs(sub);
            if (_wcsnicmp(cs.c_str(), L"ControlSet", 10) != 0) continue;
            const std::wstring base = std::wstring(kTmp) + L"\\" + cs;
            setSz(base + L"\\Control\\ComputerName\\ComputerName",
                  L"ComputerName", name);
            setSz(base + L"\\Control\\ComputerName\\ActiveComputerName",
                  L"ComputerName", name);
            setSz(base + L"\\Services\\Tcpip\\Parameters", L"Hostname", name);
            setSz(base + L"\\Services\\Tcpip\\Parameters", L"NV Hostname", name);
            wrote = true;
        }
        RegCloseKey(root);
    }
    RegUnLoadKeyW(HKEY_LOCAL_MACHINE, kTmp);
    if (!wrote)
        log += "rename: no ControlSet found in SYSTEM hive\n";
    else
        log += "rename: " + W2U(name) + " stored offline\n";
    return wrote;
}


// 就地还原：快速格式化 → wimlib 目录模式应用（PIT-008）→ 镜像带 ESP 子镜像
// 就恢复 ESP → 系统镜像才 bcdboot 修引导。**全程不重启**。
int RunDirectRestore(const RestoreRequest& req, const PartitionInfo& target,
                     const std::wstring& imagePath, int espIndex, int eaIndex,
                     std::string& err, ProgressFn progress) {
    std::wstring letter(1, target.letter[0]);
    LogInfo("in-place restore -> " + W2U(letter) + ": (" + W2U(imagePath) +
            ", index=" + std::to_string(req.index) + ")");
    // 1) 目标分区准备（PIT-214）：
    //    默认快速格式化（不传 /V：保留原卷标）；
    //    用户在确认框里**取消勾选**时 → 保留文件系统、只清空内容。
    bool oldPreserved = false;
    if (req.preserveOld) {
        // PIT-216：旧系统整体挪到 ~old，再把新系统解压到根 —— 失败可「返回旧系统」。
        std::string plog;
        ProgressUpdate("restore", 1, "preserve-old");
        const int moved = PreserveOldSystem(letter + L":\\", plog);
        LogInfo(plog);
        if (moved < 0) {
            err = Tr("就地还原：保留旧系统失败（") + plog + Tr("）");
            LogError(err);
            ProgressDone("restore", "failed");
            return 1;
        }
        oldPreserved = true;
    } else {
        // 阶段 2 收敛：格式化与「只清空内容」走同一个入口 PrepareTargetVolume，
        // 与 A 引擎的 formatSwap 共用实现（PIT-211 / PIT-214）。行为保持不变：
        // 日志文案、错误文案、残余项告警均按原样保留；前置校验在阶段 3 加入。
        std::string plog;
        std::string raw;
        const int prep = PrepareTargetVolume(letter + L":\\", letter[0],
                                             req.formatTarget, plog, &raw);
        if (req.formatTarget) {
            LogInfo(plog);
            if (prep != 0) {
                // L7（1.0）：此处 oldPreserved 恒为 false（只有 preserveOld 分支会
                // 置 true，而那是上面的 if 分支），原回滚代码是死代码，删除。
                err = Tr("就地还原：格式化目标分区失败（") + raw + Tr("）");
                LogError(err);
                ProgressDone("restore", "failed");
                return 1;
            }
        } else {
            LogInfo("no-format restore: " + plog);
            if (prep < 0) {
                err = Tr("就地还原：无法清空目标分区（") + plog + Tr("）");
                LogError(err);
                ProgressDone("restore", "failed");
                return 1;
            }
            if (prep > 0)
                LogWarn("no-format restore: " + std::to_string(prep) +
                        " item(s) could not be removed (usually SVI/$RECYCLE.BIN)");
        }
    }
    // 2) 应用镜像（目录模式）
    WimEngine wim;
    if (!wim.ok()) {
        err = Tr("wimlib 初始化失败");
        ProgressDone("restore", "failed");
        return 1;
    }
    int arc = wim.Apply(imagePath, req.index, letter + L":\\",
                        [progress](int pct, const std::string& st) {
                            ProgressUpdate("restore", pct, st);
                            // 转投给上层（GUI 用来刷新进度条/百分比）；返回 true = 中止。
                            if (progress)
                                return progress(pct, st);
                            return false;
                        });
    if (arc != 0) {
        if (oldPreserved) {
            std::string rlog;
            RollbackPreservedOld(letter + L":\\", rlog);
            LogWarn("preserve-old rollback: " + rlog);
        }
        err = Tr("就地还原：应用镜像失败 rc=") + std::to_string(arc) + " (" +
              W2U(WimEngine::ErrorString(arc)) + ")";
        LogError(err);
        ProgressDone("restore", "failed");
        return 1;
    }
    // 异机还原（2026-10-21）：Windows 侧 apply 完、修引导之前注入启动驱动。
    if (req.universal) {
        std::string ulog;
        std::wstring uerr;
        auto uProgress = [&](int pct, const std::string& st) {
            ProgressUpdate("restore", pct, st);
            return progress ? progress(pct, st) : false;
        };
        if (!PrepareUniversal(letter + L":", req.driverDir, ulog, uerr,
                              uProgress)) {
            err = Tr("就地还原已完成，但异机还原准备失败：") + W2U(uerr);
            LogError(err);
            LogError("universal log:\n" + ulog);
            ProgressDone("restore", "failed");
            return 1;
        }
        LogInfo("universal: in-place prepared\n" + ulog);
    }
    // 2.5) ESP 子镜像恢复（方案 C · 就地还原侧；2026-09-30 用户规格：
    //      "还原时根据其备份的ESP分区自动还原ESP"）。与救援层同款语义：
    //      镜像里带 ESP 子镜像（备份时勾了 --esp，任务 esp_index>0）且本机
    //      找得到 ESP → 目录模式 apply（只加不删；BCD 捕获时已排除）。
    //      · 放在 bcdboot **之前** —— 让 bcdboot 最后写，引导文件/BCD 权威；
    //      · **非致命**：失败/找不到 ESP 只记日志，主系统已还原不动摇
    //        （对齐救援层 `say ERROR` 不中断主流程的先例）；
    //      · 传空进度回调：ESP 很小、瞬间完成，不回卷 GUI 进度条。
    if (espIndex > 0) {
        std::wstring espRoot;
        bool guidMatched = false;
        std::string blog;
        if (AcquireEspRoot(espRoot, guidMatched, blog, (int)target.diskIndex)) {
            int erc = wim.Apply(imagePath, espIndex, espRoot, ProgressFn{});
            LogInfo("esp subimage apply (in-place) idx=" +
                    std::to_string(espIndex) + " -> " + W2U(espRoot) +
                    " rc=" + std::to_string(erc));
            if (erc != 0)
                LogWarn("ESP subimage restore failed rc=" +
                        std::to_string(erc) +
                        " (main system restored; ESP not updated)");
            UnmountEsp(espRoot, blog);
        } else {
            LogWarn("task has esp subimage (idx " + std::to_string(espIndex) +
                    ") but no ESP partition found (skip); " + blog);
        }
    }
    // 2.6) ZJEA 修复子镜像（PIT-122 / PIT-136；1019 案 F4 补上**就地还原侧**）：
    //      Windows 侧 apply 原生保 EA（PIT-120），但"被排除的非微软重解析点
    //      文件"（PIT-135）与整体 EA 修复数据都在 ZJEA 子镜像里 —— 就地还原
    //      原先完全没走这条链。语义与救援层一致：解出 eapack/reppack 当场
    //      写回（本进程在正常 Windows / PE，Windows API 直接可用，无需补写器
    //      与 GPO 钩子）；失败**非致命**（主系统已还原不动摇，只记日志）。
    //      顺序：先 reppack（重建被排除的文件）再 eapack（其 EA 才找得到文件）。
    if (eaIndex > 0) {
        std::wstring tmp;
        {
            wchar_t buf[MAX_PATH + 1] = {};
            DWORD n = GetTempPathW(MAX_PATH, buf);
            tmp.assign(buf, n);
        }
        while (!tmp.empty() && (tmp.back() == L'\\' || tmp.back() == L'/'))
            tmp.pop_back();
        wchar_t suffix[80] = {};
        swprintf(suffix, 80, L"\\SysRecoverEA-%lu-%llu",
                 (unsigned long)GetCurrentProcessId(),
                 (unsigned long long)GetTickCount64());
        tmp += suffix;
        if (CreateDirectoryW(tmp.c_str(), nullptr)) {
            // \* = 子镜像根全部条目（eapack.dat / reppack.dat / 补写器 / 策略
            // 片段；后两者就地路径用不到，解出来无妨）。
            int xrc = wim.ExtractPaths(imagePath, eaIndex, {L"\\*"}, tmp);
            if (xrc != 0) {
                LogWarn("EA fix pack (ZJEA) extract failed rc=" +
                        std::to_string(xrc) + " (in-place restore continues)");
            } else {
                std::wstring root = letter + L":";
                auto log = [](const std::string& s) {
                    LogInfo("ea-apply: " + s);
                };
                std::wstring repPack = tmp + L"\\reppack.dat";
                if (GetFileAttributesW(repPack.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    unsigned long long rok = 0, rbad = 0;
                    int rrc = ea::ApplyRepack(repPack, root, &rok, &rbad, log);
                    LogInfo("reppack (in-place): rc=" + std::to_string(rrc) +
                            " ok=" + std::to_string(rok) +
                            " failed=" + std::to_string(rbad));
                }
                std::wstring eaPack = tmp + L"\\eapack.dat";
                if (GetFileAttributesW(eaPack.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    ea::ApplyStats st;
                    int prc = ea::ApplyPack(eaPack, root, st, log);
                    LogInfo("eapack (in-place): rc=" + std::to_string(prc) +
                            " ok=" + std::to_string(st.ok) +
                            " missing=" + std::to_string(st.missing) +
                            " failed=" + std::to_string(st.failed));
                }
            }
            DeleteTreeRec(tmp);
        } else {
            LogWarn("EA fix temp dir create failed; skipped (in-place restore "
                    "continues)");
        }
    }
    // 3) 只有系统镜像才修引导（看目标里有没有 winload.exe）
    if (req.repairBoot) {
        std::wstring winDir = letter + L":\\Windows";
        if (GetFileAttributesW((winDir + L"\\System32\\winload.exe").c_str()) !=
            INVALID_FILE_ATTRIBUTES) {
            std::string out;
            if (UseUefiBootFor(target)) {
                // 找 ESP（含回退）+ 先预检目标模板（docs/15 · P2/P4）
                std::wstring espRoot;
                bool guidMatched = false;
                std::string blog;
                std::string detail;
                if (!AcquireEspRoot(espRoot, guidMatched, blog,
                                     (int)target.diskIndex)) {
                    LogError("ESP acquire log (in-place):\n" + blog);
                    err = Tr("就地还原已完成，但找不到可用的 ESP 引导分区，无法修复引导：\n") +
                          W2U(DescribePartitions());
                    LogError(err);
                    ProgressDone("restore", "failed");
                    return 1;
                }
                if (!guidMatched)
                    LogWarn("ESP partition type is not EFI System; used fallback");
                // PIT-216：先清掉以前失败/取消留下的**临时启动项**。它们指向的
                // 内核可能已经删了，却还顶在 BootOrder[0] —— 固件每次先启它，
                // 就永远进不了 Windows（这台客户机就是这么卡的）。
                {
                    std::string clog;
                    CleanupStaleUefiEntries(clog);
                    if (!clog.empty())
                        LogInfo("stale uefi entry cleanup: " + clog);
                }
                // 0.6.35（用户 2026-10-05 规格）：**ESP BCD 已经有效就跳过 bcdboot**
                // —— bcdboot 会把 BCD 重建成只剩 Windows 条目，第三方启动项（易数
                // 一键还原等）被抹掉（客户实测投诉）。同机还原时原 BCD 本就指向同
                // 一块 C:；只有缺失/损坏时才重建（重建前导出备份）。
                // 2026-10-27（用户反馈 2）：bcdboot **缺失或失败**时用随包资产自足
                // （极简 PE 里根本没有 bcdboot；以前这里直接失败、只能拿 fixboot.exe 修）。
                std::string vd;
                if (VerifyEspBcd(espRoot, winDir, vd)) {
                    LogInfo("ESP BCD already valid -> skip bcdboot (preserve "
                            "third-party boot entries): " + vd);
                } else {
                    LogInfo("ESP BCD invalid -> rebuild: " + vd);
                    bool rebuilt = false;
                    if (ToolAvailable(L"bcdboot.exe") &&
                        TargetBcdTemplateOk(letter[0], detail) &&
                        EspHasRoomForBcdboot(espRoot, detail)) {
                        std::wstring ldir = LogBaseDir() + L"\\logs";
                        CreateDirectoryW(ldir.c_str(), nullptr);
                        BcdExport(ldir + L"\\bcd-backup-before-bcdboot");
                        int brc = RunProcess(
                            SysToolPath(L"bcdboot.exe"),
                            winDir + L" /s " + std::wstring(1, espRoot[0]) +
                                L": /f UEFI",
                            out);
                        LogInfo("bcdboot (UEFI) rc=" + std::to_string(brc) +
                                " / " + out);
                        rebuilt = brc == 0;
                    } else {
                        LogInfo("bcdboot unavailable/not applicable -> "
                                "self-contained ESP boot");
                    }
                    if (!rebuilt) {
                        std::string slog, serr;
                        const bool sok = EnsureUefiBootFiles(
                            espRoot, std::wstring(1, letter[0]) + L":\\", winDir,
                            AppDir(), /*overwriteBcd=*/true, slog, serr);
                        LogInfo(std::string("ESP self-contained boot: ") +
                                (sok ? "ok" : "FAILED") + "\n" + slog);
                        if (!sok) {
                            err = Tr("就地还原已完成，但写入 ESP 引导失败：") + serr +
                                  Tr("\n可进入 PE 用 `SysRecover.exe repair-boot` 重试修复引导。");
                            UnmountEsp(espRoot, blog);
                            LogError(err);
                            ProgressDone("restore", "failed");
                            return 1;
                        }
                    }
                    if (!VerifyEspBcd(espRoot, winDir, detail)) {
                        err = Tr("就地还原已完成，但 ESP 引导校验未通过：") + detail +
                              Tr("\n可进入 PE 用 `SysRecover.exe repair-boot` 重试修复引导。");
                        UnmountEsp(espRoot, blog);
                        LogError(err);
                        ProgressDone("restore", "failed");
                        return 1;
                    }
                }
                // PIT-209：先清"幽灵启动项"（分区已删 / 分区里没有 Windows），
                // 否则"只有一个系统"的机器也会在开机时停在选择菜单上。
                {
                    const std::wstring store =
                        espRoot + L"EFI\\Microsoft\\Boot\\BCD";
                    std::string plog, qlog;
                    int osLeft = -1;
                    const int pruned =
                        BcdPruneDeadEntries(store, winDir, &osLeft, plog);
                    LogInfo("bcd prune (uefi in-place): removed=" +
                            std::to_string(pruned) + " reachable_os=" +
                            std::to_string(osLeft) + "\n" + plog);
                    BcdQuietSingleOsMenu(store, qlog);
                    LogInfo("bcd menu policy (uefi in-place): " + qlog);
                }
                // PIT-208：不论"跳过 bcdboot"还是"重建"，都要确保固件里有指向
                // Windows 的条目，并且**排在 BootOrder 第一位** —— 否则还原后
                // 固件可能先去启动第三方恢复工具/已删内核的陈旧条目，表现就是
                // "还原成功、开机却反复重启进不了系统"（2026-11-02 客户机）。
                {
                    std::string wlog;
                    if (EnsureWindowsBootEntry(espRoot, wlog, /*front=*/true))
                        LogInfo("firmware Windows boot entry: " + wlog);
                    else
                        LogWarn("firmware Windows boot entry (non-fatal): " +
                                wlog);
                }
                UnmountEsp(espRoot, blog);
            } else {
                // BIOS：目标自身已有可引导 BCD（bootmgr + \Boot\BCD + winload 条目）
                // → 跳过重建，保住第三方启动项（同 UEFI 规则，0.6.35）。
                // 2026-10-27（用户反馈 2）：没有 bcdboot 时用随包模板自足。
                std::string vd;
                if (BiosBcdLooksValid(letter[0], vd)) {
                    LogInfo("BIOS BCD already valid -> skip rebuild (preserve "
                            "third-party boot entries): " + vd);
                } else {
                    LogInfo("BIOS BCD invalid -> rebuild: " + vd);
                    bool rebuilt = false;
                    if (ToolAvailable(L"bcdboot.exe")) {
                        std::wstring ldir = LogBaseDir() + L"\\logs";
                        CreateDirectoryW(ldir.c_str(), nullptr);
                        BcdExport(ldir + L"\\bcd-backup-before-bcdboot");
                        int brc = RunProcess(
                            SysToolPath(L"bcdboot.exe"),
                            winDir + L" /s " + letter + L": /f BIOS", out);
                        LogInfo("bcdboot (BIOS) rc=" + std::to_string(brc) +
                                " / " + out);
                        rebuilt = brc == 0;
                    } else {
                        LogInfo("bcdboot.exe missing -> self-contained BIOS boot");
                    }
                    if (!rebuilt) {
                        std::string slog, serr;
                        const bool sok = EnsureBiosBootFiles(
                            std::wstring(1, letter[0]) + L":\\", winDir,
                            AppDir(), /*overwriteBcd=*/true, slog, serr);
                        LogInfo(std::string("BIOS self-contained boot: ") +
                                (sok ? "ok" : "FAILED") + "\n" + slog);
                        if (!sok) {
                            err = Tr("就地还原已完成，但写入引导失败：") + serr +
                                  Tr("\n可进入 PE 用 `SysRecover.exe repair-boot` 重试修复引导。");
                            LogError(err);
                            ProgressDone("restore", "failed");
                            return 1;
                        }
                    }
                    if (!BiosBcdLooksValid(letter[0], vd))
                        LogWarn("BIOS boot verify note: " + vd);
                }
                // PIT-209：清"幽灵启动项" + 单系统菜单收敛（BIOS 的库在
                // <目标>:\Boot\BCD；装了我们的常驻菜单时 displayorder 有 2 条，
                // BcdQuietSingleOsMenu 会自己让路）。
                {
                    const std::wstring store =
                        std::wstring(1, letter[0]) + L":\\Boot\\BCD";
                    std::string plog, qlog;
                    int osLeft = -1;
                    const int pruned =
                        BcdPruneDeadEntries(store, winDir, &osLeft, plog);
                    LogInfo("bcd prune (bios in-place): removed=" +
                            std::to_string(pruned) + " reachable_os=" +
                            std::to_string(osLeft) + "\n" + plog);
                    BcdQuietSingleOsMenu(store, qlog);
                    LogInfo("bcd menu policy (bios in-place): " + qlog);
                }
                // 分区引导区/磁盘 MBR 代码：format.com 之后不保证是微软引导代码，
                // 而 BIOS 引导缺了它就是黑屏光标闪（PIT-053）。**非致命**：安全软件
                // 可能拦写（火绒「写磁盘保留扇区」；bootself 内部已自动走物理盘兜底），
                // 写不进去只告警，不把还原判失败。
                std::string prlog;
                if (EnsureNtfsBootRegion(std::wstring(1, letter[0]) + L":\\",
                                         AppDir(), prlog))
                    LogInfo("pbr: " + prlog);
                else
                    LogWarn("pbr not fully written (non-fatal): " + prlog);
                if (DiskStyleOf(target.diskIndex) == PartitionStyle::MBR) {
                    std::string mrlog;
                    if (EnsureDiskMbrCode(target.diskIndex, AppDir(), mrlog))
                        LogInfo("mbr: " + mrlog);
                    else
                        LogWarn("mbr boot code (non-fatal): " + mrlog);
                }
            }
        } else {
            LogInfo("in-place: no winload.exe -> not a system image, skip "
                    "bcdboot");
        }
    }
    // PIT-211：就地还原 = 目标盘离线的完整新系统 → 直接改 SYSTEM 库
    if (!req.computerName.empty()) {
        std::string rlog;
        if (SetComputerNameOffline(letter + L":\\Windows", req.computerName,
                                   rlog))
            LogInfo("in-place rename: " + rlog);
        else
            LogWarn("in-place rename failed (non-fatal): " + rlog);
    }
    ProgressDone("restore", "done");
    LogInfo("in-place restore done (no reboot needed)");
    return 0;
}


// 往离线 ~new 的 SOFTWARE hive 里写一条 RunOnce（新系统**开机自动跑**）——
//   keepOld=false：跑 zj-swap-cleanup.cmd 静默删 ~old（无人值守默认）；
//   keepOld=true ：首启自动运行本程序 → 搬日志 + 问「是否删除旧系统」
//                  （用户 2026-10-17 规格："全自动化完成日志搬迁后再弹删除询问"）。
// 挂离线 hive 需要 SeBackup/SeRestore 特权（管理员默认持有但要显式启用）。
// 失败只 WARN 不阻断：程序已在桌面放快捷方式，用户可手动点。
//   hiveName ：临时挂载名（两个分支互斥，不会同进程冲突）；
//   valueName：RunOnce 值名（两个分支不同，深夜不会互相覆盖）；
//   cmd      ：要跑的启动命令行（REG_EXPAND_SZ 时支持 %SystemDrive% 展开）。
bool InjectOfflineRunOnce(const std::wstring& newDir, const wchar_t* hiveName,
                          const wchar_t* valueName, const wchar_t* cmd,
                          DWORD type, const char* what) {
    auto enablePriv = [](const wchar_t* name) {
        HANDLE tok = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(),
                              TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
            return false;
        TOKEN_PRIVILEGES tp = {};
        tp.PrivilegeCount = 1;
        LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid);
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), nullptr, nullptr);
        bool ok = GetLastError() == ERROR_SUCCESS;
        CloseHandle(tok);
        return ok;
    };
    enablePriv(L"SeBackupPrivilege");
    enablePriv(L"SeRestorePrivilege");
    const std::wstring hive = newDir + L"\\Windows\\System32\\config\\SOFTWARE";
    LONG rc = RegLoadKeyW(HKEY_LOCAL_MACHINE, hiveName, hive.c_str());
    if (rc != ERROR_SUCCESS) {
        LogWarn(std::string("A-engine: ") + what +
                " SOFTWARE hive load failed rc=" + std::to_string(rc));
        return false;
    }
    HKEY hKey = nullptr;
    const std::wstring sub = std::wstring(hiveName) +
                             L"\\Microsoft\\Windows\\CurrentVersion\\RunOnce";
    rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, sub.c_str(), 0, nullptr, 0,
                         KEY_WRITE, nullptr, &hKey, nullptr);
    if (rc == ERROR_SUCCESS) {
        rc = RegSetValueExW(hKey, valueName, 0, type, (const BYTE*)cmd,
                            (DWORD)((wcslen(cmd) + 1) * 2));
        RegCloseKey(hKey);
    }
    RegUnLoadKeyW(HKEY_LOCAL_MACHINE, hiveName);
    if (rc == ERROR_SUCCESS)
        LogInfo(std::string("A-engine: ") + what +
                " RunOnce injected into ~new");
    else
        LogWarn(std::string("A-engine: ") + what +
                " RunOnce inject failed rc=" + std::to_string(rc));
    return rc == ERROR_SUCCESS;
}


// 把首启清理器写进新系统，并挂 RunOnce：
//   - 只用于**日常还原的临时菜单**（menuEntry=false）；
//   - A 引擎把 helper + 公共启动文件夹 cmd 写进 ~new（不写 RunOnce）；
//   - B 引擎由救援层写同样两份文件（target 已没有离线 ~new）；
//   - cmd 先启动非提权 helper → 弹提示 → helper 自己再请求 UAC。
bool InstallFirstBootCleanup(const std::wstring& newDir,
                             const std::wstring& exeDir, std::string& log) {
    const std::wstring src = exeDir + L"\\bootfiles\\zj-restore-boot.exe";
    if (GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES) {
        log += "first-boot cleanup helper missing: zj-restore-boot.exe\n";
        return false;
    }
    const std::wstring dir = newDir + L"\\ProgramData\\SysRecover";
    const std::wstring startup =
        newDir + L"\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs\\Startup";
    CreateDirectoryW((newDir + L"\\ProgramData").c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    CreateDirectoryW(startup.c_str(), nullptr);
    const std::wstring dst = dir + L"\\zj-restore-boot.exe";
    SetFileAttributesW(dst.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
        log += "copy zj-restore-boot.exe failed err=" +
               std::to_string(GetLastError()) + "\n";
        return false;
    }
    // 启动文件夹：首次登录由资源管理器以普通权限启动 helper；helper 先弹说明，
    // 再自行 ShellExecute(runas) 请求 UAC。非提权实例会等待提权实例完成；
    // 成功后 cmd 立刻自删；UAC 取消/失败则 cmd 保留，下次登录重试。
    const char kCmd[] =
        "@echo off\r\n"
        "if exist \"%ProgramData%\\SysRecover\\boot-cleanup.done\" goto done\r\n"
        "\"%ProgramData%\\SysRecover\\zj-restore-boot.exe\"\r\n"
        "if exist \"%ProgramData%\\SysRecover\\boot-cleanup.done\" del \"%~f0\"\r\n"
        "goto :eof\r\n"
        ":done\r\n"
        "del \"%~f0\"\r\n";
    const std::wstring cmdPath = startup + L"\\SysRecover-RestoreBoot.cmd";
    HANDLE h = CreateFileW(cmdPath.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        log += "write startup cmd failed err=" +
               std::to_string(GetLastError()) + "\n";
        return false;
    }
    DWORD wr = 0;
    WriteFile(h, kCmd, (DWORD)(sizeof(kCmd) - 1), &wr, nullptr);
    CloseHandle(h);
    log += "first-boot cleanup deployed (helper + startup cmd)\n";
    return true;
}

}  // namespace sysrecover
