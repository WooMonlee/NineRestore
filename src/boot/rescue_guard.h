#pragma once
// ESP 救援载荷版本治理（2026-11-07，docs/34 N17）。
//
// 现场（2026-11-06）：ESP 上 `rescue-build.txt` 自报 0.9.92，`ZJRESTORE-status.txt`
// 也是 0.9.92，但程序已经"安装菜单成功"。根因在打包链（N10/C6）：dist 里的
// initramfs 是 11-02 的旧文件，安装器自带的源载荷就是旧码，CRC 比对"源=盘上"
// → keep，于是旧载荷一直留着，且没有任何一处提示。
//
// 治理口径（用户 2026-11-07 规格）：
//   1) 安装菜单 / 一次性暂存 / 异机还原 / 修复引导，下发前都要读 ESP 上救援环境
//      自报的版本（`EFI\ZJRESTORE\rescue-build.txt`）+ initramfs 长度/CRC，
//      与程序内置载荷比对；
//   2) 不一致时**默认重写**（写前备份旧文件与 Boot####/BootOrder）；
//   3) 不能重写就 **fail-closed**，明确提示"ESP 上为旧版救援环境（0.9.x）"；
//      禁止静默沿用旧载荷继续干活。
//
// 本头文件里的分类/决策是**纯逻辑**（零依赖），单测直接构造"ESP 上是旧 build"
// 的输入断言不会静默沿用；文件读取/备份在 rescue_guard.cpp。
#include <cstdint>
#include <string>

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
// espContentKnown = ESP 上 initramfs 的 size+CRC 都读到了。
inline RescuePayloadState ClassifyRescuePayload(
    const std::string& espBuild, const std::string& builtinBuild,
    bool espHasPayload, bool espContentKnown, uint32_t espCrc,
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
    if (espContentKnown && (espSize != builtinSize || espCrc != builtinCrc))
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

inline std::string RescuePayloadFailMessage(const std::string& espBuild,
                                            const std::string& builtinBuild) {
    const std::string eb = NormalizeRescueBuild(espBuild);
    if (eb.empty())
        return Tr("ESP 上救援环境版本无法确认（缺少 rescue-build.txt），"
                  "需更新后重试。");
    return Tr("ESP 上为旧版救援环境（") + eb + Tr("），内置为 ") +
           NormalizeRescueBuild(builtinBuild) + Tr("，需更新后重试。");
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

// 只读：ESP 上救援载荷的自报版本 + initramfs 长度/CRC。
bool ReadEspRescuePayload(const std::wstring& espRoot, EspRescuePayload& out);

// 写入前备份：旧文件 → `EFI\ZJRESTORE\backup-<tag>\`，并把当前固件启动项
// 汇总（BootOrder 顺序 + 每条 Boot#### 描述/设备路径）写成
// `backup-<tag>\firmware-boot.txt`。返回 false 仅表示有备份项失败（调用方
// 仍可继续，但必须把失败写进日志）。
bool BackupEspRescuePayload(const std::wstring& espRoot, const std::string& tag,
                            std::string& log);

// 修复引导等**不新建启动项**的入口：ESP 上是旧载荷 → 就地刷新已有文件 +
// 更新 rescue-build.txt（不创建新的 Boot####）。失败返回 false，msg 给用户。
bool RefreshEspRescuePayload(const std::wstring& espRoot,
                             const std::wstring& assetDir, std::string& log,
                             std::string& msg);

}  // namespace sysrecover
