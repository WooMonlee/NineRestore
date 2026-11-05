// 九转还原：ops_util（阶段 2 从 src/app/ops.cpp 机械拆分而来，逻辑未改）。
// 拆分动机：0.9.96 的 ops.cpp 约 4700 行，备份/暂存/就地/引导/卷操作全部
// 混在一个翻译单元里，无法按职责 review 或单测。本文件只搬代码，不改行为。
// 备份/还原共享操作层实现。语义对齐 CLI（Phase 2-4 已验证）与旧 C# 编排器。
#include "../common/i18n.h"
#include "../common/xpress.h"  // PIT-213：单文件载荷压缩
#include "ops.h"
#include "shortcut.h"
#include "swapleft.h"
#include "universal.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <vector>

#include "../boot/bcd.h"
#include "../boot/bootfix.h"
#include "../boot/bootpath.h"
#include "../boot/bootself.h"
#include "../boot/grub.h"
#include "../boot/task.h"
#include "../boot/uefi.h"
#include "../common/ea.h"
#include "../common/crypt.h"
#include "../common/logger.h"
#include "../common/pathutil.h"
#include "../common/cpucap.h"
#include "../common/process.h"
#include "../common/progress.h"
#include "../common/refscan.h"
#include "../common/relocate.h"
#include "../common/selfarch.h"
#include "../common/singleton.h"
#include "../common/vss.h"
#include "../common/version.h"
#include "../disk/disk.h"
#include "../disk/mbrcode.h"
#include "../wim/exclude.h"
#include "safety.h"
#include "selfdiag.h"
#include "imgsrc.h"

#include "ops_internal.h"

namespace sysrecover {

// 网络路径判定：UNC（\\server\share\...）或**映射网络驱动器**（DRIVE_REMOTE）。
// 用途：**暂存+重启**的还原由 Linux 救援层读镜像，而救援层**访问不到网络** →
// 网络镜像必然 image not found，必须在格式化之前就拦下（问题清单 D1/H1）。
bool IsNetworkPath(const std::wstring& p) {
    if (p.size() >= 2 && (p[0] == L'\\' || p[0] == L'/') &&
        (p[1] == L'\\' || p[1] == L'/'))
        return true;  // \\server\share\...
    if (p.size() >= 2 && p[1] == L':') {
        wchar_t root[4] = {p[0], L':', L'\\', 0};
        if (GetDriveTypeW(root) == DRIVE_REMOTE)
            return true;
    }
    return false;
}


// 用户显式「安装菜单」的标记。日常还原的临时项用另一个名字/GUID，
// 首启清理只认临时项；这个标记只用于“已安装常驻菜单”的判重，
// 避免一次日常还原静默覆盖用户菜单。
std::wstring MenuFlagPath() {
    return ContractDir() + L"\\restore-menu.flag";
}


bool HasInstalledMenu() {
    DWORD a = GetFileAttributesW(MenuFlagPath().c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}


void SetMenuFlag(bool installed) {
    std::wstring p = MenuFlagPath();
    if (!installed) {
        SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(p.c_str());
        return;
    }
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        LogWarn("menu flag write failed err=" + std::to_string(GetLastError()));
        return;
    }
    const char k[] = "installed\n";
    DWORD wr = 0;
    WriteFile(h, k, (DWORD)sizeof(k) - 1, &wr, nullptr);
    CloseHandle(h);
}


std::string W2U(const std::wstring& w) {
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0,
                                nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, 0);
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr,
                            nullptr);
    return s;
}


// UTF-8 → 宽串（读 restore-task.conf 里的路径用，conf 是 W2U 写出来的）。
std::wstring U2W(const std::string& s) {
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, 0);
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}


std::wstring ExePath() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}


bool IsDir(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}


// M9：给绝对路径加 \\?\ 前缀（UNC 用 \\?\UNC\），让 Win32 文件 API 支持
// 超过 260 字符的路径。已是长路径前缀/相对路径则原样返回（相对路径由调用方
// 先转绝对路径，这里不猜）。
std::wstring LongPath(const std::wstring& p) {
    if (p.size() >= 4 && p[0] == L'\\' && p[1] == L'\\' &&
        (p[2] == L'?' || p[2] == L'.'))
        return p;
    if (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\')
        return L"\\\\?\\UNC\\" + p.substr(2);
    if (p.size() >= 2 && ((p[0] >= L'A' && p[0] <= L'Z') ||
                          (p[0] >= L'a' && p[0] <= L'z')) &&
        p[1] == L':')
        return L"\\\\?\\" + p;
    return p;
}

// 递归删除目录树（EA 临时目录清理用；失败不致命）。
// 同 swapleft.cpp DeleteRec 的修法（PIT 用户 2026-10-15）：清属性 + 跳过 junction。
// M9：所有文件 API 走 LongPath()，深目录才能删干净。
void DeleteTreeRec(const std::wstring& dir) {
    const std::wstring d = LongPath(dir);
    SetFileAttributesW(d.c_str(), FILE_ATTRIBUTE_NORMAL);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((d + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 ||
                wcscmp(fd.cFileName, L"..") == 0)
                continue;
            const std::wstring p = d + L"\\" + fd.cFileName;
            // junction/symlink：只删链接本身，绝不递归进目标
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                RemoveDirectoryW(p.c_str()) || DeleteFileW(p.c_str());
                continue;
            }
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                DeleteTreeRec(p);  // LongPath 幂等，递归里再前缀不会叠加
                continue;
            }
            if (!DeleteFileW(p.c_str())) {
                SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(p.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    SetFileAttributesW(d.c_str(), FILE_ATTRIBUTE_NORMAL);
    RemoveDirectoryW(d.c_str());
}


// 递归复制目录（软件自保用；不处理 ACL）。
bool CopyDir(const std::wstring& src, const std::wstring& dst) {
    if (!IsDir(src))
        return false;
    CreateDirectoryW(dst.c_str(), nullptr);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((src + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    bool ok = true;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        std::wstring s = src + L"\\" + fd.cFileName;
        std::wstring d = dst + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            ok &= CopyDir(s, d);
        else if (!CopyFileW(s.c_str(), d.c_str(), FALSE))
            ok = false;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}


bool SameDrive(wchar_t a, wchar_t b) {
    return towupper(a) == towupper(b);
}

// 失败后的"下一步"（P8）已迁到 advice.cpp —— 见 advice.h。原因（PLAN §14 M1）：
// 本文件里旧版靠「中文关键词匹配中文消息」，消息一被翻译就全部失配 → 静默不给建议。
// 下面这行 = 把老签名**声明为已删除**：谁在本翻译单元里再按旧签名调用，编译期就报错。
std::string ErrorAdvice(int rc, const std::string& err) = delete;


// 历史记录（P7）：往 日志根\logs\history.jsonl 追加一行（JSON Lines，外部/AI 好读）。
// 日志根见 LogBaseDir（PIT-104/105：一般=程序目录；软件在系统盘时=数据盘 ZJRESTORE）。
// 记"什么时候做了什么"：备份完成、就地还原、暂存还原（暂存的实际结果在救援层日志里）。
void AppendHistory(const char* action, const std::wstring& image,
                   const std::wstring& target, unsigned long long ms,
                   const char* detail) {
    std::wstring dir = LogBaseDir() + L"\\logs";
    CreateDirectoryW(dir.c_str(), nullptr);
    HANDLE h = CreateFileW((dir + L"\\history.jsonl").c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    // 完整 JSON 字符串转义：`\` `"` 以及所有 < 0x20 的控制字符（\n\t\r\b\f 用短写，
    // 其余用 \u00xx）。原来只转义 `\` 和 `"` —— detail 一旦带换行就会写出非法 JSON
    //（问题清单 L-03）。
    auto esc = [](const std::string& s) {
        static const char* hx = "0123456789abcdef";
        std::string o;
        o.reserve(s.size() + 8);
        for (unsigned char c : s) {
            switch (c) {
                case '\\': o += "\\\\"; break;
                case '"': o += "\\\""; break;
                case '\n': o += "\\n"; break;
                case '\r': o += "\\r"; break;
                case '\t': o += "\\t"; break;
                case '\b': o += "\\b"; break;
                case '\f': o += "\\f"; break;
                default:
                    if (c < 0x20) {
                        o += "\\u00";
                        o += hx[(c >> 4) & 0xF];
                        o += hx[c & 0xF];
                    } else {
                        o += static_cast<char>(c);
                    }
            }
        }
        return o;
    };
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[1200];
    snprintf(buf, sizeof(buf),
             "{\"time\":\"%04u-%02u-%02u %02u:%02u:%02u\",\"action\":\"%s\","
             "\"image\":\"%s\",\"target\":\"%s\",\"elapsed_s\":%llu,"
             "\"version\":\"%s\",\"detail\":\"%s\"}\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             action, esc(W2U(image)).c_str(), esc(W2U(target)).c_str(),
             ms / 1000, SYSRECOVER_VERSION, detail ? detail : "");
    DWORD wrote = 0;
    WriteFile(h, buf, static_cast<DWORD>(strlen(buf)), &wrote, nullptr);
    CloseHandle(h);
}

}  // namespace sysrecover
