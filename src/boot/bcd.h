#pragma once
// BCD 操作（bcdedit 封装）。成熟方案见 AGENTS.md §7，严禁发明。
// 条目存在性：bcdedit /enum {GUID} 看输出是否回显 GUID（两者退出码都 0）。
#include <cctype>
#include <string>

namespace sysrecover {

// 固定恢复条目 GUID（与旧原型一致，便于对照）。
inline const wchar_t* RecoveryGuid() {
    return L"{12345678-1234-1234-1234-123456789abc}";
}

// 临时（日常还原一次性）条目 GUID：必须与常驻菜单 GUID 区分，
// 否则首启清理会误删用户显式安装的菜单。
inline const wchar_t* TemporaryGuid() {
    return L"{87654321-4321-4321-4321-cba987654321}";
}

// 解析 `bcdedit /enum {bootmgr}` 输出里的 timeout（秒），找不到 → -1（PIT-094）。
// **不依赖标签语言**：timeout 是 {bootmgr} 枚举里唯一「整行恰好两个空白分隔
// token、且第二个 token 全为数字（≤1 天）」的元素 —— device/locale/description
// 等第二个 token 都不是纯数字，displayorder 的续行只有一个 token，表头/分隔线
// 不是两 token 或第二 token 非数字。纯逻辑，单测覆盖。
inline int ParseBootmgrTimeout(const std::string& out) {
    auto isWs = [](char c) { return c == ' ' || c == '\t'; };
    size_t pos = 0;
    while (pos < out.size()) {
        size_t eol = out.find_first_of("\r\n", pos);
        if (eol == std::string::npos) eol = out.size();
        size_t a = pos;
        while (a < eol && isWs(out[a])) ++a;
        size_t b = a;
        while (b < eol && !isWs(out[b])) ++b;
        if (b > a && b < eol) {  // 有 token1，后面还有内容
            size_t c = b;
            while (c < eol && isWs(out[c])) ++c;
            size_t d = c;
            while (d < eol && !isWs(out[d])) ++d;
            size_t e = d;
            while (e < eol && isWs(out[e])) ++e;
            if (d > c && e == eol) {  // token2 后到行尾只有空白
                int v = 0;
                bool ok = true;
                for (size_t i = c; i < d; ++i) {
                    if (out[i] < '0' || out[i] > '9') {
                        ok = false;
                        break;
                    }
                    v = v * 10 + (out[i] - '0');
                    if (v > 86400) {  // 不可能是 timeout 秒数
                        ok = false;
                        break;
                    }
                }
                if (ok) return v;
            }
        }
        if (eol == out.size()) break;
        pos = eol + 1;
    }
    return -1;
}

// 条目是否存在（ASCII GUID 匹配，不受代码页影响）。
bool BcdEntryExists(const std::wstring& guid);

// ASCII GUID 不区分大小写比较（读回值来自 bcdedit 输出，大小写可能变）。
inline bool SameGuidAscii(const std::string& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) return false;
    }
    return true;
}

// 从 `bcdedit /enum {bootmgr}` 输出里取 `bootsequence` 的值（`{GUID}`）。
// 用途：**写后读回校验**（用户 2026-10-27 需求 —— 装了安全软件的机器上，
// bootsequence 可能被静默丢弃/回滚，重启后照旧进原系统，用户毫无提示）。
// 判定**不依赖其它元素的标签语言**：`bootsequence` 是 bcdedit 固定输出的
// ASCII 元素名（与 identifier/device/timeout 同族），值恒为
// `{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}`（38 字符）。找不到/形态不对 →
// 空串（= 没有单次启动项）。纯逻辑，单测覆盖。
inline std::string ParseBootsequence(const std::string& out) {
    size_t pos = 0;
    for (;;) {
        size_t eol = out.find_first_of("\r\n", pos);
        if (eol == std::string::npos) eol = out.size();
        size_t a = pos;
        while (a < eol && (out[a] == ' ' || out[a] == '\t')) ++a;
        size_t b = a;
        while (b < eol && out[b] != ' ' && out[b] != '\t') ++b;
        std::string key = out.substr(a, b - a);
        for (size_t i = 0; i < key.size(); ++i)
            key[i] = static_cast<char>(
                std::tolower(static_cast<unsigned char>(key[i])));
        if (key == "bootsequence") {
            size_t c = b;
            while (c < eol && (out[c] == ' ' || out[c] == '\t')) ++c;
            size_t d = c;
            while (d < eol && out[d] != ' ' && out[d] != '\t') ++d;
            std::string v = out.substr(c, d - c);
            if (v.size() == 38 && v.front() == '{' && v.back() == '}')
                return v;
        }
        if (eol >= out.size()) break;
        pos = eol + 1;
    }
    return std::string();
}

// 读系统存储的 `{bootmgr} timeout`（秒）；解析不到返回 -1（原文追加进 log）。
int BcdGetBootmgrTimeout(std::string& log);

// 写 `{bootmgr} timeout`（bcdedit /set {bootmgr} timeout N）。
bool BcdSetBootmgrTimeout(int seconds, std::string& log);

// 创建/刷新实模式启动扇区条目：device partition=X: + path \grldr.mbr + displayorder。
// **条目已存在时也会重设 device/path**（幂等自愈）：防止换机/换盘符/旧布局残留
// 造成"文件在新盘、entry 指旧盘"→ bootmgr 0xc000000F。
bool BcdCreateBootsector(const std::wstring& guid, const std::wstring& desc,
                         wchar_t driveLetter, std::string& log);

// 单次启动（BIOS 路径；UEFI 用固件 BootNext，见 boot/uefi.h）。
// **写后读回校验**（失败重试一次）：安全软件拦截时 bcdedit 可能返回 0 却没生效，
// 结果就是"重启后照旧进原系统、毫无提示"（用户 2026-10-27 反馈）。
// 失败时 log 里带 `bootsequence verify FAILED`，调用方应中止并提示用户。
bool BcdSetBootsequence(const std::wstring& guid, std::string& log);

// 读 `{bootmgr}` 的 `bootsequence`（`{GUID}`）；没有则空串。原文追加进 log。
std::string BcdGetBootsequence(std::string& log);

// 备份 BCD（bcdedit /export），失败不致命。
bool BcdExport(const std::wstring& backupPath);

// 解析 `bcdedit /enum {current}` 里是否还有 safeboot 元素。
// 元素名是 ASCII（safeboot / bootsequence 同族），值 Minimal/Network/...
// 也不随系统语言变化；所以只认「首 token == safeboot 且第二 token 非空」。
inline bool ParseSafeBootSet(const std::string& out) {
    size_t pos = 0;
    for (;;) {
        size_t eol = out.find_first_of("\r\n", pos);
        if (eol == std::string::npos) eol = out.size();
        size_t a = pos;
        while (a < eol && (out[a] == ' ' || out[a] == '\t')) ++a;
        size_t b = a;
        while (b < eol && out[b] != ' ' && out[b] != '\t') ++b;
        std::string key = out.substr(a, b - a);
        for (size_t i = 0; i < key.size(); ++i)
            key[i] = static_cast<char>(
                std::tolower(static_cast<unsigned char>(key[i])));
        if (key == "safeboot") {
            size_t c = b;
            while (c < eol && (out[c] == ' ' || out[c] == '\t')) ++c;
            return c < eol;  // 有值 = 置位（Minimal/Network/AlternateShell）
        }
        if (eol >= out.size()) break;
        pos = eol + 1;
    }
    return false;
}

// 安全模式开关（「日志」按钮的诊断动作）。2026-11-06 改为**一次性语义**：
//   · 设置前先武装「下一次登录自动清除」（登录计划任务，SYSTEM + 最高权限），
//     用户即使不再打开本程序，也会自动退出安全模式（见 BcdSetSafeBootOnce）；
//   · 武装失败 → 拒绝设置（宁可不进，也不把用户锁在安全模式里）；
//   · BcdSetSafeBoot 是底层原语：写后读回校验，清除方向幂等（本来没有也算成功）。
enum class SafeBootState { None, Minimal, Unknown };

// 读 `bcdedit /enum {current}` 判断 safeboot 是否被置位。
// Unknown = bcdedit 执行失败（判定不了，调用方应按"可能置位"提示）。
SafeBootState BcdGetSafeBootState(std::string& log);

// 写/清 safeboot（带读回校验）；清除幂等。原文追加进 log。
bool BcdSetSafeBoot(bool enable, std::string& log);

// 一次性语义：true = 武装自动退出后设置；false = 清除并撤销自动退出。
bool BcdSetSafeBootOnce(bool enable, std::string& log);

// 武装/撤销「下一次登录自动清除 safeboot」的任务（幂等）。
bool BcdArmSafeBootAutoExit(std::string& log);
bool BcdDisarmSafeBootAutoExit(std::string& log);

// 测试注入点（仅单元测试）：替换真实进程调用，夹具可模拟 bcdedit/schtasks。
// 传 nullptr 恢复真实调用。生产代码从不调用。
using BcdToolRunner = int (*)(const std::wstring& exe, const std::wstring& args,
                              std::string& out);
void BcdSetToolRunnerForTest(BcdToolRunner fn);

// ── 任意 BCD 存储（/store）的操作：产物断言与死条目清理（PIT-209）──────
// UEFI 的库是 <ESP>\EFI\Microsoft\Boot\BCD；BIOS 的库是 <目标>:\Boot\BCD。
// 注意：不指定 /store 时 bcdedit 操作的是**正在运行的系统**的库 —— 在 PE 里
// 那根本不是我们要修的那一份（PIT-209 现场：PE 里枚举的是 X: 的库）。

// `bcdedit /store <path> /enum all` → UTF-8 文本。失败返回 false（log 留原因）。
bool BcdEnumStore(const std::wstring& storePath, std::string& out,
                  std::string& log);

// 删除存储里的一个条目（/delete {GUID} /f）。
bool BcdDeleteEntry(const std::wstring& storePath, const std::wstring& guid,
                    std::string& log);

// `{bootmgr}` 元素的设置/删除（timeout / displaybootmenu / displayorder）。
bool BcdSetStoreBootmgr(const std::wstring& storePath, const wchar_t* element,
                        const wchar_t* value, std::string& log);
bool BcdDelStoreBootmgr(const std::wstring& storePath, const wchar_t* element,
                        std::string& log);

// 对任意存储做**统一产物断言**（UEFI/BIOS 共用）：
//   ① displayorder 非空 + 有 winload 条目；
//   ② 默认启动项的 device 指向 winDir 所在分区（列不出来时放行并记 note）；
//   ③ uefi=true 时还要求默认加载器是 winload.efi。
// BIOS 模板 BCD 的 device 是 `boot`（随固件走），不做分区校验。
bool VerifyBcdStore(const std::wstring& storePath, const std::wstring& winDir,
                    bool uefi, std::string& detail);

// 清理"死启动项"（PIT-209：还原/修引导后只剩一个系统、菜单里却有幽灵选项）：
//   逐条看 displayorder 里的条目，**默认项永不删除**，只删能确证失效的：
//     · 目标分区已不存在（设备打不开）        → 删
//     · 目标分区存在但没有 \Windows（数据盘）  → 删（仅 Windows 加载器）
//     · 判断不了（无盘符/离线/枚举不到）      → 保留（宁可留，不误删）
//   我们自己的条目（ZJRESTORE / SysRecover）一律不动。
// targetWinDir = 本次还原/修复的目标系统目录（如 L"C:\\Windows"）：
//   指向它的条目**永不删除**（哪怕文件暂时读不到 —— BitLocker/离线也不误杀）。
struct BcdPruneResult {
    bool ok = false;
    int removed = -1;    // 删除条数；!ok 时为 -1（旧接口的错误哨兵）
    int reachable = -1;  // 清完后仍在启动顺序里的 Windows 加载器条数；-1 = 判定失败
};
// 结果类型取代"int 返回计数 + int* 出参 + -1 表错"的歧义写法。
BcdPruneResult BcdPruneDeadEntries(const std::wstring& storePath,
                                   const std::wstring& targetWinDir,
                                   std::string& log);

// 只剩一个 Windows 系统时，别让它停在"选择操作系统"菜单：
//   displayorder 收敛成这一条 + `{bootmgr} timeout 0` + 清 displaybootmenu。
// 返回 true = 已经收敛（或本来就只有一个）。
bool BcdQuietSingleOsMenu(const std::wstring& storePath, std::string& log);

}  // namespace sysrecover
