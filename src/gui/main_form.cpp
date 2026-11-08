// 主窗口实现。Phase 5：CLI 逻辑经 app/ops 共享层桥接到 GUI。
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

// 下列小工具被拆分后的多个 TU 共用，故为 CMainForm 的 class static
// （定义留在本文件，声明在 main_form.h）。其余只用单文件的小工具留在各分段内。

// 分区行容量列用整数 GB（老界面观感："28 / 89 GB"、"可用 61 GB"）
std::wstring CMainForm::FormatGb(uint64_t bytes) {
    wchar_t buf[48];
    swprintf(buf, 48, L"%.0f GB", (double)bytes / kGiB);
    return buf;
}

std::wstring CMainForm::TimestampName() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[40];
    swprintf(buf, 40, L"%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// 只有日期：YYYYMMDD（备份的镜像信息与默认文件名用，用户规格 2026-09-21）。
std::wstring CMainForm::TimestampDate() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[16];
    swprintf(buf, 16, L"%04u%02u%02u", st.wYear, st.wMonth, st.wDay);
    return buf;
}

// ────────────────── 基础 ──────────────────

CMainForm::CMainForm() : m_lastProgressPost(std::chrono::steady_clock::now()) {}
CMainForm::~CMainForm() {
    if (m_tipHwnd) { ::DestroyWindow(m_tipHwnd); m_tipHwnd = NULL; }
    if (m_worker.joinable()) {
        m_cancel = true;
        m_worker.join();
    }
    // 启动后置工作线程（问题 2）：它只做 diag/收集/枚举，无取消位，等它收手
    //（正常毫秒级；插了慢速 USB2 盘时最坏等于原来 InitWindow 阻塞的那段）。
    if (m_startWorker.joinable()) m_startWorker.join();
}

CDuiString CMainForm::GetSkinFolder() { return _T("skin\\"); }
CDuiString CMainForm::GetSkinFile()  {
    // 交给 builder 的是**已按词典翻译的整段 XML**（builder 认 "<" 开头的字符串，
    // 见 UIDlgBuilder.cpp）；读不到/无译文时回退原文件名，行为与改动前一致。
    std::wstring xml = sysrecover::LoadSkinXml(L"main.xml");
    if (!xml.empty()) return CDuiString(xml.c_str());
    return _T("main.xml");
}
LPCTSTR CMainForm::GetWindowClassName() const { return _T("SysRecoverUI"); }

void CMainForm::InitWindow() {
    // 日志根（PIT-104/105）：一般=程序目录；软件在系统盘时=<数据盘>\ZJRESTORE
    //（还原/重装系统盘后日志仍留存）。LogInit 必须在第一条 StartupPhase 之前
    //（阶段耗时日志靠它落盘）。
    std::wstring logsDir = sysrecover::LogBaseDir() + L"\\logs";
    CreateDirectoryW(logsDir.c_str(), nullptr);
    LogInit(logsDir);
    ProgressInit(logsDir);
    // 每次启动都归纳日志（PIT-156，2026-10-18 起不再只限备返克隆首启）：先把
    // 救援层/旧版本留在数据盘上的日志搬进日志根，随后 StartupLazyWork 的收集器
    // 才收 `source.txt` 等痕迹、清理器才能**整个**删掉还原残留的 C:\ZJRESTORE
    // 壳（分类保护程序的家/启动菜单，见 relocate.cpp::ClassifyZjrResidue）。
    // 必须放在 StartupLazyWork（收痕迹 + 清散件）**之前** —— 否则收集器先收
    // 进 logs\collected 并清掉源目录，搬运"无源可搬"（顺序由 WM_STARTUP_LAZY
    // 在消息循环里才被处理保证）。守卫（PE / 暂存待还原）见 relocate.cpp。
    // 失败只记日志不挡流程（拿不到日志也只是日志少几行）。
    {
        const std::string mig = sysrecover::MigrateLogsToLogHome();
        if (!mig.empty()) sysrecover::LogInfo(mig);
    }
    sysrecover::StartupPhase("InitWindow");
    LogInfo("GUI InitWindow");
    // 支持把 .esd/.wim 直接拖进窗口（第一步的输入框）。原生 EDIT 子窗口由
    // CSkinEditUI 转投到这里（见 ui_skin.cpp::EnsureDropTarget），窗口空白处
    // 则由本窗口直接接收。
    ::DragAcceptFiles(m_hWnd, TRUE);
    // 本 exe 是 requireAdministrator（高完整性），而拖放源「资源管理器」是
    // 普通权限 → UIPI 会拦下拖放相关消息，表现为「拖进去没反应」。按微软
    // 官方做法放行这三个消息（0x0049 = WM_COPYGLOBALDATA，OLE 拖放用）。
    ::ChangeWindowMessageFilterEx(m_hWnd, WM_DROPFILES, MSGFLT_ALLOW, nullptr);
    ::ChangeWindowMessageFilterEx(m_hWnd, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    ::ChangeWindowMessageFilterEx(m_hWnd, 0x0049, MSGFLT_ALLOW, nullptr);
    m_sysDesc = sysrecover::DescribeRunningSystem();  // 备份信息/默认文件名要用
    // ⚠️ 问题 2（用户 2026-10-12 规格："插了 USB2 盘要等 20 秒才出界面，
    // 检查请挪到界面显示之后"）：**这里不再做任何可能被慢盘卡住的事** ——
    // 写 diag/list、收部署痕迹、清理散件、枚举磁盘全部搬到界面显示之后的
    // 后台线程（WM_STARTUP_LAZY），只留"立即出界面"必需的初始化。
    // 分区下拉在后台枚举完成前显示占位文案（下面 WM_PARTS_READY 回填）。
    if (CControlUI* pLoad = m_PaintManager.FindControl(_T("PartLoadingText")))
        pLoad->SetVisible(true);
    ApplyModeUi();
    RefreshBootMenuBtn();
    SetTimer(m_hWnd, 2, 500, nullptr);  // 路径输入兜底同步（见 WM_TIMER/wParam==2）
    m_tipHwnd = ::CreateWindowEx(0, TOOLTIPS_CLASS, NULL,
        WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        m_hWnd, NULL, ::GetModuleHandle(NULL), NULL);
    if (m_tipHwnd) {
        // cbSize 必须用 V1 尺寸：本 exe 无 comctl32 v6 清单（PIT-018）→ 跑 v5，
        // 而 MinGW 的 TOOLINFOW 多带一个 v6 才有的 void* lpReserved（sizeof=72），
        // v5 只认 V1/V2 尺寸 → 传 sizeof(TOOLINFO) 会被拒，TTM_ADDTOOL 返回 FALSE。
        m_tipInfo.cbSize = TTTOOLINFO_V1_SIZE;
        m_tipInfo.uFlags = 0;
        m_tipInfo.hwnd = m_hWnd;
        m_tipInfo.uId = 1;
        m_tipInfo.lpszText = const_cast<LPTSTR>(_T(""));
        m_tipInfo.rect = {0, 0, 0, 0};
        ::SendMessage(m_tipHwnd, TTM_ADDTOOL, 0, (LPARAM)&m_tipInfo);
        ::SendMessage(m_tipHwnd, TTM_SETMAXTIPWIDTH, 0, 300);
    }
    // 救援层失败回执检查（用户 2026-10-04 规格 2）：**等后台收集跑完再查**
    //（它要扫 logs\collected 里的回执，早了会漏）—— 见 StartupLazyWork 末尾。
    sysrecover::StartupPhase("InitWindow-ui");
    ::PostMessage(m_hWnd, WM_STARTUP_LAZY, 0, 0);
}

// ────────────────── 启动后置工作（问题 2，2026-10-12）──────────────────
// 用户规格：插着 USB2 盘时启动要等 20 秒才出界面 → 把 diag/list 写入、部署
// 痕迹收集、散件清理、磁盘枚举**全部挪到界面显示之后**，且在后台线程做。
// 界面侧只做"必须立刻有"的事；分区下拉期间显示 PartLoadingText 占位。
void CMainForm::StartupLazyWork() {
    const std::wstring logsDir = sysrecover::LogBaseDir() + L"\\logs";
    // 磁盘表**只枚举一次**，diag/list/收集/填下拉共用 —— 一次枚举要对每个
    // PhysicalDriveN 开句柄发 IOCTL，慢盘上每次都能卡几秒，过去这里
    // diag(1) + list(1) + 收集(每盘符 1) + 填下拉(1) 各来一遍。
    sysrecover::StartupPhase("lazy-start");
    std::vector<DiskInfo> disks = sysrecover::EnumerateDisks(m_showUsb);
    sysrecover::StartupPhase("enumerate-disks");  // 问题 2 的关键计时点
    sysrecover::WriteDiagFiles(logsDir, &disks);
    sysrecover::CollectDeployArtifacts(logsDir + L"\\collected", 0, &disks);
    sysrecover::StartupPhase("diag+collect");
    // PIT-137（用户 2026-10-08 规格）：每次运行"收完即清"——黑匣子已进
    // logs\collected，各盘根的散件立即清掉。契约/引导文件不动。
    sysrecover::CleanupStrayLogs();
    sysrecover::StartupPhase("cleanup-stray");
    // beta.14（PIT-241）：旧版装过菜单的机器，启动时把 menu.lst 标题/内核参数与
    // BCD 描述自愈成当前文案（"SysRecovery Environment" → NineRestore），避免
    // 旧条目一直留着或与新版并存。没装过则完全不写盘。
    for (const std::wstring& drv :
         {sysrecover::SystemDrive(), sysrecover::FindDataDrive()}) {
        std::string rlog;
        sysrecover::RefreshBootLayerNames(drv, sysrecover::ExeDir(), rlog);
        if (!rlog.empty()) sysrecover::LogInfo("boot-name refresh: " + rlog);
    }
    sysrecover::StartupPhase("boot-name-refresh");
    // beta.14（PIT-243）：还原会把 C:\ 的启动载荷与 \Boot\BCD 里的启动项一起覆盖。
    // 救援层已把载荷写回新系统根并留 bcd=pending 标记，这里补建 BCD 条目；
    // 成功改 bcd=done、失败保留 pending（下次启动重试，日志留痕）。
    {
        std::string mlog;
        if (!sysrecover::EnsureMenuAfterRestore(mlog) && !mlog.empty())
            mlog += "menu-redeploy: result FAILED (retry next start)\n";
        if (!mlog.empty()) sysrecover::LogInfo("menu-redeploy: " + mlog);
    }
    sysrecover::StartupPhase("menu-redeploy");
    // 回主线程填分区下拉（lParam 所有权交给 handler，由它 delete）。
    auto* out = new std::vector<DiskInfo>(std::move(disks));
    if (!::PostMessage(m_hWnd, WM_PARTS_READY, 0, (LPARAM)out)) {
        delete out;
        return;
    }
    // 收集完成后再查救援层失败回执（依赖 logs\collected）。
    ::PostMessage(m_hWnd, WM_CHECK_RESCUE, 0, 0);
}

// ────────────────── pre-message filter：Ctrl+Tab 切模式 ──────────────────
// WindowImplBase::OnCreate 已把 this 注册为 pre-message filter
//（WinImplBase.cpp:275），虚分派先到本覆写；这里只新增拦 Ctrl+Tab，
// 其余原样转交基类（保持 ESC 关窗等既有行为）。
// 裸 Tab 不拦（保持焦点循环）；忙碌时只吞不切 —— Notify 对忙时
// SELECTCHANGED 早退（main_form.cpp:737），切了会造成「Tab 变了、模式没变」。
LRESULT CMainForm::MessageHandler(UINT uMsg, WPARAM wParam, LPARAM lParam,
                                  bool& bHandled) {
    bHandled = false;
    if (uMsg == WM_KEYDOWN && wParam == VK_TAB &&
        (::GetKeyState(VK_CONTROL) & 0x8000)) {
        bHandled = true;  // 吞掉 → PreMessageHandler 提前返回，不再 SetNextTabControl
        if (!m_busy) {
            CControlUI* p = m_PaintManager.FindControl(
                m_backupMode ? _T("ModeRestore") : _T("ModeBackup"));
            if (p)
                static_cast<COptionUI*>(p)->Selected(true);  // → SELECTCHANGED → ApplyModeUi
        }
        return 0;
    }
    return WindowImplBase::MessageHandler(uMsg, wParam, lParam, bHandled);
}

// ────────────────── HandleMessage（worker 线程安全回调） ──────────────────

LRESULT CMainForm::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    // 回车不再触发「开始」（用户 2026-10-11 二轮规格，替代 2026-10-10 的
    // 「全局回车 = 主按钮」）：开始改走 **Alt+E**（main.xml MainAction
    // shortcut="E"）。这里只在「焦点恰好是主按钮」时吞掉回车 —— 否则
    // CButtonUI::DoEvent 对 VK_RETURN 会 Activate，回车仍能误启备份/还原；
    // 焦点在别处时回车交给标准控件语义（如 TAB 到「删除菜单」按回车 =
    // 点击它，且有确认框兜底）。焦点在原生 EDIT 子窗口时 WM_KEYDOWN 直接
    // 发给子窗口、根本到不了这里（天然只在非输入文本时生效）。
    if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
        CControlUI* pFocus = m_PaintManager.GetFocus();
        if (pFocus && _tcscmp(pFocus->GetName(), _T("MainAction")) == 0)
            return 0;
    }
    if (msg == WM_MOUSELEAVE || msg == WM_MOUSEMOVE)
        HandleTooltipMessage(msg, wParam, lParam);  // 不消费，仍交基类
    if ((msg == WM_COMMAND && HIWORD(wParam) == EN_SETFOCUS) ||
        msg == WM_OPEN_PICK_MENU ||
        (msg == WM_COMMAND && HIWORD(wParam) == EN_CHANGE))
        return HandleImagePathEditMessage(msg, wParam);
    if (msg == WM_TIMER && wParam >= 1 && wParam <= 3)
        return HandleTimerMessage(wParam);
    if (msg == WM_PROGRESS_UPDATE) {
        // 防御（PIT-096）：lParam 是 worker new 的 std::wstring*，正常永不为 0；
        // 为 0 = 消息撞车/异常投递，必须丢弃 —— 解引用空指针就是 2026-09-30
        // 用户实测的崩溃点（read address 0x8）。已另有消息重编号根治撞车。
        if (lParam) {
            OnProgressUpdate((int)wParam, *reinterpret_cast<std::wstring*>(lParam));
            delete reinterpret_cast<std::wstring*>(lParam);
        } else {
            LogWarn("progress msg with null lParam ignored (msg=" +
                    std::to_string((unsigned)msg) + " wParam=" +
                    std::to_string((unsigned long long)wParam) + ")");
        }
        return 0;
    }
    if (msg == WM_TASK_COMPLETE) {
        OnTaskComplete((int)wParam);
        return 0;
    }
    if (msg == WM_SUPPORT_DONE) {
        OnSupportDone(reinterpret_cast<sysrecover::SupportBundle*>(lParam));
    }
    if (msg == WM_CHECK_RESCUE) {
        ShowRescueFailureNotice();
        ShowMenuRescueNotice();
        ShowSafeBootExitNotice();
        ShowPendingTaskNotice();
    }
    if (msg == WM_STARTUP_LAZY) {
        // 起后台线程做启动后置工作（diag/收集/清理/枚举 —— 可能被慢速 USB2
        // 盘卡住，故绝不能在 InitWindow 里同步跑，见 StartupLazyWork）。
        if (!m_startWorker.joinable())
            m_startWorker = std::thread([this] { StartupLazyWork(); });
        return 0;
    }
    if (msg == WM_PARTS_READY) {
        // 后台枚举完成 → 回主线程填分区下拉（handler 接管指针所有权）。
        auto* disks = reinterpret_cast<std::vector<DiskInfo>*>(lParam);
        if (disks) {
            PopulatePartitions(*disks);
            delete disks;
        }
        return 0;
    }
    if (msg == WM_RETRY_DONE) return OnRetryDone(wParam, lParam);
    if (msg == WM_MENU_DONE) {
        // 装/删菜单 worker 完成（2026-10-18）：wParam=rc, lParam=install(0/1)。
        OnMenuDone((int)wParam, lParam != 0);
        return 0;
    }
    if (msg == WM_MENU_SCAN_DONE) {
        // 「安装到启动」扫描完成（2026-10-28）：wParam=1 表示可在救援层直接还原。
        OnMenuScanDone(wParam != 0);
        return 0;
    }
    if (msg == WM_SKIN_EMPTY_PARTITION) {
        // 分区下拉为空时被点击（2026-10-20）：直接打开发分区向导。
        if (!m_busy && !m_backupMode) OpenInitDiskWizard();
        return 0;
    }
    if (msg == WM_PART_INIT_DONE) return OnInitPartDone(wParam);
    if (msg == WM_FIXBOOT_DONE) return OnFixBootDone(wParam);
    if (msg == WM_VERIFY_DONE) return OnVerifyDone(wParam);
    if (msg == WM_SR_QUERY_BUSY) {
        // 新实例问「你在忙吗」——忙则不让它关掉本进程（见 instance_dlg.cpp）。
        return m_busy ? 1 : 0;
    }
    if (msg == WM_DROPFILES) {
        HandleDroppedFiles(wParam);
        return 0;
    }
    if (msg == WM_DESTROY) {
        // 关键：经典 Duilib 的 WindowImplBase 销毁窗口后**不会**结束消息循环
        // （它只做 m_pm 的清理），必须自己 PostQuitMessage，否则
        // CPaintManagerUI::MessageLoop() 永不返回 → 进程变成「没有窗口但还活着」
        // 的僵尸：任务栏看不见、但单实例互斥量仍被占用，于是下次启动只会走到
        // 「已有程序在运行」流程，且等满 WaitForSingleObject 的 5s 超时才继续。
        // 修复前实测：PostMessage(WM_CLOSE) 后窗口立刻消失、进程 6s 后仍在。
        ::PostQuitMessage(0);
    }
    if (msg == WM_CLOSE && m_busy) {
        // 关闭请求来自别处（新实例的 PostMessage(WM_CLOSE)、Alt+F4、任务栏菜单等）。
        // 任务执行中直接销毁窗口会连 worker 一起杀掉，留下半成品镜像 / 中断的暂存
        // → 不能默默关，也不能只会拒绝：问一次，允许「终止并退出」。
        if (m_cancelling)
            return 0;
        if (!AskBusyClose())
            return 0;            // 「继续等待」：不关窗，任务继续
        CancelAndExit();         // 「终止并退出」：中止 + 清理 + 关窗
        return 0;
    }
    if (msg == WM_NCHITTEST) {
        // 老界面是「无系统外框」的自绘窗口：顶栏（caption 52px）空白处必须
        // 能拖动，而按钮 / Tab / 勾选框 / 下拉 / 输入框必须拿到鼠标消息。
        // 基类 OnNcHitTest 只看 caption 高度，分不清这两者，故在此自行判定：
        // 先看命中的控件是否「可交互」，是 → HTCLIENT；否则落在顶栏 → HTCAPTION。
        POINT pt = {(short)LOWORD(lParam), (short)HIWORD(lParam)};
        ::ScreenToClient(m_hWnd, &pt);
        for (CControlUI* q = m_PaintManager.FindControl(pt); q; q = q->GetParent()) {
            LPCTSTR cls = q->GetClass();
            if (_tcscmp(cls, _T("WinBtn")) == 0 || _tcscmp(cls, _T("TabOption")) == 0 ||
                _tcscmp(cls, _T("GlyphCheck")) == 0 || _tcscmp(cls, _T("Combo")) == 0 ||
                _tcscmp(cls, _T("Edit")) == 0 || _tcscmp(cls, _T("Button")) == 0 ||
                _tcscmp(cls, _T("SkinButton")) == 0 || _tcscmp(cls, _T("SkinEdit")) == 0 ||
                _tcscmp(cls, _T("PartItem")) == 0 || _tcscmp(cls, _T("TextItem")) == 0 ||
                _tcscmp(cls, _T("ListContainerElement")) == 0)
                return HTCLIENT;
        }
        if (pt.y >= 0 && pt.y < 52) return HTCAPTION;
        return HTCLIENT;
    }
    return WindowImplBase::HandleMessage(msg, wParam, lParam);
}

void CMainForm::HandleTooltipMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_MOUSELEAVE) {
        ::SendMessage(m_tipHwnd, TTM_POP, 0, 0);
    }
    if (msg == WM_MOUSEMOVE) {
        POINT pt = {(short)LOWORD(lParam), (short)HIWORD(lParam)};
        CControlUI* pControl = m_PaintManager.FindControl(pt);
        if (pControl && !pControl->GetToolTip().IsEmpty()) {
            m_tipText = pControl->GetToolTip().GetData();
            m_tipInfo.rect = pControl->GetPos();
            m_tipInfo.lpszText = const_cast<LPTSTR>(m_tipText.c_str());
            ::SendMessage(m_tipHwnd, TTM_SETTOOLINFO, 0, (LPARAM)&m_tipInfo);
        } else {
            // 移到无说明区域：清空矩形并收起，避免 TTM_RELAYEVENT 沿用旧矩形再弹。
            m_tipInfo.rect = {0, 0, 0, 0};
            m_tipInfo.lpszText = const_cast<LPTSTR>(_T(""));
            ::SendMessage(m_tipHwnd, TTM_SETTOOLINFO, 0, (LPARAM)&m_tipInfo);
            ::SendMessage(m_tipHwnd, TTM_POP, 0, 0);
        }
        MSG msgStruct = {};
        msgStruct.hwnd = m_hWnd;
        msgStruct.message = msg;
        msgStruct.wParam = wParam;
        msgStruct.lParam = lParam;
        msgStruct.time = ::GetTickCount();
        msgStruct.pt = pt;
        ::SendMessage(m_tipHwnd, TTM_RELAYEVENT, 0, (LPARAM)&msgStruct);
    }
}

LRESULT CMainForm::HandleImagePathEditMessage(UINT msg, WPARAM wParam) {
    if (msg == WM_COMMAND && HIWORD(wParam) == EN_SETFOCUS) {
        // 点击/Tab 进入镜像输入框（还原模式）→ 若上次「搜索」有结果，在输入框
        // 下方弹下拉列表供选择（用户 2026-09-30 规格：点输入框可下拉选择某一个）。
        // 只认 ImagePath 的原生 EDIT：NoteInput 等其它 EDIT 也发 EN_SETFOCUS，
        // 用 GetFocus() 精确比对目标子窗口。
        // ⚠️ 不能在这里直接开菜单（PIT-097）：EN_SETFOCUS 是在原生 EDIT 的
        // WM_LBUTTONDOWN 处理链里同步发出的，紧接着 TrackPopupMenu 会捕获鼠标、
        // 吃掉这次点击的 WM_LBUTTONUP → EDIT 内部卡在"左键按住"状态，表现为
        // "选完镜像文字跟着鼠标拖选、必须再点一下才恢复"。改为投递延迟消息，
        // 在编辑框这次点击完整结束（左键抬起）后再开菜单。
        if (!m_backupMode && !m_busy && !m_pickGuard && !m_searchHits.empty()) {
            CControlUI* pCtl = m_PaintManager.FindControl(_T("ImagePath"));
            HWND hNative =
                pCtl ? static_cast<CSkinEditUI*>(pCtl)->GetNativeEditHWND() : NULL;
            if (hNative && ::GetFocus() == hNative)
                ::PostMessage(m_hWnd, WM_OPEN_PICK_MENU, 0, 0);
        }
        return 0;
    }
    if (msg == WM_OPEN_PICK_MENU) {
        // 「点输入框弹搜索结果下拉」的延迟执行（PIT-097）：必须等这次点击的
        // 左键抬起再开菜单，否则模态菜单吃掉 EDIT 的 WM_LBUTTONUP，EDIT 会
        // 永远停在"按住拖选"状态。等不到就 15ms 后再投一次（菜单打开期间
        // 用户不会一直按着左键，通常一两次就过）。
        if (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) {
            ::Sleep(15);
            ::PostMessage(m_hWnd, WM_OPEN_PICK_MENU, 0, 0);
            return 0;
        }
        if (!m_backupMode && !m_busy && !m_searchHits.empty()) {
            CControlUI* pCtl = m_PaintManager.FindControl(_T("ImagePath"));
            HWND hEdit =
                pCtl ? static_cast<CSkinEditUI*>(pCtl)->GetNativeEditHWND() : NULL;
            // 焦点已移走（用户这会儿点了别处）就不再弹了
            if (hEdit && ::GetFocus() == hEdit) ShowImagePickMenu();
        }
        return 0;
    }
    if (msg == WM_COMMAND && HIWORD(wParam) == EN_CHANGE) {
        // 输入框内容变化 —— 原生 EDIT 的 EN_CHANGE 会送到父窗口（=主窗口，Duilib
        // 控件本身不是窗口）。用户 2026-09-23 反馈：**手动输入**镜像路径时
        // "开始备份"按钮一直是灰的（以前只有"浏览…"/拖入才会刷新按钮）。
        // 这里把当前文本同步进 m_wimPath 并刷新按钮；还原模式下若指向存在的文件，
        // 顺手解析子镜像（用 m_lastLoadedWim 防止每敲一个键就重解析一遍）。
        CControlUI* pEdit = m_PaintManager.FindControl(_T("ImagePath"));
        if (pEdit) {
            // 手动输入/粘贴的内容在**原生 EDIT 子窗口**里，控件的 GetText() 可能
            // 还是旧的 → 直接读子窗口（双保险，配合 CSkinEditUI::SyncTextFromNative）。
            std::wstring p;
            HWND hNative =
                static_cast<CSkinEditUI*>(pEdit)->GetNativeEditHWND();
            if (hNative) {
                int n = ::GetWindowTextLengthW(hNative);
                if (n > 0) {
                    std::wstring buf(static_cast<size_t>(n) + 1, L'\0');
                    ::GetWindowTextW(hNative, &buf[0], n + 1);
                    buf.resize(static_cast<size_t>(n));
                    p = buf;
                }
            }
            if (p.empty())
                p = pEdit->GetText().GetData();
            if (p.empty() && !hNative)
                return 0;
            std::wstring& active = ActivePath();
            if (p != active) {
                active = p;
                if (!m_backupMode && !active.empty() &&
                    active != m_lastLoadedWim && ImagePathReady(active)) {
                    m_lastLoadedWim = active;
                    LoadWimImages(active);
                }
                SyncFormatFromPath();  // 备份：路径带 .wim/.esd 时自动定格
                UpdateMainAction();
            }
        }
        return 0;
    }
    return 0;
}

LRESULT CMainForm::HandleTimerMessage(WPARAM wParam) {
    if (wParam == 2) {
        // 兜底同步：不管路径是手打/粘贴/拖入/浏览进来的，每 500ms 回读一次原生 EDIT，
        // 有变化就更新 m_wimPath、必要时解析镜像、刷新主按钮。只靠 EN_CHANGE 不够稳
        // （焦点/时序/事件路径都可能漏，用户 2026-09-23 实测按钮仍不亮）。
        SyncImagePathFromUi();
        return 0;
    }
    if (wParam == 1) {
        if (m_busy)
            SetStatus(m_lastStage + Tr(L"    已用 ") + ElapsedText());
        return 0;
    }
    if (wParam == 3) {
        if (m_postBusy) {
            m_postPulse = (m_postPulse + 5) % 100;
            CRoundProgressUI* p = static_cast<CRoundProgressUI*>(
                m_PaintManager.FindControl(_T("PostProgress")));
            if (p) p->SetValue(m_postPulse);
        }
        return 0;
    }
    return 0;
}

LRESULT CMainForm::OnRetryDone(WPARAM wParam, LPARAM lParam) {
    // P3（PIT-148）一键重试完成：成功且需重启 → 自动 shutdown（与正常
    // 还原暂存一致，PIT-110）；失败 → 弹错误（静默模式也弹，fail-safe）。
    m_busy = false;
    KillTimer(m_hWnd, 1);
    ShowProgress(false);
    UpdateMainAction();
    int rc = (int)wParam;
    bool reboot = lParam != 0;
    if (rc == 0) {
        SetStatus(reboot ? Tr(L"重试暂存完成，正在重启...")
                         : Tr(L"重试完成，无需重启"));
        if (reboot)
            RebootNow();
        else
            MessageBoxW(m_hWnd, Tr(L"重试完成。重启后即可进入恢复的系统。"),
                        Tr(L"一键重试"), MB_OK | MB_ICONINFORMATION);
    } else {
        std::wstring e = m_retryErr.empty() ? Tr(L"重试失败（详见日志）")
                                            : U2W(m_retryErr);
        SetStatus(Tr(L"重试失败"));
        MessageBoxW(m_hWnd, e.c_str(), Tr(L"一键重试"),
                    MB_OK | MB_ICONERROR);
    }
    return 0;
}

LRESULT CMainForm::OnInitPartDone(WPARAM wParam) {
    m_busy = false;
    KillTimer(m_hWnd, 1);
    ShowProgress(false);
    if (m_worker.joinable()) m_worker.join();
    const int rc = (int)wParam;
    if (rc == 0) {
        PopulatePartitions(EnumerateDisks());
        SetStatus(Tr(L"新硬盘分区已创建，请选择镜像后开始还原"));
        MessageBoxW(m_hWnd,
                    Tr(L"分区已创建完成。\n请选择系统镜像，再选择新的系统分区开始还原。"),
                    Tr(L"创建分区"), MB_OK | MB_ICONINFORMATION);
    } else {
        SetStatus(Tr(L"创建分区失败"));
        MessageBoxW(m_hWnd,
                    m_initErr.empty() ? Tr(L"创建分区失败，请打开日志查看详情。")
                                      : m_initErr.c_str(),
                    Tr(L"创建分区"), MB_OK | MB_ICONERROR);
    }
    UpdateMainAction();
    return 0;
}

LRESULT CMainForm::OnFixBootDone(WPARAM wParam) {
    m_busy = false;
    KillTimer(m_hWnd, 1);
    ShowProgress(false);
    if (m_worker.joinable()) m_worker.join();
    const int rc = (int)wParam;
    LogInfo("GUI repair-boot done rc=" + std::to_string(rc) + " / " + m_fixMsg);
    SetStatus(rc == 0 ? Tr(L"引导已修复") : Tr(L"修复引导未完全成功"));
    // 2026-10-28（用户规格）：**被安全软件拦写 → 弹框让用户退出防护后点「重试」**，
    // 不再让用户自己猜"为什么没修好"。重试直接开跑（不再重复确认框）。
    if (m_fixBlocked &&
        CConfirmDlg::Ask2(
            m_hWnd, Tr(L"修复引导未完成"),
            Tr(L"写入启动项/引导区被安全软件拦截\n（如火绒/360 的「写磁盘保留扇区」）。\n请退出该防护或把本程序加入信任，再点「重试」。"),
            Tr(L"关闭"), Tr(L"重试"), /*defaultIsRight=*/true) == 1) {
        UpdateMainAction();
        StartRepairBoot(/*confirm=*/false);
        return 0;
    }
    std::wstring text = m_fixMsg.empty() ? Tr(L"修复引导失败，请打开日志查看详情。")
                                         : U2W(m_fixMsg);
    MessageBoxW(m_hWnd, text.c_str(), Tr(L"修复引导"),
                rc == 0 ? (MB_OK | MB_ICONINFORMATION)
                        : (MB_OK | MB_ICONWARNING));
    UpdateMainAction();
    return 0;
}

LRESULT CMainForm::OnVerifyDone(WPARAM wParam) {
    // 「镜像检查」worker 完成（2026-10-27）：wParam=rc(0=通过)。
    m_busy = false;
    KillTimer(m_hWnd, 1);
    ShowProgress(false);
    if (m_worker.joinable()) m_worker.join();
    const int rc = (int)wParam;
    if (m_cancel) {
        SetStatus(Tr(L"镜像检查已取消"));
        UpdateMainAction();
        return 0;
    }
    if (rc == 0) {
        SetStatus(Tr(L"镜像校验通过"));
        MessageBoxW(m_hWnd, Tr(L"镜像校验通过：文件完整，可以放心还原。"),
                    Tr(L"镜像检查"), MB_OK | MB_ICONINFORMATION);
    } else {
        SetStatus(Tr(L"镜像校验失败"));
        std::wstring e = m_verifyMsg.empty() ? Tr(L"镜像校验失败，请打开日志查看详情。")
                                             : U2W(m_verifyMsg);
        MessageBoxW(m_hWnd, e.c_str(), Tr(L"镜像检查"),
                    MB_OK | MB_ICONERROR);
    }
    UpdateMainAction();
    return 0;
}

// 自绘控件注入点：未知 UIClass 由 UIDlgBuilder 回调到这里（见 ui_skin.h 头部注释）
CControlUI* CMainForm::CreateControl(LPCTSTR pstrClass) {
    return CreateSkinControl(pstrClass);
}

// wimlib 阶段名 → 中文（进度条左侧显示，用户要求别出现英文 "write"）
// 拆分后 image 段也要用，故改为 class static（声明在 main_form.h）。
std::wstring CMainForm::StageCn(const std::wstring& s) {
    if (s.compare(0, 5, L"write") == 0) return Tr(L"写入");
    if (s.compare(0, 7, L"extract") == 0) return Tr(L"写入");
    if (s.compare(0, 4, L"scan") == 0)
        return s.size() > 5 ? Tr(L"扫描 ") + s.substr(5) : Tr(L"扫描");
    // 收官阶段（PIT-098）：100% 后的静默期靠这些名字在状态栏"报活"。
    // 都按前缀匹配 —— 带速度后缀（"  42.3 MB/s  ETA 1:23"）也能命中。
    if (s.compare(0, 3, L"esp") == 0) return Tr(L"备份ESP");
    if (s.compare(0, 6, L"verify") == 0) return Tr(L"校验镜像");
    if (s.compare(0, 5, L"probe") == 0) return Tr(L"完整性检查");
    if (s.compare(0, 7, L"ea-scan") == 0) return Tr(L"文件属性检查");
    // 异机还原（0.9.33）：apply 100% 后的驱动注入阶段，状态栏必须继续报活。
    if (s.compare(0, 9, L"universal") == 0) {
        if (s.find(L"inbox-done") != std::wstring::npos)
            return Tr(L"内置驱动已启用");
        if (s.find(L"inbox") != std::wstring::npos)
            return Tr(L"正在启用内置存储驱动");
        if (s.find(L"drivers") != std::wstring::npos)
            return Tr(L"正在注入驱动包");
        if (s.find(L"done") != std::wstring::npos)
            return Tr(L"驱动注入完成");
        return Tr(L"正在准备异机还原");
    }
    // 引导层部署（2026-10-18）：ops 在 apply 后上报的收官阶段（暂存与菜单
    // 安装都经过；菜单安装的"无响应窗"正是发生在这里的 28MB 载荷写入）。
    if (s.compare(0, 4, L"boot") == 0) return Tr(L"写入引导文件");
    // 新硬盘分区向导（2026-10-27 用户规格：每一步都要看得见，不能只写一句
    // "正在创建分区"——diskpart 建分区+格式化能跑几十秒）。阶段名来自
    // partscript.cpp：每一步**一次独立 diskpart 调用**，见 BuildDiskpartSteps。
    // 状态栏可用宽度只有约 179px（进度条一出现就收窄），所以每条压到 6~8 字。
    if (s.compare(0, 8, L"partinit") == 0) {
        if (s.find(L"letters") != std::wstring::npos) return Tr(L"正在分配盘符");
        if (s.find(L"script") != std::wstring::npos) return Tr(L"正在生成脚本");
        if (s.find(L"clean") != std::wstring::npos) return Tr(L"正在清除旧分区");
        if (s.find(L"convert") != std::wstring::npos) return Tr(L"正在转换分区表");
        if (s.find(L"esp") != std::wstring::npos) return Tr(L"正在创建ESP");
        if (s.find(L"msr") != std::wstring::npos) return Tr(L"正在创建MSR");
        if (s.find(L"system") != std::wstring::npos) return Tr(L"正在创建系统盘");
        if (s.find(L"data") != std::wstring::npos) return Tr(L"正在创建数据盘");
        if (s.find(L"bootsect") != std::wstring::npos) return Tr(L"正在写入引导代码");
        if (s.find(L"verify") != std::wstring::npos) return Tr(L"正在校验分区");
        if (s.find(L"done") != std::wstring::npos) return Tr(L"分区创建完成");
        return Tr(L"正在创建分区");
    }
    return s;
}

void CMainForm::OnProgressUpdate(int pct, const std::wstring& stage) {
    // 进度条随**第一条进度**出现（不是点按钮那一刻）：校验/暂存阶段没有进度，
    // 整行状态栏留给状态文字；真开始跑进度了，文字才收在进度条左边。
    ShowProgress(true);
    const bool universal =
        stage.compare(0, 9, L"universal") == 0;
    if (universal) {
        // 主进度已走完（100%），这里不再把它打回 0；驱动注入耗时另用
        // PostProgress 小进度条循环提示"仍在工作"。
        SetProgress(100);
        const bool done = stage.find(L"done") != std::wstring::npos;
        ShowPostProgress(!done);
    } else {
        ShowPostProgress(false);
        SetProgress(pct);
    }
    m_lastStage = StageCn(stage);
    SetStatus(m_lastStage + Tr(L"    已用 ") + ElapsedText());
    CControlUI* pPct = m_PaintManager.FindControl(_T("PercentText"));
    if (pPct) {
        wchar_t b[16];
        swprintf(b, 16, L"%d%%", universal ? 100 : pct);
        pPct->SetText(b);
    }
}

std::wstring CMainForm::ElapsedText() const {
    auto s = std::chrono::duration_cast<std::chrono::seconds>(
                 std::chrono::steady_clock::now() - m_start)
                 .count();
    wchar_t b[40];
    if (s >= 3600)
        swprintf(b, 40, L"%lld:%02lld:%02lld", (long long)(s / 3600),
                 (long long)((s % 3600) / 60), (long long)(s % 60));
    else
        swprintf(b, 40, L"%02lld:%02lld", (long long)(s / 60),
                 (long long)(s % 60));
    return b;
}

void CMainForm::OnTaskComplete(int rc) {
    KillTimer(m_hWnd, 1);
    m_busy = false;
    if (m_cancel) {
        // 用户选了「终止并退出」：不要再弹任何"失败"提示（窗口马上要关掉），
        // 只留一条日志；半成品镜像由 CancelAndExit → CleanupIncompleteOutput 清。
        LogInfo("task aborted by user (cancel-and-exit)");
        ShowProgress(false);  // 收起进度条，状态栏回到整行可用
        return;
    }
    // 任务结束立刻收起进度条：完成/失败消息（英文可达 453px）需要整行，
    // 而进度条一在，状态文字就被卡在 179px 里。
    ShowProgress(false);
    SetProgress(rc == 0 ? 100 : 0);
    CControlUI* pPct = m_PaintManager.FindControl(_T("PercentText"));
    if (pPct) pPct->SetText(rc == 0 ? _T("100%") : _T(""));
    UpdateMainAction();
    if (rc == 4) {
        SetStatus(U2W(last_err_));
        MessageBoxW(m_hWnd, U2W(last_err_).c_str(), Tr(L"安全门禁拒绝"), MB_OK | MB_ICONERROR);
    } else if (rc != 0) {
        // P8：把"下一步怎么办"一起给用户（空间不足/坏镜像等都有对应建议）。
        // M1（PLAN §14）：按**建议码**取词，不再匹配消息文本（消息一翻译关键词就失配）。
        std::string advice = sysrecover::ErrorAdvice(rc, last_adv_);
        std::wstring box = U2W(last_err_) + U2W(advice);
        SetStatus(U2W(last_err_));
        MessageBoxW(m_hWnd, box.c_str(),
                    m_backupMode ? Tr(L"备份失败") : Tr(L"暂存失败"), MB_OK | MB_ICONERROR);
    } else if (m_backupMode) {
        m_imageOk = true;  // 备份完成即写盘有效；切到还原页后按钮可以点亮
        // 增量备份：补一句"现在是第 N 个时间点，合计 X"（用户 2026-10-28 规格）。
        std::wstring tp;
        if (m_workerAppend && m_backupTimepoints > 0) {
            wchar_t buf[256] = {};
            swprintf(buf, 256,
                     Tr(L"\n现在是第 %d 个时间点，合计 %.1f GB。"),
                     m_backupTimepoints,
                     static_cast<double>(m_backupTotalBytes) / kGiB);
            tp = buf;
        }
        if (!m_skippedFiles.empty()) {
            const int n = static_cast<int>(m_skippedFiles.size());
            wchar_t head[256];
            swprintf(head, 256,
                     Tr(L"备份已完成，但有 %d 个文件未能读取，未写入镜像：\n\n"),
                     n);
            std::wstring msg = head;
            const int shown = n < 20 ? n : 20;
            for (int i = 0; i < shown; ++i) {
                msg += m_skippedFiles[static_cast<size_t>(i)];
                msg += L"\n";
            }
            if (n > shown) {
                wchar_t more[256];
                swprintf(more, 256,
                         Tr(L"\n… 另有 %d 个文件，完整清单见日志：\n"),
                         n - shown);
                msg += more;
            } else {
                msg += Tr(L"\n完整清单见日志：\n");
            }
            msg += LogBaseDir() + L"\\logs";
            // L1：固定文本 + 数值拼接（不再把译文当格式串）。
            const std::wstring status =
                Tr(L"备份完成，但有 ") + std::to_wstring(n) +
                Tr(L" 个文件未备份");
            SetStatus(status + tp);
            MessageBoxW(m_hWnd, (msg + tp).c_str(), Tr(L"备份不完整"),
                        MB_OK | MB_ICONWARNING);
        } else {
            SetStatus(Tr(L"备份完成，用时 ") + ElapsedText() + Tr(L"。"));
            if (!IsSilent())
                MessageBoxW(m_hWnd,
                            (Tr(L"系统备份已完成。\n用时 ") + ElapsedText() +
                             Tr(L"秒") + tp)
                                .c_str(),
                            Tr(L"备份成功"), MB_OK | MB_ICONINFORMATION);
        }
    } else if (m_needReboot) {
        // 需要重启（还原运行中的系统盘）：静默或用户已选「退出并重启」→ 直接重启
        SetStatus(Tr(L"暂存完成（用时 ") + ElapsedText() + Tr(L"），正在重启..."));
        RebootNow();
    } else {
        // 就地还原完成（PE 里 / 还原到非系统盘）：不重启，直接报完成。
        // ⚠️ 文案（用户 2026-09-23 反馈）：原来写"无需重启"，用户会理解成
        // "现在就能用还原好的系统了" ✗ —— 其实必须**重启**才能进入还原的系统
        // （我们只是没有自动重启而已）。所以明确写"重启后即可进入"。
        SetStatus(Tr(L"还原完成（用时 ") + ElapsedText() + Tr(L"）"));
        // 用户 2026-11-02 规格：完成要给一个「确定 / 立即重启」；**静默**
        //（含还原中途才勾上的）→ 直接自动重启，不再多问一句。
        if (IsSilent()) {
            LogInfo("restore done: silent -> auto reboot");
            SetStatus(Tr(L"还原完成，正在重启..."));
            RebootNow();
        } else {
            std::wstring doneMsg =
                Tr(L"系统还原已完成，用时 ") + ElapsedText() +
                Tr(L"。\n重启后即可进入恢复的系统。");
            // H4（1.0）：全量校验默认关闭 —— 结果页必须明示"镜像未校验"。
            if (!m_restoreVerifyImage)
                doneMsg += Tr(L"\n本次未对镜像做全量校验（仅做了毫秒级结构预检）。");
            // 左=确定（安全项，回车/ESC 都落这里）、右=立即重启
            if (CConfirmDlg::Ask2(m_hWnd, Tr(L"还原成功"), doneMsg,
                                  Tr(L"确定"), Tr(L"立即重启"),
                                  /*defaultIsRight=*/false) == 1) {
                LogInfo("restore done: user chose reboot now");
                RebootNow();
            }
        }
        // Q1 settle 警告（2026-10-18）：程序目录在目标盘上但没找到安全分区
        // → 还原后程序可能被格式化掉。弹框让用户知道（fail-safe，静默模式也弹）。
        {
            const auto& sr = sysrecover::LastSettleResult();
            if (sr.noSafeDrive)
                MessageBoxW(m_hWnd,
                            Tr(L"警告：程序目录在目标盘上且无法安全搬迁——还原后程序可能丢失。"),
                            Tr(L"警告"), MB_OK | MB_ICONWARNING);
        }
    }
}
