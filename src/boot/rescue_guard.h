#pragma once
// ESP 救援载荷版本治理（2026-11-07，docs/34 N17）。
//
// 现场（2026-11-06）：ESP 上 `rescue-build.txt` 自报 0.9.92，`ZJRESTORE-status.txt`
// 也是 0.9.92，但程序已经"安装菜单成功"。根因在打包链（N10/C6）：dist 里的
// initramfs 是 11-02 的旧文件，安装器自带的源载荷就是旧码，CRC 比对"源=盘上"
// → keep，于是旧载荷一直留着，且没有任何一处提示。
//
// 治理口径（用户 2026-11-07 规格，最新定稿）：
//   1) 安装菜单 / 一次性暂存 / 异机还原 / 修复引导，下发前都要读 ESP 上救援环境
//      自报的版本（`EFI\ZJRESTORE\rescue-build.txt`）+ initramfs 长度，与程序
//      内置载荷比对；判定走**快路径**（版本戳 + 大小，不读 17.4MB 本体算哈希），
//      只有版本戳与大小都对不上（确需重写）时才在写后做 CRC 深查；
//   2) 不一致时**不备份、不写临时文件/不改名**：先删旧载荷腾出空间，再写新载荷
//      （ESP 寸土寸金，旧载荷本身有问题，没有保留价值）；
//   3) 顺序固定：检测 → 删旧 → 写新 → 校验 → 才进入启动/安装流程；
//   4) 删除后写失败即 **fail-closed**：提示"救援载荷写入失败，请重试或更换介质"，
//      并摘掉指向残骸的 Boot####（不能让人开机撞空载荷）。
//
// 本头文件里的分类/决策是**纯逻辑**（零依赖），单测直接构造"ESP 上是旧 build"
// 的输入断言不会静默沿用；文件读取/备份在 rescue_guard.cpp。
#include <cstdint>
#include <string>
#include <vector>

#include "../common/i18n.h"

namespace sysrecover {

enum class RescuePayloadState {
    Match,            // 自报版本 + 长度/CRC 与内置一致
    NoPayload,        // ESP 上根本没有我方救援载荷（首次安装）
    UnreadableBuild,  // 有载荷但缺/读不到自报版本（不可信）
    VersionMismatch,  // 自报版本 ≠ 内置版本（典型：0.9.92）
    ContentMismatch,  // 版本相同但 initramfs 长度/CRC 不同
};

enum class RescuePayloadAction {
    Keep,        // 可继续复用 ESP 上现有载荷
    Rewrite,     // 默认：重新写入并更新启动项（写前备份）
    FailClosed,  // 不能重写：明确失败，禁止静默沿用
};

// 抹掉首尾空白与 CRLF；其余字符原样保留（版本串不做数值解析）。
inline std::string NormalizeRescueBuild(const std::string& raw) {
    std::string s;
    s.reserve(raw.size());
    for (char c : raw) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        s.push_back(c);
    }
    return s;
}

// espHasPayload = ESP 上存在 initramfs 或内核（说明确实装过救援层）。
// espCrcKnown = 调用方**深查**过 ESP 上 initramfs 的 CRC（快路径请传 false）。
inline RescuePayloadState ClassifyRescuePayload(
    const std::string& espBuild, const std::string& builtinBuild,
    bool espHasPayload, bool espCrcKnown, uint32_t espCrc,
    uint32_t builtinCrc, unsigned long long espSize,
    unsigned long long builtinSize) {
    const std::string eb = NormalizeRescueBuild(espBuild);
    const std::string bb = NormalizeRescueBuild(builtinBuild);
    if (!espHasPayload && eb.empty())
        return RescuePayloadState::NoPayload;
    if (eb.empty())
        return RescuePayloadState::UnreadableBuild;  // 有载荷却无自报版本
    if (eb != bb)
        return RescuePayloadState::VersionMismatch;
    // 快路径：内置大小可知时先用"文件大小"判同（只读文件属性，不读本体）。
    if (builtinSize > 0 && espSize != builtinSize)
        return RescuePayloadState::ContentMismatch;
    // 深查：仅当调用方明确读过 CRC（写后校验 / 需要定性）时才比。
    if (espCrcKnown && espCrc != builtinCrc)
        return RescuePayloadState::ContentMismatch;
    return RescuePayloadState::Match;
}

inline RescuePayloadAction DecideRescuePayloadAction(RescuePayloadState s,
                                                    bool canRewrite) {
    if (s == RescuePayloadState::Match || s == RescuePayloadState::NoPayload)
        return RescuePayloadAction::Keep;
    return canRewrite ? RescuePayloadAction::Rewrite
                      : RescuePayloadAction::FailClosed;
}

// 只要不是 Match/NoPayload 就必须处理（重写或 fail-closed），不得静默复用。
inline bool RescuePayloadMustNotBeReused(RescuePayloadState s) {
    return DecideRescuePayloadAction(s, true) != RescuePayloadAction::Keep;
}

// 部署顺序契约（用户 2026-11-07 口径）：检测 → 删旧 → 写新 → 校验 → 进启动流程。
// 单测钉死，防止有人改回"先备份再替换"或"先跑新载荷、成功后再替换"。
inline const std::vector<std::string>& RescuePayloadDeploySteps() {
    static const std::vector<std::string> kSteps = {
        "detect", "remove-old", "write-new", "verify", "install-entry"};
    return kSteps;
}

// 旧载荷已被删/被清之后写失败 → 摘掉 Boot####（不能留下指向残骸的启动项）。
inline bool MustRemoveBootEntryAfterFailure(bool payloadRemoved) {
    return payloadRemoved;
}

// 删除后写失败的用户提示（用户 2026-11-07 指定文案）。
inline std::string RescuePayloadWriteFailedMessage() {
    return Tr("救援载荷写入失败，请重试或更换介质。");
}

inline std::string RescuePayloadFailMessage(const std::string& espBuild,
                                            const std::string& builtinBuild) {
    const std::string eb = NormalizeRescueBuild(espBuild);
    if (eb.empty())
        return Tr("ESP 上救援环境版本无法确认（缺少 rescue-build.txt），"
                  "需更新后重试。");
    return Tr("ESP 上为旧版救援环境（") + eb + Tr("），内置为 ") +
           NormalizeRescueBuild(builtinBuild) + Tr("，需更新后重试。");
}

// 写前备份目录（`EFI\ZJRESTORE\backup-<tag>-<时间戳>`）的识别。
// 2026-11-07 历史：曾把旧载荷整目录备份到 `backup-*`（约 17.4MB/份），实测
// ESP 空间吃紧。现在**不再备份载荷**（旧载荷本身有问题），改为先删后写；
// `backup-*` 只作为老版本遗留目录被识别并清理（只删这一前缀，不碰用户文件）。
inline bool IsRescueBackupDirName(const std::wstring& name) {
    return name.size() > 7 && name.compare(0, 7, L"backup-") == 0;
}

// ── 运行期（Windows）──────────────────────────────────────────────
struct EspRescuePayload {
    bool hasBuild = false;
    bool hasInitramfs = false;
    bool hasKernel = false;
    std::string build;
    unsigned long long initramfsSize = 0;
    uint32_t initramfsCrc = 0;
    bool crcKnown = false;
};

// 只读：ESP 上救援载荷的自报版本 + initramfs 长度。deepCrc=true 时才读本体算
// CRC（17.4MB 全读，快路径不要传 true）。
bool ReadEspRescuePayload(const std::wstring& espRoot, EspRescuePayload& out,
                          bool deepCrc = false);

// 清理历史遗留的载荷备份目录：`backup-*` 逐目录删掉，日志写清单与释放字节。
// 只匹配自己创建的前缀，不碰用户文件、不碰 Secure Boot 相关文件、不碰当前载荷。
// 返回删掉的字节数（失败项也算 0）。
unsigned long long CleanupLegacyRescueBackups(const std::wstring& espRoot,
                                              std::string& log);

// 写一份几 KB 的固件启动序快照（`EFI\ZJRESTORE\firmware-boot.txt`），
// 作为"替换前 BootOrder/Boot#### 指向谁"的判据（不占载荷空间）。
void WriteFirmwareBootSnapshot(const std::wstring& espRoot, std::string& log);

// 修复引导等**不新建启动项**的入口：ESP 上是旧载荷 → 先删旧文件、再写新文件、
// 写后校验，最后更新 rescue-build.txt（不创建新的 Boot####）。失败返回 false，
// msg 为"救援载荷写入失败，请重试或更换介质。"。
bool RefreshEspRescuePayload(const std::wstring& espRoot,
                             const std::wstring& assetDir, std::string& log,
                             std::string& msg);

}  // namespace sysrecover
