// 九转还原：ops_staged（阶段 2 从 src/app/ops.cpp 机械拆分而来，逻辑未改）。
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
#include "../wim/wim.h"
#include "../wim/wim_precheck.h"
#include "safety.h"
#include "selfdiag.h"
#include "imgsrc.h"

#include "ops_internal.h"

namespace sysrecover {
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
