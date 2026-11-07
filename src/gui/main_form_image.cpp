// 拆分自 src/gui/main_form.cpp（按职责）：分区枚举 / 分区向导 / 镜像与备份文件选择 / 格式同步 / CPU 限速。
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

// ────────────────── 分区枚举 ──────────────────

void CMainForm::PopulatePartitions(const std::vector<DiskInfo>& disks) {
    auto* pCombo = static_cast<CPartitionComboUI*>(
        m_PaintManager.FindControl(_T("PartitionBox")));
    if (!pCombo) return;

    pCombo->RemoveAll();
    m_parts.clear();
    m_partStyles.clear();
    m_selPart = -1;
    int selDefault = -1;
    for (const auto& disk : disks) {
        for (const auto& part : disk.parts) {
            // M12：默认隐藏可移动盘；仅当用户勾选「显示 USB 设备」时保留 USB 盘。
            if (part.isEsp || part.isMsr || part.isRecovery ||
                (disk.isRemovable && !m_showUsb))
                continue;
            if (part.sizeBytes < 2ULL * 1024 * 1024 * 1024)
                continue;
            m_parts.push_back(part);
            m_partStyles.push_back(disk.style);
            // 分区行四列自绘（对齐老项目 PartitionRow.cs，见 ui_skin.cpp::PaintRow）
            CPartItemUI* item = new CPartItemUI;
            item->SetFixedHeight(43);
            wchar_t partNo[32];
            swprintf(partNo, 32, L"%u/%u", disk.index, part.partNumber);
            uint64_t used = part.sizeBytes > part.freeBytes
                            ? part.sizeBytes - part.freeBytes : 0;
            std::wstring cap  = FormatGb(used) + L" / " + FormatGb(part.sizeBytes);
            std::wstring freeTxt = part.freeBytes > 0
                                   ? (Tr(L"可用 ") + FormatGb(part.freeBytes))
                                   : Tr(L"系统保留");
            int pct = part.sizeBytes
                      ? (int)(used * 100 / part.sizeBytes) : 0;
            // M12：USB 盘在分区行显式标注，提醒用户不要误选。
            std::wstring modelTxt =
                disk.model.empty() ? Tr(L"本地磁盘") : disk.model;
            if (disk.isUsb)
                modelTxt += Tr(L"（USB 设备）");
            item->SetPart(part.letter.empty() ? Tr(L"—") : part.letter.c_str(),
                          U2W(StyleName(disk.style)).c_str(),
                          part.fs.c_str(), part.isSystem,
                          modelTxt.c_str(),
                          part.label.c_str(), partNo,
                          cap.c_str(), freeTxt.c_str(), pct);
            pCombo->Add(item);
            if (part.isSystem && selDefault < 0)
                selDefault = (int)m_parts.size() - 1;
        }
    }
    if (pCombo->GetCount() > 0)
        pCombo->SelectItem(selDefault >= 0 ? selDefault : 0);
    m_selPart = selDefault >= 0 ? selDefault : (pCombo->GetCount() > 0 ? 0 : -1);
    // 空列表：收起框显示提示；点击时（CPartitionComboUI::DoEvent）直接打开分区向导。
    pCombo->SetEmptyHint(Tr(L"点击此处为新硬盘创建分区…"));
    CControlUI* pLoad = m_PaintManager.FindControl(_T("PartLoadingText"));
    if (pLoad) pLoad->SetVisible(false);
    UpdateMainAction();
}

// ────────────────── 新硬盘分区向导（2026-10-20） ──────────────────

void CMainForm::OpenInitDiskWizard() {
    if (m_busy) return;
    auto empties = sysrecover::FindEmptyDisks();
    if (empties.empty()) {
        const auto disks = sysrecover::EnumerateDisks();
        std::wstring msg =
            disks.empty()
                ? Tr(L"未检测到内置硬盘（磁盘可能未联机）。\n"
                     L"请先在磁盘管理中联机后再试。")
                : Tr(L"没有可用的系统分区。\n"
                     L"若磁盘已经分区，请先用磁盘管理调整分区。");
        MessageBoxW(m_hWnd, msg.c_str(), Tr(L"创建分区"),
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (empties.size() > 1) {
        MessageBoxW(m_hWnd,
                    Tr(L"检测到多块空硬盘。请只保留要安装系统的那一块后重试。"),
                    Tr(L"创建分区"), MB_OK | MB_ICONWARNING);
        return;
    }
    sysrecover::InitDiskRequest req;
    req.disk = empties[0];
    req.uefiSession = sysrecover::IsUefiBootSession();
    std::wstring low = m_wimPath;
    for (auto& ch : low) ch = (wchar_t)towlower(ch);
    req.win7Family = low.find(L"win7") != std::wstring::npos ||
                     low.find(L"windows 7") != std::wstring::npos;
    sysrecover::DiskScheme first, second;
    sysrecover::RankPartSchemes(req.uefiSession,
                                sysrecover::IsUefiFirmware(), req.win7Family,
                                req.disk.sizeBytes, first, second,
                                req.mbrAllowed, req.gptAllowed);
    req.preferred = first;
    // 2026-10-21 用户 VM 反馈：小容量磁盘按规则推荐（60GB 盘 Win10/11 → 整盘
    // 做系统盘；Win7 → 30GB + 其余数据盘），这里只给默认/建议下限。
    const auto hint = sysrecover::SuggestPartitionSizes(
        req.win7Family, first, req.disk.sizeBytes);
    req.defaultSystemGb = hint.systemGb;
    req.minSystemGb = req.win7Family ? 30 : 60;
    const auto r = sysrecover::ShowInitDiskDialog(m_hWnd, req);
    if (!r.ok) return;
    sysrecover::PartInitPlan plan;
    plan.diskIndex = req.disk.index;
    plan.scheme = r.scheme;
    plan.systemGb = r.systemGb;
    plan.win7Family = req.win7Family;
    m_busy = true;
    UpdateMainAction();
    SetStatus(Tr(L"正在为新硬盘创建分区..."));
    SetProgress(0);
    // 进度条必须显式打开（2026-10-27 用户规格）：diskpart 建分区+格式化能跑
    // 几十秒，以前只有状态栏一行字、进度条根本不出现，看着像卡死。
    ShowProgress(true);
    SetTimer(m_hWnd, 1, 1000, nullptr);
    if (m_worker.joinable()) m_worker.join();
    m_lastProgressPost = std::chrono::steady_clock::now();
    m_lastStageRaw.clear();
    m_worker = std::thread([this, plan] {
        std::wstring err;
        std::string log;
        // 阶段 → 进度条（partinit-* 由 StageCn 翻中文）。节流与备份/还原同款：
        // 阶段名一变立刻发（否则 100ms 内首个阶段会被吞掉，PIT-098）。
        auto progressFn = [this](int pct, const std::string& stage) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - m_lastProgressPost)
                    .count();
            const bool stageChanged = stage != m_lastStageRaw;
            if (elapsed < kProgressThrottleMs && pct < 100 && !stageChanged)
                return;
            m_lastProgressPost = now;
            m_lastStageRaw = stage;
            auto* pStage = new std::wstring(U2W(stage));
            PostMessage(WM_PROGRESS_UPDATE, (WPARAM)pct, (LPARAM)pStage);
        };
        const int rc = sysrecover::ApplyPartInitPlan(plan, err, log, progressFn);
        m_initErr = err;
        m_initLog = log;
        LogInfo("partinit rc=" + std::to_string(rc) + "\n" + log);
        ::PostMessageW(m_hWnd, WM_PART_INIT_DONE, (WPARAM)rc, 0);
    });
}

void CMainForm::StartRepairBoot(bool confirm) {
    if (m_busy) return;
    if (confirm && MessageBoxW(m_hWnd,
                               Tr(L"将自动查找系统盘与 ESP，重建并校验引导文件。\n"
                                  L"确定继续吗？"),
                               Tr(L"修复引导"),
                               MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;
    LogInfo(std::string("GUI repair-boot start") + (confirm ? "" : " (retry)"));
    m_busy = true;
    m_fixBlocked = false;
    UpdateMainAction();
    SetStatus(Tr(L"正在修复引导..."));
    SetProgress(0);
    SetTimer(m_hWnd, 1, 1000, nullptr);
    if (m_worker.joinable()) m_worker.join();
    m_worker = std::thread([this] {
        sysrecover::ResetBootBlocked();
        std::string msg;
        const int rc = sysrecover::RepairBoot(-1, -1, msg);
        m_fixMsg = msg;
        // 必须在**同一 worker 线程**取（线程局部标志；PIT-184 更正段）
        m_fixBlocked = sysrecover::TakeBootBlocked();
        LogInfo("repair-boot rc=" + std::to_string(rc) +
                " blocked=" + std::to_string(m_fixBlocked ? 1 : 0) + ": " + msg);
        ::PostMessageW(m_hWnd, WM_FIXBOOT_DONE, (WPARAM)rc, 0);
    });
}

// ── 异机还原（2026-10-21，0.9）：只用一个勾选框 ────────────────────
// 勾选后：程序目录 `Drivers\` 存在且含 .inf → 自动注入；否则仅启用内置驱动。
bool CMainForm::IsUniversalChecked() {
    COptionUI* p = static_cast<COptionUI*>(
        m_PaintManager.FindControl(_T("Universal")));
    return p && p->IsSelected();
}

// ────────────────── 文件对话框 ──────────────────

void CMainForm::BrowseWimFile() {
    IFileOpenDialog* pDlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pDlg));
    if (FAILED(hr) || !pDlg) { SetStatus(Tr(L"文件对话框创建失败")); return; }
    COMDLG_FILTERSPEC filters[] = {
        {Tr(L"WIM/ESD 镜像"), L"*.wim;*.esd;*.zje"},
        {Tr(L"所有文件"), L"*.*"},
    };
    pDlg->SetFileTypes(2, filters);
    pDlg->SetTitle(Tr(L"选择系统镜像文件"));
    if (SUCCEEDED(pDlg->Show(m_hWnd))) {
        IShellItem* pItem = nullptr;
        if (SUCCEEDED(pDlg->GetResult(&pItem))) {
            LPWSTR psz = nullptr;
            pItem->GetDisplayName(SIGDN_FILESYSPATH, &psz);
            if (psz) {
                m_wimPath = psz;
                CoTaskMemFree(psz);
                SetImagePathEdit(m_wimPath);
                LoadWimImages(m_wimPath);
            }
            pItem->Release();
        }
    }
    pDlg->Release();
}

void CMainForm::BrowseSaveFile() {
    IFileSaveDialog* pDlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pDlg));
    if (FAILED(hr) || !pDlg) { SetStatus(Tr(L"文件对话框创建失败")); return; }
    // ★ 2026-09-30 用户规格：浏览框的「保存类型」必须与界面上的「类型」下拉**一致**
    //   （同样的三项），且选中后**要生效** —— 以前只有两项、选了既不回写界面下拉、
    //   也改变不了真正决定格式的东西（文件名后缀/界面下拉），属 bug。
    //   现在三项一一对应：选完立即回写界面「类型」下拉，并把文件名后缀对齐到所选类型。
    COMDLG_FILTERSPEC filters[] = {
        {Tr(L"esd(慢)"), L"*.esd"},
        {Tr(L"wim(中)"), L"*.wim"},
        {Tr(L"wim(快)"), L"*.wim"},
    };
    pDlg->SetFileTypes(3, filters);
    pDlg->SetTitle(Tr(L"选择备份保存位置"));
    int fmtIdx = BackupFmt();                 // 0=.esd 1=.wim正常 2=.wim高速
    pDlg->SetFileTypeIndex(static_cast<UINT>(fmtIdx + 1));  // COMDLG 1-based
    const wchar_t* ext = fmtIdx == 0 ? L".esd" : L".wim";
    // 默认文件名（用户规格 2026-09-21）：`20260921Win10.19044备份.esd`
    std::wstring defName = TimestampDate() +
                           (m_sysDesc.shortTag.empty() ? TimestampName()
                                                       : m_sysDesc.shortTag) +
                           Tr(L"备份") + ext;
    pDlg->SetFileName(defName.c_str());
    if (SUCCEEDED(pDlg->Show(m_hWnd))) {
        IShellItem* pItem = nullptr;
        if (SUCCEEDED(pDlg->GetResult(&pItem))) {
            LPWSTR psz = nullptr;
            pItem->GetDisplayName(SIGDN_FILESYSPATH, &psz);
            if (psz) {
                std::wstring path = psz;
                CoTaskMemFree(psz);
                // 对话框里选的「保存类型」→ 生效：回写界面「类型」下拉 + 后缀对齐
                UINT fti = 0;
                if (SUCCEEDED(pDlg->GetFileTypeIndex(&fti)) && fti >= 1 && fti <= 3) {
                    int want = static_cast<int>(fti) - 1;   // 0/1/2
                    const wchar_t* wantExt = (want == 0) ? L".esd" : L".wim";
                    size_t slash = path.find_last_of(L"\\/");
                    size_t base = (slash == std::wstring::npos) ? 0 : slash + 1;
                    size_t dot = path.find_last_of(L'.');
                    if (dot != std::wstring::npos && dot > base)
                        path = path.substr(0, dot) + wantExt;  // 换掉旧后缀
                    else
                        path += wantExt;                       // 本来就没后缀
                    CComboUI* pFmt = static_cast<CComboUI*>(
                        m_PaintManager.FindControl(_T("FormatBox")));
                    if (pFmt && pFmt->GetCount() > want) pFmt->SelectItem(want, false, false);
                }
                m_backupPath = path;
                SetImagePathEdit(m_backupPath);
                SetStatus(Tr(L"备份保存至：") + m_backupPath);
            }
            pItem->Release();
        }
    }
    pDlg->Release();
    UpdateMainAction();
}

// 「搜索」（还原模式第一步）：按 imgsearch.h 的三规则扫附近 .esd/.wim，
// 结果按修改时间新→旧，把最新一个填进输入框并立即解析子镜像。
void CMainForm::OnSearchImages() {
    if (m_busy) return;
    m_searchHits = imgsearch::Search();
    if (m_searchHits.empty()) {
        SetStatus(Tr(L"附近未找到 .esd/.wim 镜像文件"));
        return;
    }
    const std::wstring best = m_searchHits[0];
    m_wimPath = best;
    SetImagePathEdit(best);  // 同步原生 EDIT → EN_CHANGE 闭环
    // L1：译文只拼在固定文本外，不再作为 swprintf 格式串。
    const std::wstring foundMsg =
        Tr(L"找到 ") + std::to_wstring(m_searchHits.size()) +
        Tr(L" 个镜像文件，已填入最新：");
    SetStatus(foundMsg + best);  // 解析失败时会被 LoadWimImages 的错误覆盖
    LoadWimImages(best);
}

// 点输入框 → 在其下方弹「搜索结果」下拉（TrackPopupMenu，选中回填 + 解析）。
// 由 EN_SETFOCUS 进入（HandleMessage），m_pickGuard 防菜单关闭后 SetFocus 重入。
void CMainForm::ShowImagePickMenu() {
    CControlUI* pCtl = m_PaintManager.FindControl(_T("ImagePath"));
    if (!pCtl) return;
    HWND hEdit = static_cast<CSkinEditUI*>(pCtl)->GetNativeEditHWND();
    if (!hEdit) return;
    HMENU hMenu = ::CreatePopupMenu();
    if (!hMenu) return;
    for (size_t i = 0; i < m_searchHits.size(); ++i) {
        // 列表显示文件名（过长截断）；菜单项 ID = 下标 + 1（0 视为无效）
        std::wstring name = m_searchHits[i];
        size_t bs = name.find_last_of(L'\\');
        if (bs != std::wstring::npos) name = name.substr(bs + 1);
        if (name.size() > 48) name = name.substr(0, 45) + L"...";  // ASCII 截断符，不做词条
        UINT flags = MF_STRING;
        if (_wcsicmp(m_searchHits[i].c_str(), m_wimPath.c_str()) == 0)
            flags |= MF_CHECKED;  // 当前输入框里的那一个打勾
        ::AppendMenuW(hMenu, flags, (UINT)(i + 1), name.c_str());
    }
    RECT rc{};
    ::GetWindowRect(hEdit, &rc);
    m_pickGuard = true;
    int cmd = ::TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_LEFTALIGN,
                               rc.left, rc.bottom + 1, 0, m_hWnd, nullptr);
    ::DestroyMenu(hMenu);
    if (cmd > 0 && cmd <= (int)m_searchHits.size()) {
        const std::wstring pick = m_searchHits[(size_t)cmd - 1];
        m_wimPath = pick;
        pCtl->SetText(pick.c_str());
        SetStatus(std::wstring(Tr(L"已选择镜像：")) + pick);
        LoadWimImages(pick);
        UpdateMainAction();
    }
    if (::IsWindow(hEdit)) ::SetFocus(hEdit);  // 焦点还给输入框（guard 拦下再弹）
    m_pickGuard = false;
}

// WM_DROPFILES：把拖进来的 .esd/.wim 填到第一步输入框并立即解析子镜像。
void CMainForm::HandleDroppedFiles(WPARAM wParam) {
    if (m_busy) return;
    HDROP hDrop = reinterpret_cast<HDROP>(wParam);
    UINT n = ::DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
    if (n == 0) { ::DragFinish(hDrop); return; }
    std::wstring dropped;
    {
        UINT len = ::DragQueryFileW(hDrop, 0, nullptr, 0);  // 长度不含结尾 0
        if (len > 0) {
            std::vector<wchar_t> buf((size_t)len + 1, 0);
            ::DragQueryFileW(hDrop, 0, buf.data(), len + 1);
            dropped = buf.data();
        }
    }
    ::DragFinish(hDrop);
    if (dropped.empty()) return;
    // 只认镜像后缀：拖错文件时给明确提示，不静默失败
    std::wstring ext =
        dropped.size() >= 4 ? dropped.substr(dropped.size() - 4) : L"";
    for (auto& c : ext) c = (wchar_t)towlower(c);
    if (ext != L".esd" && ext != L".wim" && ext != L".zje") {
        SetStatus(Tr(L"只支持拖入 .esd / .wim / .zje 镜像文件"));
        return;
    }
    std::wstring& active = ActivePath();
    active = dropped;
    SetImagePathEdit(active);
    if (m_backupMode) {
        SetStatus(Tr(L"备份保存至：") + active);
        SyncFormatFromPath();  // 拖进来的 .wim/.esd 同样决定格式
        UpdateMainAction();
    } else {
        LoadWimImages(active);  // 内部会置 m_imageOk 并刷新主按钮
        SetStatus(Tr(L"已载入镜像：") + active);
    }
}

void CMainForm::LoadWimImages(const std::wstring& path) {
    m_imageOk = false;
    CComboUI* pCombo =
        static_cast<CComboUI*>(m_PaintManager.FindControl(_T("ImageIndexBox")));
    if (!pCombo) { UpdateMainAction(); return; }
    pCombo->RemoveAll();
    m_imgIdx.clear();
    // 加密镜像（docs/30）：先要口令（同一次运行内记住，不再反复问），
    // 再读容器 meta 列出子镜像 —— 不需要解密几十 GB 正文。
    if (sysrecover::ImageIsEncrypted(path) &&
        sysrecover::GetImagePassword(path).empty()) {
        std::wstring pwd;
        if (!CPasswordDlg::Ask(m_hWnd, Tr(L"加密镜像"),
                               Tr(L"该镜像已加密，请输入密码："), pwd)) {
            SetStatus(Tr(L"已取消输入密码"));
            UpdateMainAction();
            return;
        }
        sysrecover::SetImagePassword(path, pwd);
    }
    std::vector<ImageDesc> images;
    int rc = sysrecover::ImageList(path, images);
    if (rc != 0 || images.empty()) {
        // 关键：读取失败（例如上次备份中途退出留下"写入未完成"的坏镜像）
        // 时必须保持 m_imageOk=false 并刷新按钮，否则主按钮的状态会停在
        // 上一次的可用态（用户看到"灰色按钮切一下模式又能点了"的怪象）。
        SetStatus(std::wstring(Tr(L"镜像不可用（可能上次备份未完成，请重新备份）")) +
                  (sysrecover::ImageIsEncrypted(path)
                       ? Tr(L"：密码错误或加密镜像损坏")
                       : std::wstring(L": ") + WimEngine::ErrorString(rc)));
        UpdateMainAction();
        return;
    }
    for (const auto& img : images) {
        // 纯文字条目：必须走 CTextItemUI（重写了 DrawItemText，否则 Combo
        // 收起框空白，见 ui_skin.h 注释）
        CTextItemUI* item = new CTextItemUI;
        item->SetFixedHeight(30);
        // L1：译文不再作为格式串。数值/名称各自格式化后用 std::wstring 拼接
        //（也顺手避开 PIT-007 的 %ls/%s 宽度坑）。
        wchar_t sizeText[32];
        swprintf(sizeText, 32, L"%.1f", img.sizeBytes / 1073741824.0);
        const std::wstring row = std::to_wstring(img.index) + L" - " +
                                 img.name + Tr(L"（") + sizeText +
                                 Tr(L" GB）");
        item->SetText(row.c_str());
        pCombo->Add(item);
        m_imgIdx.push_back(img.index);
    }
    if (pCombo->GetCount() > 0) pCombo->SelectItem(0);
    m_selImageIndex = m_imgIdx.empty() ? 1 : m_imgIdx[0];
    m_imageOk = true;
    // 成功路径原先既不改状态栏、也不写日志 → 支持排查时看不出"加载过哪个镜像"，
    // 自动化测试也无法从日志判断加载成功（2026-10-07 实测）。补一行日志。
    LogInfo("GUI image loaded: " + W2U(path) + " (" +
            std::to_string(images.size()) + " subimage(s), first idx=" +
            std::to_string(m_selImageIndex) + ")");
    // 成功路径必须刷掉上一条"镜像不可用"：否则用户改正路径后，状态栏仍挂着旧错误。
    SetIdleStatus();
    UpdateMainAction();
}

// ────────────────── 备份格式 combo 辅助 ──────────────────

int CMainForm::BackupFmt() const {
    CComboUI* pFmt =
        static_cast<CComboUI*>(m_PaintManager.FindControl(_T("FormatBox")));
    int fmt = pFmt ? pFmt->GetCurSel() : 1;
    return (fmt < 0 || fmt > 2) ? 1 : fmt;
}

// 按第一步的文件后缀定「格式」（用户 2026-09-28 规格）：路径里输了 `.wim` 就自动
// 选中「.wim(正常大小)」、`.esd` 选「.esd(慢速小体积)」，不必再手点一次下拉。
// 只在**路径变化**时同步 —— 手动换格式不会改路径，所以不会被反复顶回去。
void CMainForm::SyncFormatFromPath() {
    if (!m_backupMode) return;
    std::wstring p = m_backupPath;
    while (!p.empty() && (p.back() == L' ' || p.back() == L'"' || p.back() == L'\''))
        p.pop_back();
    size_t slash = p.find_last_of(L"\\/");
    std::wstring fn = (slash == std::wstring::npos) ? p : p.substr(slash + 1);
    if (fn.size() < 4) return;  // 后缀还没输完，别乱动
    std::wstring ext = fn.substr(fn.size() - 4);
    for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
    if (ext != L".wim" && ext != L".esd") return;
    int want = (ext == L".wim") ? 1 : 0;
    CComboUI* pFmt =
        static_cast<CComboUI*>(m_PaintManager.FindControl(_T("FormatBox")));
    if (pFmt && pFmt->GetCount() > want && pFmt->GetCurSel() != want)
        pFmt->SelectItem(want, false, false);  // 程序选择：不触发 SELECTCHANGED（免回环）
}

// ★ 2026-09-30 用户规格：换「类型」→ **同步改文件名后缀**（只改后缀，文件名其余不动）。
// 例：`D:\x\20260921Win10备份.esd` 选「wim(中)」→ `...备份.wim`。
// 与 SyncFormatFromPath 互为反向：那个是「后缀→下拉」，这个是「下拉→后缀」。
void CMainForm::SyncPathExtFromFormat() {
    if (!m_backupMode || m_backupPath.empty()) return;
    const wchar_t* wantExt = (BackupFmt() == 0) ? L".esd" : L".wim";
    std::wstring p = m_backupPath;
    size_t slash = p.find_last_of(L"\\/");
    size_t base = (slash == std::wstring::npos) ? 0 : slash + 1;
    size_t dot = p.find_last_of(L'.');
    std::wstring np = (dot != std::wstring::npos && dot > base)
                          ? p.substr(0, dot) + wantExt   // 换掉旧后缀
                          : p + wantExt;                 // 本来就没后缀
    if (np == m_backupPath) return;
    m_backupPath = np;
    SetImagePathEdit(m_backupPath);
}

// 「限制CPU」下拉 → 上限百分比。条目顺序（用户 2026-10-27 规格）：
//   不限速 / CPU 75% / CPU 50% / CPU 25%（从宽到严，见 skin/main.xml）。
// 备份中改了立即生效：改的是 Job 对象的速率，不打断正在跑的 wimlib。
void CMainForm::ApplyCpuCapFromUi() {
    // 下标 → 上限（**不是** sel*25：第 1 项是 75%，顺序已按用户要求重排）
    static const int kCapByIndex[] = {0, 75, 50, 25};
    CComboUI* p =
        static_cast<CComboUI*>(m_PaintManager.FindControl(_T("CpuCapBox")));
    if (!p) return;
    int sel = p->GetCurSel();
    if (sel < 0 || sel >= (int)(sizeof(kCapByIndex) / sizeof(kCapByIndex[0])))
        sel = 0;
    m_cpuCap = kCapByIndex[sel];
    std::wstring msg = Tr(L"CPU 不限制");
    if (m_cpuCap != 0) {
        // L1：固定文本 + 数值拼接（不再把译文当格式串）。
        msg = Tr(L"CPU 限制：") + std::to_wstring(m_cpuCap) + L"%";
    }
    if (m_busy) {
        std::string why;
        if (!SetCpuCap(m_cpuCap, &why)) {
            LogWarn("cpu cap change failed: " + why);
            SetStatus(U2W(why));
            return;
        }
    }
    SetStatus(msg);
}

