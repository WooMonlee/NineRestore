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
namespace {
}  // namespace
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
    if (!AcquireOpLock()) {
        err = Tr("已有备份/还原实例在运行");
        return 1;
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
    if (rc != 0) {
        ReleaseOpLock();
        ProgressDone("backup", "failed");
        char buf[128];
        snprintf(buf, sizeof(buf), Tr("备份失败(rc=%d): "), rc);
        err = buf;
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
                    if (earc != 0) {
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
        char buf[160];
        snprintf(buf, sizeof(buf), Tr("ESP 备份失败(rc=%d): "), erc);
        std::string e = buf;
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
            char buf[128];
            snprintf(buf, sizeof(buf), Tr("备份已写出但校验失败(rc=%d): "), vrc);
            err = buf;
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

// 还原的两条"安装"模式（共用同一套检查与契约，只有"是否立即重启"不同）：
//   menuEntry=false：StageRestore —— 判定就地/暂存，暂存则设**单次启动**（BootNext/
//                    bootsequence），重启后自动执行；
//   menuEntry=true ：StageRestoreMenu —— 只写常驻契约 + 常驻启动项，**不设单次启动**，
//                    留给用户在开机菜单里自行选择（用户 2026-09-29 规格）。
int StageRestoreImpl(const RestoreRequest& req, std::string& err,
                     bool* needReboot, ProgressFn progress, ErrAdvice* adv,
                     bool menuEntry) {
    if (adv) *adv = ADV_NONE;
    if (req.image.empty()) {
        err = Tr("缺少镜像路径");
        return 1;
    }
    // 0.05) 引擎取值与「引擎 × 备返」组合校验（docs/24 P1-1/P1-2，2026-10-11）：
    //   ① --engine 只认 swap/block/空（空=默认 swap）——防手滑值写进契约后
    //      在救援层被缺省成另一种引擎（静默换承诺）；
    //   ② 备返（keepOld，保留旧系统 ~old）由 A 引擎的改名交换实现；B 引擎
    //      （--engine block）会 mkntfs 格式化目标分区，旧系统必然消失 ——
    //      静默组合 = 用户以为有保护其实没有 → 拒绝（fail-closed）。
    //      当前前端组合不可达（GUI 恒 A、CLI 无 keepold 旗标），属防御性拦截。
    if (!req.engine.empty() && req.engine != "swap" && req.engine != "block") {
        err = Tr("--engine 仅支持 swap（默认，A 引擎）或 block（B 引擎兜底），"
                 "收到未知值已中止，未动目标分区。");
        LogError(err);
        return 2;
    }
    if (req.keepOld && req.engine != "swap") {
        err = Tr("备返（保留旧系统）仅支持默认 A 引擎：--engine block 会格式化"
                 "目标分区并丢弃旧系统。请改用默认引擎，或取消备返。");
        LogError(err);
        AppendHistory("restore-rejected", req.image, req.image, 0,
                      "keepOld requested with block engine");
        return 4;
    }
    // 0.02) 加密镜像（ZJENC1，docs/30 §9 引擎矩阵）：
    //   · A 引擎（swap，Windows 侧解压 ~new）/ PE 就地还原 → 支持；
    //   · B 引擎（block）→ **支持**：暂存阶段由 Windows 解密到同卷临时明文，
    //     契约指向它、救援层用完即删（2026-10-29 用户要求 A/B 双路）；
    //   · 常驻菜单（menuEntry）→ **拒绝**（开机阶段没人能输密码）。
    // 口令只放内存（imgsrc 的进程表），不写日志/契约。
    const bool encImage = ImageIsEncrypted(req.image);
    if (encImage) {
        if (!req.password.empty())
            SetImagePassword(req.image, req.password);
        // 菜单也支持加密镜像：B 引擎在救援层用 dm-crypt 映射（开机输口令），
        // A 引擎的 ~new 是 Windows 侧解好的，Linux 不读镜像。
        if (GetImagePassword(req.image).empty()) {
            err = Tr("这是加密镜像，需要先输入密码");
            LogError(err);
            return 4;
        }
    }
    // 0) 镜像可用性**快检**（秒级，PIT-057 + PIT-106）：只拒绝"上次没写完/
    //    不完整"的镜像（否则 Linux 侧 apply rc=84，而目标分区已被格式化 →
    //    开机黑屏）；**不做全量扫描**（大镜像十几分钟，用户无法忍受）。
    {
        std::wstring why;
        if (ImageProbe(req.image, why) != 0) {
            if (adv) *adv = ADV_INCOMPLETE;
            err = Tr("镜像不可用，请重新备份：");
            err += W2U(why);
            LogError(err);
            return 5;
        }
    }
    // 0.5) 子镜像检查（方案 C，用户 2026-09-30 规格 + PIT-122 EA 子镜像）：
    //   ① 名含 "ESP" 的子镜像 = ESP 分区备份，**不能单独当系统还原** → 拒绝
    //      （GUI 弹框 / CLI 打印，退出码 4；在一切写盘动作之前）；"ZJEA" 同理；
    //   ② 顺带从镜像**内容**发现 ESP / EA 子镜像 index（esp_index / ea_index，
    //      0=没有）写进契约，救援层 apply 完主系统后按需恢复 ESP、投放 EA 修复。
    int espIndex = 0;
    int eaIndex = 0;
    {
        std::vector<ImageDesc> imgs;
        const int eff = req.index < 1 ? 1 : req.index;
        if (ImageList(req.image, imgs) != 0) {
            // Probe 刚过 → 这里失败几乎不可能；按 0 处理（不恢复 ESP/不投 EA）
            LogWarn("ListImages failed; esp/ea restore disabled");
        } else {
            for (const auto& d : imgs) {
                if (d.name.find(L"ESP") != std::wstring::npos) {
                    if (espIndex == 0)
                        espIndex = d.index;
                    if (d.index == eff) {
                        err = Tr("子镜像「") + W2U(d.name) +
                              Tr("」是 ESP 分区备份，不能单独还原为系统。恢复系统"
                                 "时会自动把 ESP 一并恢复；请选择系统子镜像。");
                        LogError(err);
                        AppendHistory("restore-rejected", req.image, req.image, 0,
                                      "esp image selected as restore target");
                        return 4;
                    }
                }
                if (d.name.find(L"ZJEA") != std::wstring::npos) {
                    if (eaIndex == 0)
                        eaIndex = d.index;
                    if (d.index == eff) {
                        err = Tr("子镜像「") + W2U(d.name) +
                              Tr("」是扩展属性（EA）修复数据，不能单独还原为"
                                 "系统；请选择系统子镜像。");
                        LogError(err);
                        AppendHistory("restore-rejected", req.image, req.image, 0,
                                      "ea image selected as restore target");
                        return 4;
                    }
                }
            }
            if (espIndex > 0)
                LogInfo("esp subimage found in image: index=" +
                        std::to_string(espIndex));
            if (eaIndex > 0)
                LogInfo("ea subimage found in image: index=" +
                        std::to_string(eaIndex));
        }
    }
    // 0.7) 镜像内容预检（I-1，2026-10-06 实测定稿；docs/19 批次 G）：
    //   **规则：镜像里必须有 `\Windows\system32\winload.exe`（Win7+ 的加载器）**，
    //   否则在**写契约/重启之前**拒绝，杜绝"格式化完才发现起不来"。覆盖：
    //   Vista 以前（XP/2003/2000/98）系统镜像、安装源类"镜像"（实测某 XP esd
    //   连 ntoskrnl 都没有）、合成/局部镜像（演练盘素材已补 dummy winload.exe）。
    //   ⚠️ 救援层还有同款兜底（双保险；老契约/绕过此检查时仍安全）。
    {
        const int eff = req.index < 1 ? 1 : req.index;
        bool winload = false;
        if (ImageHasWinload(req.image, eff, &winload) == 0 && !winload) {
            err = Tr("该镜像不是 Windows 7 及以上的系统镜像（缺少 "
                     "\\Windows\\system32\\winload.exe；Vista 以前系统 / 安装源"
                     "/ 不完整镜像均不支持还原），已中止，未动目标分区。");
            LogError(err);
            AppendHistory("restore-rejected", req.image, req.image, 0,
                          "not a win7+ system image (no winload.exe)");
            return 4;
        }
    }
    // 0.9) 异机还原前置检查（2026-10-21，0.9）：仅 Win10/11；只允许 Windows
    //      侧路径（A 引擎 / PE 就地）；x64 镜像需 64 位环境（DISM 才能服务）。
    if (req.universal) {
        if (menuEntry) {
            err = Tr("「安装菜单」不支持异机还原（菜单项在 Linux 救援层执行，"
                     "无法注入启动驱动）。请直接执行还原并勾选异机还原。");
            LogError(err);
            return 4;
        }
        int major = 0, minor = 0;
        bool img64 = false;
        std::wstring uerr;
        const int eff2 = req.index < 1 ? 1 : req.index;
        if (ProbeImageOs(req.image, eff2, &major, &minor, &img64, uerr) != 0) {
            err = Tr("异机还原：无法读取镜像内的系统版本（") + W2U(uerr) + Tr("）");
            LogError(err);
            return 1;
        }
        if (!IsUniversalOsSupported(major, minor)) {
            err = Tr("异机还原仅支持 Windows 10/11（镜像版本 ") +
                  std::to_string(major) + "." + std::to_string(minor) +
                  Tr("）。Win7/8.1 请使用同机还原、PE 就地还原或新装系统。");
            LogError(err);
            AppendHistory("restore-rejected", req.image, req.image, 0,
                          "universal restore requires Win10/11");
            return 4;
        }
        if (img64 && !IsHost64()) {
            err = Tr("异机还原：镜像是 64 位，但当前环境不是 64 位"
                     "（DISM 无法服务 x64 镜像）。请在 64 位 PE/系统中运行。");
            LogError(err);
            return 4;
        }
        if (!req.driverDir.empty()) {
            int n = 0;
            if (!ValidateDriverFolder(req.driverDir, &n, uerr)) {
                err = Tr("异机还原：驱动文件夹无效（") + W2U(uerr) + Tr("）");
                LogError(err);
                return 4;
            }
            LogInfo("universal: driver pack inf count=" + std::to_string(n));
        }
        LogInfo("universal: image OS " + std::to_string(major) + "." +
                std::to_string(minor) +
                (img64 ? " x64" : " x86") + ", engine=" +
                (req.engine.empty() ? "block" : req.engine));
    }
    // 0.8) Linux 侧无法还原的内容预检（1019 案 F5 + W2；**仅 B 引擎/block**）：
    //   B 引擎重启进 Linux 救援层用 wimlib（NTFS-3G 块模式）apply，有两类内容
    //   还原不了，都会在 **mkntfs 之后**才暴露：
    //   ① 特定重解析点（PIT-135 + 1019 修订：非微软 tag、junction 落非目录
    //      文件等）→ apply rc=58 中止（目标半成品、系统没了）。
    //   ② EFS 加密文件（W2，2026-10-19 实测）→ apply **rc=0 但文件变 0 字节**
    //      （wimlib 只打 "Ignoring EFS-encrypted data"），静默数据丢失。
    //   只读元数据扫描（非微软/junction 零误报子集 + ENCRYPTED 属性）命中即
    //   拒绝 —— 此时还没动任何分区。A 引擎（swap）由 Windows 侧 libwim 解压
    //   （EFS/重解析点都能写回），无需检查。
    //   结果随任务写进契约（reparse_scan / efs_scan：0=干净/2=不可用），救援层
    //   据此决定是否走"契约认证"快路径（W1/W2；老契约缺键 → 救援层自行扫描）。
    int reparseScan = -1;
    int efsScan = -1;
    // 注意：engine 为空 = 文档里的默认 B 引擎（block）——不能只认字面量 "block"，
    // 否则 CLI 不传 --engine 时既跳过 EFS/重解析点预检、也跳过下面的解密。
    if (req.engine != "swap" && !encImage) {
        WimEngine eng;
        std::vector<std::wstring> bad;
        size_t encrypted = 0;
        const int eff = req.index < 1 ? 1 : req.index;
        int frc =
            eng.ScanImageForLinuxRestore(req.image, eff, bad, &encrypted);
        if (frc == 0 && !bad.empty()) {
            err = Tr("镜像里含 Linux 恢复环境无法写回的重解析点（如「") +
                  W2U(bad[0]) +
                  Tr("」）—— 用「重启后还原」会中止并留下半成品。请改选「还原后"
                     "重启」，或进 PE 模式就地还原；也可用新版软件重新备份后再试。");
            LogError("image has unsettable reparse point(s): " +
                     W2U(bad[0]) + " (" + std::to_string(bad.size()) +
                     " found; rejected before touching target)");
            AppendHistory("restore-rejected", req.image, req.image, 0,
                          "image has unsettable reparse point (Linux apply "
                          "would fail; PIT-135)");
            return 4;
        }
        if (frc == 0 && encrypted > 0) {
            err = Tr("镜像里含 EFS 加密文件（") + std::to_string(encrypted) +
                  Tr(" 个），Linux 恢复环境无法还原其内容（还原后这些文件会是"
                     "空的）。请改选「还原后重启」，或进 PE 模式就地还原。");
            LogError("image has EFS-encrypted entries: " +
                     std::to_string(encrypted) +
                     " (rejected before touching target; Linux apply would "
                     "restore them empty)");
            AppendHistory("restore-rejected", req.image, req.image, 0,
                          "image has EFS-encrypted files (Linux apply would "
                          "restore empty files; W2)");
            return 4;
        }
        if (frc != 0) {
            reparseScan = 2;  // 扫描不可用 → 救援层兜底自行判定（fail-closed）
            efsScan = 2;
            LogWarn("linux-restore precheck skipped rc=" + std::to_string(frc) +
                    " (rescue layer will verify)");
        } else {
            reparseScan = 0;  // 已扫描且干净 → 救援层走契约认证快路径
            efsScan = 0;
        }
    }
    // 1) 定位目标分区与所在磁盘
    DiskInfo tdisk;
    PartitionInfo target;
    bool found = false;
    for (const auto& d : EnumerateDisks()) {
        if ((int)d.index != req.disk)
            continue;
        for (const auto& p : d.parts) {
            if ((int)p.partNumber != req.part)
                continue;
            tdisk = d;
            target = p;
            found = true;
        }
    }
    if (!found) {
        char buf[96];
        snprintf(buf, sizeof(buf), Tr("找不到目标分区 磁盘%d 分区%d"), req.disk,
                 req.part);
        err = buf;
        return 1;
    }
    // 1b) 软件/镜像若在目标分区（会被格式化）上 → 镜像移动、软件复制到数据盘。
    //     放在 <数据盘>\SysRecover\（持久保留，不参与还原后清理），绝不丢用户镜像。
    std::wstring imagePath = req.image;
    {
        std::wstring dataDrive = FindDataDrive(target.letter.empty() ? 0 : target.letter[0]);
        wchar_t tgtLetter = target.letter.empty() ? 0 : target.letter[0];
        std::wstring keep = dataDrive.empty() ? L"" : dataDrive + L"SysRecover";
        // 8) 镜像在目标分区 → 必须移走，否则还原时被一起格式化掉
        if (tgtLetter && imagePath.size() > 1 && imagePath[1] == L':' &&
            SameDrive(imagePath[0], tgtLetter)) {
            if (keep.empty()) {
                if (adv) *adv = ADV_IMAGE_IN_TARGET;
                err = Tr("镜像在目标分区内，且找不到可存放它的数据盘；请先手动移走镜像");
                return 4;
            }
            CreateDirectoryW(keep.c_str(), nullptr);
            size_t sl = imagePath.find_last_of(L"\\/");
            std::wstring fn =
                sl == std::wstring::npos ? imagePath : imagePath.substr(sl + 1);
            std::wstring dst = keep + L"\\" + fn;
            LogInfo("image on target partition; moving to " + W2U(dst));
            if (!MoveFileExW(imagePath.c_str(), dst.c_str(),
                             MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING)) {
                err = Tr("镜像在目标分区内，搬到数据盘失败（空间不足？）；请先手动移走镜像");
                if (adv) *adv = ADV_SPACE;  // 旧关键词版命中的也是"空间不足"（保留原建议）
                return 4;
            }
            if (ImageIsEncrypted(imagePath))
                SetImagePassword(dst, GetImagePassword(imagePath));
            imagePath = dst;
        }
        // 7) 软件在目标分区 → SettleHome（Q1「先落盘后搬家」）：静默整包复制到
        //    安全分区 + 日志跟过去（不 re-exec：当前进程照旧跑完本次操作）。
        std::wstring exeDir0 = ExeDir();
        if (ShouldSettleHome(exeDir0, tgtLetter)) {
            int exeDisk = VolumeDiskIndex(exeDir0[0]);
            int tgtDisk = (int)target.diskIndex;
            SettleHome(tgtDisk, exeDisk);
        }
    }
    // 2) 安全门禁（fail-closed，§11）
    std::string reason = CheckRestoreTarget(target, imagePath, adv);
    if (!reason.empty()) {
        err = reason;
        return 4;
    }
    // 2.05) 目标盘健康（PIT-086，用户 2026-09-26）：坏盘 → 记日志（**展示**由 GUI 弹框 /
    //       CLI 打印负责；这里不阻断 —— 与 BitLocker 一致，取不到 SMART 也放行）。
    {
        std::string hw = CheckDiskHealth((int)target.diskIndex);
        if (!hw.empty())
            LogWarn("target disk health: " + hw);
    }
    // 2.1) 空间预检（P1）：镜像未压缩大小 vs 目标分区大小。
    //      必须在这里（**格式化之前**）拦下 —— 否则会出现"数据没了、系统也没装上"。
    //      就地还原与"暂存+重启"两条路都从这一点过，所以放在分支之前。
    {
        std::string spErr;
        if (CheckRestoreSpace(imagePath, req.index, target, spErr) == 1) {
            err = spErr;
            if (adv) *adv = ADV_SPACE;
            LogError("space precheck failed: " + spErr);
            return 1;
        }
    }
    // 2.2) 就地还原 vs「暂存+重启」**必须在 A 引擎解压之前判定**（2026-10-12
    //      问题 4）：旧顺序是先解压 ~new（几分钟）再判就地，而 PE / 非系统盘
    //      目标走就地 → 接着 format 把刚解压的 ~new 一起格式化掉、再整镜像
    //      apply 第二遍 —— 表现为"100% 后停 16 秒、又从 1% 跑到 100%"
    //      （同一遍镜像解了两遍）。就地时整段 swap（解压/备返/RunOnce）跳过。
    std::string inplaceWhy;
    const bool inPlace = !menuEntry && CanRestoreInPlace(target, inplaceWhy);
    LogInfo(std::string("restore path: ") +
            (inPlace ? "in-place" : (menuEntry ? "menu-entry" : "staged-reboot")) +
            (inplaceWhy.empty() ? "" : " (" + inplaceWhy + ")"));
    // 日常还原（暂存+重启）会使用独立的**临时**引导条目，不能覆盖用户显式
    // 安装的常驻菜单。先让用户清菜单，避免“还原后旧菜单失效”。
    if (!inPlace && !menuEntry && HasInstalledMenu()) {
        err = Tr("已安装「安装菜单」的常驻启动项；为避免覆盖它，日常还原已中止。"
                 "请先在主界面点「清除菜单」，再开始还原。");
        LogError(err);
        AppendHistory("restore-rejected", req.image, req.image, 0,
                      "daily restore while persistent menu installed");
        return 4;
    }
    // 异机还原只能走 Windows 侧路径：暂存的 B 引擎（Linux apply）没有 DISM。
    if (!inPlace && req.universal && req.engine != "swap") {
        err = Tr("异机还原不支持「重启后还原」（B 引擎在 Linux 里执行，无法注入"
                 "启动驱动）。请改选「还原后重启」——不需要进 PE。");
        LogError(err);
        return 4;
    }

    // 2.25) 暂存路径的会话前置（Q2，2026-10-18）：就地还原任何组合放行；
    //       「暂存+重启」/菜单项依赖重启后固件沿当前同一种方式引导，两种必坏
    //       组合提前拒绝（见 bootpath.h::CheckStagedBootPath）。
    if (!inPlace) {
        switch (CheckStagedBootPath(IsUefiFirmware(),
                                     DiskStyleOf(target.diskIndex))) {
            case StagedBootPath::BiosSessionGptTarget:
                err = Tr("当前系统以传统(CSM/Legacy)方式启动，但目标盘是 GPT——"
                         "重启后进不了恢复环境。解决：① 进 BIOS 把启动模式改成 UEFI"
                         "（关闭 CSM）后重试；或 ② 用 UEFI 模式启动的 PE/U 盘就地还原"
                         "（不用重启）。");
                LogError("staged boot path rejected: BIOS session + GPT target");
                return 4;
            case StagedBootPath::UefiSessionMbrTarget:
                err = Tr("当前系统以 UEFI 方式启动，但目标盘是 MBR——重启后进不了"
                         "恢复环境。解决：① 进 BIOS 打开 CSM/Legacy 后重试；或 "
                         "② 用 Legacy 模式启动的 PE/U 盘就地还原（不用重启）。");
                LogError("staged boot path rejected: UEFI session + MBR target");
                return 4;
            case StagedBootPath::Ok:
                break;
        }
    }

    // A 引擎残留门禁（就地与暂存共用，docs/23 §5）：目标盘还有未处理的 ~old
    // （用户选「保留不动」/清理未跑）→ 不动盘、中止；重新运行程序会再次询问。
    // 就地还原**同样要拦**：就地 = format 整个目标分区，等于不询问就删掉用户
    // 刚刚选择「保留」的旧系统。
    auto swapLeftGuard = [&]() -> bool {
        if (target.letter.empty()) {
            err = Tr("目标分区无盘符，A 引擎无法解压");
            return false;
        }
        WaitSwapCleanup(target.letter[0]);  // 等启动期后台残留清理收尾
        DWORD a = GetFileAttributesW((target.letter + L":\\~old").c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
            err = Tr("目标盘存在未处理的 ~old 残留，A 引擎还原已中止"
                     "（重新运行程序会再次询问如何处理）");
            LogError(err);
            return false;
        }
        return true;
    };

    // ── 就地还原：到此直接执行（不写契约、不部署引导、不重启）────────────
    if (inPlace) {
        if (req.engine == "swap" && !swapLeftGuard())
            return 4;
        if (!AcquireOpLock()) {
            err = Tr("已有备份/还原实例在运行");
            return 1;
        }
        ProgressUpdate("restore", 0, "start");
        LogInfo("restore stage image=" + W2U(imagePath) + " disk=" +
                std::to_string(req.disk) + " part=" + std::to_string(req.part));
        LogInfo(std::string("restore mode: in-place (") + inplaceWhy + ")");
        const ULONGLONG t1 = GetTickCount64();
        // 加密镜像：就地还原是 Windows 侧读 → 解密到同卷临时明文（用完即删）。
        // 放在"镜像搬出目标分区"之后，避免临时明文被格式化掉。
        std::wstring plainImage = imagePath;
        bool plainTemp = false;
        if (ImageIsEncrypted(imagePath)) {
            std::wstring perr;
            plainImage = ResolveImagePlain(imagePath, progress, perr, &plainTemp);
            if (plainImage.empty()) {
                err = W2U(perr);
                ReleaseOpLock();
                ProgressDone("restore", "failed");
                return 1;
            }
        }
        int rc = RunDirectRestore(req, target, plainImage, espIndex, eaIndex,
                                  err, progress);
        if (plainTemp)
            ReleaseImagePlain(imagePath);
        if (needReboot)
            *needReboot = false;
        ReleaseOpLock();
        AppendHistory("restore-inplace", req.image,
                      target.letter.empty() ? L"?" : target.letter + L":",
                      GetTickCount64() - t1, rc == 0 ? "ok" : "failed");
        return rc;
    }

    // 临时引导项的首启清理需要知道 A 引擎的 ~new 位置（B 引擎没有 ~new，
    // 目前只能记录告警；见下方 InstallFirstBootCleanup 调用）。
    std::wstring swapNewDir;
    // 加密镜像：Windows 侧暂存的 ESP/EA 子镜像相对目录（写进契约 esp_fix_dir/
    // ea_fix_dir，救援层只做拷贝）。非加密镜像恒为空。
    std::wstring espFixRel;
    std::wstring eaFixRel;
    // ── A 引擎（0.7，docs/21/22/23）：Windows 原生解压到 <目标>:\~new ──
    // 必须在写契约/部署引导之前完成：解压成功才继续。失败/中断 → 报错、不碰
    // 旧系统、清理 ~new（L1 定案：让用户换镜像重试；断电由下次启动清孤儿）。
    if (req.engine == "swap") {
        if (!swapLeftGuard())
            return 4;
        unsigned long long content = 0;
        {
            std::vector<ImageDesc> imgs;
            if (ImageList(imagePath, imgs) == 0) {
                int want = req.index < 1 ? 1 : req.index;
                for (const auto& im : imgs)
                    if (im.index == want) content = im.sizeBytes;
            }
        }
        if (progress) progress(1, "extract");
        //「刚发生的接着处理」：近期完整 ~new 且 content 与所选镜像一致 → 复用
        // （免重解压）；过期/不完整/镜像已换 → 丢弃重解（docs/23 §5 决策表）。
        bool reuse = false;
        // 加密镜像：Windows 侧暂存的 ESP/EA 子镜像相对目录（见外层的
        // espFixRel/eaFixRel 声明）。
        std::wstring newDir = target.letter + L":\\~new";
        swapNewDir = newDir;
        {
            DWORD a = GetFileAttributesW(newDir.c_str());
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
                SwapLeftInfo ni;
                if (ProbeSwapNew(target.letter[0], ni)) {
                    SwapLeftVerdict v = DecideSwapLeftover(ni);
                    if (v.action == SwapLeftAction::Reuse && content > 0 &&
                        ni.markerContent == content) {
                        reuse = true;
                        LogInfo(
                            "A-engine: reuse existing ~new (content matches "
                            "selected image)");
                    } else {
                        LogInfo("A-engine: discard leftover ~new (" + v.reason +
                                (v.action == SwapLeftAction::Reuse
                                     ? "; content mismatch, redo extract"
                                     : "") +
                                ")");
                        DeleteTreeRec(newDir);
                    }
                } else {
                    DeleteTreeRec(newDir);  // 探测竞态 → 保守删除
                }
            }
        }
        if (req.formatSwap) {
            if (reuse) LogInfo("A-engine: format requested -> ignore ~new reuse");
            reuse = false;  // 要格式化就谈不上复用旧的 ~new
        }
        if (!reuse) {
        // PIT-211：用户勾了「还原后重启：先格式化目标分区」（仅目标未被占用/PE
        // 里可选）→ 整个分区先格式化，再解压。失败就中止，绝不留半套系统。
        if (req.formatSwap) {
            std::string flog;
            LogInfo("A-engine: format target before extract");
            if (!FormatTargetVolume(target.letter[0], flog)) {
                err = Tr("A 引擎：格式化目标分区失败（已中止，未解压任何文件）：") +
                      flog;
                LogError(err);
                return 1;
            }
            LogInfo("A-engine: " + flog);
        }
        // PIT-198：旧 ~new 删不掉（被占用/权限）时**改名让位**，别让整次暂存失败。
        // 改出来的 `~new.zj-old-<时间>` 会在 swap 时随根目录一起进 ~old，
        // 用户删除旧系统时一并清掉。
        if (GetFileAttributesW(newDir.c_str()) != INVALID_FILE_ATTRIBUTES) {
            wchar_t ts[40] = {};
            SYSTEMTIME st = {};
            GetLocalTime(&st);
            swprintf(ts, 40, L"%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth,
                     st.wDay, st.wHour, st.wMinute, st.wSecond);
            const std::wstring alt = newDir + L".zj-old-" + ts;
            if (MoveFileExW(newDir.c_str(), alt.c_str(), 0))
                LogWarn("A-engine: ~new undeletable -> renamed " + W2U(alt));
            else
                LogWarn("A-engine: ~new undeletable and rename failed err=" +
                        std::to_string(GetLastError()));
        }
        SwapSpaceEstimate se;
        if (!EstimateSwapSpace(target, content, se) || !se.enough) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                     Tr("A 引擎空间不足：需要约 %.1f GB（内容+页文件+余量），"
                        "目标盘可用 %.1f GB。请清理磁盘后重试，或改用 "
                        "`--engine block`（B 引擎兜底：先格式化再写入，只需"
                        "目标总容量 ≥ 镜像体积）。"),
                     se.required / 1073741824.0, se.freeBytes / 1073741824.0);
            err = buf;
            if (adv) *adv = ADV_SPACE;
            LogError(err);
            return 1;
        }
        LogInfo("A-engine: extract to " + W2U(newDir) + " (content " +
                std::to_string(content) + " bytes)");
        WimEngine we;
        if (!we.ok()) {
            err = Tr("wimlib 初始化失败");
            return 1;
        }
        // 加密镜像：Windows 侧解压前解密到同卷临时明文（A 引擎不用 Linux 读镜像）。
        std::wstring plainImage = imagePath;
        bool plainTemp = false;
        if (ImageIsEncrypted(imagePath)) {
            std::wstring perr;
            plainImage = ResolveImagePlain(imagePath, progress, perr, &plainTemp);
            if (plainImage.empty()) {
                err = W2U(perr);
                DeleteTreeRec(newDir);
                return 1;
            }
        }
        int erc = we.Apply(plainImage, req.index < 1 ? 1 : req.index, newDir,
                           progress);
        if (erc != 0) {
            DeleteTreeRec(newDir);
            if (plainTemp)
                ReleaseImagePlain(imagePath);
            char b2[160];
            snprintf(b2, sizeof(b2), Tr("A 引擎解压失败(rc=%d): "), erc);
            err = b2;
            err += W2U(WimEngine::ErrorString(erc));
            LogError(err);
            return 1;
        }
        // ── 加密镜像的 ESP / EA 子镜像：Windows 侧暂存（docs/30 §9）──
        // 救援层读不了加密容器 → 在这里把子镜像解到 ~new\ZJRESTORE\{espfix,eafix}，
        // 契约改用 esp_fix_dir/ea_fix_dir（esp_index/ea_index 置 0），Linux 只做拷贝。
        // 非加密镜像保持原行为（Linux 用 wimlib apply esp_index/ea_index）。
        if (ImageIsEncrypted(imagePath) && (espIndex > 0 || eaIndex > 0)) {
            const std::wstring zjRoot = newDir + L"\\ZJRESTORE";
            MakeDirTree(zjRoot);
            WimEngine xe;
            if (espIndex > 0) {
                const std::wstring dst = zjRoot + L"\\espfix";
                DeleteTreeRec(dst);
                MakeDirTree(dst);
                int xrc = xe.Apply(plainImage, espIndex, dst, ProgressFn{});
                if (xrc == 0) {
                    espFixRel = L"ZJRESTORE/espfix";
                    LogInfo("encrypted: ESP subimage staged to ~new\\ZJRESTORE\\espfix");
                } else {
                    LogWarn("encrypted: ESP subimage stage failed rc=" +
                            std::to_string(xrc) +
                            " (main system continues without ESP restore)");
                    DeleteTreeRec(dst);
                }
            }
            if (eaIndex > 0) {
                const std::wstring dst = zjRoot + L"\\eafix";
                DeleteTreeRec(dst);
                MakeDirTree(dst);
                int xrc = xe.Apply(plainImage, eaIndex, dst, ProgressFn{});
                if (xrc == 0) {
                    eaFixRel = L"ZJRESTORE/eafix";
                    LogInfo("encrypted: EA subimage staged to ~new\\ZJRESTORE\\eafix");
                } else {
                    LogWarn("encrypted: EA subimage stage failed rc=" +
                            std::to_string(xrc) +
                            " (main system continues without EA fix)");
                    DeleteTreeRec(dst);
                }
            }
            // 暂存成功后才把契约里的 esp_index/ea_index 清掉（避免 Linux 去读密文）
            if (!espFixRel.empty())
                espIndex = 0;
            if (!eaFixRel.empty())
                eaIndex = 0;
        }
        if (plainTemp)
            ReleaseImagePlain(imagePath);
        LogInfo("A-engine: extract/staging done");
        }  // if (!reuse)
        // ── 完成标记（复用/新解压都写）──────────────────────────────
        // 提前写：~new 本身是完整的解压结果，**即使后面异机注入失败也要保留**——
        // 30GB 重新解压约 8 分钟，而重试只需要重新注入驱动（几十秒）。
        // zjswap.sh 仍以 .zj-done 为"可交换"前提；异机失败另写
        // .zj-universal-failed，由脚本拒绝交换（双保险）。
        {
            SYSTEMTIME stw = {};
            GetLocalTime(&stw);
            char line[160];
            const int len = snprintf(
                line, sizeof(line),
                "content=%llu time=%04d-%02d-%02d %02d:%02d:%02d\n", content,
                stw.wYear, stw.wMonth, stw.wDay, stw.wHour, stw.wMinute,
                stw.wSecond);
            HANDLE h = CreateFileW((newDir + L"\\.zj-done").c_str(),
                                   GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD wr = 0;
                WriteFile(h, line, (DWORD)len, &wr, nullptr);
                CloseHandle(h);
            }
        }
        // ── 异机还原（2026-10-21；2026-10-30 改为"失败保留 ~new"）──
        // 用户案例：103/105 个驱动成功、2 个非存储驱动被 DISM 拒绝 → 整单失败，
        // 而且失败后先删 30GB ~new 才报错（8 分半无响应）。现在：
        //   · 失败只写 .zj-universal-failed，**保留 ~new**（重试不用重新解压）；
        //   · 成功则清掉失败标记。
        if (req.universal) {
            std::string ulog;
            std::wstring uerr;
            auto uProgress = [&](int pct, const std::string& st) {
                ProgressUpdate("restore", pct, st);
                return progress ? progress(pct, st) : false;
            };
            if (!PrepareUniversal(newDir, req.driverDir, ulog, uerr,
                                  uProgress)) {
                const std::wstring fm = newDir + L"\\.zj-universal-failed";
                HANDLE fh = CreateFileW(fm.c_str(), GENERIC_WRITE, 0, nullptr,
                                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                        nullptr);
                if (fh != INVALID_HANDLE_VALUE) {
                    const std::string why = W2U(uerr);
                    DWORD wr = 0;
                    WriteFile(fh, why.data(), (DWORD)why.size(), &wr, nullptr);
                    CloseHandle(fh);
                }
                err = Tr("异机还原准备失败：") + W2U(uerr) +
                      Tr("（已保留 C:\\~new，修好驱动后重试无需重新解压）");
                LogError(err);
                LogError("universal log:\n" + ulog);
                return 1;
            }
            DeleteFileW((newDir + L"\\.zj-universal-failed").c_str());
            LogInfo("universal: A-engine prepared\n" + ulog);
        } else {
            // 非异机路径：清掉上一轮遗留的失败标记，否则会让 zjswap 拒绝交换
            DeleteFileW((newDir + L"\\.zj-universal-failed").c_str());
        }
        // 复用旧 ~new 时（reuse=true）不重解子镜像：若上一轮已暂存过，契约照样要写
        // esp_fix_dir/ea_fix_dir，否则救援层会以为没有 ESP/EA。
        if (ImageIsEncrypted(imagePath)) {
            if (espFixRel.empty() &&
                GetFileAttributesW((newDir + L"\\ZJRESTORE\\espfix").c_str()) !=
                    INVALID_FILE_ATTRIBUTES)
                espFixRel = L"ZJRESTORE/espfix";
            if (eaFixRel.empty() &&
                GetFileAttributesW((newDir + L"\\ZJRESTORE\\eafix").c_str()) !=
                    INVALID_FILE_ATTRIBUTES)
                eaFixRel = L"ZJRESTORE/eafix";
            if (!espFixRel.empty())
                espIndex = 0;
            if (!eaFixRel.empty())
                eaIndex = 0;
        }
        // ── keepOld 处理 ─────────────────────────────────────────────
        // keepOld=true  → 写 .zj-keepold 标记 + 部署程序到 ~new\Program Files\SysRecover
        //                 + 建桌面快捷方式（2026-10-15 恢复 e5fb0a7 部署代码；
        //                 用户可直接点快捷方式处理 ~old）。
        // keepOld=false → RunOnce 静默 .cmd 删 ~old（开机自动清，无人值守默认）。
        // 失败仅 WARN 不阻断（用户可手动启动程序处理）。
        if (req.keepOld) {
            // 1. 写备返标记到目标根（zjswap.sh 会把它移到 ~old/.zj-keepold）
            {
                const std::wstring marker = target.letter + L":\\.zj-keepold";
                HANDLE hm = CreateFileW(marker.c_str(), GENERIC_WRITE, 0,
                                        nullptr, CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
                if (hm != INVALID_HANDLE_VALUE) {
                    const char txt[] = "keepold=1";
                    DWORD wr = 0;
                    WriteFile(hm, txt, sizeof(txt) - 1, &wr, nullptr);
                    CloseHandle(hm);
                    LogInfo("A-engine: keepold marker written to target root");
                } else {
                    LogWarn("A-engine: keepold marker write failed err=" +
                            std::to_string(GetLastError()));
                }
            }
            // 2. 部署完整程序到 ~new 下的 Program Files\SysRecover（行末不能以
            //    \ 结尾 —— 反斜杠会把下一行吃进注释，导致 progDst "未声明"）
            const std::wstring progDst =
                newDir + L"\\Program Files\\SysRecover";
            const bool copied = CopyDir(AppDir(), progDst);
            if (copied) {
                LogInfo("A-engine: program deployed to " + W2U(progDst));
                // 3. 在新系统**公用桌面**创建快捷方式（换位后生效）。
                //    用户 2026-10-15 规格：只放公用桌面一份 —— 个人桌面 + 公用
                //    桌面会出现两个图标（精简镜像的 Public\Desktop 对所有用户
                //    生效，再往个人桌面放就是重复）。
                //    目标必须写换位后的最终路径：~new 换位后变成 <盘>:\Program Files\...
                const std::wstring pubDesk =
                    newDir + L"\\Users\\Public\\Desktop";
                CreateDirectoryW(pubDesk.c_str(), nullptr);
                const std::wstring lnkTarget =
                    target.letter +
                    L":\\Program Files\\SysRecover\\SysRecoverUI.exe";
                std::wstring lnkPath;
                if (CreateShortcut(pubDesk, L"SysRecover", lnkTarget, L"",
                                   lnkPath)) {
                    LogInfo("A-engine: shortcut created: " + W2U(lnkPath));
                } else {
                    LogWarn("A-engine: shortcut create failed: " +
                            W2U(pubDesk));
                }
                // 4. RunOnce：新系统**首启自动运行本程序**（用户 2026-10-17 规格）。
                //    首启流程 = 自动启动 → 把救援层/旧版留在数据盘的日志搬回程序
                //    目录（MigrateLogsToLogHome）→ 才弹「是否删除旧系统」三选框。
                //    路径用 %SystemDrive% 让**新系统**自己展开（还原到别的盘也正确；
                //    含空格的 "Program Files" 必须带引号），REG_EXPAND_SZ 才支持展开。
                InjectOfflineRunOnce(
                    newDir, L"ZJFIRSTBOOT", L"ZJSysRecoverFirstBoot",
                    L"\"%SystemDrive%\\Program Files\\SysRecover\\SysRecoverUI.exe\"",
                    REG_EXPAND_SZ, "firstboot");
            } else {
                LogWarn("A-engine: program deploy failed, skip shortcut");
            }
        } else {
            // keepOld=false → 原方案：RunOnce 静默 .cmd 删 ~old
            // 先清掉上一次「备返」可能留在目标根的标记：否则它会被 zjswap 一起
            // 移进 ~old，让本应自动删除的 ~old 变成 AskUndo（用户明明没勾备返）。
            {
                const std::wstring marker = target.letter + L":\\.zj-keepold";
                if (GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    if (DeleteFileW(marker.c_str()))
                        LogInfo("A-engine: stale keepold marker removed");
                    else
                        LogWarn("A-engine: stale keepold marker remove failed err=" +
                                std::to_string(GetLastError()));
                }
            }
            const std::wstring cmdPath =
                newDir + L"\\Windows\\System32\\zj-swap-cleanup.cmd";
            static const wchar_t kCmd[] =
                L"@echo off\r\n"
                L"if exist \"%SystemRoot%\\System32\" "
                L"if not exist \"%SystemDrive%\\~new\" (\r\n"
                L"  rd /s /q \"%SystemDrive%\\~old\" >nul 2>&1\r\n"
                L")\r\n"
                L"del /f /q \"%~f0\" >nul 2>&1\r\n";
            HANDLE hc = CreateFileW(cmdPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
            if (hc != INVALID_HANDLE_VALUE) {
                DWORD wr = 0;
                WriteFile(hc, kCmd, (DWORD)(sizeof(kCmd) - 2), &wr, nullptr);
                CloseHandle(hc);
                InjectOfflineRunOnce(newDir, L"ZJSWAPCLEANUP",
                                     L"ZJSwapCleanup",
                                     L"%SystemRoot%\\System32\\zj-swap-cleanup.cmd",
                                     REG_SZ, "cleanup");
            } else {
                LogWarn("A-engine: cleanup cmd write failed err=" +
                        std::to_string(GetLastError()));
            }
        }
    }
    if (!AcquireOpLock()) {
        err = Tr("已有备份/还原实例在运行");
        return 1;
    }
    ProgressUpdate("restore", 0, "start");
    LogInfo("restore stage image=" + W2U(imagePath) + " disk=" +
            std::to_string(req.disk) + " part=" + std::to_string(req.part));

    // 2.5) 就地还原分支**已在 2.2 提前返回**（问题 4：必须赶在 A 引擎解压之前，
    //      否则 ~new 白解压一遍还被自己的 format 掉）—— 这里只会走到两种模式：
    //      暂存+重启（PIT-064）或常驻菜单项。判定结果复用 2.2 的 inPlace/inplaceWhy，
    //      不再二次调用 CanRestoreInPlace（会再次开/锁目标卷）。
    {
        if (menuEntry) {
            LogInfo("restore mode: menu entry (contract + persistent boot entry; "
                    "no reboot)");
        } else {
            LogInfo(std::string("restore mode: staged reboot (") + inplaceWhy + ")");
        }
        // 网络镜像防呆（问题清单 D1/H1）：暂存+重启后由 **Linux 救援层**读镜像，而救援层
        // 访问不到网络（UNC / 映射网络驱动器）→ 必然 image not found。这里**提前拒绝**
        //（就地还原不在此列 —— 那是 Windows 自己读，网络没问题）。
        if (IsNetworkPath(imagePath)) {
            err =
                Tr("镜像在网络路径上（UNC 或映射网络驱动器）。还原系统盘需要重启进救援层，" "而救援层无法访问网络 —— 请先把镜像复制到**本地分区**（如 D:\\）再还原。");
            LogError("staged restore rejected: image on network path");
            AppendHistory("restore-rejected", req.image, imagePath, 0,
                          "image on network path");
            ReleaseOpLock();
            ProgressDone("restore", "failed");
            return 2;
        }
        if (menuEntry)
            AppendHistory("restore-menu", req.image, imagePath, 0,
                          "menu entry installed (no reboot)");
        else
            AppendHistory("restore-staged", req.image, imagePath, 0,
                          "staged; 实际结果见救援层日志");
    }

    std::wstring exeDir = ContractDir();  // Q1 settle 后返回新家；否则 = ExeDir()
    // 日志根同 LogBaseDir（PIT-105）：软件在系统盘时 BCD 备份也放数据盘，
    // 免得随系统盘一起被格式化。
    std::wstring logsDir = LogBaseDir() + L"\\logs";
    CreateDirectoryW(logsDir.c_str(), nullptr);
    if (req.repairBoot)
        BcdExport(logsDir + L"\\bcd-backup");  // 改 BCD 前先备份（§11）

    // PIT-211：A 引擎改计算机名 —— 此刻 ~new 里已经是新系统，直接改离线库，
    // 换位后第一次开机就是新名字（不用等首次登录）。
    if (!swapNewDir.empty() && !req.computerName.empty()) {
        std::string rlog;
        if (SetComputerNameOffline(swapNewDir + L"\\Windows", req.computerName,
                                   rlog))
            LogInfo("A-engine rename: " + rlog);
        else
            LogWarn("A-engine rename failed (non-fatal): " + rlog);
    }
    // 3) 组装任务（字段对齐旧 C# StageRestoreAsync）
    RestoreTask t;
    t.imagePath = imagePath;
    t.imageIndex = req.index < 1 ? 1 : req.index;
    t.espIndex = espIndex;  // 方案 C：ESP 子镜像 index（0=镜像里没有）
    t.eaIndex = eaIndex;    // PIT-122：EA 修复子镜像 index（0=镜像里没有）
    // 加密镜像：子镜像已由 Windows 解到目标分区，救援层只拷贝（不读密文镜像）。
    t.espFixDir = espFixRel;
    t.eaFixDir = eaFixRel;
    t.reparseScan = reparseScan;  // 1019 案：0=干净 / 2=扫描不可用 / -1=未扫描
    t.efsScan = efsScan;  // 1019 案 W2：同义（EFS 加密内容预检结果）
    {   // 镜像字节数（1019 案）：救援层"契约认证"精确比对用（SIZE 判"镜像未变"）
        WIN32_FILE_ATTRIBUTE_DATA fad = {};
        if (GetFileAttributesExW(imagePath.c_str(), GetFileExInfoStandard, &fad))
            t.imageSize = ((uint64_t)fad.nFileSizeHigh << 32) |
                          (uint64_t)fad.nFileSizeLow;
    }
    t.targetGuid = target.guid;
    t.targetOffset = target.offsetBytes;
    t.targetSize = target.sizeBytes;
    t.targetDiskSerial = tdisk.serial;
    t.targetDisk = req.disk;
    t.targetPart = req.part;
    t.ptType = tdisk.style == PartitionStyle::GPT ? "gpt" : "mbr";
    t.repairBoot = req.repairBoot;
    t.targetDiskName = tdisk.model;
    t.targetDiskSize = tdisk.sizeBytes;
    t.targetFs = target.fs;
    t.targetVolLabel = target.label;
    t.softwarePath = ExePath();
    t.engine = req.engine;  // 0.7：""/block=B 引擎；swap=A 引擎（docs/23）
    t.keepOld = req.keepOld;
    t.targetMode = req.targetMode.empty() ? "format" : req.targetMode;
    t.computerName = req.computerName;  // PIT-148：写进 conf，供「一键重试」原样复刻任务
    // 诊断日志目录（PIT-059）：优先软件目录本身；软件在目标盘（会被格式化）
    // 或只读介质上时，回退到数据盘 <数据盘>\ZJRESTORE（建好，保留给排错用）。
    {
        wchar_t exLetter = exeDir.size() > 1 ? exeDir[0] : 0;
        wchar_t tgtLetter = target.letter.empty() ? 0 : target.letter[0];
        wchar_t exRoot[4] = {exLetter, L':', L'\\', 0};
        bool onTarget =
            exLetter && tgtLetter && SameDrive(exLetter, tgtLetter);
        bool fixedDisk = exLetter && GetDriveTypeW(exRoot) == DRIVE_FIXED;
        if (!onTarget && fixedDisk) {
            t.softwareDir = exeDir;
        } else {
            std::wstring dataDrive = FindDataDrive(target.letter.empty() ? 0 : target.letter[0]);
            if (dataDrive.empty())
                t.softwareDir = exeDir;
            else
                t.softwareDir = dataDrive + kRecoveryDir;  // D:\ZJRESTORE
            CreateDirectoryW(t.softwareDir.c_str(), nullptr);
            LogInfo("software dir unusable for logs; log dir=" +
                    W2U(t.softwareDir));
        }
    }
    // 镜像位置：盘符 → 所在分区 GUID + 相对路径（正斜杠）
    if (imagePath.size() > 3 && imagePath[1] == L':') {
        wchar_t letter = towupper(imagePath[0]);
        for (const auto& d : EnumerateDisks())
            for (const auto& p : d.parts)
                if (!p.letter.empty() && towupper(p.letter[0]) == letter)
                    t.imagePartGuid = p.guid;
        std::wstring rel = imagePath.substr(3);
        for (auto& c : rel)
            if (c == L'\\')
                c = L'/';
        t.imageRelPath = rel;
    }

    // 4) _zjresy 日志写目标分区根（restore.sh 主契约；缺日志 Linux 侧找不到
    //    目标，视为致命——较旧 C# 的 warn 更严格，fail-closed）
    if (target.letter.empty()) {
        ReleaseOpLock();
        ProgressDone("restore", "failed");
        err = Tr("目标分区无盘符，无法写恢复日志（Linux 侧将无法定位目标）");
        LogError(err);
        return 1;
    }
    std::string tlog;
    // A2（用户 2026-10-04 规格）：写新契约前，把**所有盘根**上的旧契约清掉，
    // 全系统只留即将写入的这一份 —— 救援层就不会挑到别处残留的 _zjresy 把目标
    // 指错（offset 校验仍保留作第二层防护）。
    {
        std::string clog;
        int cleaned = CleanupStrayContracts(clog);
        LogInfo("stray contracts cleanup: " + std::to_string(cleaned) +
                " removed (" + clog.substr(0, clog.size() - 1) + ")");
    }
    if (!WriteRestoreLog(target.letter[0], t, tlog)) {
        ReleaseOpLock();
        ProgressDone("restore", "failed");
        if (adv) *adv = ADV_ADMIN;
        err = Tr("写恢复日志失败（需管理员权限写 ") + W2U(target.letter) + Tr(":\\）");
        LogError(err + " / " + tlog);
        return 1;
    }
    // 5) 任务文件（**副契约**）：优先写 exe 目录；只读介质（光盘）或写不了时回退到
    //    <数据盘>\ZJRESTORE\（救援层会扫描各分区找它，PIT-035）。
    //    ⚠️ **写不了也不致命**：主契约是**目标分区根**上的 _zjresy 日志，救援层能从
    //    日志字段读出全部参数（PIT-035 的回退）。2026-09-23 实测：用户从**光盘**运行，
    //    这一步失败 + 被当成致命错误 → 还原根本没执行 ✗（而日志其实已经写好了 ✓）。
    {
        std::string wtlog;
        bool okTask = WriteRestoreTask(exeDir, t, wtlog);
        std::wstring used = exeDir;
        if (!okTask) {
            std::wstring dataDrive = FindDataDrive(target.letter.empty() ? 0 : target.letter[0]);
            std::wstring alt =
                dataDrive.empty() ? std::wstring() : dataDrive + L"ZJRESTORE";
            if (!alt.empty()) {
                CreateDirectoryW(alt.c_str(), nullptr);
                wtlog.clear();
                if (WriteRestoreTask(alt, t, wtlog)) {
                    okTask = true;
                    used = alt;
                }
            }
        }
        if (!okTask)
            LogError("restore task write failed (non-fatal; the target-root "
                     "log is the primary contract): " + wtlog);
        else
            LogInfo("restore task written to " + W2U(used));
    }
    // 引导层失败 → **契约回滚**（PIT-126，用户 2026-10-06 论坛反馈）：
    // 契约（目标根 _zjresy + exeDir restore-task.conf）在引导层**之前**写 ——
    // 菜单绑定/暂存任务都需要它；但引导层失败 = 任务永远不会执行 → 契约必须
    // 撤销，否则：① GUI 误判"已安装"（按钮变「删除菜单」，实为只写了两份契约）；
    // ② 目标根留一份 action=restore 的陈旧日志，将来进救援层可能被误执行。
    // RAII 守卫：repairBoot=true 时武装，之后**任何失败 return 都自动回滚**；
    // 成功路径在 return 0 前 disarm。
    bool rollbackArmed = req.repairBoot;
    struct RollbackGuard {
        bool* armed;
        ~RollbackGuard() {
            if (!*armed) return;
            std::string rlog;
            int n = CleanupStrayContracts(rlog);
            DeleteMenuBinding();
            std::wstring dd = FindDataDrive();
            if (!dd.empty()) {
                DeleteFileW((dd + L"ZJRESTORE\\restore-task.conf").c_str());
                DeleteFileW((dd + L"ZJRESTORE\\restore-task.json").c_str());
            }
            LogWarn("boot layer failed -> staged contracts rolled back (" +
                    std::to_string(n) + " stray removed)");
        }
    } rollbackGuard{&rollbackArmed};

    // 6) 引导层 + 单次启动（仅正常 Windows，§2 禁令3）
    // 走哪条链**看目标盘的分区风格**（MBR→BIOS/GRUB4DOS；GPT→UEFI/ESP），不看固件类型：
    // UEFI 固件 + MBR 盘的机器（CSM/Legacy 装的 Windows）上没有 ESP，必须走 BIOS 链。
    // PIT-195：`repairBoot=false` 只允许"就地还原"（写盘后立刻修引导）。
    // 暂存路径下引导层+单次启动全在 `if (req.repairBoot)` 里 —— 不装任何引导
    // 却照样写契约、返回成功、提示"重启后执行"，重启必然回到旧系统，
    // 且连回滚都不武装（rollbackArmed=req.repairBoot）。必须前置拒绝。
    if (!req.repairBoot) {
        ReleaseOpLock();
        ProgressDone("restore", "failed");
        err = Tr("「不修复引导」只能用于就地还原（PE/非系统盘）。"
                 "还原正在运行的系统盘必须先装引导交接，请取消该选项后重试。");
        LogError(err);
        return 2;
    }
    const bool useUefi = UseUefiBootFor(target);
    LogInfo(std::string("boot path: ") + (useUefi ? "UEFI/ESP" : "BIOS/GRUB4DOS") +
            " (firmware=" + (IsUefiFirmware() ? "UEFI" : "BIOS") +
            ", target disk style=" + StyleName(DiskStyleOf(target.diskIndex)) + ")");
    std::wstring espRoot;  // UEFI 分支用；单次启动复核也要用（PIT-195）
    if (req.repairBoot) {
        std::string blog;
        // 进度收官标记（2026-10-18）：apply 之后引导层还要写 ~28MB 救援载荷 +
        // 逐文件 CRC 校验（暂存与菜单安装都走这里），期间没有字节级进度 →
        // 切一个 "boot" 阶段名，让界面显示「写入引导文件」而不是停在「写入 100%」。
        if (progress) progress(99, "boot");
        if (useUefi) {
            // UEFI/GPT：写**固件启动项**（Boot#### + BootOrder），由固件直接加载
            // 内核（EFI stub）并把 OptionalData 当内核命令行 —— 全程不经 bootmgr，
            // 因此没有 bootapp 的 0xc000007b（PIT-060）。ESP 不受还原影响。
            SbMode sbm = CurrentSbMode();
            if (sbm == SbMode::Shim && !IsMokEnrolled(exeDir)) {
                // Secure Boot 开着且走 shim 链，但用户还没做过那次一次性密钥注册：
                // 此时重启会被 MokManager 拦住，还原根本不会跑（看起来"什么都没
                // 发生"）。必须在**动目标分区之前**先拦住并说清怎么注册。
                // （走 bootapp 链不需要注册，见 PIT-063。）
                ReleaseOpLock();
                ProgressDone("restore", "failed");
                err = Tr("本机开启了 Secure Boot，但还没注册启动密钥：请先点" "「安装菜单」，重启一次，在蓝底界面选 " "Enroll key -> 选 zj-mok.cer -> 注册，之后回来再还原");
                LogError(err);
                return 1;
            }
            bool guidMatched = false;
            std::string detail;
            if (!AcquireEspRoot(espRoot, guidMatched, blog,
                                 (int)target.diskIndex)) {
                LogError("ESP acquire log (staged):\n" + blog);
                ReleaseOpLock();
                ProgressDone("restore", "failed");
                err = Tr("UEFI 机器未找到可用的 ESP 引导分区，无法部署引导层。\n"
                         "常见原因：DiskGenius 重建分区时漏建 ESP，或把 ESP 标成了"
                         " Basic Data（应改为 EFI System 类型）。\n当前分区表：\n") +
                      W2U(DescribePartitions());
                LogError(err);
                return 1;
            }
            if (!guidMatched)
                LogWarn("ESP partition type is not EFI System; used fallback "
                        "(mountvol/\\EFI files)");
                // PIT-216：先清掉以前失败/取消留下的**临时启动项**。它们指向的
                // 内核可能已经删了，却还顶在 BootOrder[0] —— 固件每次先启它，
                // 就永远进不了 Windows（这台客户机就是这么卡的）。
                {
                    std::string clog;
                    CleanupStaleUefiEntries(clog);
                    if (!clog.empty())
                        LogInfo("stale uefi entry cleanup: " + clog);
                }
            // ① 目标系统盘得有 bcdboot 要的模板（docs/15 · P2c）；② ESP 还得
            //    放得下 bcdboot 要写的 ~9MB（docs/15 · P2a）。
            //    ⚠️ 这两条**只对 bcdboot 这条路成立**：不满足就改走随包资产自足
            //    （`EnsureUefiBootFiles`，不依赖 bcdboot/模板）。以前这里直接判死、
            //    整单回滚 —— 2026-10-31 用户本机实测：32 位进程被 WOW64 重定向到
            //    `SysWOW64\Config\`，明明有模板也报缺（PIT-193）。
            bool tplOk = false, spaceOk = false;
            {
                std::string td, sd;
                tplOk = TargetBcdTemplateOk(target.letter[0], td);
                spaceOk = EspHasRoomForBcdboot(espRoot, sd);
                if (!tplOk)
                    LogWarn("BCD-Template unavailable (" + td +
                            ") -> skip bcdboot, use self-contained ESP boot");
                if (!spaceOk)
                    LogWarn("ESP space not enough for bcdboot (" + sd +
                            ") -> skip bcdboot");
            }
            // ③ **先跑 bcdboot**（此刻 ESP 空间最富裕），再放我们 33MB 的救援载荷
            //    —— 顺序反了就会被自己的载荷挤掉空间（docs/15 · P2b）。
            //    0.6.35（用户 2026-10-05）：**BCD 已经有效就跳过 bcdboot** —— 它
            //    会重建 BCD（只剩 Windows 条目），第三方启动项（易数等）被抹掉。
            bool ok = true;
            {
                // 用**目标分区**的 Windows 目录（暂存时它还是旧系统，但引导文件一样）
                std::wstring winDir =
                    std::wstring(1, target.letter[0]) + L":\\Windows";
                std::string out;
                std::string vd;
                const UefiBcdPlan plan =
                    PlanUefiBcd(VerifyEspBcd(espRoot, winDir, vd), tplOk,
                                ToolAvailable(L"bcdboot.exe"), spaceOk);
                if (plan == UefiBcdPlan::KeepExistingBcd) {
                    LogInfo("ESP BCD already valid -> skip bcdboot (preserve "
                            "third-party boot entries): " + vd);
                } else {
                    LogInfo(std::string("ESP BCD invalid -> rebuild: ") + vd +
                            " / plan=" + UefiBcdPlanName(plan));
                    // 2026-10-27（用户反馈 2）：bcdboot 缺失/失败 → 随包资产自足
                    // （极简 PE 里没有 bcdboot；以前这里直接失败、只能拿 fixboot.exe 修）。
                    bool rebuilt = false;
                    if (plan == UefiBcdPlan::RunBcdboot) {
                        std::wstring args = winDir + L" /s " +
                                            std::wstring(1, espRoot[0]) + L": /f UEFI";
                        int brc = RunProcess(SysToolPath(L"bcdboot.exe"), args, out);
                        LogInfo("bcdboot (UEFI) rc=" + std::to_string(brc) + " / " +
                                out);
                        rebuilt = brc == 0;
                    } else {
                        LogInfo("bcdboot skipped (template/tool/space) -> "
                                "self-contained ESP boot");
                    }
                    if (!rebuilt) {
                        std::string slog, serr;
                        const bool sok = EnsureUefiBootFiles(
                            espRoot, std::wstring(1, target.letter[0]) + L":\\",
                            winDir, exeDir, /*overwriteBcd=*/true, slog, serr);
                        LogInfo(std::string("ESP self-contained boot: ") +
                                (sok ? "ok" : "FAILED") + "\n" + slog);
                        std::string wlog;
                        if (EnsureWindowsBootEntry(espRoot, wlog))
                            LogInfo("firmware Windows boot entry: " + wlog);
                        else
                            LogWarn("firmware Windows boot entry (non-fatal): " +
                                    wlog);
                        if (!sok) {
                            UnmountEsp(espRoot, blog);
                            ReleaseOpLock();
                            ProgressDone("restore", "failed");
                            err = Tr("写入 ESP 引导失败（无法生成引导配置）：") + serr;
                            LogError(err);
                            return 1;
                        }
                    }
                    // ④ 产物断言：displayorder 非空 + 有 winload 条目 + 关键文件齐全
                    //    + 该条目**指向本次目标分区**（PIT-208）
                    if (!VerifyEspBcd(espRoot, winDir, detail)) {
                        UnmountEsp(espRoot, blog);
                        ReleaseOpLock();
                        ProgressDone("restore", "failed");
                        err = Tr("ESP 引导校验未通过（未真正修好，已中止，未动目标分区）：") +
                              detail;
                        LogError(err);
                        return 1;
                    }
                    // PIT-209：清"幽灵启动项"；**没装常驻菜单**时再把单系统
                    // 菜单收敛掉（装了菜单要保留 timeout>=1，用户才选得到）。
                    {
                        const std::wstring store =
                            espRoot + L"EFI\\Microsoft\\Boot\\BCD";
                        std::string plog, qlog;
                        int osLeft = -1;
                        const int pruned =
                            BcdPruneDeadEntries(store, winDir, &osLeft, plog);
                        LogInfo("bcd prune (uefi staged): removed=" +
                                std::to_string(pruned) + " reachable_os=" +
                                std::to_string(osLeft) + "\n" + plog);
                        if (!menuEntry) {
                            BcdQuietSingleOsMenu(store, qlog);
                            LogInfo("bcd menu policy (uefi staged): " + qlog);
                        }
                    }
                    // PIT-208：固件里的 "Windows Boot Manager" 必须在 BootOrder
                    // 第一位 —— 否则重启/还原后固件可能先启第三方恢复工具或
                    // 已删内核的陈旧条目（2026-11-02 客户机"反复重启"）。
                    {
                        std::string wlog;
                        if (EnsureWindowsBootEntry(espRoot, wlog, /*front=*/true))
                            LogInfo("firmware Windows boot entry: " + wlog);
                        else
                            LogWarn("firmware Windows boot entry (non-fatal): " +
                                    wlog);
                    }
                }
            }
            // ⑤ 救援载荷（内核 + initramfs ≈ 33MB；它自带空间检查）
            ok = InstallUefiBootEntry(espRoot, exeDir, blog, !menuEntry);
            UnmountEsp(espRoot, blog);
            LogInfo(std::string("uefi boot entry: ") + (ok ? "ok" : "FAIL") +
                    " / " + blog);
            if (!ok) {
                ReleaseOpLock();
                ProgressDone("restore", "failed");
                err = Tr("安装 UEFI 引导层失败: ") + blog;
                LogError(err);
                return 1;
            }
        } else {
            // BIOS/MBR：部署到**目标分区**（通常 C:）；救援文件会被本次还原格式化掉，
            // 所以不必去动数据盘（D:/E:）的根目录。
            std::wstring deployDrive = std::wstring(1, target.letter[0]) + L":\\";
            // 每次都重装（幂等）：确保 menu.lst/bootfiles/initramfs 随本版本刷新。
            if (!InstallBootLayer(deployDrive, exeDir, blog, !menuEntry)) {
                ReleaseOpLock();
                ProgressDone("restore", "failed");
                err = Tr("安装引导层失败: ") + blog;
                LogError(err);
                return 1;
            }
            // 引导补全：生成 <目标>\ZJRESTORE\bootfix\（Linux 侧在 mkntfs 前拷进内存）
            {
                std::wstring rec = deployDrive + kRecoveryDir;
                std::string flog;
                bool bok = PrepareBootFixFiles(rec, flog);
                LogInfo(std::string("bootfix: ") + (bok ? "ok" : "FAILED") +
                        " / " + flog);
                if (!bok)
                    LogError("prepare bootfix failed: " + flog);
            }
        }
        // 单次启动（都只生效一次）：
        //   UEFI + shim/direct → 固件 BootNext（写后读回校验）
        //   UEFI + bootapp     → BCD bootsequence（此处再校验一遍）
        //   BIOS/MBR           → BCD bootsequence（写后读回校验）
        // 单次启动**只在"暂存+重启"模式设**；菜单项模式保持"用户自己选"（用户规格）。
        // 三处都校验：安全软件可让命令返回 0 但不落盘 → 重启照旧进原系统（用户反馈）。
        if (menuEntry) {
            // 常驻菜单**可被选中**的前提：{bootmgr} timeout > 0 —— timeout=0 时
            // bootmgr 不停留直接进 Windows，按什么键都没用（用户 2026-09-29 实测；
            // timeout=0 来源：还原后的系统 BCD 被 bootfix 写成 0，见 bootfix.cpp）。
            // 策略**只升不降**：原值 ≥1 一律保留（用户："原来大于 1 就保留原值"），
            // 0/缺失补成 1 秒（用户：1 秒足够用上下键选、几乎不拖慢开机）。
            // 仅 BCD 菜单路径需要（BIOS，或 Secure Boot 的 bootapp 模式）；
            // UEFI 常驻项在固件 BootOrder（开机按快捷启动键选，键名按品牌识别：
            // common/bioskey），与 bootmgr timeout 无关。
            if (!useUefi || CurrentSbMode() == SbMode::Bootapp) {
                std::string tlog;
                int cur = BcdGetBootmgrTimeout(tlog);
                if (cur < 1) {
                    if (!BcdSetBootmgrTimeout(1, tlog)) {
                        ReleaseOpLock();
                        ProgressDone("restore", "failed");
                        err = Tr("设置启动菜单停留时间失败（菜单将无法被选中）: ") +
                              tlog + BootWriteBlockedHint();
                        LogError(err);
                        return 1;
                    }
                    LogInfo("bootmgr timeout: " + std::to_string(cur) +
                            " -> 1s (menu selectable)");
                } else {
                    LogInfo("bootmgr timeout kept at " + std::to_string(cur) +
                            "s (>=1, menu selectable)");
                }
            }
            LogInfo("menu entry mode: no BootNext/bootsequence (user picks it in "
                    "the boot menu)");
        } else if (useUefi && CurrentSbMode() != SbMode::Bootapp) {
            // 用独立日志串：blog 在安装引导项时已经写进日志了，BootNext 的
            // 结果必须自己再打一遍（PIT-194 —— 以前这段 message 被吞掉，
            // 现场只能看到"entry installed"，看不到 BootNext 到底成没成）。
            std::string nlog;
            if (!SetUefiBootNext(nlog, true)) {
                ReleaseOpLock();
                ProgressDone("restore", "failed");
                err = Tr("设置固件单次启动失败: ") + nlog +
                      BootWriteBlockedHint();
                LogError(err);
                return 1;
            }
            LogInfo("bootnext (UEFI single boot): " + nlog);
            // 重启前复核（PIT-194/195）：条目在 + 临时内核在 + (BootNext 指向 或
            // BootOrder 首位)。复核不通过 → 重写一次；**仍然不通过就绝不重启**
            // （用户 2026-10-31：宁可这里明确失败，也不要"重启一次白等"）。
            std::string vd;
            bool bootOk = UefiTempBootStateOk(vd);
            if (!bootOk) {
                LogWarn("boot state check FAILED before reboot: " + vd +
                        " -> rewrite once");
                std::string rlog;
                SetUefiBootNext(rlog, true);
                LogInfo("bootnext retry: " + rlog);
                bootOk = UefiTempBootStateOk(vd);
                if (bootOk)
                    LogInfo("boot state verified after retry: " + vd);
            } else {
                LogInfo("boot state verified: " + vd);
            }
            if (!bootOk) {
                UnmountEsp(espRoot, blog);
                ReleaseOpLock();
                ProgressDone("restore", "failed");
                err = Tr("无法确保下次启动进入恢复环境（启动项没有生效：") + vd +
                      Tr("）。\n本次已取消、**没有重启**，目标分区一个字节都没动。\n"
                         "请先退出安全软件（火绒/360 的引导区保护）后重试；\n"
                         "或改用 Windows PE 启动 → 选镜像与 C: → 就地还原"
                         "（PE 里不需要重启交接，最稳）。") +
                      BootWriteBlockedHint();
                LogError(err);
                return 1;
            }
        } else if (!useUefi || CurrentSbMode() == SbMode::Bootapp) {
            std::string slog;
            if (!BcdSetBootsequence(TemporaryGuid(), slog)) {
                ReleaseOpLock();
                ProgressDone("restore", "failed");
                err = Tr("设置单次启动失败（重启后可能进不了还原环境）: ") + slog +
                      BootWriteBlockedHint();
                LogError(err);
                return 1;
            }
            LogInfo("bootsequence verified: " + slog);
        }
    }
    // 日常还原（非安装菜单）→ 新系统首启自动清掉**临时**启动项/载荷。
    // 只处理 A 引擎（有离线 ~new 可写 RunOnce）；B 引擎当前没有离线 hive。
    if (!menuEntry) {
        if (req.engine == "swap" && !swapNewDir.empty()) {
            std::string clog;
            if (!InstallFirstBootCleanup(swapNewDir, exeDir, clog)) {
                LogWarn("first-boot cleanup NOT installed: " + clog);
            } else {
                LogInfo(clog);
            }
        } else {
            LogWarn("first-boot cleanup skipped (engine has no offline ~new; "
                    "temporary UEFI/BCD entry may remain until manual clear)");
        }
    }

    // 把本次部署到目标盘的痕迹收进 logs（用户 2026-10-03 规格）：盘根的
    // grldr/grldr.mbr/menu.lst/_zjresy 日志 + ZJRESTORE\bootfix|scripts|logs + 清单。
    // 用户排错只需发 logs 文件夹（含这些 + diag.txt/list.txt + 救援日志）。
    CollectDeployArtifacts(logsDir + L"\\collected",
                           target.letter.empty() ? 0 : target.letter[0]);
    ReleaseOpLock();
    // ⚠️ 只有**菜单项模式**不重启；暂存+重启模式必须保留 needReboot=true，
    // 否则 GUI/CLI 走"就地完成"分支：显示"还原完成"且**不自动重启**（用户必须
    // 手动重启，看起来像"中途停止"）。此 bug 自 ≤0.6.4 起存在，0.6.21 修复：
    // 原来这里无条件 `*needReboot = false`，注释却写着"菜单项模式不重启"。
    if (menuEntry && needReboot)
        *needReboot = false;
    ProgressDone("restore", "staged");
    if (menuEntry) {
        SetMenuFlag(true);
        LogInfo("restore menu entry installed (contract written; boots into the "
                "rescue layer only if the user picks it)");
    } else {
        LogInfo("restore staged, reboot to execute");
        // 待执行标记（用户 2026-10-05 规格）：救援层是否真的跑过，用它与
        // ZJRESTORE-status.txt 的时间比较判断 —— 引导失败/开机断电导致"救援
        // **从未执行**"时，下次启动也会明确提示，而不是"什么都没发生"。
        // 只写"暂存+重启"路径；菜单安装不写（常驻菜单不是待执行任务）。
        {
            std::wstring md = LogBaseDir() + L"\\logs";
            CreateDirectoryW(md.c_str(), nullptr);
            SYSTEMTIME st2 = {};
            GetLocalTime(&st2);
            char mk[600];
            snprintf(mk, sizeof(mk),
                     "time=%04u-%02u-%02u %02u:%02u:%02u\ntarget=%s\nimage=%s\n",
                     st2.wYear, st2.wMonth, st2.wDay, st2.wHour, st2.wMinute,
                     st2.wSecond, W2U(target.letter).c_str(),
                     W2U(imagePath).c_str());
            HANDLE hm = CreateFileW(
                (md + L"\\pending-restore.txt").c_str(), GENERIC_WRITE, 0,
                nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hm != INVALID_HANDLE_VALUE) {
                std::string body(mk);
                DWORD w = 0;
                WriteFile(hm, body.data(), (DWORD)body.size(), &w, nullptr);
                CloseHandle(hm);
            }
        }
    }
    rollbackArmed = false;  // 成功：契约保留（菜单绑定 / 暂存任务）
    return 0;
}

// 公开入口：默认 = 暂存+重启（单次启动）；menuEntry = 只装常驻菜单项。
int StageRestore(const RestoreRequest& req, std::string& err, bool* needReboot,
                 ProgressFn progress, ErrAdvice* adv) {
    return StageRestoreImpl(req, err, needReboot, progress, adv,
                            /*menuEntry=*/false);
}

int StageRestoreMenu(const RestoreRequest& req, std::string& err,
                     ProgressFn progress, ErrAdvice* adv) {
    bool needReboot = false;
    return StageRestoreImpl(req, err, &needReboot, progress, adv,
                            /*menuEntry=*/true);
}
namespace {

bool ReadWholeSmall(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[4096];
    DWORD rd = 0;
    out.clear();
    while (ReadFile(h, buf, sizeof(buf), &rd, nullptr) && rd > 0) {
        out.append(buf, rd);
        if (out.size() > (256u << 10)) break;
    }
    CloseHandle(h);
    return !out.empty();
}

std::string FieldOf(const std::string& body, const char* key) {
    const std::string k = std::string(key) + "=";
    size_t pos = 0;
    while (pos < body.size()) {
        size_t e = body.find('\n', pos);
        if (e == std::string::npos) e = body.size();
        std::string line = body.substr(pos, e - pos);
        if (line.compare(0, k.size(), k) == 0) {
            std::string v = line.substr(k.size());
            while (!v.empty() && (v.back() == '\r' || v.back() == '\n'))
                v.pop_back();
            return v;
        }
        pos = e + 1;
    }
    return std::string();
}

// `~new\.zj-done` 里的 content=NNN（无/坏 = 0）
unsigned long long DoneMarkerBytes(const std::wstring& dir) {
    std::string t;
    if (!ReadWholeSmall(dir + L"\\.zj-done", t)) return 0;
    const std::string v = FieldOf(t, "content");
    if (v.empty()) return 0;
    return strtoull(v.c_str(), nullptr, 10);
}

}  // namespace
StagedTaskState CheckStagedTaskState(std::string* detailOut) {
    auto setD = [&](const std::string& d) {
        if (detailOut) *detailOut = d;
    };
    const std::wstring pend = LogBaseDir() + L"\\logs\\pending-restore.txt";
    std::string body;
    if (!ReadWholeSmall(pend, body)) {
        setD("no pending-restore marker at " + W2U(pend));
        return StagedTaskState::None;
    }
    const std::string tgt = FieldOf(body, "target");
    if (tgt.empty()) {
        setD("pending marker has no target");
        return StagedTaskState::NoContract;
    }
    const wchar_t drive = (wchar_t)toupper((unsigned char)tgt[0]);
    const std::wstring root = std::wstring(1, drive) + L":\\";

    // ① 目标盘根的契约日志（救援层就是靠它找任务）
    std::wstring contractName;
    {
        WIN32_FIND_DATAW fd = {};
        HANDLE h = FindFirstFileW((root + L"_zjresy*.log").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) {
            setD("no _zjresy contract on target root");
            return StagedTaskState::NoContract;
        }
        contractName = fd.cFileName;
        FindClose(h);
    }

    // ② ~new 是否完整（.zj-done 的 content 必须与契约里的 image_size 一致）
    const std::wstring newDir = root + L"~new";
    const unsigned long long doneBytes = DoneMarkerBytes(newDir);
    if (doneBytes == 0) {
        setD("~new\\.zj-done missing or empty");
        return StagedTaskState::MissingNew;
    }
    unsigned long long wantBytes = 0;
    {
        std::string c;
        if (ReadWholeSmall(root + contractName, c))
            wantBytes = strtoull(FieldOf(c, "image_size").c_str(), nullptr, 10);
    }
    if (wantBytes > 0 && doneBytes != wantBytes) {
        char b[160];
        snprintf(b, sizeof(b),
                 "~new content=%llu but image_size=%llu (mismatch)",
                 doneBytes, wantBytes);
        setD(b);
        return StagedTaskState::MissingNew;
    }

    // ③ UEFI：临时启动项在 + **临时内核真的在 ESP 上**。
    // 只看 NVRAM 不够：救援层跑过一次就会把临时内核删掉（自失效，PIT-197），
    // 此时条目还在、BootOrder[0] 也还是它 → 会误判"可以重启"，用户白重启一次。
    if (IsUefiFirmware()) {
        std::string d;
        if (!UefiTempBootStateOk(d)) {
            setD("temporary boot entry not ready: " + d);
            return StagedTaskState::NoBootEntry;
        }
        int diskIdx = -1;
        for (const auto& dk : EnumerateDisks())
            for (const auto& pt : dk.parts)
                if (!pt.letter.empty() &&
                    (wchar_t)towupper(pt.letter[0]) == drive)
                    diskIdx = (int)dk.index;
        std::wstring espRoot;
        bool guidMatched = false;
        std::string blog;
        if (diskIdx < 0 || !AcquireEspRoot(espRoot, guidMatched, blog, diskIdx)) {
            setD("cannot mount ESP to verify the temporary kernel");
            return StagedTaskState::NoBootEntry;
        }
        const DWORD ka = GetFileAttributesW(
            (espRoot + L"EFI\\ZJRESTORE\\vmlinuz-zjtemp.efi").c_str());
        UnmountEsp(espRoot, blog);
        if (ka == INVALID_FILE_ATTRIBUTES) {
            setD("temporary kernel missing on ESP (staged entry already disarmed)");
            return StagedTaskState::NoBootEntry;
        }
    }
    setD("staged task ready (contract + ~new + boot entry)");
    return StagedTaskState::Ok;
}

int CleanupStaleStagedTask() {
    int n = 0;
    const std::wstring pend = LogBaseDir() + L"\\logs\\pending-restore.txt";
    if (DeleteFileW(pend.c_str())) ++n;
    // 目标盘根的陈旧契约（_zjresy*.log）
    DWORD mask = GetLogicalDrives();
    for (wchar_t c = L'C'; c <= L'Z'; ++c) {
        if (!(mask & (1u << (c - L'A')))) continue;
        const std::wstring pat = std::wstring(1, c) + L":\\_zjresy*.log";
        WIN32_FIND_DATAW fd = {};
        HANDLE h = FindFirstFileW(pat.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            const std::wstring p = std::wstring(1, c) + L":\\" + fd.cFileName;
            SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (DeleteFileW(p.c_str())) ++n;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    // UEFI 临时启动项（含 BootNext）
    if (IsUefiFirmware()) {
        std::string log;
        if (RemoveUefiBootEntry(log, /*temporary=*/true)) ++n;
    }
    if (n > 0)
        LogWarn("stale staged task cleaned (" + std::to_string(n) + " item(s))");
    return n;
}
// ── 返回旧系统（PIT-139，方案 C）──────────────────────────────────────
// 只读探测：目标根是否同时具备「完整旧系统 ~/.old\Windows」与「完整新系统
// 根 \Windows」。两者都在才谈得上"反向交换"；只有其一说明上次交换中断或
// 已清理过 —— 此时改走「删除旧系统」或不再提供该入口。
bool DetectReturnOldCapability(const PartitionInfo& target, bool* hasOldTree,
                               bool* hasRootTree, std::string& detail) {
    if (hasOldTree) *hasOldTree = false;
    if (hasRootTree) *hasRootTree = false;
    detail.clear();
    if (target.letter.empty()) {
        detail = "no drive letter";
        return false;
    }
    const std::wstring root = target.letter + L":\\";
    auto isDir = [&](const std::wstring& rel) -> bool {
        DWORD a = GetFileAttributesW((root + rel).c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
    };
    const bool oldDir = isDir(L"~old");
    const bool oldWin = isDir(L"~old\\Windows");
    const bool rootWin = isDir(L"Windows");
    if (hasOldTree) *hasOldTree = oldDir && oldWin;
    if (hasRootTree) *hasRootTree = rootWin;
    detail = "~old=" + std::string(oldDir ? "yes" : "no") +
             " ~old\\Windows=" + std::string(oldWin ? "yes" : "no") +
             " root\\Windows=" + std::string(rootWin ? "yes" : "no");
    return oldDir && oldWin && rootWin;
}

int StageReturnOld(const PartitionInfo& target, std::string& err,
                   bool* needReboot) {
    if (needReboot)
        *needReboot = false;

    // 等启动期后台残留清理收尾（与 StageRestore 同款，别在它还没收手时动盘）
    if (!target.letter.empty())
        WaitSwapCleanup(target.letter[0]);

    bool hasOld = false, hasRoot = false;
    std::string det;
    DetectReturnOldCapability(target, &hasOld, &hasRoot, det);
    if (!hasOld) {
        err = std::string(Tr("目标分区上没有可用的旧系统（~old\\Windows 不存在）—— 无法返回。")) +
              "\n" + det;
        LogError("return-old rejected: " + det);
        return 4;
    }
    if (!hasRoot) {
        err = std::string(Tr("目标分区根目录不是完整的当前系统（\\Windows 不存在）—— 无法返回。")) +
              "\n" + det;
        LogError("return-old rejected: " + det);
        return 4;
    }
    if (target.letter.empty()) {
        err = Tr("目标分区无盘符，无法写恢复日志（Linux 侧将无法定位目标）");
        LogError(err);
        return 1;
    }
    if (HasInstalledMenu()) {
        err = Tr("已安装常驻恢复菜单；返回旧系统会覆盖它。请先在主界面点「清除菜单」，再重试。");
        LogError("return-old rejected: persistent menu installed");
        return 4;
    }

    // Q2 暂存路径会话前置（与 StageRestoreImpl 同款，见 bootpath.h）：
    // 返回旧系统 = 重启进 Linux 救援层，依赖重启后固件沿当前同一种方式引导。
    switch (CheckStagedBootPath(IsUefiFirmware(), DiskStyleOf(target.diskIndex))) {
        case StagedBootPath::BiosSessionGptTarget:
            err = Tr("当前会话是传统(CSM)引导，目标是 GPT 盘 —— 返回旧系统需要重启进恢复环境，"
                     "但 BIOS 链无法引导 GPT 盘。请改用 UEFI 模式启动后操作。");
            LogError("return-old rejected: BIOS session + GPT target");
            return 4;
        case StagedBootPath::UefiSessionMbrTarget:
            err = Tr("当前会话是 UEFI 引导，目标是 MBR 盘 —— 返回旧系统需要重启进恢复环境，"
                     "但 UEFI 无法执行传统引导链。请改用传统(CSM)模式启动后操作。");
            LogError("return-old rejected: UEFI session + MBR target");
            return 4;
        case StagedBootPath::Ok:
            break;
    }

    std::wstring exeDir = ContractDir();  // Q1 settle 后返回新家；否则 = ExeDir()
    // 软件目录若在目标盘上：先尝试 SettleHome（Q1「先落盘后搬家」，与 StageRestore
    // 同款）——搬成功则 exeDir 指向新家、继续；搬不动才前置拒绝（让用户把程序
    // 拷到别的分区或进 PE 再操作）。
    {
        wchar_t exLetter = exeDir.size() > 1 ? exeDir[0] : 0;
        if (ShouldSettleHome(exeDir, target.letter.empty() ? 0 : target.letter[0])) {
            int exeDisk = VolumeDiskIndex(exLetter);
            int tgtDisk = (int)target.diskIndex;
            SettleResult sr = SettleHome(tgtDisk, exeDisk);
            if (sr.moved) {
                exeDir = ContractDir();  // settle 后 ContractDir 返回新家
                LogInfo("return-old: settled to " + W2U(sr.newHome));
            } else {
                err = Tr("本程序当前运行在目标分区上 —— 返回旧系统会把该分区整树挪走，"
                         "请先把程序复制到**别的分区**（如 D:\\）再执行。");
                LogError("return-old rejected: software dir is on the target partition, "
                         "settle failed: " + sr.detail);
                return 4;
            }
        }
    }

    if (!AcquireOpLock()) {
        err = Tr("已有备份/还原实例在运行");
        return 1;
    }
    ProgressUpdate("restore", 0, "return-old-start");
    LogInfo("return-old staged: target=" + W2U(target.letter) + " " + det);

    std::wstring logsDir = LogBaseDir() + L"\\logs";
    CreateDirectoryW(logsDir.c_str(), nullptr);
    BcdExport(logsDir + L"\\bcd-backup");

    DiskInfo tdisk;
    for (const auto& d : EnumerateDisks())
        if (d.index == target.diskIndex)
            tdisk = d;

    // 契约：action=returnold + engine=swap（**关键降级**：旧救援层不认识
    // returnold 会落进 engine=swap 分支 → zjswap.sh 查不到 ~new/.zj-done →
    // exit 2 报 "old system untouched"，绝不 mkntfs；fail-closed）。
    // image_* 全空：这条路不需要镜像、不 apply。
    RestoreTask t;
    t.action = "returnold";
    t.engine = "swap";
    t.imagePath.clear();
    t.imageRelPath.clear();
    t.imagePartGuid.clear();
    t.imageIndex = 1;
    t.espIndex = 0;
    t.eaIndex = 0;
    t.targetGuid = target.guid;
    t.targetOffset = target.offsetBytes;
    t.targetSize = target.sizeBytes;
    t.targetDiskSerial = tdisk.serial;
    t.targetDisk = target.diskIndex;
    t.targetPart = (int)target.partNumber;
    t.ptType = tdisk.style == PartitionStyle::GPT ? "gpt" : "mbr";
    t.repairBoot = true;
    t.targetDiskName = tdisk.model;
    t.targetDiskSize = tdisk.sizeBytes;
    t.targetFs = target.fs;
    t.targetVolLabel = target.label;
    t.softwarePath = ExePath();
    {
        wchar_t exLetter = exeDir.size() > 1 ? exeDir[0] : L'\0';
        wchar_t exRoot[4] = {exLetter, L':', L'\\', L'\0'};
        bool fixedDisk = exLetter && GetDriveTypeW(exRoot) == DRIVE_FIXED;
        if (fixedDisk) {
            t.softwareDir = exeDir;
        } else {
            std::wstring dataDrive = FindDataDrive(target.letter.empty() ? 0 : target.letter[0]);
            t.softwareDir = dataDrive.empty() ? exeDir : dataDrive + kRecoveryDir;
            CreateDirectoryW(t.softwareDir.c_str(), nullptr);
        }
    }

    // 清场（A2）+ 主/副契约写入
    {
        std::string clog;
        int cleaned = CleanupStrayContracts(clog);
        LogInfo("stray contracts cleanup: " + std::to_string(cleaned) +
                " removed (" + clog.substr(0, clog.size() - 1) + ")");
    }
    std::string tlog;
    if (!WriteRestoreLog(target.letter[0], t, tlog)) {
        ReleaseOpLock();
        ProgressDone("restore", "failed");
        err = Tr("写恢复日志失败（需管理员权限写 ") + W2U(target.letter) + Tr(":\\）");
        LogError(err + " / " + tlog);
        return 1;
    }
    {
        std::string wtlog;
        bool okTask = WriteRestoreTask(exeDir, t, wtlog);
        std::wstring used = exeDir;
        if (!okTask) {
            std::wstring dataDrive = FindDataDrive(target.letter.empty() ? 0 : target.letter[0]);
            std::wstring alt =
                dataDrive.empty() ? std::wstring() : dataDrive + L"ZJRESTORE";
            if (!alt.empty()) {
                CreateDirectoryW(alt.c_str(), nullptr);
                wtlog.clear();
                if (WriteRestoreTask(alt, t, wtlog)) {
                    okTask = true;
                    used = alt;
                }
            }
        }
        if (!okTask)
            LogError("return-old task write failed (non-fatal; the target-root "
                     "log is the primary contract): " + wtlog);
        else
            LogInfo("return-old task written to " + W2U(used));
    }

    // 引导层失败 → 契约回滚（PIT-126 同款：契约已在引导层之前写好，引导层
    // 失败 = 任务永远不会执行 → 必须撤销，否则目标根留一份陈旧契约）。
    bool rollbackArmed = true;
    struct RollbackGuard {
        bool* armed;
        ~RollbackGuard() {
            if (!*armed) return;
            std::string rlog;
            int n = CleanupStrayContracts(rlog);
            DeleteMenuBinding();
            LogWarn("boot layer failed -> staged contracts rolled back (" +
                    std::to_string(n) + " stray removed)");
        }
    } rollbackGuard{&rollbackArmed};

    // 引导层**无条件重装**（PIT-037）：用户可能已经「删除引导项」，而本次
    // 必须能单次启动进救援层。UEFI 下 ESP 常驻项也一并刷新。
    const bool useUefi = UseUefiBootFor(target);
    LogInfo(std::string("boot path: ") + (useUefi ? "UEFI/ESP" : "BIOS/GRUB4DOS") +
            " (firmware=" + (IsUefiFirmware() ? "UEFI" : "BIOS") +
            ", target disk style=" + StyleName(DiskStyleOf(target.diskIndex)) + ")");
    std::string blog;
    if (useUefi) {
        SbMode sbm = CurrentSbMode();
        if (sbm == SbMode::Shim && !IsMokEnrolled(exeDir)) {
            ReleaseOpLock();
            ProgressDone("restore", "failed");
            err = Tr("本机开启了 Secure Boot，但还没注册启动密钥：请先点" "「安装菜单」，重启一次，在蓝底界面选 " "Enroll key -> 选 zj-mok.cer -> 注册，之后回来再操作");
            LogError(err);
            return 1;
        }
        std::wstring espRoot;
        bool guidMatched = false;
        if (!AcquireEspRoot(espRoot, guidMatched, blog,
                             (int)target.diskIndex)) {
            LogError("ESP acquire log (returnold):\n" + blog);
            ReleaseOpLock();
            ProgressDone("restore", "failed");
            err = Tr("UEFI 机器未找到可用的 ESP 引导分区，无法部署引导层。\n"
                     "常见原因：DiskGenius 重建分区时漏建 ESP，或把 ESP 标成了"
                     " Basic Data（应改为 EFI System 类型）。\n当前分区表：\n") +
                  W2U(DescribePartitions());
            LogError(err);
            return 1;
        }
        if (!InstallUefiBootEntry(espRoot, exeDir, blog, true)) {
            ReleaseOpLock();
            ProgressDone("restore", "failed");
            err = Tr("安装 UEFI 引导层失败: ") + blog;
            LogError(err);
            return 1;
        }
    } else {
        std::wstring deployDrive = std::wstring(1, target.letter[0]) + L":\\";
        if (!InstallBootLayer(deployDrive, exeDir, blog, true)) {
            ReleaseOpLock();
            ProgressDone("restore", "failed");
            err = Tr("安装引导层失败: ") + blog;
            LogError(err);
            return 1;
        }
    }
    // 单次启动（只生效一次；写后读回校验，见 BootWriteBlockedHint）
    if (useUefi && CurrentSbMode() != SbMode::Bootapp) {
        if (!SetUefiBootNext(blog, true)) {
            ReleaseOpLock();
            ProgressDone("restore", "failed");
            err = Tr("设置固件单次启动失败: ") + blog + BootWriteBlockedHint();
            LogError(err);
            return 1;
        }
    } else {
        std::string slog;
        if (!BcdSetBootsequence(TemporaryGuid(), slog)) {
            ReleaseOpLock();
            ProgressDone("restore", "failed");
            err = Tr("设置单次启动失败（重启后可能进不了还原环境）: ") + slog +
                  BootWriteBlockedHint();
            LogError(err);
            return 1;
        }
        LogInfo("bootsequence verified: " + slog);
    }
    // 返回旧系统也是一次性引导：把清理器注入即将回到根的旧系统 ~old，
    // 首启删除临时条目（常驻菜单已在入口处禁止覆盖）。
    {
        std::string clog;
        if (!InstallFirstBootCleanup(target.letter + L":\\~old", exeDir, clog))
            LogWarn("return-old first-boot cleanup NOT installed: " + clog);
        else
            LogInfo("return-old: " + clog);
    }

    CollectDeployArtifacts(logsDir + L"\\collected", target.letter[0]);
    AppendHistory("return-old-staged", L"", L"", 0,
                  "staged; 实际结果见救援层日志");
    ReleaseOpLock();

    // 待执行标记（PIT-115 同款）：救援层是否真的跑过，用它与 ZJRESTORE-status.txt
    // 的时间比较判断 —— 引导失败/断电导致救援层**从未执行**时，下次启动也会明确提示。
    {
        std::wstring md = LogBaseDir() + L"\\logs";
        CreateDirectoryW(md.c_str(), nullptr);
        SYSTEMTIME st2 = {};
        GetLocalTime(&st2);
        char mk[600];
        snprintf(mk, sizeof(mk),
                 "time=%04u-%02u-%02u %02u:%02u:%02u\ntarget=%s\nimage=%s\n",
                 st2.wYear, st2.wMonth, st2.wDay, st2.wHour, st2.wMinute,
                 st2.wSecond, W2U(target.letter).c_str(),
                 "return-old (~old -> root)");
        HANDLE hm = CreateFileW((md + L"\\pending-restore.txt").c_str(),
                                GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hm != INVALID_HANDLE_VALUE) {
            std::string body(mk);
            DWORD w = 0;
            WriteFile(hm, body.data(), (DWORD)body.size(), &w, nullptr);
            CloseHandle(hm);
        }
    }
    rollbackArmed = false;  // 成功：契约保留（救援层要靠它定位）
    if (needReboot)
        *needReboot = true;  // PIT-110 教训：**不要无条件清零**
    ProgressDone("restore", "staged");
    LogInfo("return-old staged, reboot to execute (action=returnold)");
    return 0;
}

// 便捷入口（PIT-139，给 CLI 用）：按磁盘号/分区号定位目标后走 StageReturnOld。
// disk/part 都 < 0 时自动找「根下有 Windows、且 ~old\Windows 也存在」的第一块
// 固定盘 —— 与 GUI 扫残留时看到的是同一棵树，不会挑错。
int StageReturnOldByDiskPart(int disk, int part, std::string& err,
                             bool* needReboot) {
    PartitionInfo target;
    bool found = false;
    for (const auto& d : EnumerateDisks()) {
        if (disk >= 0 && (int)d.index != disk) continue;
        for (const auto& p : d.parts) {
            if (part >= 0) {
                if ((int)p.partNumber != part) continue;
                target = p;
                found = true;
                continue;
            }
            if (p.letter.size() != 1) continue;
            PartitionInfo cand = p;
            bool hOld = false, hRoot = false;
            std::string det;
            if (DetectReturnOldCapability(cand, &hOld, &hRoot, det) && hOld &&
                hRoot) {
                target = cand;
                found = true;
                break;
            }
        }
        if (found && part < 0) break;
    }
    if (!found) {
        err = std::string(Tr("找不到「返回旧系统」的目标分区：根下没有完整系统、"
                             "~old\\Windows 也不存在。\n")) +
              Tr("可用 `list` 确认后加 --disk N --part M 指定。");
        LogError("return-old: target not found");
        return 4;
    }
    LogInfo("return-old target: disk=" + std::to_string(target.diskIndex) +
            " part=" + std::to_string(target.partNumber) + " letter=" +
            W2U(target.letter) + ":");
    return StageReturnOld(target, err, needReboot);
}
// 读"已安装菜单项"绑定的镜像/目标：<ContractDir()>\restore-task.conf（UTF-8，key=value）。
bool ReadMenuBinding(std::wstring* imagePath, int* imageIndex,
                     unsigned long long* targetOffset,
                     unsigned long long* targetSize) {
    // 只有显式「安装菜单」才写 restore-menu.flag；日常还原的临时契约不能
    // 让 GUI 把「安装菜单」按钮误显示成「删除菜单」。
    if (!HasInstalledMenu())
        return false;
    std::wstring conf = ContractDir() + L"\\restore-task.conf";
    HANDLE h = CreateFileW(conf.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    std::string text;
    {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0)
            text.append(buf, n);
        CloseHandle(h);
    }
    std::string img = TaskConfGet(text, "image_path");
    std::string idx = TaskConfGet(text, "image_index");
    std::string off = TaskConfGet(text, "target_offset");
    std::string sz = TaskConfGet(text, "target_size");
    if (img.empty())
        return false;
    if (imagePath)
        *imagePath = U2W(img);
    if (imageIndex)
        *imageIndex = idx.empty() ? 1 : std::atoi(idx.c_str());
    if (targetOffset)
        *targetOffset = off.empty() ? 0 : strtoull(off.c_str(), nullptr, 10);
    if (targetSize)
        *targetSize = sz.empty() ? 0 : strtoull(sz.c_str(), nullptr, 10);
    return true;
}

// 删除"菜单绑定"副契约（用户 2026-10-05 bug：「删除菜单」后按钮仍是「删除菜单」，
// 因为 ReadMenuBinding 读的 restore-task.conf 没被删）。
void DeleteMenuBinding() {
    SetMenuFlag(false);
    std::wstring dir = ContractDir();
    for (const wchar_t* nm : {L"restore-task.conf", L"restore-task.json"}) {
        std::wstring p = dir + L"\\" + nm;
        SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (DeleteFileW(p.c_str()))
            LogInfo("menu binding removed: " + W2U(p));
    }
    // 顺带把"待执行标记"也清掉（若用户删菜单时还没重启执行）
    std::wstring pend = LogBaseDir() + L"\\logs\\pending-restore.txt";
    SetFileAttributesW(pend.c_str(), FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(pend.c_str());
}
// 一键重试（P3 / PIT-148）：从 <ContractDir()>\restore-task.conf 重建请求并
// StageRestore。仅 A/B 类失败可走此路（调用方已用 RescueStepRetryable 把关）；
// keepOld 恒 false（重试要备返请走正常流程，见 ops.h）。
int RetryStagedRestore(std::string& err, bool* needReboot) {
    err.clear();
    if (needReboot) *needReboot = false;
    // ① 读 conf（key=value；TaskConfGet 已处理注释/转义，见 task.cpp）
    std::wstring confPath = ContractDir() + L"\\restore-task.conf";
    std::string text;
    {
        HANDLE h = CreateFileW(confPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            err = Tr("重试失败：程序目录没有 restore-task.conf，请重新执行还原");
            LogError("retry: conf missing: " + W2U(confPath));
            return 2;
        }
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0)
            text.append(buf, n);
        CloseHandle(h);
    }
    // ② 只允许 action=restore（returnold 另有入口；混用会把任务改坏）
    std::string action = TaskConfGet(text, "action");
    if (!action.empty() && action != "restore") {
        err = Tr("重试失败：上次任务不是还原（") + action +
              Tr("），请重新执行「返回旧系统」");
        LogError("retry: non-restore action=" + action);
        return 2;
    }
    std::string img = TaskConfGet(text, "image_path");
    if (img.empty()) {
        err = Tr("重试失败：restore-task.conf 缺 image_path，请重新执行还原");
        LogError("retry: conf missing image_path");
        return 2;
    }
    // ③ 按 offset/size/serial 匹配当前磁盘表（盘符可能变、sdX 字母不可靠）
    std::string offS = TaskConfGet(text, "target_offset");
    std::string szS = TaskConfGet(text, "target_size");
    std::string serialS = TaskConfGet(text, "target_disk_serial");
    unsigned long long off = offS.empty() ? 0 : strtoull(offS.c_str(), nullptr, 10);
    unsigned long long sz = szS.empty() ? 0 : strtoull(szS.c_str(), nullptr, 10);
    if (!off) {
        err = Tr("重试失败：restore-task.conf 缺 target_offset，请重新执行还原");
        LogError("retry: conf missing target_offset");
        return 2;
    }
    PartitionInfo target;
    DiskInfo tdisk;
    bool found = false;
    for (const auto& d : EnumerateDisks()) {
        if (!serialS.empty() && W2U(d.serial) != serialS) continue;
        for (const auto& p : d.parts) {
            if (p.offsetBytes != off) continue;
            if (sz && p.sizeBytes && p.sizeBytes != sz) continue;
            target = p;
            tdisk = d;
            found = true;
            break;
        }
        if (found) break;
    }
    if (!found) {
        err = Tr("重试失败：按契约 offset/size 找不到目标分区（磁盘表已变化？）");
        LogError("retry: target not found offset=" + offS + " size=" + szS +
                 " serial=" + serialS);
        return 4;
    }
    LogInfo("retry: target disk=" + std::to_string(target.diskIndex) +
            " part=" + std::to_string(target.partNumber) + " letter=" +
            W2U(target.letter) + ": offset=" + offS);
    // ④ 重建请求 → StageRestore（安全检查/契约/引导层全流程复用）
    RestoreRequest req;
    req.image = U2W(img);
    req.disk = (int)tdisk.index;
    req.part = (int)target.partNumber;
    req.index = 1;
    std::string idxS = TaskConfGet(text, "image_index");
    if (!idxS.empty()) req.index = std::atoi(idxS.c_str());
    req.repairBoot = TaskConfGet(text, "repair_boot") != "0";
    std::string eng = TaskConfGet(text, "engine");
    req.engine = eng.empty() ? "block" : eng;  // 老 conf 缺键 = B 引擎
    req.keepOld = false;  // 重试要备返请走正常流程（AGENTS 注明边界）
    bool reboot = false;
    std::string serr;
    ErrAdvice adv = ADV_NONE;
    int rc = StageRestore(req, serr, &reboot, nullptr, &adv);
    if (rc == 0) {
        if (needReboot) *needReboot = reboot;
        AppendHistory("restore-retry", req.image,
                      L"disk" + std::to_wstring(req.disk) + L" part" +
                          std::to_wstring(req.part),
                      0, "ok");
        LogInfo("retry: staged ok reboot=" + std::string(reboot ? "1" : "0"));
        return 0;
    }
    AppendHistory("restore-retry", req.image,
                  L"disk" + std::to_wstring(req.disk) + L" part" +
                      std::to_wstring(req.part),
                  0, ("failed rc=" + std::to_string(rc)).c_str());
    err = serr.empty() ? Tr("重试失败（详见日志）") : serr;
    LogError("retry: StageRestore rc=" + std::to_string(rc) + " " + err);
    return rc == 4 ? 4 : 1;
}

}  // namespace sysrecover
