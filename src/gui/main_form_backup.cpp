// 拆分自 src/gui/main_form.cpp（按职责）：备份流程（UI 校验 + worker 线程热备）。
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

// ────────────────── 备份（UI 校验 + worker 线程热备） ──────────────────

void CMainForm::StartBackup() {
    int fmt = BackupFmt();
    const char* compress = fmt == 0 ? "recovery" : (fmt == 1 ? "maximum" : "fast");
    // 目标路径
    std::wstring dest = m_backupPath;
    if (dest.empty()) {
        std::wstring dataDrive = FindDataDrive();
        if (dataDrive.empty()) {
            SetStatus(Tr(L"未找到数据盘，请用浏览指定保存位置")); return;
        }
        std::wstring dir = dataDrive + kRecoveryDir;
        CreateDirectoryW(dir.c_str(), nullptr);
        dest = dir + L"\\" + TimestampDate() +
               (m_sysDesc.shortTag.empty() ? TimestampName()
                                           : m_sysDesc.shortTag) +
               Tr(L"备份") +
               (fmt == 0 ? L".esd" : L".wim");
    }
    // 「ESP」勾选（备份专属）：只读状态，起线程前定格到 m_workerEsp
    // 加密备份（docs/30）：口令非空 → 输出改成 .zje（容器），落盘后不留明文。
    const std::wstring pwd = m_backupPassword;
    if (!pwd.empty()) {
        size_t slash = dest.find_last_of(L"\\/");
        size_t base = (slash == std::wstring::npos) ? 0 : slash + 1;
        size_t dot = dest.find_last_of(L'.');
        dest = (dot != std::wstring::npos && dot > base)
                   ? dest.substr(0, dot) + L".zje"
                   : dest + L".zje";
    } else if (dest.size() >= 4 &&
               _wcsicmp(dest.c_str() + dest.size() - 4, L".zje") == 0) {
        // 关掉加密后再备份：文件名要回到 .wim/.esd，否则会出现"名叫 .zje
        // 但内容其实是明文 WIM"的怪文件（虽然按 magic 能认出来）。
        dest = dest.substr(0, dest.size() - 4) + (fmt == 0 ? L".esd" : L".wim");
    }
    m_backupPath = dest;
    SetImagePathEdit(m_backupPath);
    bool esp = IsEspChecked();
    if (GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::wstring msg = Tr(L"目标镜像文件已存在，覆盖？");
        if (MessageBoxW(m_hWnd, msg.c_str(), Tr(L"确认备份"),
                        MB_YESNO | MB_ICONWARNING) != IDYES)
            return;
    }
    // 子镜像名
    std::wstring name = L"Backup";
    CControlUI* pNote = m_PaintManager.FindControl(_T("NoteInput"));
    if (pNote) {
        CDuiString txt = pNote->GetText();
        if (!txt.IsEmpty()) name = txt.GetData();
    }
    m_skippedFiles.clear();
    m_busy = true;
    UpdateMainAction();
    SetStatus(Tr(L"正在备份..."));
    SetProgress(0);
    m_start = std::chrono::steady_clock::now();
    SetTimer(m_hWnd, 1, 1000, nullptr);
    LogInfo(std::string("GUI backup start: ") + W2U(dest));    if (m_worker.joinable()) { m_worker.join(); }
    m_cancel = false;
    // 源路径：使用选中分区的盘符（备份模式下用户已选分区）
    std::wstring source = L"C:/";
    if (m_selPart >= 0 && m_selPart < (int)m_parts.size()) {
        const auto& part = m_parts[m_selPart];
        if (!part.letter.empty()) {
            source = part.letter + L":/";
        }
    }
    BeginBackupWorker(source, dest, compress, name, esp, /*append=*/false, pwd);
}

// ── 增量备份（追加到当前镜像，2026-10-28 用户规格 / docs/31）──────────────
// WIM/ESD 的多子镜像 = **文件级去重快照**：未变更的文件在各时间点之间共享，
// 改动过的文件整份重存；旧时间点保留（只增不减）。入口按用户要求放「更多功能>>」。
void CMainForm::StartIncrementalBackup() {
    if (m_busy) return;
    if (!m_backupMode) {
        SetStatus(Tr(L"增量备份请在「系统→文件」模式使用"));
        return;
    }
    const std::wstring dest = m_backupPath;
    if (dest.empty()) {
        SetStatus(Tr(L"请先选择已存在的镜像文件"));
        return;
    }
    if (GetFileAttributesW(dest.c_str()) == INVALID_FILE_ATTRIBUTES) {
        SetStatus(Tr(L"增量备份需要一个已存在的镜像文件"));
        MessageBoxW(m_hWnd, Tr(L"增量备份需要一个已存在的镜像文件。\n请先把「保存位置」指向已备份过的 .wim/.esd。"),
                    Tr(L"增量备份"), MB_OK | MB_ICONINFORMATION);
        return;
    }
    const wchar_t* dot = wcsrchr(dest.c_str(), L'.');
    if (!dot || (_wcsicmp(dot, L".wim") != 0 && _wcsicmp(dot, L".esd") != 0)) {
        SetStatus(Tr(L"增量备份只支持 .wim / .esd 镜像"));
        return;
    }
    // 秒级快检 + 现有时间点数（只读元数据、不解压数据）
    sysrecover::WimEngine eng;
    std::wstring why;
    std::vector<sysrecover::ImageDesc> imgs;
    if (!eng.ok() || eng.Probe(dest, why) != 0 ||
        eng.ListImages(dest, imgs) != 0 || imgs.empty()) {
        std::wstring msg = Tr(L"该文件不是可用的镜像：") +
                           (why.empty() ? dest : why);
        SetStatus(msg);
        MessageBoxW(m_hWnd, msg.c_str(), Tr(L"增量备份"),
                    MB_OK | MB_ICONERROR);
        return;
    }
    const int n = static_cast<int>(imgs.size());
    // 单个时间点的含义必须先讲清（避免用户以为"只存差异块"）。
    wchar_t head[768] = {};
    swprintf(head, 768,
             Tr(L"将把当前分区的新状态作为第 %d 个时间点追加到：\n%ls\n\n"
                L"未变更的文件不重复占空间；改动过的文件会整份重存。\n"
                L"旧时间点全部保留（镜像只增不减），还原时可选任一版本。\n\n"
                L"确定开始追加吗？"),
             n + 1, dest.c_str());
    if (CConfirmDlg::Ask2(m_hWnd, Tr(L"增量备份"), head, Tr(L"取消"),
                          Tr(L"开始追加"), /*defaultIsRight=*/false) != 1)
        return;

    const char* compress =
        BackupFmt() == 0 ? "recovery" : (BackupFmt() == 1 ? "maximum" : "fast");
    // 子镜像名沿用 "Backup"（wimlib 自动去重为 "Backup (N)"）。
    std::wstring name = L"Backup";
    CControlUI* pNote = m_PaintManager.FindControl(_T("NoteInput"));
    if (pNote) {
        CDuiString txt = pNote->GetText();
        if (!txt.IsEmpty()) name = txt.GetData();
    }
    std::wstring source = L"C:/";
    if (m_selPart >= 0 && m_selPart < (int)m_parts.size()) {
        const auto& part = m_parts[m_selPart];
        if (!part.letter.empty()) source = part.letter + L":/";
    }
    m_skippedFiles.clear();
    m_busy = true;
    UpdateMainAction();
    SetStatus(Tr(L"正在追加时间点..."));
    SetProgress(0);
    m_start = std::chrono::steady_clock::now();
    SetTimer(m_hWnd, 1, 1000, nullptr);
    LogInfo(std::string("GUI incremental backup start: ") + W2U(dest) +
            " (timepoint " + std::to_string(n + 1) + ")");
    if (m_worker.joinable()) m_worker.join();
    m_cancel = false;
    // ESP 子镜像不参与增量（每次追加会再塞一个 ESP 子镜像，把 esp_index 弄乱）。
    BeginBackupWorker(source, dest, compress, name, /*esp=*/false,
                      /*append=*/true, std::wstring());
}

// 备份/增量共用：起线程前把参数定格到成员（busy 期间 UI 已锁，防漂移）。
void CMainForm::BeginBackupWorker(const std::wstring& source,
                                  const std::wstring& dest,
                                  const std::string& compress,
                                  const std::wstring& name, bool esp,
                                  bool append, const std::wstring& password) {
    m_workerSource = source;
    m_workerDest = dest;
    m_workerCompress = compress;
    m_workerName = name;
    m_workerPassword = password;
    m_workerCpuCap = m_cpuCap;  // 起线程前定格（备份中再改走 SetCpuCap 直接生效）
    m_workerEsp = esp;
    m_workerAppend = append;
    m_lastProgressPost = std::chrono::steady_clock::now();
    m_lastStageRaw.clear();
    m_backupTimepoints = 0;
    m_backupTotalBytes = 0;
    try {
        m_worker = std::thread(&CMainForm::StartBackupAsync, this);
    } catch (...) {
        m_busy = false;
        UpdateMainAction();
        SetStatus(Tr(L"线程创建失败"));
        MessageBoxW(m_hWnd, Tr(L"无法创建工作线程，请重试"), Tr(L"错误"), MB_OK | MB_ICONERROR);
    }
}

void CMainForm::StartBackupAsync() {
    // 同 StartRestoreAsync：备份线程降到 BELOW_NORMAL，保证界面点击/弹框跟手。
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    BackupRequest req;
    req.source  = m_workerSource;
    req.dest    = m_workerDest;
    req.compress = m_workerCompress;
    req.name    = m_workerName;
    req.append  = m_workerAppend;
    req.cpuCap  = m_workerCpuCap;
    req.esp     = m_workerEsp;
    req.password = m_workerPassword;
    auto progressFn = [this](int pct, const std::string& stage) -> bool {
        if (m_cancel) return true;  // 取消信号
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastProgressPost).count();
        // 阶段名一变就发（不受 100ms 节流），理由见还原侧同款注释（PIT-098）。
        bool stageChanged = stage != m_lastStageRaw;
        if (elapsed < kProgressThrottleMs && pct < 100 && !stageChanged) return false;
        m_lastProgressPost = now;
        m_lastStageRaw = stage;
        std::wstring* pStage = new std::wstring(U2W(stage));
        PostMessage(WM_PROGRESS_UPDATE, (WPARAM)pct, (LPARAM)pStage);
        return false;
    };
    std::string err;
    std::vector<std::wstring> skipped;
    int rc = RunBackup(req, progressFn, err, &last_adv_, &skipped);
    m_skippedFiles = skipped;
    last_err_ = err;
    // 加密备份成功后把口令登记到进程表：切到还原页即可直接列出子镜像。
    if (rc == 0 && !m_workerPassword.empty())
        sysrecover::SetImagePassword(m_workerDest, m_workerPassword);
    // 增量完成后回主线程显示"第 N 个时间点 / 合计 X"（只读元数据，秒级）。
    if (rc == 0 && m_workerAppend) {
        sysrecover::WimEngine eng;
        std::vector<sysrecover::ImageDesc> imgs;
        if (eng.ok() && eng.ListImages(m_workerDest, imgs) == 0)
            m_backupTimepoints = static_cast<int>(imgs.size());
        WIN32_FILE_ATTRIBUTE_DATA fad = {};
        if (GetFileAttributesExW(m_workerDest.c_str(), GetFileExInfoStandard,
                                 &fad))
            m_backupTotalBytes =
                (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) |
                fad.nFileSizeLow;
    }
    PostMessage(WM_TASK_COMPLETE, (WPARAM)rc, 0);
}

