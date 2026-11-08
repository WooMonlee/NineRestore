// 拆分自 src/gui/main_form.cpp（按职责）：还原流程（UI 校验 + worker 线程暂存）。
// 注意：所有 C++ 标准头已通过 main_form.h 在 StdAfx.h 之前 include（PIT-012）。
#include "common/i18n.h"
#include "main_form.h"

#include <shobjidl.h>
#include <shellapi.h>

#include "gui/ui_skin.h"
#include "gui/confirm_dlg.h"
#include "gui/passwd_dlg.h"
#include "gui/imgsearch.h"
#include "app/ops.h"
#include "app/imgsrc.h"
#include "app/universal.h"
#include "app/request_precheck.h"
#include "app/safety.h"
#include "app/selfdiag.h"
#include "app/swapleft.h"
#include "app/pending_task.h"
#include "boot/bcd.h"
#include "boot/grub.h"
#include "boot/uefi.h"
#include "common/bioskey.h"
#include "common/cpucap.h"
#include "common/logger.h"
#include "common/process.h"
#include "common/relocate.h"
#include "common/pathutil.h"  // DriversRoot()
#include "common/selfarch.h"
#include "disk/disk.h"  // IsWinPE()（PIT-214：PE 重启要靠 wpeutil）
#include "common/bioskey.h"   // 厂家名（PIT-211 改机器名）
#include "common/compname.h"  // MakeComputerName / WindowsMajorOfDir  // AppDir()（导出驱动目录用）
#include "common/sysinfo.h"
#include "common/progress.h"
#include "common/units.h"
#include "disk/disk.h"
#include "disk/partinit.h"
#include "gui/initdisk_dlg.h"
#include "wim/wim.h"

using namespace sysrecover;

// ────────────────── 还原（UI 校验 + worker 线程暂存） ──────────────────

namespace {

// 确认框里「还原日志将放到 X: 盘」的提示：只有程序自身在目标盘或只读盘上时才需要提。
// （用户 2026-09-23：合并进确认框，不再单独弹第二个 MessageBox。）
std::wstring RestoreLogNote(const PartitionInfo& part) {
    std::wstring ed = ExeDir();
    wchar_t exL = ed.empty() ? 0 : ed[0];
    wchar_t tgL = part.letter.empty() ? 0 : part.letter[0];
    wchar_t exRoot[4] = {exL, L':', L'\\', 0};
    const bool onTarget = exL && tgL && towupper(exL) == towupper(tgL);
    const bool fixedDisk = exL && GetDriveTypeW(exRoot) == DRIVE_FIXED;
    if (!onTarget && fixedDisk) return std::wstring();
    std::wstring dd = FindDataDrive();
    LogInfo("logs will be placed on data drive (exe dir is target/read-only)");
    return dd.empty() ? std::wstring()
                      : (Tr(L"（还原日志将放到 ") + dd.substr(0, 1) + Tr(L": 盘）"));
}

// 启动方式与目标盘风格不一致时的提示行（只提示、不阻止 —— 用户明确要求保留选择权）。
std::wstring RestoreBootModeNote(bool sessionUefi, bool targetGpt) {
    if (!sessionUefi && targetGpt)
        return Tr(L"当前恢复环境以 Legacy 方式启动、目标盘为 GPT："
                  L"还原后请在 BIOS 中改为 UEFI 启动；若启动菜单里没有 "
                  L"Windows，用「修复引导」。");
    if (sessionUefi && !targetGpt)
        return Tr(L"当前恢复环境以 UEFI 方式启动、目标盘为 MBR："
                  L"还原后请在 BIOS 中改为 Legacy/CSM 启动。");
    return std::wstring();
}

// PIT-211：机器名 = W<大版本><厂家><YYMMDDHHMMSS>
std::wstring MakeRestoreComputerName(const PartitionInfo& part) {
    std::wstring vendor;
    LocalBootMenuKey(&vendor);
    int major = WindowsMajorOfDir(
        (part.letter.empty() ? std::wstring(L"C") : part.letter.substr(0, 1)) +
        L":\\Windows");
    if (major == 0) major = 10;
    SYSTEMTIME nowSt = {};
    GetLocalTime(&nowSt);
    return MakeComputerName(vendor, major, nowSt.wYear, nowSt.wMonth, nowSt.wDay,
                            nowSt.wHour, nowSt.wMinute, nowSt.wSecond);
}

}  // namespace

void CMainForm::StartRestore() {
    if (m_wimPath.empty()) { SetStatus(Tr(L"请先选择镜像文件")); return; }
    if (m_selPart < 0 || m_selPart >= (int)m_parts.size()) {
        SetStatus(Tr(L"请选择目标分区")); return;
    }
    const PartitionInfo& part = m_parts[m_selPart];
    const bool universal = IsUniversalChecked();
    // BitLocker 提醒（用户规格 2026-09-23）：**只要系统里有加密卷**就提醒。
    if (!AskBitLockerWarning(sysrecover::BitLockerVolumes()))
        return;
    // 目标盘健康提醒（PIT-086，用户 2026-09-26）：坏盘先警告（默认取消）。
    if (!AskDiskHealthWarning((int)part.diskIndex))
        return;
    // 静默模式默认：A 引擎 + keepOld=false（无人值守，~old 由 RunOnce 自动清理）。
    // 非静默模式下面确认框会覆盖这两个值。
    m_restoreEngine  = "swap";
    m_restoreKeepOld = false;
    m_formatSwap     = false;
    m_formatTarget   = true;
    m_preserveOld    = false;
    m_targetMode     = "format";
    m_computerName.clear();
    // 非静默模式：需重启时弹三选（取消/还原后重启/重启后还原）；就地还原仍二选。
    // 静默模式：不弹框，直接暂存并重启（静默默认 = 还原后重启 / keepOld=false）。
    if (!IsSilent()) {
        std::string why;
        InPlaceReason whyReason = INPLACE_OK;
        const bool needReboot = !CanRestoreInPlace(part, why, &whyReason);
        // 提示文案必须与**真实行为**一致（PIT-072）：PE 里 / 还原到非系统盘走的是
        // "就地还原、不重启"，以前这里一律写"重启后还原"，用户 2026-09-21 在 PE 里
        // 实测被误导。判断复用 ops 层同一个函数（CanRestoreInPlace），两边不漂移。
        const std::wstring logNote = RestoreLogNote(part);
        const bool sessionUefi = IsUefiBootSession();
        const bool targetGpt =
            m_selPart >= 0 && m_selPart < (int)m_partStyles.size() &&
            m_partStyles[m_selPart] == PartitionStyle::GPT;
        const std::wstring bootNote = RestoreBootModeNote(sessionUefi, targetGpt);
        if (needReboot) {
            if (!AskRebootRestoreConfirm(part, universal, bootNote)) return;
        } else {
            if (!AskInPlaceRestoreConfirm(part, logNote, bootNote)) return;
        }
    }
    // （日志落点的提示已**合并进上面的确认框**，不再单独弹窗 —— 用户 2026-09-23
    //   反馈连续两个提示框很烦。这里只留一行日志备查。）
    m_busy = true;
    UpdateMainAction();
    SetStatus(Tr(L"正在校验镜像并暂存还原任务..."));
    SetProgress(0);
    m_start = std::chrono::steady_clock::now();
    SetTimer(m_hWnd, 1, 1000, nullptr);
    if (m_worker.joinable()) { m_worker.join(); }
    m_cancel = false;
    try {
        m_worker = std::thread(&CMainForm::StartRestoreAsync, this);
    } catch (...) {
        m_busy = false;
        UpdateMainAction();
        SetStatus(Tr(L"线程创建失败"));
        MessageBoxW(m_hWnd, Tr(L"无法创建工作线程，请重试"), Tr(L"错误"), MB_OK | MB_ICONERROR);
    }
}


// 需要重启时的三选确认（A 引擎「还原后重启」/ B 引擎「重启后还原」）。
// 返回 false = 用户取消；选项写进 m_* 成员。
bool CMainForm::AskRebootRestoreConfirm(const PartitionInfo& part,
                                        bool universal,
                                        const std::wstring& bootNote) {
    wchar_t confirm[1200];
    // 2026-10-19（1019 案 W3）：第 4 行前移特殊镜像提示 —— 含 EFS 加密
    // 文件/特殊重解析点的镜像走「重启后还原」会被暂存预检拒绝（W1/W2），
    // 在选择时刻就告诉用户该选哪个（CConfirmDlg 已扩到 4 行）。
    // 2026-10-20（用户规格）：空间不足时，开始前就显示「需要/可用」，
    // 并把「还原后重启」按钮变成红色「空间不足」且不可点；空间够则
    // 与以前完全一样（不追加提示）。默认回车落在「取消」（安全项）。
    sysrecover::SwapSpaceEstimate se{};
    bool spaceShort = false;
    if (sysrecover::QuerySwapSpace(m_wimPath, m_selImageIndex, part,
                                   se) &&
        se.freeBytes > 0 && !se.enough) {
        spaceShort = true;
        LogWarn("A-engine space short: need=" +
                std::to_string(se.required) + " free=" +
                std::to_string(se.freeBytes) +
                " (mid button disabled)");
    }
    if (sysrecover::ImageIsEncrypted(m_wimPath))
        swprintf(confirm, 1200,
                 Tr(L"即将把镜像还原到 %ls: 盘（磁盘%u 分区%u）。\n"
                    L"还原后重启，是解压镜像后重启修复引导，可以恢复到现在状态\n"
                    L"重启后还原，是重启格式化%ls: 盘后还原镜像，无法恢复现在状态\n"
                    L"加密镜像：重启后还原需要输入密码"),
                 part.letter.empty() ? L"?" : part.letter.c_str(),
                 part.diskIndex,
                 part.displayNo ? part.displayNo
                                : (part.ordinal ? part.ordinal : part.partNumber),
                 part.letter.empty() ? L"?" : part.letter.c_str());
    else
        swprintf(confirm, 1200,
                 Tr(L"即将把镜像还原到 %ls: 盘（磁盘%u 分区%u）。\n"
                    L"还原后重启，是解压镜像后重启修复引导，可以恢复到现在状态\n"
                    L"重启后还原，是重启格式化%ls: 盘后还原镜像，无法恢复现在状态\n"
                    L"含 EFS 加密文件或特殊重解析点的镜像，请选「还原后重启」"),
                 part.letter.empty() ? L"?" : part.letter.c_str(),
                 part.diskIndex,
                 part.displayNo ? part.displayNo
                                : (part.ordinal ? part.ordinal : part.partNumber),
                 part.letter.empty() ? L"?" : part.letter.c_str());
    if (spaceShort) {
        wchar_t sp[192];
        swprintf(sp, 192,
                 Tr(L"A 引擎需要约 %.1f GB 可用空间，当前 %.1f GB"
                    "（已禁用）"),
                 se.required / 1073741824.0,
                 se.freeBytes / 1073741824.0);
        wcscat(confirm, L"\n");
        wcscat(confirm, sp);
    }
    // 2026-10-30：异机还原只能在 Windows 侧注入驱动 → 「重启后还原」
    // （B 引擎）直接置灰并说明，不再让用户点完再弹"请去 PE"。
    if (universal) {
        wcscat(confirm, L"\n");
        wcscat(confirm,
               Tr(L"异机还原需要在 Windows 侧注入启动驱动：只能选「还原后重启」"
                  L"（不需要 PE）"));
    }
    // PIT-211：机器名 = W<大版本><厂家><YYMMDDHHMMSS>
    const std::wstring newName = MakeRestoreComputerName(part);
    ConfirmOptions opt;
    opt.showFormatSwap = true;
    {
        // 目标分区"没被占用"才谈得上格式化（PE / 非系统盘）。
        std::string fwhy;
        InPlaceReason freason = INPLACE_OK;
        opt.formatSwapEnabled =
            sysrecover::CanRestoreInPlace(part, fwhy, &freason);
    }
    opt.formatSwap = opt.formatSwapEnabled;
    opt.showFormatBlock = true;
    opt.formatBlock = true;
    opt.showRename = true;
    opt.rename = false;
    opt.renameLabel = Tr(L"改机器名：") + newName;
    if (!bootNote.empty()) {
        wcscat(confirm, L"\n");
        wcscat(confirm, bootNote.c_str());
    }
    int r = CConfirmDlg::Ask3(
        m_hWnd, Tr(L"确认还原"), confirm, Tr(L"取消"),
        spaceShort ? Tr(L"空间不足") : Tr(L"还原后重启"),
        Tr(L"重启后还原"), /*defaultIdx=*/spaceShort ? 0 : 1,
        /*midDisabled=*/spaceShort, /*escIdx=*/0,
        /*centerTitle=*/false, /*rightDisabled=*/universal,
        &opt);
    if (r == 0) return false;  // 取消
    m_formatSwap = opt.formatSwap;
    m_targetMode = opt.formatBlock ? "format" : "keep";
    m_computerName = opt.rename ? newName : std::wstring();
    if (r == 2 && universal) {
        // 双保险（按钮已禁用，正常走不到这里）
        MessageBoxW(m_hWnd,
                    Tr(L"异机还原需要在 Windows 侧注入启动驱动，"
                       L"只能选「还原后重启」——不需要进 PE。"),
                    Tr(L"异机还原"), MB_OK | MB_ICONWARNING);
        return false;
    }
    if (r == 1) {        // 还原后重启：A 引擎 swap + keepOld
        m_restoreEngine  = "swap";
        m_restoreKeepOld = true;
    } else {             // 重启后还原：B 引擎 block，内容全丢
        m_restoreEngine  = "block";
        m_restoreKeepOld = false;
    }
    return true;
}

// 就地还原的二选确认。返回 false = 用户取消。
bool CMainForm::AskInPlaceRestoreConfirm(const PartitionInfo& part,
                                         const std::wstring& logNote,
                                         const std::wstring& bootNote) {
    wchar_t confirm[1200];
    swprintf(confirm, 1200,
             Tr(L"即将把镜像还原到 %ls: 盘（磁盘%u 分区%u）。\n"
                L"该分区上的所有数据将丢失！%ls\n"
                L"目标分区当前未被占用，将立即就地还原，不需要重启。"),
             part.letter.empty() ? L"?" : part.letter.c_str(),
             part.diskIndex,
             part.displayNo ? part.displayNo
                            : (part.ordinal ? part.ordinal : part.partNumber),
             logNote.c_str());
    if (!bootNote.empty()) {
        wcscat(confirm, L"\n");
        wcscat(confirm, bootNote.c_str());
    }
    const std::wstring newName2 = MakeRestoreComputerName(part);
    ConfirmOptions opt2;
    // PIT-214（用户 2026-11-03）：格式化是**可选项**，默认勾选；
    // 取消后保留文件系统、只清空内容（不是"保留旧文件"）。
    // PIT-216：保留旧系统（旧系统移到 ~old，可「返回旧系统」）——
    // 空间按"镜像内容 + 余量"估；不够就置灰。
    {
        sysrecover::SwapSpaceEstimate se2{};
        const bool spaceOk2 =
            sysrecover::QuerySwapSpace(m_wimPath, m_selImageIndex, part,
                                       se2) &&
            se2.enough;
        opt2.showPreserveOld = true;
        opt2.preserveOldEnabled = spaceOk2;
        opt2.preserveOld = false;
        opt2.preserveOldLabel = spaceOk2
            ? Tr(L"保留旧系统（移到 ~old，可返回旧系统）")
            : Tr(L"保留旧系统（空间不足，已禁用）");
    }
    opt2.showFormatBlock = true;
    opt2.formatBlockLabel =
        Tr(L"格式化目标分区（取消则保留文件系统、仅清空内容）");
    opt2.formatBlockEnabled = true;
    opt2.formatBlock = true;
    opt2.showRename = true;
    opt2.rename = false;
    opt2.renameLabel = Tr(L"改机器名：") + newName2;
    if (CConfirmDlg::Ask2(m_hWnd, Tr(L"确认还原"), confirm, Tr(L"取消"),
                          Tr(L"开始还原"), /*defaultIsRight=*/true,
                          Tr(L"丢失！"), &opt2) != 1)
        return false;
    m_formatSwap = false;
    m_formatTarget = opt2.formatBlock;
    m_preserveOld = opt2.preserveOld;
    if (m_preserveOld) m_formatTarget = false;  // 保留旧系统时不做格式化
    m_computerName = opt2.rename ? newName2 : std::wstring();
    // 就地还原不走交换，引擎无意义但保持默认 swap（StageRestore 内部按需处理）
    m_restoreEngine  = "swap";
    m_restoreKeepOld = false;
    return true;
}

void CMainForm::StartRestoreAsync() {
    // 让出 CPU 给界面：任务吃满所有核时界面会明显发飘（"点了半天没反应"）。
    // 降到 BELOW_NORMAL 后交互明显跟手，任务耗时几乎不变（只让出空闲时间片）。
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    RestoreRequest req;
    req.image = m_wimPath;
    req.disk  = m_selPart >= 0 && m_selPart < (int)m_parts.size()
                ? (int)m_parts[m_selPart].diskIndex : 0;
    req.part  = m_selPart >= 0 && m_selPart < (int)m_parts.size()
                ? (int)m_parts[m_selPart].partNumber : 1;
    req.index = m_selImageIndex;
    req.repairBoot = true;
    req.engine = m_restoreEngine;       // "swap"=还原后重启 / "block"=重启后还原
    req.keepOld = m_restoreKeepOld;     // swap 时 true=保留 ~old 可返回；block 恒 false
    req.universal = IsUniversalChecked();  // 异机还原（2026-10-21）
    req.formatSwap = m_formatSwap;               // PIT-211
    req.formatTarget = m_formatTarget;           // PIT-214
    req.preserveOld = m_preserveOld;             // PIT-216
    req.targetMode = m_targetMode;               // "format" | "keep"
    req.computerName = m_computerName;           // 空 = 不改名
    req.verifyImage = m_restoreVerifyImage;      // H4（1.0）：默认关
    // 加密镜像：口令来自本次运行输入（LoadWimImages 里问过并记在 imgsrc 表里）。
    if (sysrecover::ImageIsEncrypted(req.image))
        req.password = sysrecover::GetImagePassword(req.image);
    if (req.universal) {
        // 只认程序目录下的 Drivers\（用户放驱动包的位置）；没有 .inf 就仅内置。
        std::wstring drivers = sysrecover::DriversRoot();
        int n = 0;
        std::wstring uerr;
        if (GetFileAttributesW(drivers.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (sysrecover::ValidateDriverFolder(drivers, &n, uerr)) {
                req.driverDir = drivers;
                // 用户要求「导出即自动生效」：这里留一条证据，排错时能确认
                // 本次到底自动吃了哪个目录、多少个 INF（2026-10-30）。
                LogInfo("universal: auto driver dir=" + W2U(drivers) +
                        " inf=" + std::to_string(n));
            } else {
                LogWarn("universal: Drivers folder ignored (" + W2U(uerr) + ")");
            }
        } else {
            LogInfo("universal: no Drivers folder -> in-box storage drivers only");
        }
    }
    std::string err;
    bool needReboot = true;
    // 进度：把 ops 层 apply 的进度转投到界面 —— 用户 2026-09-23 反馈"还原时进度条
    // 和百分比全程不动、最后才一下跳满"，因为这条链路以前**根本没接** ✗
    //（备份那条接了、还原这条漏了）。约定：回调返回 **true = 请求中止**。
    auto progressFn = [this](int pct, const std::string& stage) -> bool {
        if (m_cancel) return true;
        auto now = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now - m_lastProgressPost)
                      .count();
        // 阶段名一变就发（不受 100ms 节流）：收官标记（esp/verify/probe）与
        // 上一次上报常在 100ms 内，会被吞掉 → 状态栏永远停在旧阶段（PIT-098）。
        bool stageChanged = stage != m_lastStageRaw;
        if (ms < kProgressThrottleMs && pct < 100 && !stageChanged) return false;
        m_lastProgressPost = now;
        m_lastStageRaw = stage;
        PostMessage(WM_PROGRESS_UPDATE, (WPARAM)pct,
                    (LPARAM)new std::wstring(U2W(stage)));
        return false;
    };
    m_lastProgressPost = std::chrono::steady_clock::now();
    m_lastStageRaw.clear();
    int rc = StageRestore(req, err, &needReboot, progressFn, &last_adv_);
    last_err_ = err;
    m_needReboot = needReboot;
    PostMessage(WM_TASK_COMPLETE, (WPARAM)rc, 0);
}

