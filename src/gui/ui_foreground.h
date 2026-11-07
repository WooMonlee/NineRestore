#pragma once
// 主窗回到前台的统一入口（N19，2026-11-07）。
//
// 背景（docs/34 §3.5）：自绘模态框关闭后原来只调 `SetForegroundWindow(owner)`，
// 在前台锁下会失败且返回值没人看 → 用户偶尔遇到"主窗被压到别的窗口后面"。
// 这里把 SetForegroundWindow → AttachThreadInput 重试 → Z 序回退 串成一条链，
// 并返回真实结果；两条路径都失败时写日志（不静默）。
#include <windows.h>

namespace sysrecover {

// 把 owner 恢复为前台窗口（最小化则先 SW_RESTORE）。
// 返回 true = 结束时 owner 确实是前台窗口；false = 各兜底路径都没成功（已写日志）。
bool RestoreForeground(HWND owner);

}  // namespace sysrecover
