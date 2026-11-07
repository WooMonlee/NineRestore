// 拆分自 src/gui/main_form.cpp（按职责）：启动还原菜单（安装 / 删除 / 扫描 / 更多功能）。
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

// ────────────────── 启动还原（常驻引导模块） ──────────────────
// BIOS：GRUB4DOS(grldr/grldr.mbr/menu.lst) + BCD 实模式启动扇区条目；
// UEFI：**固件启动项**（NVRAM Boot#### + BootOrder），由固件直接加载内核。
// 装好后即使 Windows 蓝屏/引导损坏，也能从开机启动菜单进救援。

static bool BootMenuInstalled(std::string& detail) {
    if (IsUefiFirmware())
        return UefiBootEntryExists(detail);
    return !NeedsInstall(detail);  // NeedsInstall=true 表示还没装
}

void CMainForm::RefreshBootMenuBtn() {
    CControlUI* p = m_PaintManager.FindControl(_T("BootMenuBtn"));
    if (!p) return;
    // 文字色在 CButtonUI 上（SkinButton 派生自它；CControlUI 没有 SetTextColor）。
    CButtonUI* btn = static_cast<CButtonUI*>(p);
    std::string detail;
    // "已安装" = 救援环境在（UEFI 启动项 / BIOS 四件套）**或**契约里有绑定
    //（后者覆盖"把救援层装到非系统盘"的情况，见 StageRestoreMenu）。
    std::wstring boundImg;
    int boundIdx = 1;
    unsigned long long boundOff = 0, boundSz = 0;
    const bool hasBinding =
        ReadMenuBinding(&boundImg, &boundIdx, &boundOff, &boundSz);
    const bool installed = BootMenuInstalled(detail) || hasBinding;

    wchar_t tip[512] = {};
    if (installed) {
        // 已安装（2026-10-27 用户规格）：按钮**只负责安装**，已装即置灰；删除入口
        // 唯一化到「更多功能>>」→「清除启动中还原菜单」（避免同一件事两个入口，
        // 也避免按钮在两种语义间跳字）。悬停仍说清"绑的是哪个镜像 → 哪个分区"。
        if (hasBinding) {
            std::wstring tgt;
            bool uefiPath = false;
            for (const auto& d : EnumerateDisks()) {
                for (const auto& q : d.parts) {
                    if (boundOff && q.offsetBytes == boundOff &&
                        q.sizeBytes == boundSz && !q.letter.empty()) {
                        tgt = q.letter + Tr(L": 盘");
                        uefiPath = UseUefiBootFor(q);  // UEFI=固件启动项，选法不同
                    }
                }
            }
            if (tgt.empty()) tgt = Tr(L"（原目标分区）");
            if (uefiPath) {
                // 开机键按品牌/机型识别（bioskey：表移植自 BiosKC，2026-10-18）；
                // 识别不到用中性措辞，绝不写死 F12（HP 是 F9，见客户反馈）。
                swprintf(tip, 512,
                         Tr(L"已安装：开机按 %ls 选「SysRecover」，\n把 %ls（第 %d 个镜像）还原到 %ls。\n要清除请用「更多功能>>」→「清除启动中还原菜单」\n镜像请勿移动或删除，否则该菜单无法还原"),
                         BootMenuKeyHint().c_str(), boundImg.c_str(), boundIdx,
                         tgt.c_str());
            } else
                swprintf(tip, 512,
                         Tr(L"已安装：开机菜单里选「SysRecover」即可把\n%ls（第 %d 个镜像）还原到 %ls。\n要清除请用「更多功能>>」→「清除启动中还原菜单」\n镜像请勿移动或删除，否则该菜单无法还原"),
                         boundImg.c_str(), boundIdx, tgt.c_str());
        } else {
            wcscpy_s(tip, Tr(L"已安装启动还原（旧版安装，未绑定镜像）。\n要清除请用「更多功能>>」→「清除启动中还原菜单」"));
        }
        // 已安装（2026-11-06 用户反馈 P2）：按钮文字改为「已安装」，**不再置灰**——
        // 置灰状态下用户看不到悬停提示，会误以为程序坏了。点击只弹说明（见
        // ToggleBootMenu），删除/重装入口唯一化到「更多功能>>」→「清除启动中还原菜单」。
        p->SetText(Tr(L"已安装"));
        p->SetEnabled(!m_busy);
        p->SetBkColor(0xFFFFFFFF);
        btn->SetTextColor(ui_skin::C_TEXT);
    } else {
        // 未安装：**必须先选到可用镜像 + 目标分区**才可点（用户 2026-09-29 规格：
        // 没选镜像时按钮应为灰色，否则"装成功却不知道装了什么"）。备份模式不涉及
        // 镜像绑定 → 一律灰（该按钮只在还原模式有意义）。
        const bool can = !m_backupMode && !m_wimPath.empty() && m_imageOk &&
                         m_selPart >= 0 && !m_busy;
        if (!can && !m_busy)
            LogInfo("GUI boot menu disabled: backup=" +
                    std::to_string(m_backupMode ? 1 : 0) +
                    " pathEmpty=" + std::to_string(m_wimPath.empty() ? 1 : 0) +
                    " imageOk=" + std::to_string(m_imageOk ? 1 : 0) +
                    " part=" + std::to_string(m_selPart) + " busy=" +
                    std::to_string(m_busy ? 1 : 0));
        p->SetText(Tr(L"安装到启动"));
        p->SetEnabled(can);
        p->SetBkColor(0xFFFFFFFF);
        btn->SetTextColor(ui_skin::C_TEXT);
        wcscpy_s(tip, can
            ? Tr(L"把当前镜像写进开机菜单：重启后选「SysRecover」进恢复环境，\n自动把该镜像还原到第二步选中的目标分区\n镜像请勿移动或删除，否则该菜单无法还原")
            : Tr(L"请先选择镜像与目标分区，再安装到启动"));
    }
    p->SetToolTip(tip);
    p->Invalidate();
}

void CMainForm::ToggleBootMenu() {
    // 「安装到启动」= **只负责安装**（2026-10-27 用户规格）：已安装时按钮文字变
    //「已安装」且仍可点（点击只弹说明，见下）；删除入口唯一化到「更多功能>>」→
    //「清除启动中还原菜单」（ClearBootMenuEntry）。这里不保留"已安装就走删除"的分支。
    if (m_busy) return;
    // 已安装（P2）：按钮可点但只说明状态 —— 不重复安装、也不在这里删除
    //（删除入口唯一化到「更多功能>>」→「清除启动中还原菜单」）。
    {
        std::string detail;
        std::wstring boundImg;
        int boundIdx = 1;
        unsigned long long boundOff = 0, boundSz = 0;
        if (BootMenuInstalled(detail) ||
            ReadMenuBinding(&boundImg, &boundIdx, &boundOff, &boundSz)) {
            MessageBoxW(m_hWnd,
                        Tr(L"启动还原菜单已安装，无需重复安装。\n"
                           L"要清除或换绑镜像，请用「更多功能>>」→"
                           L"「清除启动中还原菜单」，之后本按钮会恢复为可安装。"),
                        Tr(L"已安装"), MB_OK | MB_ICONINFORMATION);
            return;
        }
    }
    // ── 安装（用户 2026-09-29 规格）：把**当前镜像 → 第二步选中的目标分区**写成
    //    常驻任务契约，并装常驻启动项；开机菜单里选「SysRecover」即执行该还原。
    if (m_wimPath.empty() || m_selPart < 0 ||
        m_selPart >= (int)m_parts.size()) {
        SetStatus(Tr(L"请先选择镜像与目标分区，再安装到启动"));
        return;
    }
    {
        const PartitionInfo& part = m_parts[m_selPart];
        // ⚠️ CConfirmDlg 的正文只显示**最多 3 行**、且是**单行标签**（PIT-039），
        // 超长/超行会被静默裁掉（行宽预算 376px/fs13 ≈ 28 全角字）—— 刻意压成
        // 3 行短句：第 1 行 = 用法（按引导链分措辞：BIOS 是 bootmgr 启动菜单 /
        // UEFI 是固件启动项，键名按品牌/机型识别（bioskey），识别不到用中性
        // 「快捷启动键」——不能写"BIOS菜单"会把用户误导进 BIOS 设置，也不能
        // 写死 F12（HP 是 F9，2026-10-18 客户反馈）；第 2 行只显示文件名（路径
        // 可能很长，裁到 18 字符）；第 3 行**必须保留"会格式化"的危险告知**
        //（§11 二次确认红线）。
        std::wstring name = m_wimPath;
        size_t sl = name.find_last_of(L"\\/");
        if (sl != std::wstring::npos)
            name = name.substr(sl + 1);
        if (name.size() > 18)
            name = name.substr(0, 18) + L"...";
        wchar_t msg[512];
        if (UseUefiBootFor(part))
            swprintf(msg, 512,
                     Tr(L"重启后按 %ls 选「SysRecover」还原\n"
                        L"镜像：%ls（第 %d 个）\n"
                        L"目标：%ls: 盘 —— 选中即格式化该分区"),
                     BootMenuKeyHint().c_str(), name.c_str(), m_selImageIndex,
                     part.letter.empty() ? L"?" : part.letter.c_str());
        else
            swprintf(msg, 512,
                     Tr(L"重启后在启动菜单选「SysRecover」还原\n"
                        L"镜像：%ls（第 %d 个）\n"
                        L"目标：%ls: 盘 —— 选中即格式化该分区"),
                     name.c_str(), m_selImageIndex,
                     part.letter.empty() ? L"?" : part.letter.c_str());
        // 与"确认还原"一致：右按钮（动作）高亮且为回车默认项，ESC 一律取消。
        // 2026-10-28（用户规格）：「安装到启动」给两个选项 ——
        //   「扫描后安装」= 只检查镜像能否**只靠救援层**还原（block 引擎，不另占空间），
        //                  通过就直接装启动项；
        //   「解压并安装」= 现在就解压到 C:\~new 再装启动项（A 引擎；EFS/特殊重解析点必须走这条）。
        // 最后一行括号里的容量 = 镜像**未压缩内容大小**（即解压后实际写入 ~new 的量），
        // 由 QuerySwapSpace 现算（只读元数据）；算不出来就退回不带数字的说法。
        double needGb = 0.0;
        {
            sysrecover::SwapSpaceEstimate est;
            if (sysrecover::QuerySwapSpace(m_wimPath, m_selImageIndex, part, est) &&
                est.content > 0)
                needGb = (double)est.content / kGiB;
        }
        std::wstring full = msg;
        if (needGb > 0.0) {
            // 自适应单位：≥1G 用 G（1 位小数），否则用 M（整数），再小用 K ——
            // 避免小镜像显示 "0.0G"/"0M"。
            wchar_t cap[32] = {};
            if (needGb >= 1.0)
                swprintf(cap, 32, L"%.1fG", needGb);
            else if (needGb * 1024.0 >= 1.0)
                swprintf(cap, 32, L"%.0fM", needGb * 1024.0);
            else
                swprintf(cap, 32, L"%.0fK", needGb * 1024.0 * 1024.0);
            wchar_t line[320] = {};
            swprintf(line, 320,
                     Tr(L"\n「扫描」= 检查镜像条件并安装恢复选项（不另占空间）\n「解压」= 解压到 C:\\~new 并安装恢复选项（占 C 盘 %ls）"),
                     cap);
            full += line;
        } else {
            full += Tr(L"\n「扫描」= 检查镜像条件并安装恢复选项（不另占空间）\n「解压」= 解压到 C:\\~new 并安装恢复选项（占 C 盘空间）");
        }
        const int pick = CConfirmDlg::Ask3(
            m_hWnd, Tr(L"安装到启动"), full, Tr(L"扫描后安装"), Tr(L"解压并安装"),
            Tr(L"取消"), /*defaultIdx=*/0, /*midDisabled=*/false, /*escIdx=*/2,
            /*centerTitle=*/true);
        if (pick == 2) return;
        if (pick == 1) {
            LogInfo("GUI menu install: user chose 解压 (engine=swap)");
            StartMenuToggle(/*install=*/true, /*engine=*/"swap");
            return;
        }
        LogInfo("GUI menu install: user chose 扫描");
        StartMenuScan();
        return;
    }
    RefreshBootMenuBtn();
    UpdateMainAction();
}

// 装/删菜单的异步执行（2026-10-18，用户实测"点了没反应"的修复）：
// 之前 StageRestoreMenu 在 UI 线程同步跑（写 ~28MB 救援载荷 + 逐文件双读
// CRC 校验 ≈ 56MB I/O），Windows 5 秒无消息泵即判定"无响应" → 幽灵窗。
// 现在与备份/还原同款：worker 线程 + WM_PROGRESS_UPDATE 进度 + 完成消息。
void CMainForm::StartMenuToggle(bool install, const std::string& engine) {
    if (m_busy) return;
    // 安装的请求在此定格（镜像/分区/子镜像索引在 busy 期间不可改）；
    // 删除不用请求，但统一传一份默认值，worker 里按 install 分支取用。
    RestoreRequest req;
    req.image = m_wimPath;
    if (m_selPart >= 0 && m_selPart < (int)m_parts.size()) {
        req.disk = (int)m_parts[m_selPart].diskIndex;
        req.part = (int)m_parts[m_selPart].partNumber;
    }
    req.index = m_selImageIndex;
    req.repairBoot = true;
    // 2026-10-28（用户规格）：engine 由「扫描/解压」决定 —— 扫描通过用 block
    // （救援层直接还原，安装时不解压），否则 swap（现在就解压到 C 盘）。
    req.engine = engine;
    m_busy = true;
    m_menuInstall = install;
    m_menuErr.clear();
    m_menuBlocked = false;
    LogInfo("GUI menu " + std::string(install ? "install" : "remove") +
            " request: image=" + W2U(req.image) + " disk=" +
            std::to_string(req.disk) + " part=" + std::to_string(req.part) +
            " index=" + std::to_string(req.index) +
            " engine=" + (install ? req.engine : std::string("-")) +
            " keepOld=" + std::to_string(req.keepOld ? 1 : 0));
    last_adv_ = sysrecover::ADV_NONE;
    // m_lastStage 给 WM_TIMER(1) 每秒刷新「已用」用（状态栏文字 + 流逝时间）。
    m_lastStage = install ? Tr(L"正在安装启动还原菜单（写入契约 + 部署救援环境）...")
                          : Tr(L"正在删除启动还原菜单...");
    SetStatus(m_lastStage);
    SetProgress(0);
    m_start = std::chrono::steady_clock::now();
    SetTimer(m_hWnd, 1, 1000, nullptr);
    UpdateMainAction();
    RefreshBootMenuBtn();
    LogInfo(std::string("GUI boot menu toggle: ") +
            (install ? "install" : "remove"));
    if (m_worker.joinable()) { m_worker.join(); }
    m_cancel = false;
    try {
        m_worker = std::thread(&CMainForm::MenuToggleAsync, this, install, req);
    } catch (...) {
        m_busy = false;
        UpdateMainAction();
        RefreshBootMenuBtn();
        SetStatus(Tr(L"线程创建失败"));
        MessageBoxW(m_hWnd, Tr(L"无法创建工作线程，请重试"), Tr(L"错误"),
                    MB_OK | MB_ICONERROR);
    }
}

// 「安装到启动」→「扫描」（2026-10-28 用户规格）：只读元数据，判断镜像能否
// **只靠救援层**还原（block 引擎）。能 → 直接免解压安装；不能 → 提示改用「解压」。
void CMainForm::StartMenuScan() {
    if (m_busy) return;
    m_busy = true;
    m_scanBad = 0;
    m_scanEnc = 0;
    m_lastStage = Tr(L"正在检查镜像能否由救援层直接还原...");
    SetStatus(m_lastStage);
    SetProgress(0);
    m_start = std::chrono::steady_clock::now();
    SetTimer(m_hWnd, 1, 1000, nullptr);
    UpdateMainAction();
    RefreshBootMenuBtn();
    const std::wstring image = m_wimPath;
    const int index = m_selImageIndex;
    if (m_worker.joinable()) m_worker.join();
    m_cancel = false;
    m_worker = std::thread([this, image, index] {
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        size_t bad = 0, enc = 0;
        std::string err;
        const bool ok =
            sysrecover::CanRestoreInLinuxOnly(image, index, &bad, &enc, err);
        m_scanBad = bad;
        m_scanEnc = enc;
        LogInfo(std::string("GUI menu scan: linux_only=") +
                (ok ? "yes" : "no") + " reparse=" + std::to_string(bad) +
                " efs=" + std::to_string(enc) +
                (err.empty() ? "" : (" / " + err)));
        ::PostMessageW(m_hWnd, WM_MENU_SCAN_DONE, ok ? 1 : 0, 0);
    });
}

void CMainForm::OnMenuScanDone(bool linuxOnly) {
    KillTimer(m_hWnd, 1);
    m_busy = false;
    ShowProgress(false);
    if (m_worker.joinable()) m_worker.join();
    if (m_cancel) {
        UpdateMainAction();
        return;
    }
    SetProgress(0);
    if (linuxOnly) {
        // 能在救援层直接还原 → 免解压安装（安装时只写契约 + 引导层）
        StartMenuToggle(/*install=*/true, /*engine=*/"block");
        return;
    }
    // 不能 → 说明为什么，并把「解压并安装」放到默认项（回车=解压、ESC=取消）
    std::wstring why;
    if (m_scanBad > 0)
        why = Tr(L"镜像含特殊重解析点，救援层无法写回。");
    else if (m_scanEnc > 0)
        why = Tr(L"镜像含 EFS 加密文件，救援层会还原成空文件。");
    else
        why = Tr(L"镜像无法只在救援层中还原。");
    why += Tr(L"\n要现在解压到 C 盘吗？（约需与镜像内容等量的空间）");
    if (CConfirmDlg::Ask2(m_hWnd, Tr(L"该镜像需要先解压"), why, Tr(L"取消"),
                          Tr(L"解压并安装"), /*defaultIsRight=*/true) == 1) {
        StartMenuToggle(/*install=*/true, /*engine=*/"swap");
        return;
    }
    UpdateMainAction();
}

void CMainForm::MenuToggleAsync(bool install, const RestoreRequest& req) {    // 同 StartRestoreAsync：让出 CPU，写盘期间窗口仍可拖动（否则拖窗发飘）。
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    sysrecover::ResetBootBlocked();
    int rc = 0;
    if (install) {
        // 进度：ops 层 StageRestoreImpl 的 Apply（菜单安装同样要完整解压
        // ~new，见 ops.cpp 注释）、引导层前有 progress(99,"boot") 收官标记。
        // 约定：回调返回 **true = 请求中止**（与还原链路一致）。
        auto progressFn = [this](int pct, const std::string& stage) -> bool {
            if (m_cancel) return true;
            auto now = std::chrono::steady_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now - m_lastProgressPost).count();
            bool stageChanged = stage != m_lastStageRaw;
            if (ms < kProgressThrottleMs && pct < 100 && !stageChanged)
                return false;
            m_lastProgressPost = now;
            m_lastStageRaw = stage;
            PostMessage(WM_PROGRESS_UPDATE, (WPARAM)pct,
                        (LPARAM)new std::wstring(U2W(stage)));
            return false;
        };
        m_lastProgressPost = std::chrono::steady_clock::now();
        m_lastStageRaw.clear();
        std::string err;
        rc = StageRestoreMenu(req, err, progressFn, &last_adv_);
        m_menuErr = err;
    } else {
        std::string log;
        bool ok = RemoveBootLayer(log);
        // 绑定副契约必须一起删：否则 ReadMenuBinding 仍判"已安装" → 按钮
        // 停在「删除菜单」回不到「安装菜单」（用户 2026-10-05 实测 bug）。
        DeleteMenuBinding();
        m_menuErr = log;
        rc = ok ? 0 : 1;
    }
    // 线程局部标志必须在 worker 线程取（PIT-184 更正段）
    m_menuBlocked = sysrecover::TakeBootBlocked();
    PostMessage(WM_MENU_DONE, (WPARAM)rc, install ? 1 : 0);
}

void CMainForm::OnMenuDone(int rc, bool install) {
    KillTimer(m_hWnd, 1);
    m_busy = false;
    if (m_worker.joinable()) m_worker.join();
    LogInfo(std::string("GUI boot menu ") + (install ? "install" : "remove") +
            " done rc=" + std::to_string(rc) + " / " + m_menuErr);
    if (m_cancel) {
        // 「终止并退出」流程：不要再弹提示（窗口马上关），只留日志。
        ShowProgress(false);
        return;
    }
    ShowProgress(false);
    if (rc == 0) {
        if (install) {
            // 两条引导链的"进菜单"方式不同（PIT-092/094）：BIOS 在 bootmgr 的
            // 选择菜单里选；UEFI 是固件启动项，要开机按快捷启动键（键名按品牌/
            // 机型识别，bioskey；识别不到用中性措辞，不写死 F12）。
            bool uefiPath = m_selPart >= 0 && m_selPart < (int)m_parts.size() &&
                            UseUefiBootFor(m_parts[m_selPart]);
            if (uefiPath) {
                wchar_t done[256];
                swprintf(done, 256,
                         Tr(L"启动还原菜单已安装：开机按 %ls 选「SysRecover」即可还原该镜像"),
                         BootMenuKeyHint().c_str());
                SetStatus(done);
            } else {
                SetStatus(Tr(L"启动还原菜单已安装：开机选「SysRecover」即可还原该镜像"));
            }
        } else {
            SetStatus(Tr(L"已删除启动还原"));
        }
    } else if (install) {
        // 失败必须看得见（静默模式也弹，fail-safe —— 与 OnTaskComplete 同款）。
        std::wstring e = m_menuErr.empty() ? Tr(L"安装启动还原菜单失败")
                                           : U2W(m_menuErr);
        std::string advice = sysrecover::ErrorAdvice(rc, last_adv_);
        SetStatus(e);
        // 2026-10-28（用户规格）：被安全软件拦写 → 弹「退出防护 → 重试」，
        // 用户退出火绒后点重试即可成功，不用自己去猜原因。
        if (m_menuBlocked &&
            CConfirmDlg::Ask2(
                m_hWnd, Tr(L"安装启动还原菜单未完成"),
                Tr(L"写入启动项/引导区被安全软件拦截\n（如火绒/360 的「写磁盘保留扇区」）。\n请退出该防护或把本程序加入信任，再点「重试」。"),
                Tr(L"关闭"), Tr(L"重试"), /*defaultIsRight=*/true) == 1) {
            RefreshBootMenuBtn();
            UpdateMainAction();
            StartMenuToggle(/*install=*/true);
            return;
        }
        MessageBoxW(m_hWnd, (e + U2W(advice)).c_str(),
                    rc == 4 ? Tr(L"安全门禁拒绝") : Tr(L"安装启动还原菜单失败"),
                    MB_OK | MB_ICONERROR);
    } else {
        SetStatus(Tr(L"删除启动还原未完全成功"));
    }
    RefreshBootMenuBtn();
    UpdateMainAction();
}

// ── 「更多功能>>」下拉（2026-10-27 用户规格） ─────────────────────
// 原「异机」勾选框、「修复引导」按钮、原「清除菜单」按钮 + 新增「镜像检查」
// 全部收进这个菜单：第三行原本挤了 6 个按钮，英文/高分屏下必然裁字。
// 用原生 TrackPopupMenu（与 ShowImagePickMenu 同款）：菜单项的勾选/禁用态
// 每次打开时按当前状态现算，不依赖皮肤控件（也就没有尺寸/裁剪问题）。
void CMainForm::ShowMoreMenu() {
    if (m_busy) return;
    HMENU hMenu = ::CreatePopupMenu();
    if (!hMenu) return;
    std::string detail;
    std::wstring boundImg;
    int boundIdx = 1;
    unsigned long long boundOff = 0, boundSz = 0;
    const bool installed =
        BootMenuInstalled(detail) ||
        ReadMenuBinding(&boundImg, &boundIdx, &boundOff, &boundSz);
    // 2026-10-29 用户规格：菜单内容**按模式分开**——
    //   「文件>系统」（还原）只放还原/引导相关：异机还原、修复引导、清除菜单、镜像检查；
    //   「系统>文件」（备份）只放备份相关：增量备份、加密备份。
    //   不相关项直接不显示（不是灰掉），避免用户点进来看到一堆用不了的条目。
    if (m_backupMode) {
        // 增量备份：保存位置是**已存在的** .wim/.esd 才可点（没有镜像无从"追加"）。
        bool canAppend = false;
        if (!m_backupPath.empty() &&
            GetFileAttributesW(m_backupPath.c_str()) !=
                INVALID_FILE_ATTRIBUTES) {
            const wchar_t* dot = wcsrchr(m_backupPath.c_str(), L'.');
            canAppend = dot && (_wcsicmp(dot, L".wim") == 0 ||
                                _wcsicmp(dot, L".esd") == 0);
        }
        ::AppendMenuW(hMenu, MF_STRING | (canAppend ? 0u : MF_GRAYED), 5,
                      Tr(L"增量备份（追加到当前镜像）"));
        // 加密备份（docs/30）：勾选态 = 已设置口令；只保护**下一次**备份（输出 .zje）。
        const bool encOn = !m_backupPassword.empty();
        ::AppendMenuW(hMenu,
                      MF_STRING | (encOn ? MF_CHECKED : 0u), 6,
                      Tr(L"加密备份为 .zje 格式，用密码保护镜像"));
    } else {
        // 「清除」只在真装了东西时可点（否则空转，用户会以为没生效）。
        // 「镜像检查」只在镜像已解析成功时可点（对坏镜像做全量校验没有意义）。
        const bool canVerify = !m_wimPath.empty() && m_imageOk;
        // 2026-11-05 用户规格：与「异机还原」对调 —— 异机还原变成第三步常驻勾选框，
        // 「打包日志」收进本菜单（菜单第一项，占原「使用异机还原」的位置）。
        ::AppendMenuW(hMenu, MF_STRING, 1, Tr(L"打包日志"));
        ::AppendMenuW(hMenu, MF_STRING, 2, Tr(L"修复引导"));
        ::AppendMenuW(hMenu, MF_STRING | (installed ? 0u : MF_GRAYED), 3,
                      Tr(L"清除启动中还原菜单"));
        ::AppendMenuW(hMenu, MF_STRING | (canVerify ? 0u : MF_GRAYED), 4,
                      Tr(L"镜像检查（完整校验）"));
        // H4（1.0）：还原前全量校验开关（默认关；快速结构预检始终自动做）。
        ::AppendMenuW(hMenu,
                      MF_STRING |
                          (m_restoreVerifyImage ? MF_CHECKED : 0u),
                      10,
                      Tr(L"还原前完整校验镜像（默认关，较耗时）"));
        // 导出本机启动关键驱动（异机还原用；只导存储/输入类）
        ::AppendMenuW(hMenu, MF_STRING, 7,
                      Tr(L"导出本机硬盘驱动供异机还原"));
    }
    // PIT-207：与模式无关的"包"操作（两种模式都给）
    ::AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    // M12：USB 设备（含 USB 外接硬盘，它上报 BusTypeUsb 会被默认过滤）
    // 默认不显示；只有用户显式勾选才枚举出来，并在分区行标注「USB 设备」。
    ::AppendMenuW(hMenu, MF_STRING | (m_showUsb ? MF_CHECKED : 0u), 11,
                  Tr(L"显示 USB 设备（默认隐藏，谨慎选择）"));
    ::AppendMenuW(hMenu, MF_STRING, 8, Tr(L"打包成单文件…"));
    ::AppendMenuW(hMenu, MF_STRING, 9, Tr(L"解压到文件夹…"));

    // MoreBtn 是顶层 float 控件 → GetPos() 已是客户区坐标；换屏幕坐标再弹。
    RECT rc{};
    CControlUI* pBtn = m_PaintManager.FindControl(_T("MoreBtn"));
    if (pBtn) rc = pBtn->GetPos();
    POINT pt = {rc.left, rc.bottom + 2};
    ::ClientToScreen(m_hWnd, &pt);
    // m_pickGuard 防"菜单关闭后焦点回来又把别的下拉顶出来"（同 ShowImagePickMenu）。
    m_pickGuard = true;
    const int cmd = ::TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_LEFTALIGN, pt.x,
                                     pt.y, 0, m_hWnd, nullptr);
    ::DestroyMenu(hMenu);
    m_pickGuard = false;
    if (cmd <= 0) return;
    if (cmd == 1) {
        // 2026-11-05：菜单第一项现在是「打包日志」（原为「使用异机还原」开关）。
        ExportSupportFlow();
    } else if (cmd == 2) {
        StartRepairBoot();
    } else if (cmd == 3) {
        ClearBootMenuEntry();
    } else if (cmd == 4) {
        StartVerifyImage();
    } else if (cmd == 11) {
        // M12：显式开关 —— 打开后才枚举 USB 设备；关闭时恢复默认（隐藏）。
        m_showUsb = !m_showUsb;
        SetStatus(m_showUsb ? Tr(L"正在枚举磁盘（含 USB 设备）…")
                            : Tr(L"正在枚举磁盘（已隐藏 USB 设备）…"));
        PopulatePartitions(sysrecover::EnumerateDisks(m_showUsb));
        SetStatus(m_showUsb ? Tr(L"已显示 USB 设备（还原目标请谨慎选择）")
                            : Tr(L"已隐藏 USB 设备"));
        LogInfo(std::string("GUI show usb devices: ") +
                (m_showUsb ? "on" : "off"));
    } else if (cmd == 10) {
        m_restoreVerifyImage = !m_restoreVerifyImage;
        SetStatus(m_restoreVerifyImage
                      ? Tr(L"已开启还原前完整校验（耗时与镜像大小成正比）")
                      : Tr(L"已关闭还原前完整校验（仍会做毫秒级结构预检）"));
        LogInfo(std::string("GUI restore verify: ") +
                (m_restoreVerifyImage ? "on" : "off"));
    } else if (cmd == 5) {
        StartIncrementalBackup();
    } else if (cmd == 7) {
        ExportDriversFlow();
    } else if (cmd == 8) {
        PackSelfFlow();
    } else if (cmd == 9) {
        ExtractSelfFlow();
    } else if (cmd == 6) {
        if (!m_backupPassword.empty()) {
            m_backupPassword.clear();
            SetStatus(Tr(L"已关闭备份加密"));
            LogInfo("GUI backup encryption: off");
            return;
        }
        std::wstring pwd;
        if (!CPasswordDlg::Ask(
                m_hWnd, Tr(L"加密备份"),
                Tr(L"请设置备份密码（忘记后无法恢复镜像）："), pwd,
                Tr(L"启用加密")))
            return;
        m_backupPassword = pwd;
        SetStatus(Tr(L"已启用备份加密（下次备份输出 .zje，请牢记密码）"));
        LogInfo("GUI backup encryption: on");
    }
}

// 「清除启动中还原菜单」（原 RepairBootBtn 的逻辑）：确认 → **异步**删引导层 +
// 绑定副契约。异步是必须的：删除要卸载 ESP 上的 ~28MB 载荷，同步跑在慢盘上
// 会触发 Windows 的"无响应"幽灵窗（PIT-152 同款）。
void CMainForm::ClearBootMenuEntry() {
    if (m_busy) return;
    if (CConfirmDlg::Ask2(
            m_hWnd, Tr(L"清除启动中还原菜单"),
            Tr(L"将删除开机启动菜单里的 SysRecover 条目，\n"
               L"并清理目标分区上的救援文件。\n"
               L"确定继续吗？"),
            Tr(L"取消"), Tr(L"清除"),
            /*defaultIsRight=*/false) != 1)
        return;
    StartMenuToggle(/*install=*/false);
}

