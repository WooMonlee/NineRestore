// 九转还原：ops_selfpack（阶段 2 从 src/app/ops.cpp 机械拆分而来，逻辑未改）。
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

// ── PIT-207：自我打包 / 解包 ──────────────────────────────────────────
namespace {

bool SelfPackSkip(const std::wstring& name) {
    std::wstring low = name;
    for (auto& c : low) c = (wchar_t)towlower(c);
    if (low == L"logs" || low == L".zj-stamp") return true;
    if (low == L"sha256sums.txt") return true;
    if (low.rfind(L"sysrecover-portable", 0) == 0) return true;
    return false;
}

unsigned long long FileTime64(const FILETIME& ft) {
    return ((unsigned long long)ft.dwHighDateTime << 32) |
           (unsigned long long)ft.dwLowDateTime;
}

bool WriteAll(HANDLE h, const void* p, DWORD n) {
    DWORD wr = 0;
    return WriteFile(h, p, n, &wr, nullptr) && wr == n;
}

bool PackDir(HANDLE out, const std::wstring& dir, const std::wstring& rel,
             std::string& err, int* count, int depth,
             const std::wstring& skipFull) {
    if (depth > 32) return true;
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return true;
    bool ok = true;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        if (SelfPackSkip(fd.cFileName)) continue;
        const std::wstring full = dir + L"\\" + fd.cFileName;
        if (!skipFull.empty() && _wcsicmp(full.c_str(), skipFull.c_str()) == 0)
            continue;  // 输出文件本身（用户可能就把单文件存进程序目录）
        const std::wstring r = rel.empty() ? fd.cFileName : rel + L"/" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ok &= PackDir(out, full, r, err, count, depth + 1, skipFull);
            continue;
        }
        HANDLE f = CreateFileW(full.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;  // 跳过打不开的（占用/权限）
        LARGE_INTEGER li = {};
        GetFileSizeEx(f, &li);
        const size_t rawLen = (size_t)li.QuadPart;
        std::vector<unsigned char> raw(rawLen);
        DWORD got = 0;
        bool readOk = rawLen == 0 ||
                      (ReadFile(f, raw.data(), (DWORD)rawLen, &got, nullptr) &&
                       got == rawLen);
        CloseHandle(f);
        if (!readOk) continue;
        // name -> UTF-8
        int n8 = WideCharToMultiByte(CP_UTF8, 0, r.c_str(), -1, nullptr, 0,
                                     nullptr, nullptr);
        std::string name(n8 > 0 ? n8 - 1 : 0, 0);
        if (n8 > 0)
            WideCharToMultiByte(CP_UTF8, 0, r.c_str(), -1, &name[0], n8, nullptr,
                                nullptr);
        const unsigned int nameLen = (unsigned int)name.size();
        const unsigned long long mtime = FileTime64(fd.ftLastWriteTime);
        // PIT-213：能压就压（ZJF2），压不动（已经压缩过的内核/initramfs）就存原样
        std::vector<unsigned char> packed;
        const bool compressed = xpress::Compress(raw, packed);
        const unsigned long long storedLen =
            compressed ? (unsigned long long)packed.size() : rawLen;
        const char* magic = compressed ? "ZJF2" : "ZJF1";
        const unsigned long long rawLen64 = rawLen;
        if (!WriteAll(out, magic, 4) || !WriteAll(out, &nameLen, 4) ||
            !WriteAll(out, &storedLen, 8) || !WriteAll(out, &rawLen64, 8) ||
            !WriteAll(out, &mtime, 8) || !WriteAll(out, name.data(), nameLen)) {
            err = "write header failed";
            ok = false;
            break;
        }
        if (compressed) {
            if (!WriteAll(out, packed.data(), (DWORD)packed.size())) {
                err = "write data failed";
                ok = false;
                break;
            }
        } else if (rawLen > 0) {
            if (!WriteAll(out, raw.data(), (DWORD)raw.size())) {
                err = "write data failed";
                ok = false;
                break;
            }
        }
        if (!ok) break;
        if (count) ++(*count);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

}  // namespace

bool PackSelfToSingleFile(const std::wstring& appDir,
                          const std::wstring& outFile,
                          const std::wstring& stubPath, std::string& err,
                          int* fileCount) {
    if (fileCount) *fileCount = 0;
    HANDLE stub = CreateFileW(stubPath.c_str(), GENERIC_READ,
                              FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0,
                              nullptr);
    if (stub == INVALID_HANDLE_VALUE) {
        err = "stub missing: bootfiles\\portable-stub.exe";
        return false;
    }
    HANDLE out = CreateFileW(outFile.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        CloseHandle(stub);
        err = "cannot create output file (err=" +
              std::to_string(GetLastError()) + ")";
        return false;
    }
    // 1) 先把 stub 原样拷过去
    unsigned long long stubSize = 0;
    {
        char buf[64 * 1024];
        DWORD got = 0;
        while (ReadFile(stub, buf, sizeof(buf), &got, nullptr) && got > 0) {
            if (!WriteAll(out, buf, got)) {
                err = "write stub failed";
                CloseHandle(stub);
                CloseHandle(out);
                DeleteFileW(outFile.c_str());
                return false;
            }
            stubSize += got;
        }
        CloseHandle(stub);
    }
    // 2) 条目
    const bool ok = PackDir(out, appDir, L"", err, fileCount, 0, outFile);
    // 3) 结束标记 + 尾部（stub 偏移）
    if (ok) {
        if (!WriteAll(out, "ZJEND", 4)) err = "write end failed";
    }
    if (err.empty()) {
        if (!WriteAll(out, "ZJFT", 4) || !WriteAll(out, &stubSize, 8))
            err = "write footer failed";
    }
    CloseHandle(out);
    if (!err.empty()) {
        DeleteFileW(outFile.c_str());  // 失败别留半成品
        return false;
    }
    LogInfo("self-pack: " + std::to_string(fileCount ? *fileCount : 0) +
            " file(s) -> " + W2U(outFile));
    return true;
}

namespace {

bool CopyDirFiltered(const std::wstring& src, const std::wstring& dst,
                     std::string& err, int* count, int depth) {
    if (depth > 32) return true;
    CreateDirectoryW(dst.c_str(), nullptr);
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW((src + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return true;
    bool ok = true;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        if (SelfPackSkip(fd.cFileName)) continue;
        const std::wstring s = src + L"\\" + fd.cFileName;
        const std::wstring d = dst + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ok &= CopyDirFiltered(s, d, err, count, depth + 1);
            continue;
        }
        if (!CopyFileW(s.c_str(), d.c_str(), FALSE)) {
            err = "copy failed: " + W2U(fd.cFileName);
            ok = false;
            continue;
        }
        if (count) ++(*count);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

}  // namespace

bool ExtractSelfToFolder(const std::wstring& appDir,
                         const std::wstring& destDir, std::string& err,
                         int* fileCount) {
    if (fileCount) *fileCount = 0;
    if (!IsDir(appDir)) {
        err = "app dir missing";
        return false;
    }
    if (!CreateDirectoryW(destDir.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        err = "cannot create destination (err=" +
              std::to_string(GetLastError()) + ")";
        return false;
    }
    const bool ok = CopyDirFiltered(appDir, destDir, err, fileCount, 0);
    if (ok)
        LogInfo("self-extract: " + std::to_string(fileCount ? *fileCount : 0) +
                " file(s) -> " + W2U(destDir));
    return ok;
}

}  // namespace sysrecover
