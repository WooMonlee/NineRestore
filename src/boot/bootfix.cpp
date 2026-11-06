// 引导补全实现（BIOS/MBR）。见 bootfix.h 与 AGENTS.md §7。
//
// 2026-10-27（用户反馈 2）：这条链**不再依赖 bcdedit / bcdboot**。极简 WinPE
// （如 Win8PE）里没有这两个工具，以前 `bcdedit /export` 失败 → "bootfix INCOMPLETE"
// → 还原后的目标分区没有可用 BCD（开机 0xc000000f / 黑屏）。现在改为全静态资产：
//   · \bootmgr   ← 目标系统自己（<target>\bootmgr 或 <target>\Windows\Boot\PCAT，
//                  本机 Windows 只作兜底 —— 本机可能是 UEFI-only 安装，没有 PCAT）
//   · \Boot\BCD  ← 随包模板 bootfiles\bcd\bios.bcd（{bootmgr}/{default} 都是
//                  device boot = 机器无关；2026-10-27 QEMU/SeaBIOS 实测可启动）
//   · 字体/资源/语言/boot.sdi ← 尽力复制（缺则 WARN，精简镜像本来也可能没有）
// 分区引导区（PBR）不在这里写：Linux 侧在 mkntfs/apply 之后自己写（zjrestore-lite.sh）。
#include "bootfix.h"

#include <windows.h>

#include <cstdio>

#include "../common/i18n.h"   // W2U（全项目唯一实现）
#include "../common/pathutil.h"  // BootAssetDir()
#include "../common/selfarch.h"
#include "bootself.h"

namespace sysrecover {

bool PrepareBootFixFiles(const std::wstring& recoveryDir, std::string& log) {
    // recoveryDir 形如 "C:\ZJRESTORE"（= 目标盘上的恢复目录）→ 目标盘根 "C:\"。
    if (recoveryDir.size() < 3 || recoveryDir[1] != L':') {
        log += "bootfix: bad recovery dir " + W2U(recoveryDir) + "\n";
        return false;
    }
    const std::wstring targetRoot = recoveryDir.substr(0, 3);
    const std::wstring srcWin = targetRoot + L"Windows";

    std::wstring bf = recoveryDir + L"\\bootfix";
    CreateDirectoryW(recoveryDir.c_str(), nullptr);
    CreateDirectoryW(bf.c_str(), nullptr);

    std::string err;
    // destRoot 必须以反斜杠结尾（bootself 里按 "destRoot + Boot" 拼）。
    bool ok = StageBiosBootTree(bf + L"\\", srcWin, BootAssetDir(),
                                /*overwriteBcd=*/true, log, err);
    if (!ok) log += "bootfix: " + err + "\n";
    log += ok ? "bootfix prepared\n" : "bootfix INCOMPLETE\n";
    return ok;
}

}  // namespace sysrecover
