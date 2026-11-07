// 九转还原：ops_backup（阶段 2 从 src/app/ops.cpp 机械拆分而来，逻辑未改）。
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

// ────────────────── ESP 分区备份（方案 C：并入主镜像，用户 2026-09-30 规格） ──────────────────

// ESP 子镜像的 <DESCRIPTION> 注释（"镜像里直接加注释"的落点；镜像内元数据，
// 不做 Tr 界面翻译）。还原侧防呆按子镜像 **name 含 "ESP"** 判断（见 StageRestore）。
static const wchar_t kEspImageDesc[] =
    L"SysRecover ESP partition backup; restored to the ESP automatically "
    L"when restoring the system (never restore this image alone)";

// EA 修复子镜像的 <DESCRIPTION>（PIT-122）：内容 = eapack.dat（带 EA 的文件
// 路径+名字+值）+ zj-ea-apply.exe（首启补写器）+ zj-regpol.bin（同步策略片段）。
// 救援层按契约 ea_index 解包投放，目标系统首启（登录前 SYSTEM）自动写回 EA。
static const wchar_t kEaImageDesc[] =
    L"SysRecover extended-attributes (EA) fix data; deployed to the target by "
    L"the rescue layer and applied on first boot (never restore this image alone)";

// ESP 捕获的排除配置（wimscript 格式，临时文件，同 EnsureExclusionConfig 做法）：
//   · \EFI\ZJRESTORE —— 我们的常驻救援载荷（约 33MB，还原后 Windows 侧会重新
//     部署，进镜像纯属浪费，且它不是"原厂 ESP 内容"）；
//   · \System Volume Information —— 系统卷信息，无备份价值（读得到也排除）；
//   · \EFI\Microsoft\Boot\BCD(+.LOG*) —— **实测 rc=47 的真凶**：运行中的 Windows
//     把 ESP 的 BCD 当注册表 hive 挂着（HKLM\BCD00000000）→ 独占锁，wimlib 一读
//     就 Failed to open a file（PIT-051 同源：那里 CopyFile 也必失败）。BCD 还原
//     后由 `bcdboot` 重建（PIT-060/090 已在流程里），且 Linux 恢复用目录模式
//     apply（只新增/覆盖、不删）→ ESP 上现役 BCD 不会被并入的子镜像动到，正合适。
// ⚠️ 排除目录写法**无尾斜杠**（与 exclude.cpp 系统盘清单一致）：wimlib/DISM 语义
//    是"匹配目录条目本身即跳过整棵树"，带尾反斜杠的 glob 反而不匹配（实测）。
static std::wstring EnsureEspExclusionConfig() {
    static const char kContent[] =
        "[ExclusionList]\n"
        "\\EFI\\ZJRESTORE\n"
        "\\System Volume Information\n"
        "\\EFI\\Microsoft\\Boot\\BCD\n"
        "\\EFI\\Microsoft\\Boot\\BCD.LOG\n"
        "\\EFI\\Microsoft\\Boot\\BCD.LOG1\n"
        "\\EFI\\Microsoft\\Boot\\BCD.LOG2\n";
    wchar_t tmpDir[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, tmpDir) == 0)
        return L"";
    std::wstring path =
        std::wstring(tmpDir) + L"zjrestore-esp-exclusion.ini";
    HANDLE h =
        CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return L"";
    DWORD written = 0;
    BOOL ok = WriteFile(h, kContent, sizeof(kContent) - 1, &written, nullptr);
    CloseHandle(h);
    if (!ok || written != sizeof(kContent) - 1)
        return L"";
    return path;
}


// 备份目标可写预检（2026-10-21）：wimlib 只回 rc=47 "Failed to open a file"，
// 分不清“权限/网络路径/无效名/路径过长”。这里用 CreateFileW 在目标目录建一个
// 探针文件（与资源管理器同一 API），失败时把 Win32 错误码翻译成可操作的提示。
static std::wstring Win32ErrorText(DWORD e) {
    wchar_t* p = nullptr;
    const DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, e, 0, (LPWSTR)&p, 0, nullptr);
    std::wstring s = (n && p) ? std::wstring(p, n) : std::wstring();
    if (p) LocalFree(p);
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' ||
                          s.back() == L' '))
        s.pop_back();
    return s;
}


static bool ProbeBackupDest(const std::wstring& dest, std::string& err) {
    const std::wstring dir = ParentDir(dest);
    if (dir.empty()) return true;
    wchar_t name[64] = {};
    swprintf(name, 64, L"\\zjprobe-%lu.tmp",
             (unsigned long)GetCurrentProcessId());
    const std::wstring probe = dir + name;
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        std::wstring msg = Tr(L"备份目标不可写（错误码 ") +
                           std::to_wstring((unsigned long)e) + Tr(L"）：") +
                           Win32ErrorText(e);
        if (e == 5)
            msg += Tr(L"\n提示：本程序以管理员运行；若共享是按当前登录用户"
                      L"授权的，UAC 提权可能使用了另一个账号，请确认提权后"
                      L"的账号也能访问该共享。");
        else if (e == 53 || e == 67 || e == 64)
            msg += Tr(L"\n提示：请检查机器名/共享名是否正确、网络是否可达。");
        else if (e == 3 || e == 2)
            msg += Tr(L"\n提示：目标文件夹不存在。");
        else if (e == 123)
            msg += Tr(L"\n提示：路径格式不正确（应为 \\\\机器名\\共享名\\...）。");
        else if (e == 206)
            msg += Tr(L"\n提示：路径过长。");
        err = W2U(msg);
        LogError("backup preflight failed: " + err);
        return false;
    }
    CloseHandle(h);
    DeleteFileW(probe.c_str());
    return true;
}


int RunBackup(const BackupRequest& reqIn, ProgressFn progress,
              std::string& err, ErrAdvice* adv,
              std::vector<std::wstring>* skipped) {
    if (adv) *adv = ADV_NONE;
    if (skipped) skipped->clear();
    std::vector<std::wstring> skippedList;
    if (reqIn.dest.empty()) {
        err = Tr("缺少目标镜像路径");
        return 1;
    }
    // ── 加密备份（ZJENC1，docs/30）──
    // 明文先写到 <目标>.plain.tmp（沿用现有 stage/原子写/Probe 全流程），
    // 收尾再加密成 <目标>.zje 并**删除明文**。任何失败都不留明文镜像。
    BackupRequest req = reqIn;
    const bool encrypt = !reqIn.password.empty();
    // 兜底：任何提前 return 都把明文中转删掉（加密备份的红线是"绝不留明文"）。
    struct PlainGuard {
        std::wstring p;
        bool on = false;
        ~PlainGuard() {
            if (on)
                DeleteFileW(p.c_str());
        }
    } plainGuard{reqIn.dest, false};
    if (encrypt) {
        if (reqIn.append) {
            err = Tr("加密备份不支持追加（增量请用未加密的 .wim/.esd）");
            return 2;
        }
        if (reqIn.dest.size() < 4 ||
            _wcsicmp(reqIn.dest.c_str() + reqIn.dest.size() - 4, L".zje") != 0) {
            err = Tr("加密备份的输出文件名必须以 .zje 结尾");
            return 2;
        }
        req.dest = reqIn.dest + L".plain.tmp";
        plainGuard.p = req.dest;
        plainGuard.on = true;
    }
    // 源路径归一（2026-09-28 用户 CLI 复现：`--source C:` 备份报 rc=47）：
    //   · Win32 里 `C:` 是“该盘的**当前目录**”，不是根，也不带 wimlib 要求的
    //     尾斜杠 → 盘符根判定落空 → 没走 VSS 热备，直接扫活动系统；
    //   · 扫活动系统会在某个正被占用的文件上失败，wimlib 只回
    //     `rc=47 Failed to open a file`，看不出是谁的锅。
    // 统一成 `C:/` 后，`C:` / `C:\` / `C:/` 三条写法等价（GUI 本来就传 `X:/`）。
    const std::wstring source = NormalizeVolumeRoot(req.source);
    const bool volumeRoot = IsVolumeRoot(source);
    // 目标目录不存在也要建出来：`D:\backup\x.esd` 里 D:\backup 没建时，
    // wimlib 建不出输出文件，同样只报 rc=47（2026-09-28 实测 3 秒即失败）。
    {
        std::wstring dir = ParentDir(req.dest);
        if (!dir.empty() && !MakeDirTree(dir)) {
            err = std::string(Tr("无法创建备份目录：")) + W2U(dir);
            LogError(err);
            return 1;
        }
    }
    if (!ProbeBackupDest(req.dest, err)) return 1;
    // M7（1.0）：备份前空间与 FAT32 单文件上限预检。
    // 0.9.96 直接开写：FAT32 目标卷写满 4GB 才失败，而且可能已覆盖旧文件。
    {
        const std::wstring dir = ParentDir(req.dest);
        ULONGLONG freeB = 0;
        ULARGE_INTEGER totalB = {}, totalFreeB = {};
        wchar_t fsName[16] = {};
        if (dir.empty() ||
            !GetDiskFreeSpaceExW(dir.c_str(), nullptr, &totalB, &totalFreeB) ||
            !GetVolumeInformationW(dir.c_str(), nullptr, 0, nullptr, nullptr,
                                   nullptr, fsName, 16)) {
            LogWarn("backup precheck: cannot query dest volume; continue");
        } else {
            freeB = totalFreeB.QuadPart;
            ULONGLONG srcUsed = 0;
            ULARGE_INTEGER srcTotal = {}, srcFree = {};
            if (volumeRoot &&
                GetDiskFreeSpaceExW(source.c_str(), nullptr, &srcTotal, &srcFree))
                srcUsed = srcTotal.QuadPart - srcFree.QuadPart;
            const bool fat = _wcsicmp(fsName, L"FAT32") == 0 ||
                             _wcsicmp(fsName, L"FAT") == 0;
            // FAT32：单文件硬上限 4GB（减 64MB 留 FAT 表/簇开销）。
            if (fat && srcUsed > (4ULL << 30) - (64ULL << 20)) {
                err = std::string(Tr("目标卷是 FAT32，单个镜像文件不能超过 4GB；源卷已用约 ")) +
                      std::to_string(srcUsed >> 30) +
                      Tr(" GB。请把镜像写到 NTFS/exFAT 分区，或缩小备份范围。");
                LogError(err);
                if (adv) *adv = ADV_SPACE;
                return 1;
            }
            // 空间：按"源已用 × 0.5 + 128MB"估（WIM 常规压缩经验值，仅作下限）。
            const ULONGLONG needB =
                volumeRoot ? srcUsed / 2 + (128ULL << 20) : (128ULL << 20);
            if (freeB < needB) {
                err = std::string(Tr("备份目标空间不足：预计至少需要约 ")) +
                      std::to_string(needB >> 20) +
                      std::string(Tr(" MB，可用 ")) + std::to_string(freeB >> 20) +
                      Tr(" MB。请清理目标分区后重试。");
                LogError(err);
                if (adv) *adv = ADV_SPACE;
                return 1;
            }
            LogInfo("backup precheck: fs=" + W2U(fsName) +
                    " freeMB=" + std::to_string(freeB >> 20) +
                    " needMB=" + std::to_string(needB >> 20));
        }
    }
    if (!AcquireOpLock()) {
        err = Tr("已有备份/还原实例在运行");
        return 1;
    }
    // M10（1.0）：拿到单实例锁之后，清理上一次异常退出（断电/杀进程）留下的
    // 明文中转（<目标>.plain.tmp / .stage / .stage.tmp）。加密备份的红线是
    // "磁盘上绝不留明文"，0.9.96 只在正常路径删，崩溃后明文会一直躺着。
    if (encrypt) {
        for (const auto& p : {req.dest, req.dest + L".stage",
                              req.dest + L".stage.tmp"}) {
            if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) {
                SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(p.c_str());
                LogWarn("stale plaintext temp removed: " + W2U(p));
            }
        }
    }
    ProgressUpdate("backup", 0, "start");
    LogInfo("backup source=" + W2U(source) + " dest=" + W2U(req.dest) +
            " compress=" + req.compress +
            (volumeRoot ? " (volume root -> VSS hot backup)" : ""));
    const ULONGLONG t0 = GetTickCount64();  // P7：历史记录用
    // 盘符根（C:/ 或 C:\）→ 热备：VSS 快照 + 排除配置（PIT-008/009）；
    // 也可用 req.snapshot 显式强制（例如备份正在使用的数据库目录）；
    // req.noSnapshot 显式**禁止**（PE / 离线卷 / 挂载的 VHD 卷上 VSS 不可用）。
    bool snapshot = (req.snapshot || volumeRoot) && !req.noSnapshot;
    if (req.noSnapshot && volumeRoot)
        LogInfo("snapshot disabled by --no-snapshot (cold backup of volume root)");
    // CPU 硬上限（BackupRequest::cpuCap，GUI 的「限制CPU」下拉）：在动数据之前
    // 设好，所有成功/失败退出路径都由 guard 复位回“不限”。
    struct CpuCapGuard {
        int pct;
        explicit CpuCapGuard(int p) : pct(p) {
            if (pct <= 0) return;
            std::string why;
            if (!SetCpuCap(pct, &why))
                LogWarn("cpu cap not applied: " + why);
            else
                LogInfo("cpu cap = " + std::to_string(pct) + "%");
        }
        ~CpuCapGuard() {
            if (pct <= 0) return;
            std::string why;
            SetCpuCap(0, &why);
        }
    } cpuGuard(req.cpuCap);
    std::wstring errw;  // VSS 检查的多行提示（成功路径不会用到）
    // VSS 前置检查（用户 2026-09-26 规格）：服务停着就帮用户启动、起不来就给
    // 明确的处理指引（别等 wimlib 报 rc=89 让人干猜）；guard 析构把我们启动的
    // 服务停回原状（"完成后关闭"），覆盖下面所有成功/失败退出路径。
    vss::BackupGuard vss_guard;
    if (snapshot && !vss_guard.Ensure(errw)) {
        ReleaseOpLock();
        ProgressDone("backup", "failed");
        if (adv) *adv = ADV_VSS;  // 消息自带处理步骤 → ErrorAdvice 不再追加
        err = W2U(errw);  // 多行中文处理指引（GUI MessageBox / CLI 原样打印）
        LogError(err);
        return 1;
    }
    // 热备前把注册表 hive 刷盘：运行中的 hive 常处于"脏"状态
    // （REGF base block 的 primary_seq != secondary_seq），刷一次可让 VSS 抓到
    // 干净的 hive；即便仍是脏的，随包捕获的 *.LOG1/.LOG2 也能在开机时恢复
    // （PIT-056：这两者缺一，还原后就会黑屏起不来）。
    if (snapshot) {
        RegFlushKey(HKEY_LOCAL_MACHINE);
        RegFlushKey(HKEY_USERS);
        LogInfo("registry hives flushed before snapshot");
    }
    WimEngine engine;
    if (!engine.ok()) {
        ReleaseOpLock();
        ProgressDone("backup", "failed");
        err = Tr("wimlib 初始化失败");
        return 1;
    }
    // 重解析点检出（PIT-135，2026-10-08；1019 案修订，2026-10-19）：Linux 侧
    // wimlib（NTFS-3G 块模式）无法设置非微软 reparse（libntfs-3g 已知 bug，
    // wimlib NEWS 明说），个别微软 tag 变体（junction 落文件等）同为 set 链
    // EINVAL 必失败——实测 Intel ipfsrv.dptf 令整个 apply rc=58 中止、目标分区
    // 只剩半成品。判定 = IsUnsettableReparse（valid_reparse_data 移植）。
    // 备份时排除（并留日志），否则重启类还原必然失败。
    std::vector<std::wstring> badRep;
    int badN = ea::FindUnsettableReparse(source, badRep);
    if (badN > 0)
        LogWarn("reparse: " + std::to_string(badN) +
                " unsettable reparse entry(ies) excluded (Linux apply cannot "
                "set them, PIT-135); first: " + W2U(badRep[0]));
    std::wstring cfg =
        EnsureExclusionConfig(source, badN > 0 ? &badRep : nullptr);
    if (cfg.empty())
        LogError("排除配置生成失败，热备可能因易失文件报 rc=88（PIT-009）");
    else
        LogInfo("exclusion config: " + W2U(cfg));
    // ── ESP / EA 并入主镜像 ──
    // ESP（方案 C，用户 2026-09-30 规格 / 无忧 66 楼）：作为**同一个文件里的
    // 子镜像**（name="ESP"），还原时由契约 esp_index 定位、救援层恢复；
    // EA（PIT-122，2026-10-06）：子镜像 name="ZJEA"（见下）。
    // 原子性：非 append 时**一律**先写 `<dest>.stage`（ESP/EA 并入结束且 stage
    // 确认可用后才 rename 成正式文件）—— 并入失败绝不损坏已写好的主镜像；
    // 无子镜像时多一次 rename，代价可忽略（Windows 同卷 rename 是元数据操作）。
    const bool stage = !req.append;
    const std::wstring mainDest = stage ? req.dest + L".stage" : req.dest;
    if (stage) {
        DeleteFileW(mainDest.c_str());             // 上次异常残留的 stage
        DeleteFileW((mainDest + L".tmp").c_str());  // 其 Capture 半成品
    }
    int rc;
    if (req.append)
        rc = engine.Append(source, req.dest, req.compress, req.name,
                           snapshot, cfg, progress, L"", &skippedList);
    else
        rc = engine.Capture(source, mainDest, req.compress, req.name,
                            snapshot, cfg, progress, &skippedList);
    if (rc != 0 && rc != WimEngine::kWimPartialSkipped) {
        ReleaseOpLock();
        ProgressDone("backup", "failed");
        // L1：译文不再当 printf 格式串（译文含 % 会读错参数）。
        err = Tr("备份失败(rc=");
        err += std::to_string(rc);
        err += "): ";
        err += W2U(WimEngine::ErrorString(rc));
        LogError(err);
        return 1;
    }

    // ── ESP 并入（主镜像写出之后、rename/verify 之前）──
    // 没勾 --esp，或本机没有 ESP（BIOS/MBR 等）→ 只记日志跳过（不算失败），
    // 绝大多数任务不带；stage 流程照样走（多一次无害的 rename）。
    int erc = 0;
    if (req.esp) {
        ProgressUpdate("backup", 100, "esp");
        // 阶段标记进 GUI 状态栏/CLI 控制台（ProgressUpdate 只写 progress.json，
        // 外部工具看得到、用户看不到 → 100% 后像卡死，PIT-098）。
        if (progress) progress(100, "esp");
        std::wstring espRoot;
        bool guidMatched = false;
        std::string blog;
        if (!AcquireEspRoot(espRoot, guidMatched, blog)) {
            LogWarn("esp merge skipped (no ESP found): " + blog);
        } else {
            if (!guidMatched)
                LogWarn("ESP partition type is not EFI System; used fallback "
                        "(esp merge)");
            WimEngine espEng;  // 新会话读 stage 里刚写完的主镜像（见下）
            std::wstring espSrc = espRoot;
            espSrc.back() = L'/';  // wimlib 源路径强制盘符根尾斜杠（§6）
            // snapshot=false：ESP 是 FAT 小分区，没有"易失文件"问题，
            // 也没必要再开一次 VSS 快照。
            erc = espEng.Append(espSrc, mainDest, req.compress, L"ESP", false,
                                EnsureEspExclusionConfig(), progress,
                                kEspImageDesc, &skippedList);
            UnmountEsp(espRoot, blog);
            LogInfo("esp append rc=" + std::to_string(erc) + " dest=" +
                    W2U(mainDest) + " (" + blog + ")");
        }
    }

    if (erc == WimEngine::kWimPartialSkipped) {
        // ESP 子镜像部分成功（有文件读不到被跳过）：主镜像完整可用，交给
        // skipped 清单统一提示（GUI/CLI 已有"备份不完整"），不判整个备份失败。
        LogWarn("esp append partial: some files skipped (see skipped list)");
        erc = 0;
    }

    // ── EA 采集并入（PIT-122，2026-10-06）──
    // 背景：Linux 侧 wimlib apply 丢 Windows EA（apply.out: `Ignoring extended
    // attributes of N files`，PIT-120 客户实测 804 个文件）；Windows 侧不丢，
    // 但重启类还原必须走 Linux（§2 红线 1）。做法：把源里带 EA 的文件（路径+
    // 名字+值）打包成 eapack.dat，连同首启补写器/同步策略片段打成子镜像
    // "ZJEA"；救援层投放后由目标系统**首次开机**（登录前、SYSTEM）自动写回。
    // 非致命：采集/并入失败只警告 —— EA 修复是增益，主镜像照常交付。
    {
        std::wstring root = source;
        while (!root.empty() && (root.back() == L'/' || root.back() == L'\\'))
            root.pop_back();
        if (root.empty())
            root = L"C:";
        // 审计文本沿用 logs\ea-scan-<时间>.txt（用户 2026-10-05 规格）
        std::wstring logsDir = LogBaseDir() + L"\\logs";
        CreateDirectoryW(logsDir.c_str(), nullptr);
        SYSTEMTIME stw = {};
        GetLocalTime(&stw);
        wchar_t stamp[40] = {};
        swprintf(stamp, 40, L"%04u%02u%02u-%02u%02u%02u", stw.wYear, stw.wMonth,
                 stw.wDay, stw.wHour, stw.wMinute, stw.wSecond);
        std::wstring auditPath = logsDir + L"\\ea-scan-" + stamp + L".txt";
        // 临时目录：eapack.dat + zj-regpol.bin + zj-ea-apply.exe
        std::wstring eaTmp = req.dest + L".ea.tmp";
        DeleteTreeRec(eaTmp);
        if (MakeDirTree(eaTmp)) {
            if (progress) progress(99, "ea-scan");
            ProgressUpdate("backup", 99, "ea-scan");
            std::string eaErr;
            int eaN = ea::CaptureVolume(root, eaTmp + L"\\eapack.dat",
                                        auditPath, eaErr);
            if (eaN < 0) {
                LogWarn("EA capture failed: " + eaErr);
            } else if (eaN == 0) {
                LogInfo("EA capture: no EA-bearing files in source");
            } else {
                LogInfo("EA capture: " + std::to_string(eaN) +
                        " EA-bearing file(s); audit: " + W2U(auditPath));
            }
            if (eaN <= 0)  // 无 EA（或失败）时不留空包：补写器将只做引用清理
                DeleteFileW((eaTmp + L"\\eapack.dat").c_str());
            // 非微软重解析点打包（PIT-136，用户 2026-10-08 规格）：Linux 侧
            // wimlib 写不了这类文件（libntfs-3g bug）→ 主镜像已排除（见上），
            // 这里把"内容+reparse 原始数据+属性"打包，首启补写器原生写回。
            std::string repErr;
            int repN =
                ea::CaptureReparse(root, eaTmp + L"\\reppack.dat", repErr);
            if (repN > 0)
                LogInfo("reparse pack: " + std::to_string(repN) +
                        " non-Microsoft reparse file(s) packed for first-boot "
                        "restore" +
                        (repErr.empty() ? "" : " (" + repErr + ")"));
            else if (repN < 0)
                LogWarn("reparse pack failed: " + repErr);
            // 悬空引用扫描（PIT-132）：注册表引用指向"会被备份排除的易失目录"
            // （Temp 等）且文件存在 → 还原后必然缺失（实测：豆包便携版右键扩展
            // DLL 在 Temp → 每次右键卡死）。>0 时并入修复包，首启自动清理。
            std::vector<refscan::DanglingRef> refs =
                refscan::ScanDanglingRefs(true);
            if (!refs.empty()) {
                int miss = 0;
                for (auto& r : refs)
                    if (r.missingNow) miss++;
                LogInfo("ref scan: " + std::to_string(refs.size()) +
                        " volatile-dir reference(s) (" + std::to_string(miss) +
                        " missing now, " +
                        std::to_string((int)refs.size() - miss) +
                        " at-risk) -> first-boot auto-clean");
                for (size_t i = 0; i < refs.size() && i < 10; ++i)
                    LogInfo("ref scan: " + W2U(refs[i].display));
            }
            if (eaN > 0 || !refs.empty() || repN > 0) {
                // 子镜像内容：pack（有 EA 时）+ reppack（有非微软 reparse 时）+
                // 策略片段 + 补写器（x86，x86/x64 都能跑）
                bool okFiles = ea::WriteBytes(eaTmp + L"\\zj-regpol.bin",
                                              ea::BuildSyncPolFragment());
                std::wstring applier = AppDir() + L"\\bootfiles\\zj-ea-apply.exe";
                if (okFiles)
                    okFiles = CopyFileW(applier.c_str(),
                                        (eaTmp + L"\\zj-ea-apply.exe").c_str(),
                                        FALSE) != 0;
                if (!okFiles) {
                    LogWarn("EA deploy files incomplete (zj-ea-apply.exe "
                            "missing?); EA fix skipped");
                } else {
                    WimEngine eaEng;
                    std::wstring eaSrc = eaTmp + L"\\";
                    int earc = eaEng.Append(eaSrc, mainDest, req.compress,
                                            L"ZJEA", false, L"", progress,
                                            kEaImageDesc, &skippedList);
                    if (earc != 0 && earc != WimEngine::kWimPartialSkipped) {
                        LogWarn("EA pack append failed rc=" +
                                std::to_string(earc) +
                                " (main image continues without EA fix)");
                    } else {
                        LogInfo("EA/fix pack appended as subimage ZJEA (ea=" +
                                std::to_string(eaN) + " refs=" +
                                std::to_string(refs.size()) + ")");
                    }
                }
            }
        } else {
            LogWarn("EA temp dir create failed; EA fix skipped");
        }
        DeleteTreeRec(eaTmp);
    }

    // ESP 失败时统一的错误文本（tail 由调用方给）
    auto espFail = [&](const std::string& tail) {
        // L1：固定格式串拼接，避免把译文当 printf 格式串。
        std::string e = Tr("ESP 备份失败(rc=");
        e += std::to_string(erc);
        e += "): ";
        // -1 = 引擎未初始化（wimlib.h 不外泄 → ErrorString(-1) 不可用）
        e += erc == -1 ? std::string(Tr("wimlib 初始化失败"))
                       : W2U(WimEngine::ErrorString(erc));
        e += tail;
        return e;
    };

    // stage → 正式文件（主镜像原子落盘）
    if (stage) {
        if (erc != 0) {
            // ESP 并入半途失败可能把 stage 写坏（wimlib_overwrite 原地重写）→
            // 先 Probe：坏 → 删掉重报；好 → 照常交付（主镜像只是不含 ESP）。
            if (progress) progress(99, "probe");
            WimEngine probe;
            std::wstring why;
            if (probe.Probe(mainDest, why) != 0) {
                DeleteFileW(mainDest.c_str());
                ReleaseOpLock();
                ProgressDone("backup", "failed");
                err = espFail(Tr("（主镜像写入损坏，已删除，请重新备份）"));
                LogError(err);
                return 1;
            }
        }
        if (!MoveFileExW(mainDest.c_str(), req.dest.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DWORD e = GetLastError();
            DeleteFileW(mainDest.c_str());
            ReleaseOpLock();
            ProgressDone("backup", "failed");
            err = Tr("备份写出失败：无法提交镜像文件(err=") +
                  std::to_string(e) + ")";
            LogError(err);
            return 1;
        }
    }
    if (erc != 0) {
        // 走到这里主镜像已落盘（stage 已 rename / append 原地写完）→ 报 ESP
        // 失败但**不回滚**主镜像（A 方案同款语义：主镜像可用，只是不含 ESP）。
        if (!stage) {
            // append 模式是原地写：确认既有文件没被半途 overwrite 写坏
            if (progress) progress(99, "probe");
            WimEngine probe;
            std::wstring why;
            if (probe.Probe(req.dest, why) != 0) {
                ReleaseOpLock();
                ProgressDone("backup", "failed");
                err = espFail(Tr("（镜像写入损坏，请重新备份）"));
                LogError(err);
                return 1;
            }
        }
        ReleaseOpLock();
        ProgressDone("backup", "failed");
        err = espFail(Tr("（主镜像已生成，但未包含 ESP 备份）"));
        LogError(err);
        return 1;
    }

    // 收尾可用性检查（PIT-057 / PIT-106）：默认只做**秒级快检**（打开 +
    // "写入未完成"标记 + 子镜像数）；全量校验只在用户显式 --verify 时跑一次
    // （带真实进度）。不再默认全扫：大镜像扫一遍十几分钟，用户无法忍受。
    bool didVerify = false;
    if (req.verify) {
        ProgressUpdate("backup", 99, "verify");
        if (progress) progress(99, "verify");
        int vrc = engine.Verify(req.dest, progress);
        if (vrc != 0) {
            ReleaseOpLock();
            ProgressDone("backup", "verify-failed");
            // L1：固定格式串拼接。
            err = Tr("备份已写出但校验失败(rc=");
            err += std::to_string(vrc);
            err += "): ";
            err += W2U(WimEngine::ErrorString(vrc));
            LogError(err);
            return 5;
        }
        didVerify = true;
        LogInfo("backup verify passed");
    }
    if (!didVerify) {
        if (progress) progress(99, "probe");
        WimEngine probe;
        std::wstring why;
        if (probe.Probe(req.dest, why) != 0) {
            ReleaseOpLock();
            ProgressDone("backup", "failed");
            if (adv) *adv = ADV_INCOMPLETE;
            err = Tr("备份镜像不可用：");
            err += W2U(why);
            LogError(err);
            return 1;
        }
    }
    // ── 加密收尾（docs/30）：明文 → .zje，删明文 ──
    if (encrypt) {
        if (progress) progress(99, "encrypt");
        ProgressUpdate("backup", 99, "encrypt");
        std::vector<ZjeImage> meta;
        {
            WimEngine pe;
            std::vector<ImageDesc> imgs;
            if (pe.ListImages(req.dest, imgs) == 0) {
                for (const auto& d : imgs) {
                    ZjeImage z;
                    z.index = d.index;
                    z.name = d.name;
                    z.description = d.description;
                    z.sizeBytes = d.sizeBytes;
                    z.creationTime = d.creationTime;
                    bool has = false;
                    if (pe.ImagePathExists(
                            req.dest, d.index,
                            L"\\Windows\\system32\\winload.exe", &has) == 0)
                        z.hasWinload = has;
                    meta.push_back(std::move(z));
                }
            }
        }
        CryptProgress cp = [&](unsigned long long done,
                               unsigned long long total) -> bool {
            if (!progress)
                return false;
            const int pct = total ? (int)(done * 100 / total) : -1;
            return progress(pct, "encrypt");
        };
        std::wstring werr;
        LogInfo("encrypting backup -> " + W2U(reqIn.dest));
        const int zrc =
            ZjeEncryptFile(req.dest, reqIn.dest, reqIn.password, meta, cp, werr);
        // 明文不论成败都不留（失败时更不能留：用户以为已加密）
        DeleteFileW(req.dest.c_str());
        if (zrc != 0) {
            ReleaseOpLock();
            ProgressDone("backup", "failed");
            err = Tr("加密备份失败：") + W2U(werr);
            LogError(err);
            return 1;
        }
        // 容器可用性（密码 + 元数据 MAC）
        std::wstring neverr;
        if (ZjeReadMeta(reqIn.dest, reqIn.password, nullptr, nullptr, neverr) !=
            0) {
            DeleteFileW(reqIn.dest.c_str());
            ReleaseOpLock();
            ProgressDone("backup", "failed");
            err = Tr("加密镜像写出后不可用：") + W2U(neverr);
            LogError(err);
            return 1;
        }
        LogInfo("encrypted backup done: " + W2U(reqIn.dest));
    }
    // 源文件"异常属性"审计 + EA 修复并入：已在上面的 `ea-scan` 块完成
    // （PIT-122 起审计与 eapack/ZJEA 子镜像一体化，不再单独扫第二遍）。
    if (skipped) *skipped = skippedList;
    if (!skippedList.empty()) {
        LogWarn("backup: incomplete, skipped " +
                std::to_string(skippedList.size()) +
                " unreadable file(s); see skipped unreadable file lines");
    }
    ReleaseOpLock();
    ProgressDone("backup", "done");
    LogInfo("backup done");
    AppendHistory("backup", reqIn.dest, source, GetTickCount64() - t0,
                  skippedList.empty() ? "ok" : "ok-skipped");
    return 0;
}

}  // namespace sysrecover
