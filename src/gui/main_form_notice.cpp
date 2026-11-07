// 拆分自 src/gui/main_form.cpp（按职责）：提示 / 确认 / 支持包与事件路由（Notify）。
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

namespace {

// 未写完的备份产物路径 —— 必须与 wim.cpp::Capture 的原子写一致
// （它先写 `<目标>.tmp`，成功后 MoveFileEx 改名；中途失败自己会删 tmp）。
// 备份走的是 Capture（req.append=false），所以"半成品"永远是这一个 `.tmp`；
// 最终路径上的文件要么是旧的有效镜像、要么是刚改名完成的成品，**都不能删**。
// 方案 C（ESP 并入）另有中转文件 `<目标>.stage` / `.stage.tmp`（见
// ops.cpp::RunBackup 的 stage 流程），删除白名单见 CleanupIncompleteOutput。
std::wstring IncompletePath(const std::wstring& dest) {
    return dest.empty() ? std::wstring() : dest + L".tmp";
}

// 文件被 worker 持有（或删除失败）时，登记为"下次开机删除"——需要管理员权限，
// 本程序已提权。用于「终止任务并退出」时清掉当下删不掉的半成品镜像。
void RegisterPendingDelete(const std::wstring& path) {
    if (path.empty())
        return;
    ::MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
}

// 「讨论」链接（用户规格 2026-09-23）：初期指向无忧论坛的这个帖子；
// 成熟后换成我们自己的站点 + 报错上报（用网站收集日志）。**只改这一处**。
const wchar_t* kSiteUrl =
    L"https://bbs.wuyou.net/forum.php?mod=viewthread&tid=453597";

}  // namespace

// ── 「日志」按钮：一键取证（支持包） ─────────────────────────────────────
// 用户规格（2026-10-03）：把"让用户做的事"全部自动化 —— 收集各盘日志 + 抓
// explorer 转储 + 导出事件日志 → 打包到桌面；结果框里再提供"重启进入安全模式"。
void CMainForm::ExportSupportFlow() {
    if (m_busy)
        return;
    m_busy = true;
    m_lastStage = Tr(L"正在收集日志与诊断信息…");
    SetStatus(m_lastStage);
    UpdateMainAction();
    if (m_worker.joinable())
        m_worker.join();
    m_worker = std::thread([this] {
        auto* r = new sysrecover::SupportBundle(
            sysrecover::BuildSupportBundle(std::wstring()));
        ::PostMessage(m_hWnd, WM_SUPPORT_DONE, 0, (LPARAM)r);
    });
}

void CMainForm::OnSupportDone(sysrecover::SupportBundle* rp) {
    sysrecover::SupportBundle r = rp ? *rp : sysrecover::SupportBundle();
    delete rp;
    if (m_worker.joinable())
        m_worker.join();
    m_busy = false;
    UpdateMainAction();
    if (r.zipPath.empty()) {
        std::wstring msg = Tr(L"导出诊断包失败：") + r.error;
        SetStatus(msg);
        MessageBoxW(m_hWnd, msg.c_str(), Tr(L"日志"), MB_OK | MB_ICONERROR);
        return;
    }
    size_t pos = r.zipPath.find_last_of(L'\\');
    std::wstring file =
        pos == std::wstring::npos ? r.zipPath : r.zipPath.substr(pos + 1);
    SetStatus(Tr(L"诊断包已导出：") + file);
    LogInfo("support bundle exported: " + W2U(r.zipPath) + " (dumps=" +
            std::to_string(r.dumps) + ", events=" + std::to_string(r.eventFiles) +
            ")");
    // ⚠️ CConfirmDlg 的正文按 '\n' 分行（≤6 行）且单行有宽度上限——
    // 文案必须压成短句；文件名靠「完成」后的资源管理器选中来展示。
    bool safe = ::GetSystemMetrics(SM_CLEANBOOT) != 0;
    std::wstring msg = std::wstring(Tr(L"诊断包已导出到桌面。\n")) +
                       Tr(L"请把这个文件发给技术支持；\n");
    int choice;
    if (safe) {
        msg += Tr(L"当前在安全模式，是否退出并重启？");
        choice = CConfirmDlg::Ask2(m_hWnd, Tr(L"日志"), msg, Tr(L"完成"),
                                   Tr(L"退出安全模式并重启"),
                                   /*defaultIsRight=*/false);
    } else {
        // 进安全模式的风险前置说明（2026-11-06）：双系统 {current} 未必指向
        // 用户以为的系统；BitLocker 机器改 BCD 后重启可能要求恢复密钥。
        msg += Tr(L"双系统下 {current} 可能是另一个系统；\n");
        msg += Tr(L"BitLocker 机器重启可能要求恢复密钥。\n");
        msg += Tr(L"是否重启进安全模式再测一次？");
        choice = CConfirmDlg::Ask2(m_hWnd, Tr(L"日志"), msg, Tr(L"完成"),
                                   Tr(L"重启进安全模式再测"),
                                   /*defaultIsRight=*/false);
    }
    if (choice == 1) {
        std::string log;
        // 一次性语义：设置前先武装"下一次登录自动清除"，装不上就拒绝设置。
        bool ok = sysrecover::BcdSetSafeBootOnce(!safe, log);
        LogInfo(std::string("safe boot toggle: ") + log);
        if (!ok) {
            MessageBoxW(m_hWnd, (Tr(L"安全模式设置失败：") + U2W(log)).c_str(),
                        Tr(L"日志"), MB_OK | MB_ICONERROR);
        } else {
            RebootNow();
        }
    } else {
        // 「完成」：在资源管理器里选中刚导出的包，方便用户直接拖进聊天窗口。
        std::wstring args = L"/select,\"" + r.zipPath + L"\"";
        // N19：以主窗为 owner（错误提示有归属）；这是用户显式选择的「完成」，抢前台是预期的。
        ::ShellExecuteW(m_hWnd, L"open", L"explorer.exe", args.c_str(), nullptr,
                        SW_SHOWNORMAL);
    }
}

// 启动检查：救援层失败回执（黑匣子，用户 2026-10-04 规格 2）。任何失败都要
// "开机即报"，而不是让用户回忆/录像。回执由救援层写到每个分区根/ESP，启动
// 收集时已收进 logs\collected；同一回执只提示一次（去重在 CheckLastRescueFailure）。
// P3（PIT-148，用户 2026-10-14）：A/B 类失败（未动目标/引导未验证）→ 三选框
// 「知道了 / 打开日志 / 一键重试」；C 类（已动盘）保持原二选框（只给日志路径）。
void CMainForm::ShowRescueFailureNotice() {
    RescueNotice n = CheckLastRescueFailure();
    if (n.msg.empty())
        return;
    // N20 ③：失败提示统一附上求助渠道（救援层首屏、docs/33 三处一致）。
    n.msg += L"\n\n";
    n.msg += Tr(L"多次失败请把日志发给我们分析：QQ 群 1128237828 / 邮箱 ms_2@qq.com");
    size_t nl = n.msg.find(L'\n');
    SetStatus(nl == std::wstring::npos ? n.msg : n.msg.substr(0, nl));
    int choice;
    if (n.retryable) {
        // 左=知道了（安全默认）、中=打开日志、右=一键重试；回车/ESC 落"知道了"
        //（Ask3 的 defaultIdx=0 = 左按钮，见 confirm_dlg.h）。
        choice = CConfirmDlg::Ask3(
            m_hWnd, Tr(L"还原结果"), n.msg, Tr(L"知道了"), Tr(L"打开日志"),
            Tr(L"一键重试"), /*defaultIdx=*/0);
    } else {
        choice = CConfirmDlg::Ask2(m_hWnd, Tr(L"还原结果"), n.msg,
                                   Tr(L"知道了"), Tr(L"打开日志文件夹"),
                                   /*defaultIsRight=*/false);
        if (choice == 1) {
            std::wstring dir = LogBaseDir() + L"\\logs\\collected";
            ::ShellExecuteW(m_hWnd, L"open", dir.c_str(), nullptr, nullptr,
                            SW_SHOWNORMAL);
        }
        return;
    }
    if (choice == 1) {  // 打开日志
        std::wstring dir = LogBaseDir() + L"\\logs\\collected";
        ::ShellExecuteW(m_hWnd, L"open", dir.c_str(), nullptr, nullptr,
                        SW_SHOWNORMAL);
    } else if (choice == 2) {  // 一键重试
        // 二次确认（fail-safe）：重试会重新暂存 + 可能重启。
        std::wstring cfm = std::wstring(
                               Tr(L"将按上次任务参数重新执行还原（镜像→目标分区）。\n")) +
                           Tr(L"确认继续？");
        int r = CConfirmDlg::Ask2(m_hWnd, Tr(L"一键重试"), cfm, Tr(L"取消"),
                                  Tr(L"立即重试"),
                                  /*defaultIsRight=*/false);  // 默认取消=安全
        if (r == 1)
            StartRetryStagedRestore();
    }
}

// N20 批2（④）：救援层回滚成功后，在旧系统 ProgramData\SysRecover 写下一行
// "上次任务未完成"标记。这里**读到即删**（先删再提示，避免二次打扰），提示里给
// 求助渠道；只碰这一个文件，不写注册表/启动项/服务。
void CMainForm::ShowPendingTaskNotice() {
    wchar_t pd[MAX_PATH] = {};
    if (!::GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH) || !pd[0]) return;
    std::wstring content;
    if (!sysrecover::TakePendingTaskMarker(pd, content)) return;
    LogInfo("pending-task marker consumed (self-deleted)");
    std::wstring msg = Tr(L"上次还原任务未完成：系统已回滚到原来的系统。");
    // 标记里的 when= 行帮用户对上时间；没有就不显示这一行。
    size_t wp = content.find(L"when=");
    if (wp != std::wstring::npos) {
        size_t e = content.find(L'\n', wp);
        std::wstring when = content.substr(
            wp + 5, (e == std::wstring::npos ? content.size() : e) - wp - 5);
        if (!when.empty()) msg += std::wstring(L"\n") + Tr(L"记录时间：") + when;
    }
    msg += L"\n\n";
    msg += Tr(L"请确认系统是否正常；如需要分析，请把日志发给我们：QQ 群 1128237828 / 邮箱 ms_2@qq.com");
    SetStatus(Tr(L"上次还原任务未完成"));
    MessageBoxW(m_hWnd, msg.c_str(), Tr(L"还原结果"), MB_OK | MB_ICONWARNING);
}

// 菜单模式自证（2026-11-06）：装了常驻菜单 ≠ 救援层跑过。判据 = 本次开机晚于
// 安装时刻、但 collected 里没有任何晚于安装时刻的新回执。命中就明确提示，
// 不再"默认已安装即成功"。提示看过后写 .menu-ack，下次启动才允许清掉现场。
void CMainForm::ShowMenuRescueNotice() {
    std::string installTime, detail;
    sysrecover::MenuRescueState st =
        sysrecover::CheckMenuRescueExecution(installTime, detail);
    if (st != sysrecover::MenuRescueState::NeverExecuted)
        return;
    LogWarn("menu rescue never executed: " + detail);
    std::wstring msg = Tr(L"已安装的还原菜单没有被引导执行。\n");
    msg += Tr(L"安装时间 ") + U2W(installTime) +
           Tr(L"，之后没有新的救援回执。\n");
    // 2026-11-06 用户确认“确实按了 F10 并选了 SysRecover 仍回到 Windows”，
    // 不能再把“无救援回执”直接归因于没选：固件跳过失效/非首位启动项同样会静默回落。
    msg += Tr(L"可能原因：选了 SysRecover 但固件跳过了该启动项（BootOrder 前面有失效条目时常见），或救援层没有启动。\n");
    // 2026-11-06 P0-2：热键原来在本条提示里写死成 F10；本机（Gigabyte/AMI）实际识别到的是 F12，
    // 用户照 F10 按自然没反应。这里改为运行时把占位 "F10" 换成本机识别到的键（译文键不变）。
    std::wstring step = Tr(L"正确操作：开机时按 F10（该机型也可能是 F12/Esc）→ 选 SysRecover。\n");
    {
        const std::wstring key = BootMenuKeyHint();
        const size_t p = step.find(L"F10");
        if (p != std::wstring::npos && !key.empty())
            step.replace(p, 3, key);
    }
    msg += step;
    msg += Tr(L"若按该键无反应：重启进入固件设置（Windows 设置→恢复→高级启动→立即重启→疑难解答→UEFI 固件设置），在启动项里直接选 SysRecover。\n");
    msg += Tr(L"若确认已选仍回到 Windows，请把 logs\\collected 发给技术支持；启动项丢失时重新运行本程序安装菜单。");
    MessageBoxW(m_hWnd, msg.c_str(), Tr(L"还原菜单未执行"),
                MB_OK | MB_ICONWARNING);
    sysrecover::AcknowledgeMenuRescue();
}

// 启动兜底（2026-11-06）：safeboot 仍被置位。正常情况下「日志」入口设置时会
// 武装"下一次登录自动清除"；这里兜两种情况：用户手动塞了 safeboot、自动退出
// 任务被安全软件拦住、或旧版本留下的设置。提示必须明确，不能默认成功。
void CMainForm::ShowSafeBootExitNotice() {
    std::string log;
    sysrecover::SafeBootState st = sysrecover::BcdGetSafeBootState(log);
    if (st == sysrecover::SafeBootState::None) return;
    LogWarn(std::string("startup: safeboot state=") +
            (st == sysrecover::SafeBootState::Minimal ? "minimal" : "unknown") +
            " | " + log);
    if (st == sysrecover::SafeBootState::Unknown) return;  // 判定不了就别乱弹
    const bool inSafe = ::GetSystemMetrics(SM_CLEANBOOT) != 0;
    std::wstring msg =
        inSafe ? Tr(L"当前处于安全模式（safeboot 仍被置位）。\n")
               : Tr(L"BCD 里仍开着安全模式，下次启动会进入安全模式。\n");
    msg += Tr(L"双系统下 {current} 可能是另一个系统；\n");
    msg += Tr(L"BitLocker 机器重启可能要求恢复密钥。\n");
    msg += Tr(L"是否现在退出安全模式并重启？");
    int choice = CConfirmDlg::Ask2(m_hWnd, Tr(L"安全模式"), msg,
                                   Tr(L"暂时保留"), Tr(L"退出并重启"),
                                   /*defaultIsRight=*/false);
    if (choice != 1) {
        LogInfo("safe boot exit declined by user");
        return;
    }
    std::string slog;
    bool ok = sysrecover::BcdSetSafeBootOnce(false, slog);
    LogInfo("safe boot exit: " + slog);
    if (!ok) {
        MessageBoxW(m_hWnd, (Tr(L"退出安全模式失败：") + U2W(slog)).c_str(),
                    Tr(L"安全模式"), MB_OK | MB_ICONERROR);
        return;
    }
    RebootNow();
}

// 一键重试（P3 / PIT-148）：读 conf → StageRestore；成功后若需重启则自动
// shutdown /r（与正常还原暂存一致）。worker 用 BELOW_NORMAL，与既有 worker
// 同款（PIT-071：别把 UI 饿死）。
void CMainForm::StartRetryStagedRestore() {
    if (m_busy)
        return;
    m_busy = true;
    UpdateMainAction();
    SetStatus(Tr(L"正在重试..."));
    SetProgress(0);
    ShowProgress(true);
    m_start = std::chrono::steady_clock::now();
    SetTimer(m_hWnd, 1, 1000, nullptr);
    LogInfo("GUI retry start");
    if (m_worker.joinable())
        m_worker.join();
    m_cancel = false;
    m_retryErr.clear();
    m_worker = std::thread([this] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        bool reboot = false;
        std::string err;
        int rc = RetryStagedRestore(err, &reboot);
        m_retryErr = err;
        last_err_ = err;
        PostMessage(WM_RETRY_DONE, (WPARAM)rc, (LPARAM)(reboot ? 1 : 0));
    });
}

// ────────────────── 忙时关闭 / 终止并退出（2026-09-20 用户规格） ──────────────────
//
// 以前：任务执行中点关闭**完全没有反应**（Notify 的 m_busy 早退把点击吞了），
// 唯一出路是干等任务跑完。现在：默认「继续等待」，也可选「终止并退出」——
// 立即中止任务并删除未写完的镜像。
// 中止走既有取消链路：m_cancel=true → 进度回调返回 ABORT（wim.cpp:109）→
// wimlib 在**下一次进度回调**就收手（实测亚秒级），不会留下"写入中"的坏镜像。

bool CMainForm::AskBusyClose() {
    if (!m_busy)
        return true;
    std::wstring msg = Tr(L"任务正在执行中（已用 ") + ElapsedText() + Tr(L"）。\n" L"选择「继续等待」：任务照常进行，不受影响。\n" L"选择「终止并退出」：立即中止任务，并删除未写完的镜像文件。");
    // 默认项 = 左按钮 =「继续等待」：回车/ESC 都落在安全项上，防误取消。
    int r = CConfirmDlg::Ask2(m_hWnd, Tr(L"任务执行中"), msg, Tr(L"继续等待"),
                              Tr(L"终止并退出"), /*defaultIsRight=*/false);
    return r != 0;  // 0 = 继续等待
}

// BitLocker 提醒（用户规格 2026-09-23）：有加密卷时提醒"没密钥则数据无法恢复"，
// 用户可选继续/退出（默认**退出**=安全项）。返回 true = 继续。
// 静默模式跳过弹框（无人值守），但仍写一条日志。
bool CMainForm::AskBitLockerWarning(const std::vector<std::wstring>& vols) {
    if (vols.empty())
        return true;
    std::wstring list;
    for (size_t i = 0; i < vols.size(); ++i) {
        if (i)
            list += Tr(L"、");
        list += vols[i];
    }
    LogInfo("BitLocker volumes detected: " + W2U(list));
    if (IsSilent())
        return true;
    // 文案按用户 2026-09-23 的反馈断行（原第 2 行太长被右边裁掉）：
    // 在"这些卷的数据在还原后"处加逗号换行，末尾挪到下一行。
    std::wstring msg = Tr(L"检测到本机有 BitLocker 加密的卷：") + list + Tr(L"\n" L"如果你没有对应的密码 / 恢复密钥，这些卷的数据在还原后，\n" L"将无法恢复。是否继续还原？");
    return CConfirmDlg::Ask2(m_hWnd, Tr(L"BitLocker 提醒"), msg, Tr(L"退出"), Tr(L"继续"),
                             /*defaultIsRight=*/false) == 1;
}

// 目标盘健康警告（PIT-086，用户 2026-09-26）：还原要格式化目标分区，若目标盘已现坏道，
// 还原完系统照样可能起不来 —— 先警告，让用户决定要不要先换盘。返回 true = 继续。
// 静默模式跳过弹框（无人值守），但仍写一条日志。
bool CMainForm::AskDiskHealthWarning(int diskIndex) {
    std::string hw = sysrecover::CheckDiskHealth(diskIndex);
    if (hw.empty())
        return true;  // 健康 或 取不到 SMART（NVMe/RAID/USB）→ 放行
    LogInfo("target disk health warning: " + hw);
    if (IsSilent())
        return true;
    std::wstring msg = U2W(hw) + Tr(L"\n\n是否仍要继续还原？");
    return CConfirmDlg::Ask2(m_hWnd, Tr(L"磁盘健康警告"), msg, Tr(L"取消"), Tr(L"仍然继续"),
                             /*defaultIsRight=*/false) == 1;
}

void CMainForm::CancelAndExit() {
    if (m_cancelling)
        return;
    m_cancelling = true;
    LogInfo("GUI: user chose cancel-and-exit while busy");
    m_cancel = true;  // → 进度回调返回 true → wimlib ABORT

    // 等 worker 收手（正常 <1s）。期间保持消息泵活着，否则窗口白屏卡住。
    ::EnableWindow(m_hWnd, FALSE);
    const auto t0 = std::chrono::steady_clock::now();
    while (m_busy) {
        MSG m;
        while (::PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&m);
            ::DispatchMessageW(&m);
        }
        if (!m_busy)
            break;
        if (std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - t0)
                .count() >= 10)
            break;
        ::Sleep(20);
    }
    if (m_busy) {
        // 兜底：优雅取消没生效（例如卡在不可中断的 IO/驱动阶段）→ 直接杀进程。
        // 此刻临时文件仍被 worker 持有，删不掉 → 登记"下次开机删除"。
        LogInfo("GUI: worker still busy after 10s -> hard kill");
        for (const auto& p : {IncompletePath(m_workerDest),
                              m_workerDest + L".stage",
                              m_workerDest + L".stage.tmp",
                              // 加密备份的明文中转（绝不能留下明文）
                              m_workerDest + L".plain.tmp",
                              m_workerDest + L".plain.tmp.stage",
                              m_workerDest + L".plain.tmp.stage.tmp"})
            RegisterPendingDelete(p);
        ::TerminateProcess(::GetCurrentProcess(), 0);
        return;
    }
    if (m_worker.joinable())
        m_worker.join();  // 已自然结束，立即返回
    CleanupIncompleteOutput();
    ::EnableWindow(m_hWnd, TRUE);
    Close();  // → WM_CLOSE（m_busy 已清）→ 正常关窗
}

void CMainForm::CleanupIncompleteOutput() {
    // 只删**本次正在写的半成品**（见 wim.cpp::Capture 的原子写与 ops.cpp::RunBackup
    // 的 stage 流程）：`<目标>.tmp`（Capture）、`<目标>.stage` + `<目标>.stage.tmp`
    // （ESP 并入中转）。**绝不能删最终路径** —— 那里可能是用户之前就存在的
    // 有效镜像，或 ESP 并入失败时已 rename 交付的主镜像。
    if (m_workerDest.empty())
        return;
    for (const auto& tmp : {IncompletePath(m_workerDest),
                            m_workerDest + L".stage",
                            m_workerDest + L".stage.tmp",
                            // 加密备份的明文中转（绝不能留下明文）
                            m_workerDest + L".plain.tmp",
                            m_workerDest + L".plain.tmp.stage",
                            m_workerDest + L".plain.tmp.stage.tmp"}) {
        if (::GetFileAttributesW(tmp.c_str()) == INVALID_FILE_ATTRIBUTES)
            continue;  // 正常取消时 Capture/RunBackup 已经删过了，这里是兜底
        ::SetFileAttributesW(tmp.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (::DeleteFileW(tmp.c_str())) {
            LogInfo("cancel: removed incomplete temp " + W2U(tmp));
        } else {
            LogError("cancel: cannot remove incomplete temp (err=" +
                     std::to_string(::GetLastError()) +
                     "), scheduled at reboot");
            RegisterPendingDelete(tmp);
        }
    }
}

// ────────────────── 事件路由 ──────────────────

void CMainForm::Notify(TNotifyUI& msg) {
    // 关闭按钮必须**先于** m_busy 早退处理：原来那句 `if (m_busy) return;` 把
    // 关闭点击也一起吞掉了，于是下面那段按 m_busy 分流的关闭分支成了**死代码**，
    // 表现为「任务执行中点右上角叉叉完全没反应」（用户 2026-09-20 实测吐槽）。
    if (msg.sType == DUI_MSGTYPE_CLICK &&
        msg.pSender->GetName() == CDuiString(_T("CloseBtn"))) {
        if (m_cancelling)
            return;
        if (!m_busy) {
            Close();
            return;
        }
        if (AskBusyClose())
            CancelAndExit();  // 选了「继续等待」则什么都不做（任务继续）
        return;
    }
    // 任务执行中只放行一件事：改「限制CPU」—— 用户 2026-09-28 规格明确要求
    // 「点击或**备份过程中**点击」都能调（改的是 Job 速率，随时可改，安全）。
    const bool cpuSel = msg.sType == DUI_MSGTYPE_ITEMSELECT &&
                        msg.pSender && msg.pSender->GetName() == _T("CpuCapBox");
    const bool silentClick = msg.sType == DUI_MSGTYPE_CLICK && msg.pSender &&
                             msg.pSender->GetName() == _T("Silent");
    const bool windowBtn = msg.sType == DUI_MSGTYPE_CLICK && msg.pSender &&
                           (msg.pSender->GetName() == _T("MinBtn") ||
                            msg.pSender->GetName() == _T("MaxBtn"));
    if (m_busy && !cpuSel && !silentClick && !windowBtn)
        return;  // 任务执行中忽略其他点击
    if (msg.sType == DUI_MSGTYPE_CLICK) {
        CDuiString name = msg.pSender->GetName();
        if (name == _T("BrowseBtn")) {
            if (m_backupMode) BrowseSaveFile(); else BrowseWimFile();
        } else if (name == _T("SearchBtn")) {
            OnSearchImages();  // 还原模式专属（备份模式按钮隐藏）
        } else if (name == _T("MainAction")) {
            if (m_backupMode) StartBackup(); else StartRestore();
        } else if (name == _T("FixBootBtn")) {
            // 皮肤里已隐藏（2026-10-27 收进「更多功能>>」）；保留路由给旧皮肤。
            StartRepairBoot();
        } else if (name == _T("MoreBtn")) {
            ShowMoreMenu();   // 「更多功能>>」下拉（打包日志/修复引导/清除菜单/镜像检查…）
        } else if (name == _T("BootMenuBtn")) {
            ToggleBootMenu();
        } else if (name == _T("LogBtn")) {
            ExportSupportFlow();
        } else if (name == _T("SiteLink")) {
            // 右下角「讨论」链接 → 用系统默认浏览器打开。
            // （2026-09-23 曾临时改成弹 BitLocker 演示框，用户确认文案后已改回。）
            ::ShellExecuteW(m_hWnd, L"open", kSiteUrl, nullptr, nullptr,
                            SW_SHOWNORMAL);
        } else if (name == _T("MinBtn")) {
            SendMessage(WM_SYSCOMMAND, SC_MINIMIZE, 0);
        }
    } else if (msg.sType == DUI_MSGTYPE_SELECTCHANGED) {
        CDuiString name = msg.pSender->GetName();
        if (name == _T("ModeBackup") || name == _T("ModeRestore")) {
            // 同组 Option 无论「被选中」还是「被取消选中」都会发 SELECTCHANGED，
            // 且两者顺序不保证（实测：先发新选中项、再发旧项的取消 → 旧项会把
            // 界面又刷回上一个模式，表现为「Tab 已经切了、内容却没变」）。
            // 所以只认「当前确实处于选中态」的那一次通知。
            COptionUI* pOpt = static_cast<COptionUI*>(msg.pSender);
            if (pOpt && pOpt->IsSelected()) {
                bool bak = (name == _T("ModeBackup"));
                if (bak != m_backupMode) {
                    m_backupMode = bak;
                    ApplyModeUi();
                    LogInfo(std::string("GUI mode: ") +
                            (bak ? "backup" : "restore") +
                            " path=" + W2U(ActivePath()));
                }
            }
        } else if (name == _T("FormatBox")) { /* 见下方 ITEMSELECT 分支（Combo 发的是 ITEMSELECT） */ }
    } else if (msg.sType == DUI_MSGTYPE_ITEMSELECT) {
        CDuiString name = msg.pSender->GetName();
        if (name == _T("PartitionBox")) {
            m_selPart = static_cast<CComboUI*>(msg.pSender)->GetCurSel();
            UpdateMainAction();
        } else if (name == _T("CpuCapBox")) {
            ApplyCpuCapFromUi();  // 空闲或备份中都直接生效
        } else if (name == _T("ImageIndexBox")) {
            int sel = static_cast<CComboUI*>(msg.pSender)->GetCurSel();
            if (sel >= 0 && sel < (int)m_imgIdx.size())
                m_selImageIndex = m_imgIdx[sel];
        } else if (name == _T("FormatBox")) {
            // 用户 2026-09-30 规格：换「类型」→ **同步改文件名后缀**。
            // 坑：Combo 发出的是 DUI_MSGTYPE_ITEMSELECT，**不是** SELECTCHANGED——
            // 之前挂在 SELECTCHANGED 分支上，所以点了「类型」文件名后缀不变。
            SyncPathExtFromFormat();
        }
    } else if (msg.sType == DUI_MSGTYPE_TEXTCHANGED) {
        if (msg.pSender && msg.pSender->GetName() == _T("ImagePath"))
            SyncImagePathFromUi();
    }
    WindowImplBase::Notify(msg);
}

