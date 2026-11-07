#include "selfdiag.h"

#include "ops.h"  // CheckStagedTaskState / CleanupStaleStagedTask（PIT-198）

#include <windows.h>
#include <shlobj.h>  // SHGetFolderPathW（桌面路径）

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <vector>

#include "wimlib.h"
#include "../boot/grub.h"  // MountEsp / UnmountEsp（ESP 日志收集）
#include "../boot/uefi.h"
#include "../common/crash.h"  // WriteProcessMiniDump / FindProcessIdsByName
#include "../common/i18n.h"
#include "../common/logger.h"
#include "../common/process.h"
#include "../common/refscan.h"
#include "../common/relocate.h"
#include "../common/rescue_decision.h"  // DecideRescueReport（纯逻辑，单测覆盖）
#include "../common/selfarch.h"
#include "../common/units.h"
#include "../common/version.h"
#include "../common/zip.h"
#include "../disk/disk.h"

namespace sysrecover {

namespace selfdiag_detail {

// 定义在 selfdiag.cpp（诊断报告与采集）；清理/扫描单元跨 TU 复用。
const std::vector<DiskInfo>& UseDisks(const std::vector<DiskInfo>* in,
                                      std::vector<DiskInfo>& cache);
std::string ToUtf8(const std::wstring& w);
bool WriteUtf8File(const std::wstring& path, const std::string& data);
int CollectOneDrive(wchar_t letter, const std::wstring& dstBase,
                    const std::vector<DiskInfo>* disks);
int CollectEspFiles(const std::wstring& dstBase);
std::wstring ZipStamp();
bool RemoveTreeAny(const std::wstring& dir);
int ZipAddTree(ZipWriter& zip, const std::wstring& dir,
               const std::wstring& prefix, int depth,
               const std::wstring& skipFull, int& added);
bool ExportEventLogsTo(const std::wstring& dir, int& n);
void FindStatusFiles(const std::wstring& dir, int depth,
                     std::vector<std::wstring>& out);
bool CaptureScreenBmp(const std::wstring& path);
bool ReadSmallFile(const std::wstring& path, std::string& text);
std::string StatusField(const std::string& text, const char* key);

}  // namespace selfdiag_detail
using namespace selfdiag_detail;

// ── 菜单模式自证 + 失败现场保留（2026-11-06）──────────────────────────
// 背景：菜单模式装完只写契约与常驻启动项，没有任何"救援层是否真的跑过"的
// 回执；用户选了菜单但救援没起来时，GUI 仍显示"已安装"，等于默认成功。
namespace {

std::wstring MenuMarkerPath() {
    return LogBaseDir() + L"\\logs\\menu-install.txt";
}
std::wstring MenuAckPath() { return LogBaseDir() + L"\\logs\\.menu-ack"; }

// 本次开机时刻（UTC FILETIME，100ns 单位）：现在 - 开机时长。
unsigned long long BootUtcFileTime() {
    FILETIME now = {};
    ::GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER u;
    u.LowPart = now.dwLowDateTime;
    u.HighPart = now.dwHighDateTime;
    const unsigned long long up = ::GetTickCount64() * 10000ull;
    return u.QuadPart > up ? u.QuadPart - up : 0;
}

bool FileCreationUtcFileTime(const std::wstring& path,
                             unsigned long long& out) {
    HANDLE h = CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    FILETIME create = {}, access = {}, write = {};
    const bool ok = GetFileTime(h, &create, &access, &write) != FALSE;
    CloseHandle(h);
    if (!ok)
        return false;
    ULARGE_INTEGER u;
    u.LowPart = create.dwLowDateTime;
    u.HighPart = create.dwHighDateTime;
    out = u.QuadPart;
    return true;
}

std::string LatestReceiptTime() {
    std::wstring base = LogBaseDir() + L"\\logs";
    std::vector<std::wstring> files;
    FindStatusFiles(base + L"\\collected", 4, files);
    FindStatusFiles(base, 1, files);
    std::string best;
    for (const auto& f : files) {
        std::string t;
        if (!ReadSmallFile(f, t))
            continue;
        const std::string tm = StatusField(t, "time");
        if (!tm.empty() && (best.empty() || tm > best))
            best = tm;
    }
    return best;
}

// 最新一条失败回执的去重键（与 CheckLastRescueFailure 同一算法）。
bool LatestFailedKey(std::string& key) {
    std::wstring base = LogBaseDir() + L"\\logs";
    std::vector<std::wstring> files;
    FindStatusFiles(base + L"\\collected", 4, files);
    FindStatusFiles(base, 1, files);
    std::string bestTime, bestStep, bestBuild;
    for (const auto& f : files) {
        std::string t;
        if (!ReadSmallFile(f, t))
            continue;
        const std::string tm = StatusField(t, "time");
        const std::string result = StatusField(t, "result");
        if (result != "FAILED" && result != "OK_BOOT_UNFIXED")
            continue;
        if (bestTime.empty() || tm > bestTime) {
            bestTime = tm;
            bestStep = result == "OK_BOOT_UNFIXED" ? "boot-unfixed"
                                                   : StatusField(t, "step");
            bestBuild = StatusField(t, "build");
        }
    }
    if (bestTime.empty())
        return false;
    key = bestTime + "|" + bestBuild + "|" + bestStep;
    return true;
}

}  // namespace

MenuRescueState CheckMenuRescueExecution(std::string& installTime,
                                         std::string& detail) {
    std::string body;
    if (!ReadSmallFile(MenuMarkerPath(), body))
        return MenuRescueState::NotInstalled;
    installTime = StatusField(body, "time");
    if (installTime.empty())
        return MenuRescueState::NotInstalled;  // 标记损坏：不下结论
    // 标记是本次开机之后写的 → 就是安装那一次，还没重启，先不下结论。
    unsigned long long ct = 0;
    if (FileCreationUtcFileTime(MenuMarkerPath(), ct) &&
        ct >= BootUtcFileTime())
        return MenuRescueState::SameBoot;
    const std::string latest = LatestReceiptTime();
    detail = "install=" + installTime +
             " latest=" + (latest.empty() ? std::string("-") : latest);
    if (!latest.empty() && latest > installTime)
        return MenuRescueState::Executed;  // 安装后确有新回执
    return MenuRescueState::NeverExecuted;  // 重启过 + 没有新回执 = 没被引导
}

void AcknowledgeMenuRescue() { WriteUtf8File(MenuAckPath(), "seen"); }

bool RescueEvidenceShouldBeKept() {
    // 1) 菜单装过、提示还没被用户看过：保留现场（否则下次启动擦掉唯一证据）。
    std::string marker;
    if (ReadSmallFile(MenuMarkerPath(), marker) &&
        GetFileAttributesW(MenuAckPath().c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::string it, d;
        if (CheckMenuRescueExecution(it, d) == MenuRescueState::NeverExecuted)
            return true;
    }
    // 2) 最新失败回执还没被 CheckLastRescueFailure 提示过（去重键不同）。
    std::string key;
    if (LatestFailedKey(key)) {
        std::string old;
        if (!ReadSmallFile(LogBaseDir() + L"\\logs\\.last-rescue-status", old))
            return true;
        while (!old.empty() && (old.back() == '\n' || old.back() == '\r' ||
                                old.back() == ' '))
            old.pop_back();
        return old != key;
    }
    return false;
}

// 收集后清理散落日志（用户 2026-10-04 规格 4：不该到处放这种东西）。
int CleanupStrayLogs() {
    int n = 0;
    // 2026-11-06：失败现场（盘根 status/last.log/三件套 + ZJRESTORE 残留 +
    // ESP 日志）在用户看过提示之前必须保留，不能"下次启动就把证据擦掉"。
    const bool keepEvidence = RescueEvidenceShouldBeKept();
    if (keepEvidence)
        LogInfo("rescue evidence kept (notice not acknowledged yet)");
    std::wstring activeBase = LogBaseDir();
    DWORD mask = GetLogicalDrives();
    for (wchar_t c = L'C'; c <= L'Z'; ++c) {
        if (!(mask & (1u << (c - L'A'))))
            continue;
        wchar_t rootBuf[5] = {c, L':', L'\\', 0};
        if (GetDriveTypeW(rootBuf) != DRIVE_FIXED)
            continue;
        std::wstring root = rootBuf;
        // PIT-135（2026-10-08 客户实测）：.out 三件套（apply/mkntfs/esp）此前
        // 不在清理清单 → 失败还原后所有分区根都残留（客户逐盘可见的"垃圾文件"）。
        for (const wchar_t* nm :
             {L"ZJRESTORE-last.log", L"ZJRESTORE-probe.txt",
              L"ZJRESTORE-status.txt", L"zjrestore-boot.log",
              L"ZJRESTORE-apply.out", L"ZJRESTORE-mkntfs.out",
              L"ZJRESTORE-esp.out"}) {
            if (keepEvidence)
                break;
            std::wstring p = root + nm;
            SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (DeleteFileW(p.c_str()))
                ++n;
        }
        if (keepEvidence)
            continue;
        std::wstring fb = root + L"ZJRESTORE-logs";
        if (GetFileAttributesW(fb.c_str()) != INVALID_FILE_ATTRIBUTES) {
            RemoveTreeAny(fb);
            ++n;
        }
        std::wstring zjr = root + L"ZJRESTORE";
        if (_wcsicmp(zjr.c_str(), activeBase.c_str()) != 0 &&
            GetFileAttributesW(zjr.c_str()) != INVALID_FILE_ATTRIBUTES) {
            std::wstring zl = zjr + L"\\logs";
            if (GetFileAttributesW(zl.c_str()) != INVALID_FILE_ATTRIBUTES) {
                RemoveTreeAny(zl);
                ++n;
            }
            // PIT-156（2026-10-18，用户规格）：日志已收进 collected → 整个残留
            // 文件夹按纯逻辑分类器处置：我方还原期重建的（ea/bootfix/source.txt…）
            // → 删掉，不再在盘上留 C:\ZJRESTORE；程序的家（SettleHome 副本）与
            // 已装启动菜单（boot/scripts）→ 保留。分类保守：看不懂就留。
            std::vector<std::wstring> top;
            WIN32_FIND_DATAW fd = {};
            HANDLE h = FindFirstFileW((zjr + L"\\*").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (!wcscmp(fd.cFileName, L".") ||
                        !wcscmp(fd.cFileName, L".."))
                        continue;
                    top.push_back(fd.cFileName);
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
            switch (sysrecover::ClassifyZjrResidue(top)) {
                case sysrecover::ZjrFolderKind::Delete:
                    RemoveTreeAny(zjr);
                    ++n;
                    LogInfo("residue folder removed: " + ToUtf8(zjr));
                    break;
                case sysrecover::ZjrFolderKind::Empty:
                    SetFileAttributesW(zjr.c_str(), FILE_ATTRIBUTE_NORMAL);
                    if (RemoveDirectoryW(zjr.c_str())) {
                        ++n;
                        LogInfo("empty residue folder removed: " + ToUtf8(zjr));
                    }
                    break;
                default:
                    break;  // Keep
            }
        }
    }
    // ESP：清 \EFI\ZJRESTORE\logs\*（救援模块本身保留）
    std::string log;
    std::wstring esp = keepEvidence ? std::wstring()
                                    : sysrecover::MountEsp(log);
    if (!esp.empty()) {
        for (const wchar_t* nm :
             {L"ZJRESTORE-last.log", L"ZJRESTORE-probe.txt",
              L"ZJRESTORE-status.txt", L"ZJRESTORE-mkntfs.out",
              L"ZJRESTORE-apply.out", L"ZJRESTORE-esp.out"}) {
            std::wstring p = esp + L"EFI\\ZJRESTORE\\logs\\" + nm;
            SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (DeleteFileW(p.c_str()))
                ++n;
        }
        sysrecover::UnmountEsp(esp, log);
    }
    if (n)
        LogInfo("stray logs cleaned after collect: " + std::to_string(n));
    return n;
}

int CollectDeployArtifacts(const std::wstring& dstDir, wchar_t onlyDrive,
                           const std::vector<DiskInfo>* disks) {
    // 先建 collected 这一级：否则 CreateDirectoryW("<logs>\\collected\\C") 会因
    // 父目录不存在而失败 → 收集静默全废（实测踩到）。
    CreateDirectoryW(dstDir.c_str(), nullptr);
    int n = 0;
    // 盘符 ↔ 物理身份对照表：PE 与正常 Windows 盘符不一致，靠这张表对号
    //（用户 2026-10-04 规格 1/6：LOG 里能查到本机所有盘/分区）。
    // dsCache：整条收集链共用**一次**枚举（含下面 CollectOneDrive 里的身份查询）。
    std::vector<DiskInfo> dsCache;
    {
        std::string map;
        char buf[512];
        for (const auto& d : UseDisks(disks, dsCache)) {
            snprintf(buf, sizeof(buf), "Disk %u  %s  serial=%s  %.1fGB  %s\n",
                     d.index, ToUtf8(d.model).c_str(),
                     ToUtf8(d.serial.empty() ? std::wstring(L"?") : d.serial)
                         .c_str(),
                     d.sizeBytes / kGiB,
                     sysrecover::StyleName(d.style));
            map += buf;
            for (const auto& p : d.parts) {
                snprintf(buf, sizeof(buf),
                         "  p%-2u off=%-12llu size=%-12llu letter=%-3s fs=%-7s "
                         "label=%s%s%s%s%s\n",
                         p.partNumber, (unsigned long long)p.offsetBytes,
                         (unsigned long long)p.sizeBytes,
                         p.letter.empty() ? "-" : ToUtf8(p.letter).c_str(),
                         ToUtf8(p.fs).c_str(), ToUtf8(p.label).c_str(),
                         p.isSystem ? " [System]" : "", p.isEsp ? " [ESP]" : "",
                         p.isMsr ? " [MSR]" : "",
                         p.isRecovery ? " [Recovery]" : "");
                map += buf;
            }
        }
        if (WriteUtf8File(dstDir + L"\\drive-map.txt", map))
            ++n;
    }
    if (onlyDrive) {
        n += CollectOneDrive((wchar_t)towupper(onlyDrive), dstDir, &dsCache);
    } else {
        DWORD mask = GetLogicalDrives();
        for (wchar_t c = L'C'; c <= L'Z'; ++c)
            if (mask & (1u << (c - L'A')))
                n += CollectOneDrive(c, dstDir, &dsCache);
    }
    // ESP 无盘符：mountvol 临时挂上收（含黑匣子回执）
    n += CollectEspFiles(dstDir);
    if (n)
        LogInfo("deploy artifacts collected: " + std::to_string(n) +
                " file(s) -> " + ToUtf8(dstDir));
    return n;
}

SupportBundle BuildSupportBundle(const std::wstring& outZip) {
    SupportBundle r;
    std::wstring logsDir = LogBaseDir() + L"\\logs";
    CreateDirectoryW(logsDir.c_str(), nullptr);

    // 1) 先把各盘部署痕迹收进 logs\collected（0.6.18 收集器，幂等）
    CollectDeployArtifacts(logsDir + L"\\collected", 0);

    // 1b) 屏幕截图（用户 2026-10-05 规格）：现场画面进 logs\screens\（随 logs
    //     递归进包；BMP 零依赖）。抓不到只记警告，不影响出包。
    {
        std::wstring scr = logsDir + L"\\screens";
        CreateDirectoryW(scr.c_str(), nullptr);
        std::wstring shot = scr + L"\\screen-" + ZipStamp() + L".bmp";
        if (CaptureScreenBmp(shot))
            LogInfo("screen shot: " + ToUtf8(shot));
        else
            LogWarn("screen shot failed");
    }

    // 2) 暂存目录（dumps/events）放 %TEMP%，避免被打进 logs 自身
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring stage = std::wstring(tmp) + L"zj-support-" +
                         std::to_wstring(GetCurrentProcessId()) + L"-" +
                         std::to_wstring(GetTickCount64());
    CreateDirectoryW(stage.c_str(), nullptr);

    // 3) explorer 转储（含全部线程栈 → 看等待链卡在哪）
    std::wstring dumpDir = stage + L"\\dumps";
    CreateDirectoryW(dumpDir.c_str(), nullptr);
    for (unsigned long pid : FindProcessIdsByName(L"explorer.exe")) {
        wchar_t nm[64] = {};
        swprintf(nm, 64, L"explorer-%lu.dmp", pid);
        if (WriteProcessMiniDump(pid, dumpDir + L"\\" + nm))
            ++r.dumps;
    }

    // 4) 事件日志（Application Hang/Error、System 存储/错误级）
    int ev = 0;
    bool evOk = ExportEventLogsTo(stage + L"\\events", ev);
    r.eventFiles = ev;

    // 5) 输出路径：桌面；拿不到桌面则回退日志目录
    std::wstring zipPath = outZip;
    if (zipPath.empty()) {
        wchar_t desk[MAX_PATH] = {};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr,
                                       0, desk)) &&
            desk[0])
            zipPath = std::wstring(desk) + L"\\SysRecover-logs-" + ZipStamp() +
                      L".zip";
        else
            zipPath = logsDir + L"\\SysRecover-logs-" + ZipStamp() + L".zip";
    }
    {  // --out 指向的目录可能不存在：建一层（尽力）
        size_t p = zipPath.find_last_of(L'\\');
        if (p != std::wstring::npos && p > 3)
            CreateDirectoryW(zipPath.substr(0, p).c_str(), nullptr);
    }

    sysrecover::ZipWriter zip(zipPath);
    if (!zip.ok()) {
        r.error = Tr(L"无法创建诊断包文件");
        RemoveTreeAny(stage);
        return r;
    }
    int n = 0;
    {
        std::string t;
        DiagReportText(t);
        if (zip.AddData(t.data(), t.size(), "diag.txt"))
            ++n;
    }
    {
        std::string t;
        DiskListText(t);
        if (zip.AddData(t.data(), t.size(), "list.txt"))
            ++n;
    }
    {
        std::wstring v = AppDir() + L"\\version.json";
        if (GetFileAttributesW(v.c_str()) != INVALID_FILE_ATTRIBUTES &&
            zip.AddFile(v, "version.json"))
            ++n;
    }

    // 当前日志目录（递归；跳过输出包自身，防自吞）
    ZipAddTree(zip, logsDir, L"logs", 8, zipPath, n);
    // GUI 与 CLI 在部分 PE 下可能选中不同的日志根（GUI 主日志在程序目录、
    // CLI support 在数据盘）；把程序目录 logs 也收进来，避免主日志缺失。
    {
        std::wstring appLogs = AppDir() + L"\\logs";
        if (_wcsicmp(appLogs.c_str(), logsDir.c_str()) != 0 &&
            GetFileAttributesW(appLogs.c_str()) != INVALID_FILE_ATTRIBUTES)
            ZipAddTree(zip, appLogs, L"app-logs", 8, zipPath, n);
    }
    // 其它固定盘的 ZJRESTORE\logs（覆盖"单文件/temp 运行、日志落数据盘"的情况）
    {
        DWORD mask = GetLogicalDrives();
        for (wchar_t c = L'C'; c <= L'Z'; ++c) {
            if (!(mask & (1u << (c - L'A'))))
                continue;
            wchar_t root[4] = {c, L':', L'\\', 0};
            if (GetDriveTypeW(root) != DRIVE_FIXED)
                continue;
            std::wstring fl = std::wstring(root) + L"ZJRESTORE\\logs";
            if (GetFileAttributesW(fl.c_str()) == INVALID_FILE_ATTRIBUTES)
                continue;
            if (_wcsicmp(fl.c_str(), logsDir.c_str()) == 0)
                continue;
            ZipAddTree(zip, fl,
                       std::wstring(L"other/") + c + L"/ZJRESTORE/logs", 8,
                       zipPath, n);
        }
    }
    // 暂存目录（dumps/events）
    ZipAddTree(zip, stage, L"", 8, zipPath, n);

    // 包说明（人/机都好读）
    {
        SYSTEMTIME st = {};
        GetLocalTime(&st);
        char tbuf[64] = {};
        snprintf(tbuf, sizeof(tbuf), "%04u-%02u-%02u %02u:%02u:%02u", st.wYear,
                 st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        std::string info;
        info += "SysRecover support bundle\n";
        info += "version  : " SYSRECOVER_VERSION "\n";
        info += "time     : ";
        info += tbuf;
        info += "\n";
        info += "exe_dir  : " + ToUtf8(AppDir()) + "\n";
        info += "log_dir  : " + ToUtf8(logsDir) + "\n";
        info += "zip      : " + ToUtf8(zipPath) + "\n";
        info += "dumps    : " + std::to_string(r.dumps) + "\n";
        info += "events   : " + std::to_string(r.eventFiles) +
                (evOk ? "" : " (wevtutil unavailable)") + "\n";
        info += "logfiles : " + std::to_string(n) + "\n";
        zip.AddData(info.data(), info.size(), "bundle-info.txt");
    }

    if (!zip.Close()) {
        r.error = Tr(L"写诊断包失败");
        RemoveTreeAny(stage);
        return r;
    }
    RemoveTreeAny(stage);
    r.logFiles = n;
    r.zipPath = zipPath;
    LogInfo("support bundle: " + ToUtf8(zipPath) +
            " logs=" + std::to_string(n) + " dumps=" + std::to_string(r.dumps) +
            " events=" + std::to_string(r.eventFiles));
    // 出包成功 → 清掉散落在各盘的日志（用户 2026-10-04 规格 4：收集后不该
    // 到处放）。只删已知日志文件/目录，契约与引导文件不动。
    CleanupStrayLogs();
    return r;
}

// ── EA（NTFS 扩展属性）扫描（用户 2026-10-05）────────────────────────────
// 背景：客户 Linux 侧还原的 apply.out 报 `Ignoring extended attributes of
// 804 files` —— EA 只在 Linux 还原路径丢失，Windows（PE 就地）保留。此命令
// 列出"哪些文件带 EA、EA 叫什么"，供取证（在 PE 还原后的系统上跑一次）。
extern "C" long __stdcall NtQueryEaFile(HANDLE, void*, void*, unsigned long,
                                        unsigned char, void*, unsigned long,
                                        unsigned long*, unsigned char);
namespace {
const long kStatusBufferOverflow = (long)0x80000005;

void EaScanRec(const std::wstring& dir, std::string& out,
               unsigned long long& files, unsigned long long& hit,
               bool& capped) {
    static std::vector<unsigned char> buf;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        std::wstring p = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                EaScanRec(p, out, files, hit, capped);
            continue;
        }
        ++files;
        HANDLE f = CreateFileW(p.c_str(), FILE_READ_EA | FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE |
                                   FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (f == INVALID_HANDLE_VALUE)
            continue;
        if (buf.empty())
            buf.assign(256 * 1024, 0);
        struct {
            long status;
            void* info;
        } iosb = {};
        long st = NtQueryEaFile(f, &iosb, buf.data(), (unsigned long)buf.size(),
                                FALSE, nullptr, 0, nullptr, FALSE);
        CloseHandle(f);
        if (st != 0 && st != kStatusBufferOverflow)
            continue;  // 没有 EA / 无权限
        ++hit;
        if (capped)
            continue;
        if (out.size() > (8u << 20)) {  // 输出上限 8MB（防爆内存）
            capped = true;
            continue;
        }
        out += ToUtf8(p);
        if (st == kStatusBufferOverflow) {
            out += " | (EA too large, names truncated)\r\n";
            continue;
        }
        unsigned long off = 0;
        for (;;) {
            unsigned char* e = buf.data() + off;
            unsigned long next = 0;
            memcpy(&next, e, sizeof(next));
            unsigned char nameLen = e[5];
            unsigned short valLen = 0;
            memcpy(&valLen, e + 6, sizeof(valLen));
            out += " | ";
            out.append((char*)e + 8, nameLen);
            out += "=";
            size_t vn = valLen < 32 ? valLen : 32;
            for (size_t i = 0; i < vn; ++i) {
                char b[4];
                snprintf(b, sizeof(b), "%02X", e[8 + nameLen + 1 + i]);
                out += b;
            }
            if (valLen > 32)
                out += "..";
            if (next == 0 || (off + next + 8) >= buf.size())
                break;
            off += next;
        }
        out += "\r\n";
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
}  // namespace

int ScanEaFiles(const std::wstring& root, const std::wstring& outPath,
                std::string& err) {
    if (GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES) {
        err = "root not found";
        return -1;
    }
    std::string out;
    out += "# SysRecover scan-ea " SYSRECOVER_VERSION " root=" + ToUtf8(root) +
           "\r\n";
    unsigned long long files = 0, hit = 0;
    bool capped = false;
    EaScanRec(root, out, files, hit, capped);
    out += "# files=" + std::to_string(files) +
           " withEA=" + std::to_string(hit) +
           (capped ? " (listing capped)" : "") + "\r\n";
    std::wstring dst =
        outPath.empty() ? (LogBaseDir() + L"\\logs\\ea-scan.txt") : outPath;
    CreateDirectoryW((LogBaseDir() + L"\\logs").c_str(), nullptr);
    if (!WriteUtf8File(dst, out)) {
        err = "write failed";
        return -1;
    }
    LogInfo("ea-scan: files=" + std::to_string(files) +
            " withEA=" + std::to_string(hit) + " -> " + ToUtf8(dst));
    return (int)hit;
}

}  // namespace sysrecover
