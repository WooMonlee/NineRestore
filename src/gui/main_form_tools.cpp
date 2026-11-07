// 拆分自 src/gui/main_form.cpp（按职责）：导出驱动 / 自打包解包 / 镜像检查。
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

// 导出本机启动关键驱动（2026-10-30，用户规格）：只导 SCSIAdapter/HDC/System/
// DiskDrive/Volume/USB/HIDClass/Keyboard/Mouse，避免 `dism /export-driver`
// 那种 1.8GB 全量（网络/打印/蓝牙/显卡全在里面）。
// ── PIT-207：自我打包 / 解包（用户 2026-11-01：「更多功能」里随时可用）──
// 打包：把当前程序目录打成一个自解压单文件（stub 取自 bootfiles\portable-stub.exe）。
void CMainForm::PackSelfFlow() {
    if (m_busy) return;
    wchar_t defName[96] = {};
    swprintf(defName, 96, L"%ls%hs.exe", Tr(L"九转大还原"), SYSRECOVER_VERSION);
    std::wstring out;
    {
        IFileSaveDialog* pDlg = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr,
                                      CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pDlg));
        if (FAILED(hr) || !pDlg) {
            SetStatus(Tr(L"保存对话框创建失败"));
            return;
        }
        COMDLG_FILTERSPEC filters[] = {{Tr(L"单文件程序"), L"*.exe"}};
        pDlg->SetFileTypes(1, filters);
        pDlg->SetDefaultExtension(L"exe");
        pDlg->SetFileName(defName);
        pDlg->SetTitle(Tr(L"打包成单文件（保存到）"));
        if (SUCCEEDED(pDlg->Show(m_hWnd))) {
            IShellItem* pItem = nullptr;
            if (SUCCEEDED(pDlg->GetResult(&pItem)) && pItem) {
                LPWSTR psz = nullptr;
                pItem->GetDisplayName(SIGDN_FILESYSPATH, &psz);
                if (psz) {
                    out = psz;
                    CoTaskMemFree(psz);
                }
                pItem->Release();
            }
        }
        pDlg->Release();
    }
    if (out.empty()) {
        SetStatus(Tr(L"已取消打包"));
        return;
    }
    SetStatus(Tr(L"正在打包成单文件..."));
    std::string err;
    int n = 0;
    const std::wstring stub = AppDir() + L"\\bootfiles\\portable-stub.exe";
    const bool ok = sysrecover::PackSelfToSingleFile(AppDir(), out, stub, err, &n);
    if (!ok) {
        SetStatus(Tr(L"打包失败"));
        MessageBoxW(m_hWnd, (Tr(L"打包失败：") + U2W(err)).c_str(),
                    Tr(L"打包成单文件"), MB_OK | MB_ICONERROR);
        return;
    }
    unsigned long long sz = 0;
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (GetFileAttributesExW(out.c_str(), GetFileExInfoStandard, &fad))
        sz = ((unsigned long long)fad.nFileSizeHigh << 32) + fad.nFileSizeLow;
    wchar_t msg[700] = {};
    swprintf(msg, 700,
             Tr(L"已打包 %d 个文件 →\n%ls\n（%.1f MB）\n\n"
                L"该单文件可选「文件→系统/系统→文件」，也可以直接当命令行用。"),
             n, out.c_str(), sz / 1048576.0);
    SetStatus(Tr(L"打包完成"));
    MessageBoxW(m_hWnd, msg, Tr(L"打包成单文件"), MB_OK | MB_ICONINFORMATION);
}

// 解压：把当前程序目录复制到用户选的文件夹（跳过 logs）。
void CMainForm::ExtractSelfFlow() {
    if (m_busy) return;
    std::wstring dest;
    {
        IFileOpenDialog* pDlg = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                      CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pDlg));
        if (FAILED(hr) || !pDlg) {
            SetStatus(Tr(L"文件夹对话框创建失败"));
            return;
        }
        DWORD opts = 0;
        pDlg->GetOptions(&opts);
        pDlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                         FOS_PATHMUSTEXIST);
        pDlg->SetTitle(Tr(L"解压到文件夹（选目标目录）"));
        if (SUCCEEDED(pDlg->Show(m_hWnd))) {
            IShellItem* pItem = nullptr;
            if (SUCCEEDED(pDlg->GetResult(&pItem)) && pItem) {
                LPWSTR psz = nullptr;
                pItem->GetDisplayName(SIGDN_FILESYSPATH, &psz);
                if (psz) {
                    dest = psz;
                    CoTaskMemFree(psz);
                }
                pItem->Release();
            }
        }
        pDlg->Release();
    }
    if (dest.empty()) {
        SetStatus(Tr(L"已取消解压"));
        return;
    }
    SetStatus(Tr(L"正在解压到文件夹..."));
    std::string err;
    int n = 0;
    const bool ok = sysrecover::ExtractSelfToFolder(AppDir(), dest, err, &n);
    if (!ok) {
        SetStatus(Tr(L"解压失败"));
        MessageBoxW(m_hWnd, (Tr(L"解压失败：") + U2W(err)).c_str(),
                    Tr(L"解压到文件夹"), MB_OK | MB_ICONERROR);
        return;
    }
    wchar_t msg[700] = {};
    swprintf(msg, 700,
             Tr(L"已解压 %d 个文件到：\n%ls\n\n双击其中的 SysRecoverUI.exe 即可运行。"),
             n, dest.c_str());
    SetStatus(Tr(L"解压完成"));
    MessageBoxW(m_hWnd, msg, Tr(L"解压到文件夹"), MB_OK | MB_ICONINFORMATION);
}

void CMainForm::ExportDriversFlow() {
    if (m_busy) return;
    // PIT-212（用户 2026-11-02）：单文件运行时，驱动要**打进新的单文件**。
    if (sysrecover::IsPortableRuntime()) {
        ExportDriversPortableFlow();
        return;
    }

    // PIT-206（用户 2026-11-01）：**导出之前先让用户选文件夹** ——
    // 程序自身是解压到 %LOCALAPPDATA% 的（临时性质），不能"悄悄"导在那里。
    const std::wstring root = sysrecover::DriversRoot();  // 默认建议位置
    std::wstring picked;
    {
        IFileOpenDialog* pDlg = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                      CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pDlg));
        if (FAILED(hr) || !pDlg) {
            SetStatus(Tr(L"文件夹对话框创建失败"));
            return;
        }
        DWORD opts = 0;
        pDlg->GetOptions(&opts);
        pDlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                         FOS_PATHMUSTEXIST);
        pDlg->SetTitle(Tr(L"选择驱动导出的文件夹"));
        {
            IShellItem* psi = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(root.c_str(), nullptr,
                                                      IID_PPV_ARGS(&psi))) &&
                psi) {
                pDlg->SetFolder(psi);
                psi->Release();
            }
        }
        if (SUCCEEDED(pDlg->Show(m_hWnd))) {
            IShellItem* pItem = nullptr;
            if (SUCCEEDED(pDlg->GetResult(&pItem)) && pItem) {
                LPWSTR psz = nullptr;
                pItem->GetDisplayName(SIGDN_FILESYSPATH, &psz);
                if (psz) {
                    picked = psz;
                    CoTaskMemFree(psz);
                }
                pItem->Release();
            }
        }
        pDlg->Release();
        if (picked.empty()) {
            SetStatus(Tr(L"已取消导出驱动"));
            return;
        }
    }

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    wchar_t sub[80] = {};
    swprintf(sub, 80, L"\\boot-%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute, st.wSecond);
    const std::wstring dir = picked + sub;
    SetStatus(Tr(L"正在导出本机启动驱动..."));
    std::wstring err;
    int n = 0;
    const int rc = sysrecover::ExportBootCriticalDrivers(dir, &n, err);
    if (rc != 0) {
        SetStatus(Tr(L"导出启动驱动失败"));
        MessageBoxW(m_hWnd, (Tr(L"导出启动驱动失败：") + err).c_str(),
                    Tr(L"导出驱动"), MB_OK | MB_ICONERROR);
        return;
    }
    // 只有导到"自动目录"下，异机还原才会自动调用；否则提示用户手动指向。
    std::wstring lowPick = picked, lowRoot = root;
    for (auto& c : lowPick) c = (wchar_t)towlower(c);
    for (auto& c : lowRoot) c = (wchar_t)towlower(c);
    const bool autoUsed = lowPick.compare(0, lowRoot.size(), lowRoot) == 0;
    wchar_t msg[900] = {};
    if (autoUsed)
        swprintf(msg, 900,
                 Tr(L"已导出 %d 个硬盘（存储/输入）驱动到：\n%ls\n\n"
                    L"只含存储/输入类驱动，异机还原时将自动调用此驱动。"),
                 n, dir.c_str());
    else
        swprintf(msg, 900,
                 Tr(L"已导出 %d 个硬盘（存储/输入）驱动到：\n%ls\n\n"
                    L"该目录不在自动目录（%ls）内：异机还原时请把「驱动目录」"
                    L"指向它，或把它复制到自动目录。"),
                 n, dir.c_str(), root.c_str());
    SetStatus(Tr(L"启动驱动已导出"));
    MessageBoxW(m_hWnd, msg, Tr(L"导出驱动"), MB_OK | MB_ICONINFORMATION);
}

// PIT-212：单文件版导出驱动 —— 打进包内 Drivers，再重新打包成一个新的单文件
// （放到当前运行的单文件旁边，名字 = <用户名>加驱动.exe）。
// 原程序不动；新程序运行时异机还原会自动用包内驱动。
void CMainForm::ExportDriversPortableFlow() {
    const std::wstring origin = sysrecover::PortableOriginDir();
    if (origin.empty()) {
        SetStatus(Tr(L"无法确定单文件所在目录"));
        return;
    }
    const std::wstring outPath = origin + L"\\" +
                                 sysrecover::CurrentUserName() +
                                 Tr(L"加驱动") + L".exe";
    wchar_t msg[900] = {};
    swprintf(msg, 900,
             Tr(L"驱动会先导出到临时文件夹，然后重新打包成本软件，放到：\n%ls\n\n"
                L"原程序不受影响，可以继续使用。"),
             outPath.c_str());
    if (CConfirmDlg::Ask2(m_hWnd, Tr(L"导出驱动"), msg, Tr(L"取消"),
                          Tr(L"确定"), /*defaultIsRight=*/true) != 1) {
        SetStatus(Tr(L"已取消导出驱动"));
        return;
    }

    SetStatus(Tr(L"正在导出驱动..."));
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    wchar_t sub[80] = {};
    swprintf(sub, 80, L"\\boot-%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute, st.wSecond);
    const std::wstring dir = AppDir() + L"\\Drivers" + sub;
    std::wstring err;
    int n = 0;
    const int rc = sysrecover::ExportBootCriticalDrivers(dir, &n, err);
    if (rc != 0) {
        SetStatus(Tr(L"导出启动驱动失败"));
        MessageBoxW(m_hWnd, (Tr(L"导出启动驱动失败：") + err).c_str(),
                    Tr(L"导出驱动"), MB_OK | MB_ICONERROR);
        return;
    }

    SetStatus(Tr(L"正在重新打包..."));
    std::string perr;
    int files = 0;
    // 用户 2026-11-03：GUI 里打出来的单文件要能直接双击 —— 用 GUI 子系统 stub
    //（没有控制台黑框）；没有就退回控制台版。
    std::wstring stub = AppDir() + L"\\bootfiles\\portable-stub-gui.exe";
    if (GetFileAttributesW(stub.c_str()) == INVALID_FILE_ATTRIBUTES)
        stub = AppDir() + L"\\bootfiles\\portable-stub.exe";
    if (!sysrecover::PackSelfToSingleFile(AppDir(), outPath, stub, perr, &files)) {
        SetStatus(Tr(L"重新打包失败"));
        MessageBoxW(m_hWnd, (Tr(L"重新打包失败：") + U2W(perr)).c_str(),
                    Tr(L"导出驱动"), MB_OK | MB_ICONERROR);
        return;
    }
    wchar_t done[900] = {};
    swprintf(done, 900,
             Tr(L"已导出 %d 个硬盘（存储/输入）驱动，并重新打包成：\n%ls\n\n"
                L"这个新文件已经带驱动，异机还原时会自动使用。"),
             n, outPath.c_str());
    SetStatus(Tr(L"驱动已导出并重新打包"));
    // N19（2026-11-07）：原来这里自动用 /select 打开资源管理器，Explorer 会抢走前台，
    // 用户会感觉"程序自己沉下去了"。改成用户显式点「打开所在文件夹」才打开。
    int openDir = CConfirmDlg::Ask2(m_hWnd, Tr(L"导出驱动"), done,
                                    Tr(L"完成"), Tr(L"打开所在文件夹"),
                                    /*defaultIsRight=*/false);
    if (openDir == 1) {
        ::ShellExecuteW(m_hWnd, L"open", L"explorer.exe",
                        (L"/select,\"" + outPath + L"\"").c_str(), nullptr,
                        SW_SHOWNORMAL);
    }
}

// 「镜像检查」= 完整校验（wimlib_verify_wim 全文件扫描，CLI 的 `verify` 命令同源）。
// 与备份收尾的 Probe（秒级快检，PIT-106）区分：这里用户显式要求，愿意等。
void CMainForm::StartVerifyImage() {
    if (m_busy) return;
    if (m_wimPath.empty() || !m_imageOk) {
        SetStatus(Tr(L"请先选择可用的镜像文件"));
        return;
    }
    // 大镜像（几十 GB）全量扫描要十几分钟，必须先讲清代价（默认项 = 取消）。
    // ⚠️ Ask2(owner, title, msg, ...) —— title 只能是**单行**（PIT-183）：
    // 2026-10-27 用户实测曾把整段提示写成 title，标题被裁成
    // 「将对整个镜像做完整性校验（逐字扫描）。几十GB的镜」且右边超出外框。
    if (CConfirmDlg::Ask2(
            m_hWnd, Tr(L"镜像检查"),
            Tr(L"将对整个镜像做完整性校验（逐字节扫描）。\n"
               L"几十 GB 的镜像可能需要十几分钟。\n"
               L"确定现在校验吗？"),
            Tr(L"取消"), Tr(L"开始校验"),
            /*defaultIsRight=*/false) != 1)
        return;
    m_busy = true;
    m_verifyMsg.clear();
    m_lastStageRaw.clear();
    m_lastStage = Tr(L"正在校验镜像...");
    SetStatus(m_lastStage);
    SetProgress(0);
    m_start = std::chrono::steady_clock::now();
    SetTimer(m_hWnd, 1, 1000, nullptr);
    UpdateMainAction();
    if (m_worker.joinable()) m_worker.join();
    m_cancel = false;
    try {
        m_worker = std::thread(&CMainForm::VerifyImageAsync, this);
    } catch (...) {
        m_busy = false;
        KillTimer(m_hWnd, 1);
        ShowProgress(false);
        UpdateMainAction();
        SetStatus(Tr(L"无法启动校验线程"));
    }
}

void CMainForm::VerifyImageAsync() {
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const std::wstring img = m_wimPath;  // busy 期间 UI 已锁，这里只是取快照
    auto progressFn = [this](int pct, const std::string& stage) -> bool {
        if (m_cancel) return true;   // 返回 true = 请求中止（与其它 worker 同约定）
        auto now = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now - m_lastProgressPost).count();
        const bool stageChanged = stage != m_lastStageRaw;
        if (ms < kProgressThrottleMs && pct < 100 && !stageChanged) return false;
        m_lastProgressPost = now;
        m_lastStageRaw = stage;
        PostMessage(WM_PROGRESS_UPDATE, (WPARAM)pct,
                    (LPARAM)new std::wstring(U2W(stage)));
        return false;
    };
    WimEngine wim;
    int rc = -1;
    if (wim.ok()) {
        std::wstring perr;
        bool ptmp = false;
        const std::wstring plain =
            sysrecover::ResolveImagePlain(img, progressFn, perr, &ptmp);
        if (!plain.empty()) {
            rc = wim.Verify(plain, progressFn);
            if (ptmp)
                sysrecover::ReleaseImagePlain(img);
        } else {
            m_verifyMsg = W2U(perr);
        }
    }
    if (rc == 0)
        m_verifyMsg.clear();
    else
        m_verifyMsg = W2U(WimEngine::ErrorString(rc));
    LogInfo("GUI verify rc=" + std::to_string(rc) +
            (m_verifyMsg.empty() ? "" : (" / " + m_verifyMsg)));
    ::PostMessageW(m_hWnd, WM_VERIFY_DONE, (WPARAM)rc, 0);
}

