// 拆分自 src/gui/main_form.cpp（按职责）：模式切换 / 可用态 / UI 辅助 / 重启。
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

// ────────────────── 模式切换 / 可用态 ──────────────────

void CMainForm::SetImagePathEdit(const std::wstring& p) {
    CControlUI* pEdit = m_PaintManager.FindControl(_T("ImagePath"));
    if (pEdit) pEdit->SetText(p.c_str());
}

void CMainForm::SetBusyControls(bool busy) {
    auto enable = [this, busy](const TCHAR* name, bool keep) {
        CControlUI* p = m_PaintManager.FindControl(name);
        if (p) p->SetEnabled(!busy || keep);
    };
    enable(_T("Silent"), true);
    enable(_T("CpuCapBox"), true);
    enable(_T("BrowseBtn"), false);
    enable(_T("SearchBtn"), false);
    enable(_T("ImagePath"), false);
    enable(_T("ImageIndexBox"), false);
    enable(_T("NoteInput"), false);
    enable(_T("PartitionBox"), false);
    enable(_T("FormatBox"), false);
    enable(_T("ModeRestore"), false);
    enable(_T("ModeBackup"), false);
    // 「更多功能>>」是一个按钮（内部四项：异机还原/修复引导/清除启动中还原菜单/
    // 镜像检查）——任务执行中整组禁用，避免同一时刻两路写盘（2026-10-27）。
    enable(_T("MoreBtn"), false);
    enable(_T("FixBootBtn"), false);
    enable(_T("LogBtn"), false);
    enable(_T("BootMenuBtn"), false);
    enable(_T("EspChk"), false);
    enable(_T("Universal"), false);
    enable(_T("SiteLink"), false);
    enable(_T("MainAction"), false);
    CControlUI* pEdit = m_PaintManager.FindControl(_T("ImagePath"));
    if (pEdit) {
        HWND h = static_cast<CSkinEditUI*>(pEdit)->GetNativeEditHWND();
        if (h) ::EnableWindow(h, busy ? FALSE : TRUE);
    }
}

void CMainForm::ApplyModeUi() {
    auto Vis = [this](const TCHAR* name, bool vis) {
        CControlUI* p = m_PaintManager.FindControl(name);
        if (p) p->SetVisible(vis);
    };
    auto Text = [this](const TCHAR* name, const TCHAR* t) {
        CControlUI* p = m_PaintManager.FindControl(name);
        if (p) p->SetText(t);
    };
    auto Pos = [this](const TCHAR* name, int l, int t, int r, int b) {
        CControlUI* p = m_PaintManager.FindControl(name);
        if (p) { RECT rc = { l, t, r, b }; p->SetPos(rc); }
    };
    if (m_backupMode) {
        // 2026-10-11 用户规格：去掉「…」——还原页同位按钮（浏览系统镜像）本来
        // 就没有省略号，两页文字都是完整显示的，策略保持一致。
        Text(_T("BrowseBtn"),   Tr(L"浏览保存位置"));
        Vis(_T("SearchBtn"),    false);  // 搜索是还原专属（2026-09-30 用户规格）
        // 「ESP」勾选占用搜索按钮在第一步卡片里的同一位置（备份专属，2026-09-30）
        Vis(_T("EspChk"),       true);
        Text(_T("MainAction"),  Tr(L"开始备份"));
        Text(_T("NoteLabel"),   Tr(L"备份备注："));
        Vis(_T("ImageIndexBox"), false);
        Vis(_T("NoteText"),     false);
        Vis(_T("NoteInput"),    true);
        // 「格式：」文字标签已由「类型」下拉自身显示（collapsedtext），不再需要
        Vis(_T("FormatLabel"),  false);
        Vis(_T("FormatBox"),    true);
        // CPU 下拉同理：**必须先显示再选默认项** —— CComboUI::SelectItem 在
        // 条目 IsVisible()==false 时直接返回 false（容器隐藏会把子控件的
        // internVisible 一起压掉），先选后显示的话收起框会是空白的。
        Vis(_T("CpuCapBox"),    true);
        // FormatBox 的条目是 XML 里静态写的，首次显示前没有任何选中项 →
        // 收起框空白（老界面里显示「.esd（高压比省空间）」）。仅在未选中时补默认值，
        // 这样用户在两个模式间来回切换时不会丢失已选格式。
        {
            CComboUI* pFmt = static_cast<CComboUI*>(
                m_PaintManager.FindControl(_T("FormatBox")));
            if (pFmt && pFmt->GetCurSel() < 0 && pFmt->GetCount() > 0)
                pFmt->SelectItem(0, false, false);  // 程序选择：不触发 SELECTCHANGED/ITEMSELECT
            CComboUI* pCpu = static_cast<CComboUI*>(
                m_PaintManager.FindControl(_T("CpuCapBox")));
            if (pCpu && pCpu->GetCurSel() < 0 && pCpu->GetCount() > 0)
                pCpu->SelectItem(0);
            SyncFormatFromPath();  // 路径里已带 .wim/.esd 时按后缀定格式
        }
        // 镜像信息（用户规格 2026-09-21）：在文件名下方给出「日期 + 系统类型 + 备份」，
        // 例如 `20260921 Windows 10 IoT 企业版 LTSC 21H2 19044.4046 备份`。
        // 只在用户尚未填写时预填，不覆盖手输内容；这个值同时会成为 WIM 里的子镜像名。
        {
            CControlUI* pNote = m_PaintManager.FindControl(_T("NoteInput"));
            if (pNote && pNote->GetText().IsEmpty() && !m_sysDesc.full.empty()) {
                std::wstring info =
                    TimestampDate() + L" " + m_sysDesc.full + Tr(L" 备份");
                pNote->SetText(info.c_str());
            }
        }
        // 备份模式不给这些还原专属按钮（2026-09-28 / 2026-10-27 规格）：
        // 「安装到启动」只在还原模式出现；「打包日志」也在还原模式的「更多功能>>」菜单里；
        // 「更多功能>>」**两模式都要**（2026-10-29 用户规格：加密备份/增量备份都在这个菜单里）。
        Vis(_T("FixBootBtn"),   false);  // 皮肤已隐藏；入口在「更多功能>>」
        Vis(_T("Universal"), false);  // 异机还原是还原模式专属（备份模式隐藏）
        Vis(_T("MoreBtn"),      true);
        Vis(_T("BootMenuBtn"),  false);
        Vis(_T("LogBtn"),       false);  // 入口已收进还原模式的「更多功能>>」菜单
        Text(_T("PartTitle"),   Tr(L"备份源分区"));
        Text(_T("PartHint"),    Tr(L"选择要备份为镜像的源分区（移动盘已隐藏）"));
        // 第三步右侧：备份模式 = 静默模式 + CPU 下拉；引导按钮两模式共用同一位置（XML 定）。
        // ★ 2026-09-30 二次调整（用户规格）：「类型」下拉移到**第一步「浏览」右侧**
        //   （与「搜索」按钮同位置同尺寸 639,77,710,115），ESP 勾选换到第三步原
        //   「格式：」处（388,296,510,329）。第三步右侧一组：
        //   静默 248..340 / ESP 388..510 / CPU 下拉 530..612 / 菜单 618..706
        Pos(_T("Silent"),        248, 302, 330, 326);
        Pos(_T("FormatBox"),     639, 77, 710, 115);   // 第一步：类型下拉
        // 2026-10-29 用户规格：两模式第三步右侧**右边界对齐**（更多功能永远 570..721）。
        // 备份模式：静默 248..330 / ESP 338..456 / CPU 464..562 / 更多 570..721
        //（与还原模式的 340..438、446..562 同一条 562 分界，视觉上数量/位置都相仿）。
        Pos(_T("EspChk"),        338, 298, 456, 331);  // 第三步：ESP 勾选
        Pos(_T("CpuCapBox"),     464, 298, 562, 331);
        Pos(_T("MoreBtn"),       570, 298, 721, 331);
        // 注：状态栏在空闲时显示**当前模式说明**（见本函数末尾），任务执行中
        // 显示进度文字；进度条只在跑进度时出现（ShowProgress）。
    } else {
        Text(_T("BrowseBtn"),   Tr(L"浏览系统镜像"));
        Vis(_T("SearchBtn"),    true);
        Vis(_T("EspChk"),       false);  // 还原模式没有 ESP 勾选（ESP 子镜像由救援层自动处理）
        Text(_T("MainAction"),  Tr(L"开始恢复"));
        Text(_T("NoteLabel"),   Tr(L"镜像说明："));
        Vis(_T("ImageIndexBox"), true);
        Vis(_T("NoteText"),     true);
        Vis(_T("NoteInput"),    false);
        Vis(_T("FormatLabel"),  false);
        Vis(_T("FormatBox"),    false);
        Vis(_T("CpuCapBox"),    false);
        // 「修复引导」仍隐藏（入口在「更多功能>>」）；「异机还原」勾选框常驻第三步
        // （2026-11-05 用户规格：与「打包日志」位置对调，后者收进菜单）。
        Vis(_T("FixBootBtn"),   false);
        Vis(_T("Universal"),    true);
        Vis(_T("MoreBtn"),      true);
        Vis(_T("BootMenuBtn"),  true);
        Vis(_T("LogBtn"),       false);  // 打包日志已收进「更多功能>>」菜单
        Text(_T("PartTitle"),   Tr(L"系统安装位置"));
        Text(_T("PartHint"),    Tr(L"选择要安装恢复镜像的目标分区（移动盘已隐藏）"));
        // 还原模式几何（切回时必须复位第三步右侧那几个）。2026-11-05 用户规格：
        //   静默 248..330 / 异机还原 340..438 / 安装到启动 446..562 / 更多功能>> 570..721
        // （与备份模式的 338..456、464..562 同一条 562 分界；`FixBootBtn` 仍隐藏不占位。）
        Pos(_T("Silent"),        248, 302, 330, 326);
        Pos(_T("Universal"),     340, 298, 438, 331);
        Pos(_T("BootMenuBtn"),   446, 298, 562, 331);
        Pos(_T("MoreBtn"),       570, 298, 721, 331);
        Pos(_T("FormatLabel"),   350, 302, 388, 326);
        Pos(_T("FormatBox"),     388, 296, 510, 329);
    }
    UpdateMainAction();
    // 空闲时状态栏 = **当前模式说明**（用户 2026-09-27 规格）：
    //   · 鼠标悬停走 tooltip，键盘/不悬停的人看这一行 —— 两层解释并存；
    //   · 只在切 Tab 时写一次，切到另一个模式自然被新的一条替换；
    //   · 期间任何 SetStatus（选文件/报错/进度）都会盖掉它，再切回来又恢复
    //     —— 没有定时器、没有“谁优先”，正好是规格里“切换即替换”的语义。
    // 放在 UpdateMainAction() **之后**：那里面在按钮不可用时会写
    // 「请先选择镜像文件」之类的提示，会把说明行冲掉。
    SetImagePathEdit(ActivePath());
    SetIdleStatus();
}

// 空闲态状态栏：中文/英文只此一处，切 Tab 与"镜像加载成功"共用。
// 解决 2026-11-06 实机缺陷：先输错路径（状态栏写"镜像不可用"），再选中合法镜像时
// 成功路径不写状态栏 → 错误文案一直挂着，用户会以为镜像仍不可用。
void CMainForm::SetIdleStatus() {
    SetStatus(Tr(m_backupMode
                     ? L"把当前系统热备份为镜像文件（VSS 快照，可在运行中备份）"
                     : L"选择镜像与目标分区后开始；还原系统盘会重启进恢复环境，非系统盘就地还原"));
}

void CMainForm::UpdateMainAction() {
    SetBusyControls(m_busy);
    CButtonUI* p =
        static_cast<CButtonUI*>(m_PaintManager.FindControl(_T("MainAction")));
    if (!p) return;
    // 还原模式同样需要先选好分区；备份模式还要选源分区，主按钮才转蓝；
    // 还原模式要求 m_imageOk，否则按钮保持灰色（避免用户拿不可读的镜像去还原）。
    std::wstring& active = ActivePath();
    bool ok = !active.empty() && m_selPart >= 0 && !m_busy &&
              (m_backupMode || m_imageOk);
    p->SetEnabled(ok);
    // 经典 Duilib 的 CButtonUI 没有状态图时 DoPaint 继承自 CControlUI，
    // 只会画 bkcolor/border → 可用态必须在代码里换底色和文字色（老界面：
    // 可用 = 蓝底白字 #8CA0DE，禁用 = 灰底灰字 #E2E6EE/#9AA3B2）
    p->SetBkColor(ok ? ui_skin::C_PRIMARY : ui_skin::C_DISABLED);
    p->SetTextColor(ok ? 0xFFFFFFFF : ui_skin::C_DIS_TEXT);
    p->Invalidate();
    // 按钮为什么是灰的？在状态栏说清楚（用户 2026-09-23：手输路径后按钮不亮，
    // 完全无从判断缺哪一步）。只在空闲且确实不可用时提示，避免覆盖任务中的状态。
    if (!ok && !m_busy) {
        if (active.empty())
            SetStatus(m_backupMode ? Tr(L"请先选择保存位置") : Tr(L"请先选择镜像文件"));
        else if (m_selPart < 0)
            SetStatus(m_backupMode ? Tr(L"请选择第二步的源分区")
                                   : Tr(L"请选择第二步的目标分区"));
        else if (!m_backupMode && !m_imageOk)
            SetStatus(Tr(L"镜像不可用（解析失败或文件不存在）"));
    }
    // 「安装菜单」按钮的状态随"是否已选镜像/目标分区"变（用户 2026-09-29 规格：
    // 没选镜像时置灰）；已安装时它显示「删除菜单」并说明绑定关系。
    RefreshBootMenuBtn();
}

// ────────────────── UI 辅助 ──────────────────

// 从界面回读镜像路径（原生 EDIT 才是"真身"，见 PIT-077），同步状态并刷新按钮。
void CMainForm::SyncImagePathFromUi() {
    CControlUI* pEdit = m_PaintManager.FindControl(_T("ImagePath"));
    if (!pEdit)
        return;
    std::wstring p;
    HWND hNative = static_cast<CSkinEditUI*>(pEdit)->GetNativeEditHWND();
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
    std::wstring& active = ActivePath();
    if (p.empty() && !hNative) {
        if (!active.empty() && pEdit->GetText().IsEmpty())
            SetImagePathEdit(active);
        return;
    }
    if (p == active)
        return;
    active = p;
    if (!m_backupMode && !active.empty() && active != m_lastLoadedWim &&
        ImagePathReady(active)) {
        m_lastLoadedWim = active;
        LoadWimImages(active);
    }
    SyncFormatFromPath();  // 备份：路径带 .wim/.esd 时自动定格
    UpdateMainAction();
}

bool CMainForm::RebootNow() {
    // PIT-214（用户 2026-11-03 实测）：**WinPE 里 ExitWindowsEx 经常"返回成功但不重启"**。
    // PE 优先 wpeutil reboot（官方命令）；正常系统仍走 ExitWindowsEx（不弹"一分钟"提示）；
    // 都不行再 shutdown /r /t 0 /f。全失败时**必须让用户看见**，不能像以前那样静默。
    bool ok = false;
    std::string detail;
    if (sysrecover::IsWinPE()) {
        std::string out;
        const int rc = RunProcess(SysToolPath(L"wpeutil.exe"), L"reboot", out);
        detail += "wpeutil rc=" + std::to_string(rc) + " (" + out + "); ";
        ok = (rc == 0);
    }
    if (!ok) {
        // 正常系统（或 PE 里 wpeutil 不可用）：ExitWindowsEx
        HANDLE hToken = nullptr;
        if (::OpenProcessToken(::GetCurrentProcess(),
                               TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
            TOKEN_PRIVILEGES tp = {};
            tp.PrivilegeCount = 1;
            if (::LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME,
                                        &tp.Privileges[0].Luid)) {
                tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                ::AdjustTokenPrivileges(hToken, FALSE, &tp, 0, nullptr, nullptr);
            }
            ::CloseHandle(hToken);
        }
        ::SetLastError(0);
        const BOOL ewe = ::ExitWindowsEx(EWX_REBOOT | EWX_FORCEIFHUNG,
                                         SHTDN_REASON_MAJOR_APPLICATION |
                                             SHTDN_REASON_MINOR_MAINTENANCE |
                                             SHTDN_REASON_FLAG_PLANNED);
        detail += "ExitWindowsEx=" + std::to_string((int)ewe) +
                  " err=" + std::to_string(::GetLastError()) + "; ";
        ok = ewe != FALSE;
    }
    if (!ok) {
        std::string out;
        const int rc = RunProcess(SysToolPath(L"shutdown.exe"),
                                  L"/r /t 0 /f", out);
        detail += "shutdown rc=" + std::to_string(rc) + " (" + out + ")";
        ok = (rc == 0);
    }
    if (ok) {
        LogInfo("reboot: initiated; " + detail);
        return true;
    }
    LogError("reboot: ALL METHODS FAILED; " + detail);
    MessageBoxW(m_hWnd,
                Tr(L"自动重启没有成功。\n请手动重启电脑（开始菜单 → 重启）。"),
                Tr(L"需要重启"), MB_OK | MB_ICONWARNING);
    return false;
}

bool CMainForm::IsSilent() {
    COptionUI* p =
        static_cast<COptionUI*>(m_PaintManager.FindControl(_T("Silent")));
    return p && p->IsSelected();
}

bool CMainForm::IsEspChecked() {
    COptionUI* p =
        static_cast<COptionUI*>(m_PaintManager.FindControl(_T("EspChk")));
    return p && p->IsSelected();
}

// 状态文字的可用宽度：进度条只在任务跑进度时出现（见 ShowProgress），
// 静止时可以一直写到右下角「讨论」链接前面（约 638px）；进度条一出现就
// 必须收在它左边（179px）。
// 超宽一律「掐头 + … + 留尾」—— 尾部多是时间 / 文件名 / 错误码，信息价值
// 最高；这样任意长的路径和英文长句都不会压到进度条上（英文状态消息最长
// 453px，是中文的 1.5 倍，以前直接画穿进度条）。
static std::wstring FitStatusLine(CPaintManagerUI& pm, const std::wstring& s) {
    CControlUI* pStatus = pm.FindControl(_T("StatusText"));
    if (!pStatus) return s;
    RECT rc = pStatus->GetPos();
    int right = rc.right;
    CControlUI* pProg = pm.FindControl(_T("Progress"));
    if (pProg && pProg->IsVisible()) {
        right = pProg->GetPos().left - 4;
    } else {
        CControlUI* pLink = pm.FindControl(_T("SiteLink"));
        if (pLink) right = pLink->GetPos().left - 8;
    }
    int avail = right - rc.left;
    if (avail <= 16) return s;
    const int px = 13;  // StatusText 的 fontsize（skin/main.xml）
    HDC hdc = ::GetDC(nullptr);
    if (!hdc) return s;
    std::wstring out = s;
    if (ui_skin::TextW(hdc, s.c_str(), px, false) > avail) {
        const std::wstring ell = L"\x2026";  // …
        const size_t n = s.size();
        const size_t tailLen = (n > 8) ? 8 : 0;
        const std::wstring tail = s.substr(n - tailLen);
        size_t lo = 0, hi = n - tailLen;
        while (lo < hi) {  // 二分：前缀 + … + 尾部 尽量长
            size_t mid = (lo + hi + 1) / 2;
            std::wstring cand = s.substr(0, mid) + ell + tail;
            if (ui_skin::TextW(hdc, cand.c_str(), px, false) <= avail) lo = mid;
            else hi = mid - 1;
        }
        out = s.substr(0, lo) + ell + tail;
        if (ui_skin::TextW(hdc, out.c_str(), px, false) > avail) {  // 尾部本身就放不下
            out = ell;
            for (wchar_t c : s) {
                std::wstring t = out + c;
                if (ui_skin::TextW(hdc, t.c_str(), px, false) > avail) break;
                out.swap(t);
            }
        }
    }
    ::ReleaseDC(nullptr, hdc);
    return out;
}

void CMainForm::SetStatus(const std::wstring& text) {
    // StatusText 是自绘的 CSkinLabelUI（不是 CLabelUI）
    m_lastStatus = text;
    CControlUI* p = m_PaintManager.FindControl(_T("StatusText"));
    if (p) p->SetText(FitStatusLine(m_PaintManager, text).c_str());
    LogInfo(std::string("GUI status: ") + W2U(text));
}

void CMainForm::ShowProgress(bool show) {
    if (!show) ShowPostProgress(false);
    for (const TCHAR* name : {_T("Progress"), _T("PercentText")}) {
        CControlUI* p = m_PaintManager.FindControl(name);
        if (p && p->IsVisible() != show) p->SetVisible(show);
    }
    // 进度条显隐会改变状态文字的可用宽度 → 把已显示的那条按新宽度重新收放
    if (!m_lastStatus.empty()) {
        CControlUI* p = m_PaintManager.FindControl(_T("StatusText"));
        if (p) p->SetText(FitStatusLine(m_PaintManager, m_lastStatus).c_str());
    }
}

void CMainForm::ShowPostProgress(bool show) {
    CRoundProgressUI* p = static_cast<CRoundProgressUI*>(
        m_PaintManager.FindControl(_T("PostProgress")));
    if (p && p->IsVisible() != show) p->SetVisible(show);
    m_postBusy = show;
    if (show) {
        m_postPulse = 0;
        if (p) p->SetValue(0);
        SetTimer(m_hWnd, 3, 120, nullptr);
    } else {
        KillTimer(m_hWnd, 3);
        if (p) p->SetValue(0);
    }
}

void CMainForm::SetProgress(int pct) {
    CRoundProgressUI* p =
        static_cast<CRoundProgressUI*>(m_PaintManager.FindControl(_T("Progress")));
    if (p) p->SetValue(pct);
    // 百分比文字一起更新 —— 用户 2026-09-23 反馈："跑的过程中少了百分比" ✓。
    // 以前只有 OnTaskComplete 里写 PercentText（所以完成时 100% 正常、中途是空的），
    // 而且上一轮的 100% 会残留下来（看着像进度条旁边一个奇怪的字符）。
    // 0 → 清空，任务一开始就把旧值抹掉。
    CControlUI* pPct = m_PaintManager.FindControl(_T("PercentText"));
    if (pPct)
        pPct->SetText(pct > 0 ? (std::to_wstring(pct) + L"%").c_str() : L"");
}
