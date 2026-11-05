// 九转还原：ops_boot（阶段 2 从 src/app/ops.cpp 机械拆分而来，逻辑未改）。
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


// 0.9.38：ESP 诊断必须有不依赖主日志的落点。主日志在部分 PE 的 support 包里
// 会缺失（GUI/CLI 日志根或文件共享问题），所以这里直接写 `ESP-diag.txt`：
// 同时写“日志根\logs”和“程序目录\logs”，support 的 logs 树一定能收到。
void EspDiagAppendAt(const std::wstring& dir, const std::string& text) {
    if (dir.empty()) return;
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring file = dir + L"\\ESP-diag.txt";
    HANDLE h = CreateFileW(file.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(h, text.data(), (DWORD)text.size(), &wrote, nullptr);
    CloseHandle(h);
}

void EspDiagLine(const std::string& s) {
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    char ts[64] = {};
    snprintf(ts, sizeof(ts), "%04u-%02u-%02u %02u:%02u:%02u ",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::string line = std::string(ts) + s + "\r\n";
    const std::wstring primary = LogBaseDir() + L"\\logs";
    const std::wstring app = AppDir() + L"\\logs";
    EspDiagAppendAt(primary, line);
    if (_wcsicmp(primary.c_str(), app.c_str()) != 0)
        EspDiagAppendAt(app, line);
    LogFileInfo("ESP-DIAG " + s);
}

void EspDiagVolumes() {
    EspDiagLine("--- logical drives ---");
    DWORD mask = GetLogicalDrives();
    for (wchar_t c = L'C'; c <= L'Z'; ++c) {
        if (!(mask & (1u << (c - L'A')))) continue;
        std::wstring root = std::wstring(1, c) + L":\\";
        const UINT type = GetDriveTypeW(root.c_str());
        wchar_t label[64] = {}, fs[32] = {};
        GetVolumeInformationW(root.c_str(), label, 64, nullptr, nullptr,
                              nullptr, fs, 32);
        ULARGE_INTEGER total = {};
        GetDiskFreeSpaceExW(root.c_str(), nullptr, &total, nullptr);
        wchar_t vn[MAX_PATH] = {};
        GetVolumeNameForVolumeMountPointW(root.c_str(), vn, MAX_PATH);
        char line[512] = {};
        snprintf(line, sizeof(line),
                 "%c: type=%u label=%ls fs=%ls total=%llu volume=%ls",
                 (char)c, type, label, fs,
                 (unsigned long long)total.QuadPart, vn);
        EspDiagLine(line);
    }
}


// 目标盘的分区风格（找不到该盘 → Unknown）。
PartitionStyle DiskStyleOf(uint32_t diskIndex) {
    for (const auto& d : EnumerateDisks())
        if (d.index == diskIndex)
            return d.style;
    return PartitionStyle::Unknown;
}


// ②b：目标盘 ESP **无盘符**时按**卷 GUID** 直接挂（GPT 下卷 GUID = 分区 GUID，
// 由 GuidToString 给出、已去花括号小写；mountvol 只认 `\\?\Volume{...}` 格式，
// GUID 解析大小写不敏感，这里仍转大写求稳）。多盘机「还原到非固件盘」时 ①（固件
// ESP 在别的盘）被盘号校验拒掉、③ 又要求有盘符 —— 没有 ②b 就只能硬失败。
bool MountEspByGuid(const std::wstring& volumeOrGuid, std::wstring& root,
                    std::string& log) {
    if (volumeOrGuid.empty())
        return false;
    // volumeName 来自 FindFirstVolume，已经是 \\?\Volume{...}\；只有传分区
    // GUID 时（老调用/兜底）才拼出卷名。0.9.33：刚分区的 ESP 用真实卷名
    // 才能立即挂上，分区 GUID 在少数 PE/盘上要等重启后的 mount manager。
    std::wstring vn = volumeOrGuid;
    if (vn.rfind(L"\\\\?\\", 0) != 0) {
        std::wstring g = volumeOrGuid;
        for (auto& ch : g)
            ch = (wchar_t)towupper(ch);
        vn = L"\\\\?\\Volume{" + g + L"}\\";
    }
    if (!vn.empty() && vn.back() != L'\\')
        vn += L'\\';
    DWORD mask = GetLogicalDrives();
    for (wchar_t c = L'S'; c <= L'Z'; ++c) {
        if (mask & (1u << (c - L'A')))
            continue;
        std::wstring r = std::wstring(1, c) + L":\\";
        // 0.9.33：优先用 Win32 SetVolumeMountPointW，不依赖外部的
        // mountvol.exe —— 部分精简 PE 根本没有 mountvol，表现为“找不到 ESP”，
        // 换到正常 Windows 重试又正常。
        if (SetVolumeMountPointW(r.c_str(), vn.c_str())) {
            if (GetFileAttributesW(r.c_str()) != INVALID_FILE_ATTRIBUTES) {
                root = r;
                log += "ESP mounted by volume at ";
                log += (char)c;
                log += ":\\ (Win32)\n";
                return true;
            }
            DeleteVolumeMountPointW(r.c_str());
        }
        std::string out;
        std::wstring args = std::wstring(1, c) + L": " + vn;
        if (RunProcess(SysToolPath(L"mountvol.exe"), args, out) != 0)
            continue;
        if (GetFileAttributesW(r.c_str()) == INVALID_FILE_ATTRIBUTES) {
            std::string o2;  // 挂上了却看不到内容 → 撤掉换下一个空闲符
            RunProcess(SysToolPath(L"mountvol.exe"),
                       std::wstring(1, c) + L": /d", o2);
            continue;
        }
        root = r;
        log += "ESP mounted by volume at ";
        log += (char)c;
        log += ":\\ (mountvol)\n";
        return true;
    }
    return false;
}


// 0.9.37：PE 10586 这类 PE 的卷枚举有时拿不到“卷 ↔ 分区”的 extent 映射，
// 即使 Explorer 里 ESP 明明有 E: 盘符，`PartitionInfo.letter` 仍为空。
// 最后按 ESP 的卷标 `ESP` + FAT 文件系统 + 分区大小核对，直接用已有盘符。
// 只接受大小与目标 ESP 分区一致的 FAT 卷，避免误选数据盘。
std::wstring FindEspDriveByLabel(const PartitionInfo& esp) {
    DWORD mask = GetLogicalDrives();
    for (wchar_t c = L'D'; c <= L'Z'; ++c) {
        if (!(mask & (1u << (c - L'A')))) continue;
        std::wstring root = std::wstring(1, c) + L":\\";
        const UINT type = GetDriveTypeW(root.c_str());
        if (type == DRIVE_CDROM || type == DRIVE_REMOTE ||
            type == DRIVE_NO_ROOT_DIR)
            continue;
        wchar_t label[64] = {}, fs[32] = {};
        if (!GetVolumeInformationW(root.c_str(), label, 64, nullptr, nullptr,
                                   nullptr, fs, 32))
            continue;
        if (_wcsicmp(label, L"ESP") != 0) continue;
        if (_wcsicmp(fs, L"FAT32") != 0 && _wcsicmp(fs, L"FAT") != 0)
            continue;
        ULARGE_INTEGER total = {};
        if (esp.sizeBytes > 0) {
            // PE10586 实测：300MB 的 ESP，p1=314,572,800 字节，E: 的
            // GetDiskFreeSpaceEx total=310,378,496（差 4MB，FAT32/分区保留区
            // 与分区长度本来就不完全相等）。允许 16MB 误差，避免把正确 ESP
            // 误判成“大小不匹配”。
            const unsigned long long kTolerance = 16ull << 20;
            if (!GetDiskFreeSpaceExW(root.c_str(), nullptr, &total, nullptr) ||
                total.QuadPart == 0)
                continue;
            const unsigned long long diff =
                total.QuadPart > esp.sizeBytes
                    ? total.QuadPart - esp.sizeBytes
                    : esp.sizeBytes - total.QuadPart;
            if (diff > kTolerance)
                continue;
        }
        return root;
    }
    return L"";
}


// 取一个**可用的 ESP 根**（形如 "X:\\"）。顺序（docs/15 · P4 + Q2 盘号限定，
// 2026-10-18）：
//   ① `mountvol <空闲盘符>: /s` —— 固件**真正启动**的那个分区。关键点：它按固件
//      启动项找，**不依赖分区类型 GUID**，所以 DiskGenius 把 ESP 标成 Basic Data
//      时它照样挂得上。**Q2：targetDisk>=0 时挂完校验盘号**（VolumeDiskIndex =
//      IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS）—— 多盘机固件 ESP 可能在别的盘，
//      往那儿写引导 = 写错盘；**证实**在别的盘 → UnmountEsp + 清空 espRoot 续查
//      ②③（UnmountEsp 是 const 引用、不清字符串，必须手动 clear）；盘号取不到
//      （-1）→ 保留挂载 + WARN（fail-open：ioctl 失败极罕见，不能因它把合法还原
//      卡死）。
//   ② 目标盘上类型 GUID 认对的 ESP：**有盘符**直接用（mountvol 失败时的兜底）；
//      **无盘符** → ②b 按卷 GUID 挂载（见上）。
//   ③ 回退：目标盘 FAT 分区且根下有 \EFI\Microsoft\Boot\bootmgfw.efi（故障 B 的正解）。
// targetDisk>=0 且三路都失败 → false（**绝不回退到错盘**）；-1 = 不限盘
// （备份本机 ESP 等，行为与 Q2 之前完全一致）。
// guidMatched=true 表示"GPT 类型 GUID 也认对了"（正常机器；false 时调用方只记警告）。
bool AcquireEspRoot(std::wstring& espRoot, bool& guidMatched, std::string& log,
                    int targetDisk) {
    // 0.9.33（用户 2026-10-26 实测）：刚用向导建好分区的磁盘，Windows 的
    // mount manager 偶尔要过几百毫秒才把 ESP 卷注册完整 —— 第一次找 ESP
    // 失败、重启后同一套分区就正常。这里重试三次（并每次重新枚举，
    // 等卷名/盘符出现），而不是一次失败就直接判死。
    for (int attempt = 0; attempt < 3; ++attempt) {
        EspDiagLine("AcquireEspRoot start attempt=" + std::to_string(attempt + 1) +
                    " targetDisk=" + std::to_string(targetDisk));
        EspDiagVolumes();
        EspDiagLine("partitions: " + W2U(DescribePartitions()));
        PartitionInfo esp;
        guidMatched = FindEspPartition(esp, targetDisk);
        EspDiagLine("FindEspPartition matched=" +
                    std::string(guidMatched ? "1" : "0") +
                    " letter=" + W2U(esp.letter) +
                    " volumeName=" + W2U(esp.volumeName) +
                    " guid=" + W2U(esp.guid) +
                    " offset=" + std::to_string(esp.offsetBytes) +
                    " size=" + std::to_string(esp.sizeBytes));
        // ① 固件 ESP（mountvol /s）+ 盘号校验
        espRoot = MountEsp(log);
        EspDiagLine("MountEsp /s -> " + W2U(espRoot));
        if (!espRoot.empty()) {
            if (targetDisk >= 0) {
                int d = VolumeDiskIndex(espRoot[0]);
                if (d >= 0 && d != targetDisk) {
                    log += "mounted ESP is on disk " + std::to_string(d) +
                           " but target is disk " + std::to_string(targetDisk) +
                           "; unmount, look on target disk\n";
                    UnmountEsp(espRoot, log);
                    espRoot.clear();
                } else if (d < 0) {
                    log += "WARN: cannot verify mounted ESP disk index; keeping it\n";
                    EspDiagLine("MountEsp disk index unknown; keeping root");
                }
            }
            if (!espRoot.empty()) {
                EspDiagLine("AcquireEspRoot OK via mountvol/s: " + W2U(espRoot));
                return true;
            }
        }
        // ② 目标盘 GUID ESP：有盘符
        if (guidMatched && !esp.letter.empty()) {
            std::wstring root = esp.letter + L":\\";
            if (GetFileAttributesW(root.c_str()) != INVALID_FILE_ATTRIBUTES) {
                espRoot = root;
                log += "ESP via existing drive letter\n";
                EspDiagLine("AcquireEspRoot OK via existing letter: " + W2U(root));
                return true;
            }
            EspDiagLine("existing ESP letter not accessible: " + W2U(root));
        }
        // ②b 目标盘 GUID ESP：无盘符 → 用真实卷名挂（卷名取不到才退回
        //     partition GUID）。这是刚分区 ESP 的正解。
        if (guidMatched && esp.letter.empty()) {
            const std::wstring& volume =
                esp.volumeName.empty() ? esp.guid : esp.volumeName;
            EspDiagLine("trying MountEspByGuid volume=" + W2U(volume));
            if (MountEspByGuid(volume, espRoot, log)) {
                EspDiagLine("AcquireEspRoot OK via volume GUID: " + W2U(espRoot));
                return true;
            }
        }
        // ②c PE 卷枚举拿不到 letter 时的兜底：已有盘符 + 卷标 ESP + 大小核对。
        if (guidMatched) {
            const std::wstring byLabel = FindEspDriveByLabel(esp);
            if (!byLabel.empty()) {
                espRoot = byLabel;
                log += "ESP via label/size fallback at ";
                log += (char)espRoot[0];
                log += ":\\\n";
                EspDiagLine("AcquireEspRoot OK via label/size: " + W2U(byLabel));
                return true;
            }
            EspDiagLine("label/size fallback found nothing");
        }
        // ③ 回退（仍限定目标盘）
        PartitionInfo fb;
        if (FindEspPartitionFallback(fb, targetDisk) && !fb.letter.empty()) {
            espRoot = fb.letter + L":\\";
            log += "ESP via fallback (FAT partition with \\EFI boot files)\n";
            EspDiagLine("AcquireEspRoot OK via FAT/EFI fallback: " + W2U(espRoot));
            return true;
        }
        EspDiagLine("AcquireEspRoot attempt " + std::to_string(attempt + 1) +
                    " failed; retry/backoff");
        if (attempt < 2) {
            log += "ESP acquire retry " + std::to_string(attempt + 1) +
                   "/3 (volume not ready)\n";
            Sleep(attempt == 0 ? 300 : 700);
        }
    }
    EspDiagLine("AcquireEspRoot FAILED targetDisk=" + std::to_string(targetDisk) +
                " partitions=" + W2U(DescribePartitions()));
    return false;
}


// BIOS 就地还原：目标自身已有可引导的 BCD（bootmgr + \Boot\BCD + winload 条目）
// → 跳过 bcdboot，保住第三方启动项（0.6.35，与 UEFI 路径同规则）。
// 2026-10-27（用户反馈 2）：极简 PE 没有 bcdedit.exe → **退化为文件级判定**
// （BCD 里 UTF-16LE 出现 winload 即认为有条目），否则永远判 invalid、
// 白白去跑不存在的 bcdboot。
// PIT-209：有 bcdedit 时用**与 UEFI 同一套**产物断言（displayorder + winload +
// 默认项指向本分区）——以前只看 `{default}` 里有没有 "winload" 字样，指向
// D: 的旧条目也能过。
bool BiosBcdLooksValid(wchar_t letter, std::string& detail) {
    std::wstring root = std::wstring(1, letter) + L":\\";
    bool bootmgr = GetFileAttributesW((root + L"bootmgr").c_str()) !=
                   INVALID_FILE_ATTRIBUTES;
    std::wstring bcdPath = root + L"Boot\\BCD";
    bool bcd = GetFileAttributesW(bcdPath.c_str()) != INVALID_FILE_ATTRIBUTES;
    bool winload = false;
    bool haveBcdEdit = false;
    if (bcd) {
        const std::wstring bcdedit = SysToolPath(L"bcdedit.exe");
        haveBcdEdit = GetFileAttributesW(bcdedit.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (haveBcdEdit) {
            std::string vd;
            winload = VerifyBcdStore(bcdPath,
                                     std::wstring(1, letter) + L":\\Windows",
                                     /*uefi=*/false, vd);
            detail = std::string("verify=") + (winload ? "ok" : "fail") +
                     (vd.empty() ? "" : (" (" + vd + ")"));
        } else {
            std::string fd;
            winload = BcdFileLooksUsable(bcdPath, fd);
            detail = fd;
        }
    }
    detail = "bootmgr=" + std::to_string((int)bootmgr) +
             " bcd=" + std::to_string((int)bcd) +
             " bcdedit=" + std::to_string((int)haveBcdEdit) +
             " winload=" + std::to_string((int)winload) + " " + detail;
    return bootmgr && bcd && winload;
}


// 系统工具是否可用。极简 PE（Win8PE/NewBee 等）里 bcdboot / bcdedit / bootsect
// 常常一个都没有 —— 引导必须能靠随包资产自足（2026-10-27，用户反馈 2）。
bool ToolAvailable(const wchar_t* exe) {
    return GetFileAttributesW(SysToolPath(exe).c_str()) != INVALID_FILE_ATTRIBUTES;
}


// 目标分区该走 UEFI/ESP 分支吗？**以目标磁盘分区风格为准**（boot/bootpath.h）：
//   MBR 目标 → BIOS/GRUB4DOS 链（哪怕固件是 UEFI：那台机器本来就是 legacy 启动的）；
//   GPT 目标 → UEFI/ESP 链。
// 2026-09-29 修：此前只看 `IsUefiFirmware()`，导致 "UEFI 固件 + MBR 系统盘" 的机器
// 误走 ESP 分支 → "未找到 ESP 分区" 直接失败（用户实测，旧版 0.4.1 起一直存在）。
// 对外声明在 ops.h（GUI 要用它给"安装菜单成功"的提示分链措辞，PIT-094）——
// 定义必须在匿名命名空间**之外**，否则 ops.h 的声明没有外部定义、链接不到。
bool UseUefiBootFor(const PartitionInfo& t) {
    return ShouldUseUefiBoot(IsUefiFirmware(), DiskStyleOf(t.diskIndex));
}

// 启动项/固件变量写入失败时的统一提示（用户 2026-10-27 需求）：
// 装了金山毒霸/360 一类底层拦截的机器上，bcdedit / BootNext 都会"假成功"——
// 「被安全软件拦截」的结构化信号（2026-10-28，用户规格）：
// 火绒/360 会拦本进程写引导区/启动项（实测：bootsect 这类微软签名工具放行，
// 我们自己的进程一律 ERROR_ACCESS_DENIED）。GUI 据此弹「请退出防护 → 重试」。
// 用线程局部标志**不靠解析文案**（文案会翻译/改动）；工作线程内 Reset → 取走。
namespace {
thread_local bool t_bootBlocked = false;
void MarkBootBlocked() { t_bootBlocked = true; }
}  // namespace

void ResetBootBlocked() { t_bootBlocked = false; }
bool TakeBootBlocked() {
    const bool b = t_bootBlocked;
    t_bootBlocked = false;
    return b;
}

// 「安装到启动」前的能力扫描（2026-10-28 用户规格）。
// 与 StageRestoreImpl 的 block 预检同源（W1/W2），但**只读元数据、不动盘**，
// 让 GUI 在安装前就能告诉用户"要不要解压"。
bool CanRestoreInLinuxOnly(const std::wstring& image, int index,
                           size_t* badReparse, size_t* encrypted,
                           std::string& err) {
    if (badReparse) *badReparse = 0;
    if (encrypted) *encrypted = 0;
    // 加密镜像（ZJENC2）：救援层用 dm-crypt 映射（开机输口令）→ **可以用 B 引擎**。
    // 但内容检查（EFS/重解析点）要在救援层里对着解密后的镜像做 —— 脚本在
    // mkntfs 之前完成，命中即中止、不动目标分区（PIT-138 同款 fail-closed）。
    if (ImageIsEncrypted(image)) {
        err.clear();
        LogInfo("menu scan: encrypted image -> linux-only allowed (password at boot)");
        return true;
    }
    WimEngine eng;
    std::vector<std::wstring> bad;
    size_t enc = 0;
    const int eff = index < 1 ? 1 : index;
    const int rc = eng.ScanImageForLinuxRestore(image, eff, bad, &enc);
    if (badReparse) *badReparse = bad.size();
    if (encrypted) *encrypted = enc;
    if (rc != 0) {
        err = Tr("读取镜像元数据失败（rc=") + std::to_string(rc) + Tr("）");
        LogWarn("menu scan: ScanImageForLinuxRestore rc=" + std::to_string(rc) +
                " image=" + W2U(image));
        return false;
    }
    LogInfo("menu scan: linux-only check image=" + W2U(image) + " index=" +
            std::to_string(eff) + " reparse=" + std::to_string(bad.size()) +
            " efs=" + std::to_string(enc));
    return bad.empty() && enc == 0;
}

// 命令返回 0，实际没落盘，重启后照旧进原系统，用户完全没有提示。凡写后
// 读回校验失败（或命令本身失败）都要带上这句，指向"退出防护重试/PE 就地还原"。
std::string BootWriteBlockedHint() {
    MarkBootBlocked();
    return Tr(" 可能被安全软件（如火绒/360）拦截，写入未生效："
              "请退出其防护后重试，或用 PE 就地还原。");
}


// ── 修复引导（docs/15 · P5）──────────────────────────────────────
// 定位系统盘 → 找 ESP（GUID → mountvol → FAT 回退）→ bcdboot → 产物断言。
// 用于：还原后引导坏了、或从 PE 里救一台起不来的机器（不必重装）。
int RepairBoot(int disk, int part, std::string& msg) {
    // ① 目标系统盘
    PartitionInfo target;
    bool found = false;
    auto disks = EnumerateDisks();
    for (const auto& d : disks) {
        for (const auto& p : d.parts) {
            if (p.letter.empty())
                continue;
            if (disk > 0 || part > 0) {
                if ((int)d.index != disk || (int)p.partNumber != part)
                    continue;
            }
            std::wstring win = p.letter + L":\\Windows\\System32\\winload.exe";
            if (GetFileAttributesW(win.c_str()) == INVALID_FILE_ATTRIBUTES)
                continue;
            target = p;
            found = true;
            if (p.isSystem)
                break;  // 优先正在运行的系统盘
        }
        if (found && target.isSystem)
            break;
        if (found && (disk > 0 || part > 0))
            break;
    }
    if (!found) {
        msg = Tr("未找到系统盘（没有分区含 \\Windows\\System32\\winload.exe）。\n"
                 "若指定了 --disk/--part，请确认分区号正确且有盘符。\n当前分区表：\n") +
              W2U(DescribePartitions());
        LogError("repair-boot: target system partition not found");
        return 1;
    }
    std::wstring winDir = target.letter + L":\\Windows";
    LogInfo("repair-boot: target=" + W2U(target.letter) + ": disk=" +
            std::to_string(target.diskIndex) + " part=" +
            std::to_string(target.partNumber));
    std::string out;
    std::string blog;
    if (!UseUefiBootFor(target)) {
        // BIOS/MBR：优先 bcdboot（可用时最省事），缺失/失败 → 随包资产自足
        //（2026-10-27，用户反馈 2：极简 PE 没有 bcdboot）。
        const std::wstring root = target.letter + L":\\";
        bool done = false;
        if (ToolAvailable(L"bcdboot.exe")) {
            int rc = RunProcess(SysToolPath(L"bcdboot.exe"),
                                winDir + L" /s " + target.letter + L": /f BIOS",
                                out);
            LogInfo("repair-boot bcdboot (BIOS) rc=" + std::to_string(rc) + " / " +
                    out);
            done = rc == 0;
        } else {
            LogInfo("repair-boot: bcdboot.exe missing -> self-contained BIOS boot");
        }
        if (!done) {
            std::string slog, serr;
            const bool sok =
                StageBiosBootTree(root, winDir, AppDir(),
                                  /*overwriteBcd=*/false, slog, serr);
            LogInfo(std::string("repair-boot self-contained BIOS: ") +
                    (sok ? "ok" : "FAILED") + "\n" + slog);
            if (!sok) {
                msg = Tr("修复引导失败：") + serr;
                LogError(msg);
                return 1;
            }
        }
        // 分区引导区 / 磁盘 MBR 代码：运行中的系统盘可能锁不住、安全软件也可能拦写
        //（火绒「写磁盘保留扇区」）。bootself 内部已按「先查 → bootsect → 自足写」，
        // 到这一步还失败 = 确实写不进去 → 如实告诉用户怎么办，而不是假装成功。
        std::string prlog;
        if (EnsureNtfsBootRegion(root, AppDir(), prlog)) {
            LogInfo("repair-boot pbr: " + prlog);
        } else {
            LogWarn("repair-boot pbr (non-fatal): " + prlog);
            MarkBootBlocked();  // GUI 据此弹「退出防护 → 重试」
            msg += Tr("\n注意：分区引导区（PBR）未能写入 —— 可能被安全软件拦写。请临时退出火绒/360 的「写磁盘保留扇区」保护（或把本程序加入信任）后重试，或在 PE 里修复。");
        }
        std::string mrlog;
        if (EnsureDiskMbrCode(target.diskIndex, AppDir(), mrlog))
            LogInfo("repair-boot mbr: " + mrlog);
        else
            LogWarn("repair-boot mbr (non-fatal): " + mrlog);
        // PIT-209：修完引导顺手清"幽灵启动项"，并把单系统菜单收敛掉
        //（这是用户实际报的"只有一个系统却停在选择菜单"）。
        {
            const std::wstring store = root + L"Boot\\BCD";
            std::string plog, qlog;
            int osLeft = -1;
            const int pruned = BcdPruneDeadEntries(store, winDir, &osLeft, plog);
            LogInfo("repair-boot bcd prune (bios): removed=" +
                    std::to_string(pruned) + " reachable_os=" +
                    std::to_string(osLeft) + "\n" + plog);
            BcdQuietSingleOsMenu(store, qlog);
            LogInfo("repair-boot menu policy (bios): " + qlog);
        }
        msg = Tr("引导已修复（BIOS）。");
        return 0;
    }
    // UEFI：找 ESP（含回退）
    std::wstring espRoot;
    bool guidMatched = false;
    std::string detail;
    if (!AcquireEspRoot(espRoot, guidMatched, blog, (int)target.diskIndex)) {
        msg = Tr("未找到可用的 ESP 引导分区，无法修复引导。\n"
                 "常见原因：DiskGenius 重建分区时漏建 ESP，或把 ESP 标成了 Basic "
                 "Data（应改为 EFI System 类型）。\n当前分区表：\n") +
              W2U(DescribePartitions());
        LogError(msg);
        return 1;
    }
    if (!guidMatched)
        LogWarn("repair-boot: ESP partition type is not EFI System; used fallback");
    // PIT-209：清掉"指向已删内核"的临时启动项 —— 固件每次开机都会先去试它，
    // 有的固件就此卡住/反复重启（客户机 2026-11-02 的嫌疑之一）。
    // 有暂存任务时函数内部会自己跳过。
    {
        std::string clog;
        CleanupStaleUefiEntries(clog);
        if (!clog.empty())
            LogInfo("repair-boot stale entry cleanup: " + clog);
    }
    // 2026-10-27（用户反馈 2）：bcdboot 缺失/失败 → 随包资产自足（含 GUID 替换与
    // 固件 "Windows Boot Manager" 条目）。模板/空间预检只对 bcdboot 这条路生效。
    bool rebuilt = false;
    if (ToolAvailable(L"bcdboot.exe") && TargetBcdTemplateOk(target.letter[0], detail) &&
        EspHasRoomForBcdboot(espRoot, detail)) {
        int rc = RunProcess(
            SysToolPath(L"bcdboot.exe"),
            winDir + L" /s " + std::wstring(1, espRoot[0]) + L": /f UEFI", out);
        LogInfo("repair-boot bcdboot (UEFI) rc=" + std::to_string(rc) + " / " +
                out);
        rebuilt = rc == 0;
    } else {
        LogInfo("repair-boot: bcdboot unavailable/not applicable -> "
                "self-contained ESP boot (" + detail + ")");
    }
    if (!rebuilt) {
        std::string slog, serr;
        const bool sok = EnsureUefiBootFiles(
            espRoot, std::wstring(1, target.letter[0]) + L":\\", winDir, AppDir(),
            /*overwriteBcd=*/false, slog, serr);
        LogInfo(std::string("repair-boot self-contained ESP: ") +
                (sok ? "ok" : "FAILED") + "\n" + slog);
        if (!sok) {
            UnmountEsp(espRoot, blog);
            msg = Tr("修复引导失败：") + serr;
            LogError(msg);
            return 1;
        }
    }
    // PIT-209：先清"幽灵启动项"（分区已删/分区里没有 Windows）+ 单系统菜单收敛。
    {
        const std::wstring store = espRoot + L"EFI\\Microsoft\\Boot\\BCD";
        std::string plog, qlog;
        int osLeft = -1;
        const int pruned = BcdPruneDeadEntries(store, winDir, &osLeft, plog);
        LogInfo("repair-boot bcd prune (uefi): removed=" +
                std::to_string(pruned) + " reachable_os=" +
                std::to_string(osLeft) + "\n" + plog);
        BcdQuietSingleOsMenu(store, qlog);
        LogInfo("repair-boot menu policy (uefi): " + qlog);
    }
    // PIT-208：修引导的收口就是"固件下一次真的去启 Windows" —— 条目必须在
    // BootOrder 第一位（客户机 2026-11-02：还原成功却反复重启，元凶就在这）。
    {
        std::string wlog;
        if (EnsureWindowsBootEntry(espRoot, wlog, /*front=*/true))
            LogInfo("repair-boot firmware entry: " + wlog);
        else
            LogWarn("repair-boot firmware entry (non-fatal): " + wlog);
        std::vector<std::string> bo;
        UefiBootOrderSummary(bo);
        for (size_t i = 0; i < bo.size(); ++i)
            LogInfo(std::string("repair-boot bootorder") +
                    (i == 0 ? "[0] " : "    ") + bo[i]);
    }
    if (!VerifyEspBcd(espRoot, winDir, detail)) {
        UnmountEsp(espRoot, blog);
        msg = Tr("引导仍不完整（校验未通过）：") + detail;
        LogError(msg);
        return 1;
    }
    // 打印 ESP BCD 的关键行，便于用户/我们确认（displayorder 是不是有值）
    {
        std::string enumOut;
        RunProcess(SysToolPath(L"bcdedit.exe"),
                   L"/store \"" + espRoot + L"EFI\\Microsoft\\Boot\\BCD\" /enum {bootmgr}",
                   enumOut);
        for (size_t pos = 0; pos < enumOut.size();) {
            size_t e = enumOut.find('\n', pos);
            std::string line = enumOut.substr(pos, e == std::string::npos ? e : e - pos);
            if (line.find("displayorder") != std::string::npos)
                LogInfo("repair-boot: " + line);
            if (e == std::string::npos)
                break;
            pos = e + 1;
        }
    }
    UnmountEsp(espRoot, blog);
    msg = Tr("引导已修复并校验通过（UEFI）\n");
    return 0;
}

}  // namespace sysrecover
