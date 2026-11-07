#include "ui_foreground.h"

#include <cstdint>
#include <string>

#include "common/logger.h"

namespace sysrecover {
namespace {

// 兜底：只改 Z 序与激活。前台锁拒绝 SetForegroundWindow 时，至少保证 owner
// 不再被别的窗口压在下面（不移动、不改尺寸）。
void RaiseZOrder(HWND owner) {
    ::BringWindowToTop(owner);
    ::SetWindowPos(owner, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    ::SetActiveWindow(owner);
}

}  // namespace

bool RestoreForeground(HWND owner) {
    if (!owner || !::IsWindow(owner))
        return false;
    if (::IsIconic(owner))
        ::ShowWindow(owner, SW_RESTORE);

    // 第一次失败通常是前台锁：本线程不是前台进程。
    if (::SetForegroundWindow(owner))
        return true;

    // 把本线程的输入上下文临时接到"当前前台窗口"的线程，再试一次；
    // 无论成败都摘掉，避免影响后续输入分发。
    HWND fg = ::GetForegroundWindow();
    const DWORD myTid = ::GetCurrentThreadId();
    const DWORD fgTid = fg ? ::GetWindowThreadProcessId(fg, nullptr) : 0;
    bool attached = false;
    if (fgTid && fgTid != myTid)
        attached = ::AttachThreadInput(myTid, fgTid, TRUE) != 0;
    const bool ok = ::SetForegroundWindow(owner) != 0;
    if (attached)
        ::AttachThreadInput(myTid, fgTid, FALSE);
    if (ok && ::GetForegroundWindow() == owner)
        return true;

    RaiseZOrder(owner);
    if (::GetForegroundWindow() == owner)
        return true;

    // 不静默：留痕便于支持包定位（hwnd 用无符号整数，避免格式串跨平台问题）。
    LogInfo("foreground: restore failed hwnd=" +
            std::to_string(static_cast<unsigned long long>(
                reinterpret_cast<uintptr_t>(owner))) +
            " err=" + std::to_string(static_cast<unsigned long>(::GetLastError())));
    return false;
}

}  // namespace sysrecover
