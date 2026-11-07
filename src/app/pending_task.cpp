#include "app/pending_task.h"

#include <windows.h>

namespace sysrecover {

std::wstring PendingTaskMarkerPath(const std::wstring& baseDir) {
    if (baseDir.empty()) return std::wstring();
    std::wstring p = baseDir;
    if (p.back() != L'\\') p += L'\\';
    p += L"SysRecover\\last-task-unfinished.txt";
    return p;
}

bool TakePendingTaskMarker(const std::wstring& baseDir, std::wstring& content) {
    content.clear();
    const std::wstring path = PendingTaskMarkerPath(baseDir);
    if (path.empty()) return false;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        char buf[4096] = {};
        DWORD n = 0;
        if (ReadFile(h, buf, sizeof(buf) - 1, &n, nullptr) && n > 0) {
            // 救援层写的是 ASCII/UTF-8；按 UTF-8 解，失败就留空（提示不依赖内容）。
            const int wlen = MultiByteToWideChar(CP_UTF8, 0, buf, (int)n, nullptr, 0);
            if (wlen > 0) {
                content.resize((size_t)wlen);
                MultiByteToWideChar(CP_UTF8, 0, buf, (int)n, &content[0], wlen);
            }
        }
        CloseHandle(h);
    }
    // 读即自删：即使内容解析失败或后续提示没弹出来，也不再留第二次打扰。
    DeleteFileW(path.c_str());
    return true;
}

}  // namespace sysrecover
