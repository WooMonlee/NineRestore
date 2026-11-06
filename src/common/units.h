#pragma once
// 容量换算常量（字节 → KiB/MiB/GiB）。
// 起因：界面、CLI、自检各写一遍 `1024.0 * 1024 * 1024`，字面量写法还不一致
// （有的漏写 `.0`），改一处容易漏另一处。这里集中定义，只表达"换算比例"，
// 不改变任何显示格式与舍入方式。
namespace sysrecover {

constexpr double kKiB = 1024.0;
constexpr double kMiB = 1024.0 * 1024.0;
constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;

}  // namespace sysrecover
