// zj-restore-boot.exe —— “恢复引导菜单”首启清理器
//
// 日常还原（非「安装菜单」）把本程序投放到新系统 ProgramData，并由公共
// 启动文件夹的 cmd 在第一次登录时启动；启动时先弹说明框，再自行请求 UAC
// （非提权进程 → MessageBox → ShellExecute runas），删除**临时**引导项：
//   - UEFI：描述为 "SysRecover Temporary Restore" 的 Boot#### + BootNext；
//   - BIOS：临时 GUID 的 BCD 条目 + bootsequence；
//   - 若无常驻菜单，还清 ESP\EFI\ZJRESTORE 和系统盘根 boot 文件。
// 常驻菜单（"SysRecover" / 原 RecoveryGuid）绝不触碰。
//
// 不链接产品库（保持 x86 小体积、可在 x86/x64 目标运行）；bcdedit/mountvol
// 走 Sysnative（32 位进程在 64 位系统上必须避开 SysWOW64，PIT-082）。
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <shellapi.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "../boot/bcd.h"
#include "../boot/bootnames.h"
#include "../boot/cleanup_marker.h"

namespace {

using namespace sysrecover;

const wchar_t kGlobalGuid[] = L"{8BE4DF61-93CA-11D2-AA0D-00E098032B8C}";
const wchar_t kRunOnceName[] = L"\u6062\u590d\u5f15\u5bfc\u83dc\u5355";

std::wstring gLog;

void Log(const std::string& s) {
    if (gLog.empty()) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char ts[48];
    snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d ",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::string line = std::string(ts) + s + "\r\n";
    HANDLE h = CreateFileW(gLog.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        WriteFile(h, line.data(), (DWORD)line.size(), &wr, nullptr);
        CloseHandle(h);
    }
}

std::wstring ProgramData() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"ProgramData", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return buf;
    wchar_t win[MAX_PATH] = {};
    GetWindowsDirectoryW(win, MAX_PATH);
    std::wstring s = win;
    if (s.size() >= 2 && s[1] == L':') s = s.substr(0, 2);
    return s + L"\\ProgramData";
}

// 本程序在 ProgramData 下的固定子目录（%ProgramData%\SysRecover）。
constexpr wchar_t kProgramDataSubdir[] = L"\\SysRecover";

std::wstring Sys32(const wchar_t* exe) {
    wchar_t dir[MAX_PATH] = {};
    GetSystemDirectoryW(dir, MAX_PATH);
    std::wstring base = dir;
    // WOW64：32 位进程的 System32 被重定向；bcdedit/mountvol 只在原生目录。
    BOOL wow = FALSE;
    if (IsWow64Process(GetCurrentProcess(), &wow) && wow) {
        size_t p = base.find_last_of(L"\\/");
        if (p != std::wstring::npos) base = base.substr(0, p) + L"\\Sysnative";
    }
    return base + L"\\" + exe;
}

void EnableEnvPrivilege() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
        return;
    LUID luid{};
    if (LookupPrivilegeValueW(nullptr, SE_SYSTEM_ENVIRONMENT_NAME, &luid)) {
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    }
    CloseHandle(tok);
}

std::wstring VarName(int n) {
    wchar_t b[16];
    swprintf(b, 16, L"Boot%04X", n);
    return b;
}

bool ReadVar(const std::wstring& name, std::vector<BYTE>& out) {
    DWORD cap = 8192;
    out.resize(cap);
    DWORD n = GetFirmwareEnvironmentVariableW(name.c_str(), kGlobalGuid,
                                              out.data(), cap);
    if (n == 0) {
        out.clear();
        return false;
    }
    out.resize(n);
    return true;
}

bool WriteVar(const std::wstring& name, const void* data, DWORD size) {
    return SetFirmwareEnvironmentVariableW(name.c_str(), kGlobalGuid,
                                            const_cast<void*>(data), size) != 0;
}

bool DeleteVar(const std::wstring& name) {
    return SetFirmwareEnvironmentVariableW(name.c_str(), kGlobalGuid, nullptr,
                                            0) != 0;
}

std::wstring OptionDesc(const std::vector<BYTE>& b) {
    if (b.size() < 8) return {};
    WORD fpll = 0;
    memcpy(&fpll, &b[4], 2);
    (void)fpll;
    std::wstring d;
    size_t off = 6;
    while (off + 2 <= b.size()) {
        wchar_t c = 0;
        memcpy(&c, &b[off], 2);
        off += 2;
        if (c == 0) break;
        d.push_back(c);
    }
    return d;
}

// L5（1.0）：取 EFI_LOAD_OPTION 里的设备路径（描述之后、FilePathListLength 字节）。
std::vector<BYTE> OptionDevicePath(const std::vector<BYTE>& b) {
    if (b.size() < 8) return {};
    WORD fpll = 0;
    memcpy(&fpll, &b[4], 2);
    size_t off = 6;
    while (off + 2 <= b.size()) {
        wchar_t c = 0;
        memcpy(&c, &b[off], 2);
        off += 2;
        if (c == 0) break;
    }
    if (off + fpll > b.size()) return {};
    return std::vector<BYTE>(b.begin() + off, b.begin() + off + fpll);
}

// 设备路径里可打印的 ASCII 足够判归属（与 uefi.cpp 的 IsOurs 同一思路）。
bool PathContains(const std::vector<BYTE>& p, const char* needle) {
    if (p.empty()) return false;
    std::string s;
    s.reserve(p.size());
    for (BYTE c : p)
        if (c >= 0x20 && c < 0x7f) s.push_back((char)c);
    std::string hay = s, pat = needle;
    for (auto& c : hay) c = (char)tolower((unsigned char)c);
    for (auto& c : pat) c = (char)tolower((unsigned char)c);
    return hay.find(pat) != std::string::npos;
}

// L5：按「描述 + 设备路径」双条件匹配。0.9.96 只看描述，同名条目会误删。
// requireOurs=true 时要求路径带本项目特征（ZJRESTORE/bootmgfw），用于清理
// 我们自己创建的临时/菜单项；两条都满足才返回。
int FindUefiEntry(const wchar_t* desc, bool requireOurs) {
    std::vector<BYTE> order;
    if (!ReadVar(L"BootOrder", order)) return -1;
    for (size_t i = 0; i + 1 < order.size(); i += 2) {
        WORD v = 0;
        memcpy(&v, &order[i], 2);
        std::vector<BYTE> b;
        if (!ReadVar(VarName(v), b)) continue;
        if (OptionDesc(b) != desc) continue;
        if (requireOurs) {
            const auto path = OptionDevicePath(b);
            if (!PathContains(path, "zjrestore") &&
                !PathContains(path, "bootmgfw"))
                continue;
        }
        return v;
    }
    return -1;
}

// L5：返回值改为 bool —— NVRAM 写保护/写满时 WriteVar/DeleteVar 会失败，
// 0.9.96 忽略返回值，失败后仍打 "removed" 日志且上层当成功。
bool RemoveUefiEntry(int num) {
    if (num < 0) return true;
    bool ok = true;
    std::vector<BYTE> order;
    if (ReadVar(L"BootOrder", order)) {
        std::vector<WORD> keep;
        for (size_t i = 0; i + 1 < order.size(); i += 2) {
            WORD v = 0;
            memcpy(&v, &order[i], 2);
            if (v != (WORD)num) keep.push_back(v);
        }
        std::vector<BYTE> nb(keep.size() * 2);
        for (size_t i = 0; i < keep.size(); ++i)
            memcpy(&nb[i * 2], &keep[i], 2);
        if (!WriteVar(L"BootOrder", nb.data(), (DWORD)nb.size())) {
            ok = false;
            Log("ERROR: write BootOrder failed (NVRAM read-only/full?)");
        }
    }
    if (!DeleteVar(VarName(num))) {
        ok = false;
        Log("ERROR: delete Boot#### failed (NVRAM read-only/full?)");
    }
    WORD next = 0;
    DWORD n = GetFirmwareEnvironmentVariableW(L"BootNext", kGlobalGuid,
                                              &next, sizeof(next));
    if (n == sizeof(next) && next == (WORD)num &&
        !DeleteVar(L"BootNext")) {
        ok = false;
        Log("ERROR: delete BootNext failed");
    }
    Log(ok ? "uefi temporary entry removed"
           : "uefi temporary entry removal FAILED");
    return ok;
}

// 同步执行并抓 stdout（bcdedit 条目存在性、mountvol 不必抓）。
bool RunCapture(const std::wstring& exe, const std::wstring& args,
                std::string& out) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (ok) {
        char tmp[1024];
        DWORD n = 0;
        while (ReadFile(rd, tmp, sizeof(tmp), &n, nullptr) && n > 0)
            out.append(tmp, n);
        WaitForSingleObject(pi.hProcess, 30000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    CloseHandle(rd);
    return ok != FALSE;
}

bool RunNoCapture(const std::wstring& exe, const std::wstring& args) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!ok) return false;
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD rc = 1;
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return rc == 0;
}

bool BcdMenuExists() {
    std::string out;
    if (!RunCapture(Sys32(L"bcdedit.exe"), std::wstring(L"/enum ") + RecoveryGuid(),
                    out))
        return false;
    return out.find("{12345678-1234-1234-1234-123456789abc}") !=
           std::string::npos ||
           out.find("{12345678-1234-1234-1234-123456789ABC}") !=
               std::string::npos;
}

bool BcdTempExists() {
    std::string out;
    if (!RunCapture(Sys32(L"bcdedit.exe"),
                    std::wstring(L"/enum ") + TemporaryGuid(), out))
        return false;
    return out.find("{87654321-4321-4321-4321-cba987654321}") !=
           std::string::npos;
}

bool RemoveTree(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return true;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        return DeleteFileW(path.c_str()) != 0;
    }
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((path + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 ||
                wcscmp(fd.cFileName, L"..") == 0)
                continue;
            RemoveTree(path + L"\\" + fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    return RemoveDirectoryW(path.c_str()) != 0;
}

void RemoveEspRescue() {
    DWORD used = GetLogicalDrives();
    for (wchar_t c = L'S'; c <= L'Z'; ++c) {
        if (used & (1u << (c - L'A'))) continue;
        wchar_t root[4] = {c, L':', L'\\', 0};
        std::wstring args = std::wstring(1, c) + L": /s";
        if (!RunNoCapture(Sys32(L"mountvol.exe"), args)) continue;
        std::wstring dir = std::wstring(root) + L"EFI\\ZJRESTORE";
        if (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) {
            RemoveTree(dir);
            Log("ESP rescue dir removed");
        }
        RunNoCapture(Sys32(L"mountvol.exe"), std::wstring(1, c) + L": /d");
        return;
    }
    Log("ESP not mounted (no free letter or mountvol failed)");
}

void RemoveBcdTemp() {
    if (!BcdTempExists()) return;  // 不要动用户自己的 bootsequence
    RunNoCapture(Sys32(L"bcdedit.exe"),
                 std::wstring(L"/delete ") + TemporaryGuid() + L" /f");
    RunNoCapture(Sys32(L"bcdedit.exe"),
                 L"/deletevalue {bootmgr} bootsequence");
    Log("temporary BCD entry removed");
}

void RemoveSelfAndRunOnce() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
                      0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        RegDeleteValueW(k, kRunOnceName);
        RegCloseKey(k);
    }
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (!MoveFileExW(exe, nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) {
        // 延迟删除失败也不影响清理结果；下次可手工删除。
        Log("self delete schedule failed");
    }
}

bool IsElevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
        return false;
    TOKEN_ELEVATION te{};
    DWORD n = 0;
    BOOL ok = GetTokenInformation(tok, TokenElevation, &te, sizeof(te), &n);
    CloseHandle(tok);
    return ok && te.TokenIsElevated;
}

bool SelfElevate(const std::wstring& logDir) {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = L"--elevated";
    sei.lpDirectory = logDir.c_str();
    sei.nShow = SW_HIDE;
    SetLastError(0);
    if (!ShellExecuteExW(&sei)) {
        DWORD e = GetLastError();
        Log("elevation request failed err=" + std::to_string(e) +
            (e == ERROR_CANCELLED ? " (user cancelled UAC)" : ""));
        return false;
    }
    if (sei.hProcess) {
        // 等提权实例真正清理完，启动 cmd 才能看到 boot-cleanup.done 并立即自删；
        // 否则普通权限实例先退出，cmd 会在清理完成前误判失败/继续保留。
        DWORD w = WaitForSingleObject(sei.hProcess, 300000);
        DWORD ec = 0;
        GetExitCodeProcess(sei.hProcess, &ec);
        CloseHandle(sei.hProcess);
        Log("elevated cleanup wait=" + std::to_string(w) +
            " exit=" + std::to_string(ec));
    }
    return true;
}

// PIT-211：B 引擎的改机器名 —— Linux 侧只会把名字写进
// %ProgramData%\SysRecover\computer-name.txt，这里在首次登录（已提权）时落到
// 注册表。与 Rename-Computer 等价；新名字要**下次重启**才生效（Windows 机制）。
void ApplyComputerName() {
    const std::wstring p = ProgramData() + kProgramDataSubdir +
                           L"\\computer-name.txt";
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char buf[256] = {};
    DWORD got = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(h);
    std::wstring name;
    for (DWORD i = 0; i < got; ++i) {
        if (buf[i] == '\r' || buf[i] == '\n' || buf[i] == ' ') continue;
        if (!iswalnum((unsigned char)buf[i])) continue;  // ASCII 名，防御性过滤
        name.push_back((wchar_t)buf[i]);
    }
    if (name.empty()) {
        DeleteFileW(p.c_str());
        return;
    }
    auto setSz = [&](const std::wstring& sub, const wchar_t* value) -> bool {
        HKEY k = nullptr;
        const LONG open = RegCreateKeyExW(HKEY_LOCAL_MACHINE, sub.c_str(), 0,
                                          nullptr, 0, KEY_SET_VALUE, nullptr,
                                          &k, nullptr);
        if (open != ERROR_SUCCESS) {
            Log("computer name: open key failed err=" + std::to_string(open));
            return false;
        }
        const LONG wr =
            RegSetValueExW(k, value, 0, REG_SZ, (const BYTE*)name.c_str(),
                           (DWORD)((name.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
        if (wr != ERROR_SUCCESS) {
            Log("computer name: set " +
                std::string(value, value + std::wcslen(value)) +
                " failed err=" + std::to_string(wr));
            return false;
        }
        return true;
    };
    const std::wstring cs = L"SYSTEM\\CurrentControlSet";
    bool ok = true;
    if (!setSz(cs + L"\\Control\\ComputerName\\ComputerName",
               L"ComputerName"))
        ok = false;
    if (!setSz(cs + L"\\Control\\ComputerName\\ActiveComputerName",
               L"ComputerName"))
        ok = false;
    if (!setSz(cs + L"\\Services\\Tcpip\\Parameters", L"Hostname"))
        ok = false;
    if (!setSz(cs + L"\\Services\\Tcpip\\Parameters", L"NV Hostname"))
        ok = false;
    // 写失败时保留源文件：名字不丢，便于排查/下次重试；全部成功才删。
    if (!ok) {
        Log("computer name: registry write incomplete; source kept");
        return;
    }
    DeleteFileW(p.c_str());
    Log("computer name applied: " + std::string(name.begin(), name.end()) +
        " (effective after next reboot)");
}

void RemoveStartupCmd() {
    const std::wstring p =
        ProgramData() +
        L"\\Microsoft\\Windows\\Start Menu\\Programs\\Startup\\SysRecover-RestoreBoot.cmd";
    SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (DeleteFileW(p.c_str()))
        Log("startup cmd removed");
    else if (GetLastError() != ERROR_FILE_NOT_FOUND)
        Log("startup cmd delete failed err=" +
            std::to_string(GetLastError()));
}

// 标记必须带结果（ok=1 / ok=0）：Startup 的 cmd 只认 ok=1 才自删，
// ok=0 时保留 cmd 与 helper，下次登录重试。见 boot/cleanup_marker.h。
void WriteDoneMarker(bool ok) {
    const std::wstring p = ProgramData() + kProgramDataSubdir +
                           L"\\boot-cleanup.done";
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        // 写不出标记 = 上层永远判不了结果；至少日志要能看出来。
        Log("ERROR: done marker write failed (result=ok=" +
            std::string(ok ? "1" : "0") + ")");
        return;
    }
    const char* k = ok ? kCleanupMarkerOkText : kCleanupMarkerFailText;
    DWORD wr = 0;
    WriteFile(h, k, (DWORD)strlen(k), &wr, nullptr);
    CloseHandle(h);
}

// 清理失败必须可感知：NVRAM 只读/写满时框里给出下一步（自动重试 + 日志位置），
// 不再像 0.9.96 那样当作成功静默退出。
void NotifyCleanupFailure() {
    const wchar_t* msg =
        L"临时引导项清理未完成（NVRAM 可能被写保护或已写满）。\n\n"
        L"系统会在下次登录时自动重试；如多次失败，请查看\n"
        L"%ProgramData%\\SysRecover\\boot-cleanup.log 并以管理员身份运行\n"
        L"zj-restore-boot.exe。";
    MessageBoxW(nullptr, msg, L"SysRecover 引导清理",
                MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const bool elevatedArg =
        argc > 1 && wcscmp(argv[1], L"--elevated") == 0;
    const std::wstring pd = ProgramData() + kProgramDataSubdir;
    CreateDirectoryW((ProgramData()).c_str(), nullptr);
    CreateDirectoryW(pd.c_str(), nullptr);
    gLog = pd + L"\\boot-cleanup.log";

    // 先告知，再请求 UAC：本进程是普通权限（manifest=asInvoker），
    // 对话框结束后才 ShellExecute(runas)，所以 UAC 不会抢在提示之前。
    if (!elevatedArg && !IsElevated()) {
        Log("non-elevated start: show notice before UAC");
        const wchar_t* msg =
            L"本次还原使用了一次性启动菜单。\n"
            L"接下来会清理临时引导项，需要管理员权限。\n\n"
            L"点“确定”后，请在 Windows 的 UAC 提示中选择“是”。\n"
            L"如果取消，系统会在下次登录时再次提示。";
        int r = MessageBoxW(nullptr, msg, L"恢复引导菜单",
                            MB_OKCANCEL | MB_ICONINFORMATION | MB_SETFOREGROUND |
                                MB_TOPMOST);
        if (r != IDOK) {
            Log("user cancelled notice (no UAC requested)");
            return 0;
        }
        if (!SelfElevate(pd)) return 1;
        return 0;
    }

    Log("restore-boot cleanup start (elevated=" +
        std::string(elevatedArg ? "arg" : "token") + ")");
    EnableEnvPrivilege();

    ApplyComputerName();  // PIT-211：B 引擎的改机器名
    bool ok = true;
    // L5：双条件匹配（描述 + 设备路径），避免误删同名但非本项目的条目。
    const int temp = FindUefiEntry(kTempEntryDesc, true);
    const bool menuUefi = FindUefiEntry(kMenuEntryDesc, true) >= 0;
    if (!RemoveUefiEntry(temp)) {
        // 写入失败（NVRAM 只读/写满）→ 明确记录，不假装清理成功。
        ok = false;
        Log("WARN: temporary UEFI entry could not be fully removed");
    }
    RemoveBcdTemp();

    const bool menu = menuUefi || BcdMenuExists();
    if (menu) {
        Log("persistent menu present -> keep menu files/entry");
    } else {
        RemoveEspRescue();
        wchar_t win[MAX_PATH] = {};
        GetWindowsDirectoryW(win, MAX_PATH);
        std::wstring root = win;
        size_t p = root.find(L":\\");
        if (p != std::wstring::npos) root = root.substr(0, p + 2);
        for (const wchar_t* nm : {L"grldr", L"grldr.mbr", L"menu.lst"}) {
            std::wstring f = root + nm;
            SetFileAttributesW(f.c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(f.c_str());
        }
        Log("temporary boot files removed");
    }
    WriteDoneMarker(ok);
    Log("restore-boot cleanup done (menu=" + std::string(menu ? "1" : "0") +
        ", ok=" + std::string(ok ? "1" : "0") + ")");
    if (ok) {
        RemoveStartupCmd();
        RemoveSelfAndRunOnce();
    } else {
        // 失败：保留 cmd / helper / RunOnce，下次登录自动重试；同时给用户一个
        // 可感知的提示（0.9.96 静默失败，用户只看到"菜单还在"）。
        Log("cleanup incomplete: startup cmd + helper kept for retry");
        NotifyCleanupFailure();
    }
    // L5：把 NVRAM 写失败反映到退出码，上层（首启助手）可提示重试。
    return ok ? 0 : 1;
}
