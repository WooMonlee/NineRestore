#pragma once
// N20 批 2（用户决策 ④）：旧系统"上次任务未完成"标记。
//
// 写入方：救援层回滚成功时（`bootfiles/alpine/init` 的 `mark_pending_task`）写到
//         `<旧系统根>\ProgramData\SysRecover\last-task-unfinished.txt`。
// 读取方：本模块；**读到即删**（删在提示之前完成，保证只打扰一次）。
// 范围锁死：只这一个文件，不碰注册表、不新增启动项/服务。
#include <string>

namespace sysrecover {

// 标记的完整路径（baseDir 一般是 %ProgramData%；测试用临时目录）。
std::wstring PendingTaskMarkerPath(const std::wstring& baseDir);

// 读取并删除标记。返回 true = 本次读到（content 为文件内容，可能为空）。
// 文件不存在返回 false。读失败但仍尝试删除（自删语义优先）。
bool TakePendingTaskMarker(const std::wstring& baseDir, std::wstring& content);

}  // namespace sysrecover
