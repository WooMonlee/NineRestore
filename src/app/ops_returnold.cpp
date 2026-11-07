// 九转还原：ops_returnold（阶段 2 从 src/app/ops.cpp 机械拆分而来，逻辑未改）。
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

// ── 返回旧系统（PIT-139，方案 C）──────────────────────────────────────
// 只读探测：目标根是否同时具备「完整旧系统 ~/.old\Windows」与「完整新系统
// 根 \Windows」。两者都在才谈得上"反向交换"；只有其一说明上次交换中断或
// 已清理过 —— 此时改走「删除旧系统」或不再提供该入口。
bool DetectReturnOldCapability(const PartitionInfo& target, bool* hasOldTree,
                               bool* hasRootTree, std::string& detail) {
    if (hasOldTree) *hasOldTree = false;
    if (hasRootTree) *hasRootTree = false;
    detail.clear();
    if (target.letter.empty()) {
        detail = "no drive letter";
        return false;
    }
    const std::wstring root = target.letter + L":\\";
    auto isDir = [&](const std::wstring& rel) -> bool {
        DWORD a = GetFileAttributesW((root + rel).c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
    };
    const bool oldDir = isDir(L"~old");
    const bool oldWin = isDir(L"~old\\Windows");
    const bool rootWin = isDir(L"Windows");
    if (hasOldTree) *hasOldTree = oldDir && oldWin;
    if (hasRootTree) *hasRootTree = rootWin;
    detail = "~old=" + std::string(oldDir ? "yes" : "no") +
             " ~old\\Windows=" + std::string(oldWin ? "yes" : "no") +
             " root\\Windows=" + std::string(rootWin ? "yes" : "no");
    return oldDir && oldWin && rootWin;
}

int StageReturnOld(const PartitionInfo& target, std::string& err,
                   bool* needReboot) {
    if (needReboot)
        *needReboot = false;

    // 等启动期后台残留清理收尾（与 StageRestore 同款，别在它还没收手时动盘）
    if (!target.letter.empty())
        WaitSwapCleanup(target.letter[0]);

    bool hasOld = false, hasRoot = false;
    std::string det;
    DetectReturnOldCapability(target, &hasOld, &hasRoot, det);
    if (!hasOld) {
        err = std::string(Tr("目标分区上没有可用的旧系统（~old\\Windows 不存在）—— 无法返回。")) +
              "\n" + det;
        LogError("return-old rejected: " + det);
        return 4;
    }
    if (!hasRoot) {
        err = std::string(Tr("目标分区根目录不是完整的当前系统（\\Windows 不存在）—— 无法返回。")) +
              "\n" + det;
        LogError("return-old rejected: " + det);
        return 4;
    }
    if (target.letter.empty()) {
        err = Tr("目标分区无盘符，无法写恢复日志（Linux 侧将无法定位目标）");
        LogError(err);
        return 1;
    }
    if (HasInstalledMenu()) {
        err = Tr("已安装常驻恢复菜单；返回旧系统会覆盖它。请先在主界面点「清除菜单」，再重试。");
        LogError("return-old rejected: persistent menu installed");
        return 4;
    }

    // Q2 暂存路径会话前置（与 StageRestoreImpl 同款，见 bootpath.h）：
    // 返回旧系统 = 重启进 Linux 救援层，依赖重启后固件沿当前同一种方式引导。
    switch (CheckStagedBootPath(IsUefiFirmware(), DiskStyleOf(target.diskIndex))) {
        case StagedBootPath::BiosSessionGptTarget:
            err = Tr("当前会话是传统(CSM)引导，目标是 GPT 盘 —— 返回旧系统需要重启进恢复环境，"
                     "但 BIOS 链无法引导 GPT 盘。请改用 UEFI 模式启动后操作。");
            LogError("return-old rejected: BIOS session + GPT target");
            return 4;
        case StagedBootPath::UefiSessionMbrTarget:
            err = Tr("当前会话是 UEFI 引导，目标是 MBR 盘 —— 返回旧系统需要重启进恢复环境，"
                     "但 UEFI 无法执行传统引导链。请改用传统(CSM)模式启动后操作。");
            LogError("return-old rejected: UEFI session + MBR target");
            return 4;
        case StagedBootPath::Ok:
            break;
    }

    std::wstring exeDir = ContractDir();  // Q1 settle 后返回新家；否则 = ExeDir()
    // 软件目录若在目标盘上：先尝试 SettleHome（Q1「先落盘后搬家」，与 StageRestore
    // 同款）——搬成功则 exeDir 指向新家、继续；搬不动才前置拒绝（让用户把程序
    // 拷到别的分区或进 PE 再操作）。
    {
        wchar_t exLetter = exeDir.size() > 1 ? exeDir[0] : 0;
        if (ShouldSettleHome(exeDir, target.letter.empty() ? 0 : target.letter[0])) {
            int exeDisk = VolumeDiskIndex(exLetter);
            int tgtDisk = (int)target.diskIndex;
            SettleResult sr = SettleHome(tgtDisk, exeDisk);
            if (sr.moved) {
                exeDir = ContractDir();  // settle 后 ContractDir 返回新家
                LogInfo("return-old: settled to " + W2U(sr.newHome));
            } else {
                // P4（beta.6 复核结论）：这是**合理的安全约束**，不是多余的拦路。
                // 返回旧系统会把目标分区整棵树（含本程序所在目录）改名到 ~new 并
                // 丢弃 —— 运行中的程序会随之消失，用户下次没有任何入口再操作。
                // SettleHome 已先尝试把程序整包复制到别的分区；这里失败说明没有
                // 可用的安全分区/复制失败，继续就等于悄悄删掉用户的工具。
                // 保留拒绝，但把"为什么"和"自动复制为什么失败"写清楚。
                err = std::string(Tr("本程序当前运行在目标分区上 —— 「返回旧系统」会把该分区整棵新系统树（含本程序）移入 ~new 后丢弃，运行中的程序也会随之消失。")) + "\n" +
                      Tr("已先尝试自动把程序复制到其他分区，但未成功") + " (" + sr.detail + ").\n" +
                      Tr("请先把本程序复制到别的分区（如 D:\\）并从那里运行；或从 U 盘 / PE 中运行本程序再操作。");
                LogError("return-old rejected: software dir is on the target partition, "
                         "settle failed: " + sr.detail);
                return 4;
            }
        }
    }

    if (!AcquireOpLock()) {
        err = Tr("已有备份/还原实例在运行");
        return 1;
    }
    ProgressUpdate("restore", 0, "return-old-start");
    LogInfo("return-old staged: target=" + W2U(target.letter) + " " + det);

    std::wstring logsDir = LogBaseDir() + L"\\logs";
    CreateDirectoryW(logsDir.c_str(), nullptr);
    BcdExport(logsDir + L"\\bcd-backup");

    DiskInfo tdisk;
    for (const auto& d : EnumerateDisks())
        if (d.index == target.diskIndex)
            tdisk = d;

    // 契约：action=returnold + engine=swap（**关键降级**：旧救援层不认识
    // returnold 会落进 engine=swap 分支 → zjswap.sh 查不到 ~new/.zj-done →
    // exit 2 报 "old system untouched"，绝不 mkntfs；fail-closed）。
    // image_* 全空：这条路不需要镜像、不 apply。
    RestoreTask t;
    t.action = "returnold";
    t.engine = "swap";
    t.imagePath.clear();
    t.imageRelPath.clear();
    t.imagePartGuid.clear();
    t.imageIndex = 1;
    t.espIndex = 0;
    t.eaIndex = 0;
    t.targetGuid = target.guid;
    t.targetOffset = target.offsetBytes;
    t.targetSize = target.sizeBytes;
    t.targetDiskSerial = tdisk.serial;
    t.targetDisk = target.diskIndex;
    t.targetPart = (int)target.partNumber;
    t.ptType = tdisk.style == PartitionStyle::GPT ? "gpt" : "mbr";
    t.repairBoot = true;
    t.targetDiskName = tdisk.model;
    t.targetDiskSize = tdisk.sizeBytes;
    t.targetFs = target.fs;
    t.targetVolLabel = target.label;
    t.softwarePath = ExePath();
    {
        wchar_t exLetter = exeDir.size() > 1 ? exeDir[0] : L'\0';
        wchar_t exRoot[4] = {exLetter, L':', L'\\', L'\0'};
        bool fixedDisk = exLetter && GetDriveTypeW(exRoot) == DRIVE_FIXED;
        if (fixedDisk) {
            t.softwareDir = exeDir;
        } else {
            std::wstring dataDrive = FindDataDrive(target.letter.empty() ? 0 : target.letter[0]);
            t.softwareDir = dataDrive.empty() ? exeDir : dataDrive + kRecoveryDir;
            CreateDirectoryW(t.softwareDir.c_str(), nullptr);
        }
    }

    // 清场（A2）+ 主/副契约写入
    {
        std::string clog;
        int cleaned = CleanupStrayContracts(clog);
        LogInfo("stray contracts cleanup: " + std::to_string(cleaned) +
                " removed (" + clog.substr(0, clog.size() - 1) + ")");
    }
    std::string tlog;
    if (!WriteRestoreLog(target.letter[0], t, tlog)) {
        ReleaseOpLock();
        ProgressDone("restore", "failed");
        err = Tr("写恢复日志失败（需管理员权限写 ") + W2U(target.letter) + Tr(":\\）");
        LogError(err + " / " + tlog);
        return 1;
    }
    {
        std::string wtlog;
        bool okTask = WriteRestoreTask(exeDir, t, wtlog);
        std::wstring used = exeDir;
        if (!okTask) {
            std::wstring dataDrive = FindDataDrive(target.letter.empty() ? 0 : target.letter[0]);
            std::wstring alt =
                dataDrive.empty() ? std::wstring() : dataDrive + L"ZJRESTORE";
            if (!alt.empty()) {
                CreateDirectoryW(alt.c_str(), nullptr);
                wtlog.clear();
                if (WriteRestoreTask(alt, t, wtlog)) {
                    okTask = true;
                    used = alt;
                }
            }
        }
        if (!okTask)
            LogError("return-old task write failed (non-fatal; the target-root "
                     "log is the primary contract): " + wtlog);
        else
            LogInfo("return-old task written to " + W2U(used));
    }

    // 引导层失败 → 契约回滚（PIT-126 同款：契约已在引导层之前写好，引导层
    // 失败 = 任务永远不会执行 → 必须撤销，否则目标根留一份陈旧契约）。
    bool rollbackArmed = true;
    struct RollbackGuard {
        bool* armed;
        ~RollbackGuard() {
            if (!*armed) return;
            std::string rlog;
            int n = CleanupStrayContracts(rlog);
            DeleteMenuBinding();
            LogWarn("boot layer failed -> staged contracts rolled back (" +
                    std::to_string(n) + " stray removed)");
        }
    } rollbackGuard{&rollbackArmed};

    // 引导层**无条件重装**（PIT-037）：用户可能已经「删除引导项」，而本次
    // 必须能单次启动进救援层。UEFI 下 ESP 常驻项也一并刷新。
    const bool useUefi = UseUefiBootFor(target);
    LogInfo(std::string("boot path: ") + (useUefi ? "UEFI/ESP" : "BIOS/GRUB4DOS") +
            " (firmware=" + (IsUefiFirmware() ? "UEFI" : "BIOS") +
            ", target disk style=" + StyleName(DiskStyleOf(target.diskIndex)) + ")");
    std::string blog;
    if (useUefi) {
        SbMode sbm = CurrentSbMode();
        if (sbm == SbMode::Shim && !IsMokEnrolled(exeDir)) {
            ReleaseOpLock();
            ProgressDone("restore", "failed");
            err = Tr("本机开启了 Secure Boot，但还没注册启动密钥：请先点" "「安装菜单」，重启一次，在蓝底界面选 " "Enroll key -> 选 zj-mok.cer -> 注册，之后回来再操作");
            LogError(err);
            return 1;
        }
        std::wstring espRoot;
        bool guidMatched = false;
        if (!AcquireEspRoot(espRoot, guidMatched, blog,
                             (int)target.diskIndex)) {
            LogError("ESP acquire log (returnold):\n" + blog);
            ReleaseOpLock();
            ProgressDone("restore", "failed");
            err = Tr("UEFI 机器未找到可用的 ESP 引导分区，无法部署引导层。\n"
                     "常见原因：DiskGenius 重建分区时漏建 ESP，或把 ESP 标成了"
                     " Basic Data（应改为 EFI System 类型）。\n当前分区表：\n") +
                  W2U(DescribePartitions());
            LogError(err);
            return 1;
        }
        if (!InstallUefiBootEntry(espRoot, exeDir, blog, true)) {
            ReleaseOpLock();
            ProgressDone("restore", "failed");
            err = Tr("安装 UEFI 引导层失败: ") + blog;
            LogError(err);
            return 1;
        }
    } else {
        std::wstring deployDrive = std::wstring(1, target.letter[0]) + L":\\";
        if (!InstallBootLayer(deployDrive, exeDir, blog, true)) {
            ReleaseOpLock();
            ProgressDone("restore", "failed");
            err = Tr("安装引导层失败: ") + blog;
            LogError(err);
            return 1;
        }
    }
    // 单次启动（只生效一次；写后读回校验，见 BootWriteBlockedHint）
    if (useUefi && CurrentSbMode() != SbMode::Bootapp) {
        if (!SetUefiBootNext(blog, true)) {
            ReleaseOpLock();
            ProgressDone("restore", "failed");
            err = Tr("设置固件单次启动失败: ") + blog + BootWriteBlockedHint();
            LogError(err);
            return 1;
        }
    } else {
        std::string slog;
        if (!BcdSetBootsequence(TemporaryGuid(), slog)) {
            ReleaseOpLock();
            ProgressDone("restore", "failed");
            err = Tr("设置单次启动失败（重启后可能进不了还原环境）: ") + slog +
                  BootWriteBlockedHint();
            LogError(err);
            return 1;
        }
        LogInfo("bootsequence verified: " + slog);
    }
    // 返回旧系统也是一次性引导：把清理器注入即将回到根的旧系统 ~old，
    // 首启删除临时条目（常驻菜单已在入口处禁止覆盖）。
    {
        std::string clog;
        if (!InstallFirstBootCleanup(target.letter + L":\\~old", exeDir, clog))
            LogWarn("return-old first-boot cleanup NOT installed: " + clog);
        else
            LogInfo("return-old: " + clog);
    }

    CollectDeployArtifacts(logsDir + L"\\collected", target.letter[0]);
    AppendHistory("return-old-staged", L"", L"", 0,
                  "staged; 实际结果见救援层日志");
    ReleaseOpLock();

    // 待执行标记（PIT-115 同款）：救援层是否真的跑过，用它与 ZJRESTORE-status.txt
    // 的时间比较判断 —— 引导失败/断电导致救援层**从未执行**时，下次启动也会明确提示。
    {
        std::wstring md = LogBaseDir() + L"\\logs";
        CreateDirectoryW(md.c_str(), nullptr);
        SYSTEMTIME st2 = {};
        GetLocalTime(&st2);
        char mk[600];
        snprintf(mk, sizeof(mk),
                 "time=%04u-%02u-%02u %02u:%02u:%02u\ntarget=%s\nimage=%s\n",
                 st2.wYear, st2.wMonth, st2.wDay, st2.wHour, st2.wMinute,
                 st2.wSecond, W2U(target.letter).c_str(),
                 "return-old (~old -> root)");
        HANDLE hm = CreateFileW((md + L"\\pending-restore.txt").c_str(),
                                GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hm != INVALID_HANDLE_VALUE) {
            std::string body(mk);
            DWORD w = 0;
            WriteFile(hm, body.data(), (DWORD)body.size(), &w, nullptr);
            CloseHandle(hm);
        }
    }
    rollbackArmed = false;  // 成功：契约保留（救援层要靠它定位）
    if (needReboot)
        *needReboot = true;  // PIT-110 教训：**不要无条件清零**
    ProgressDone("restore", "staged");
    LogInfo("return-old staged, reboot to execute (action=returnold)");
    return 0;
}

// 便捷入口（PIT-139，给 CLI 用）：按磁盘号/分区号定位目标后走 StageReturnOld。
// disk/part 都 < 0 时自动找「根下有 Windows、且 ~old\Windows 也存在」的第一块
// 固定盘 —— 与 GUI 扫残留时看到的是同一棵树，不会挑错。
int StageReturnOldByDiskPart(int disk, int part, std::string& err,
                             bool* needReboot) {
    PartitionInfo target;
    bool found = false;
    for (const auto& d : EnumerateDisks()) {
        if (disk >= 0 && (int)d.index != disk) continue;
        for (const auto& p : d.parts) {
            if (part >= 0) {
                if ((int)p.partNumber != part) continue;
                target = p;
                found = true;
                continue;
            }
            if (p.letter.size() != 1) continue;
            PartitionInfo cand = p;
            bool hOld = false, hRoot = false;
            std::string det;
            if (DetectReturnOldCapability(cand, &hOld, &hRoot, det) && hOld &&
                hRoot) {
                target = cand;
                found = true;
                break;
            }
        }
        if (found && part < 0) break;
    }
    if (!found) {
        err = std::string(Tr("找不到「返回旧系统」的目标分区：根下没有完整系统、"
                             "~old\\Windows 也不存在。\n")) +
              Tr("可用 `list` 确认后加 --disk N --part M 指定。");
        LogError("return-old: target not found");
        return 4;
    }
    LogInfo("return-old target: disk=" + std::to_string(target.diskIndex) +
            " part=" + std::to_string(target.partNumber) + " letter=" +
            W2U(target.letter) + ":");
    return StageReturnOld(target, err, needReboot);
}

}  // namespace sysrecover
