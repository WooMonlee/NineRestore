// ESP 救援载荷版本治理（实现）。纯逻辑见 rescue_guard.h。
#include "rescue_guard.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "../common/process.h"
#include "../common/version.h"
#include "../common/zip.h"  // Crc32
#include "uefi.h"

namespace sysrecover {
namespace {

bool FileSizeOf(const std::wstring& path, unsigned long long& out) {
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
        return false;
    out = ((unsigned long long)fad.nFileSizeHigh << 32) |
          (unsigned long long)fad.nFileSizeLow;
    return true;
}

bool FileCrc32(const std::wstring& path, uint32_t& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    std::vector<char> buf;
    char tmp[1 << 20];
    DWORD rd = 0;
    while (ReadFile(h, tmp, sizeof(tmp), &rd, nullptr) && rd > 0)
        buf.insert(buf.end(), tmp, tmp + rd);
    CloseHandle(h);
    if (buf.empty())
        return false;
    out = Crc32(buf.data(), buf.size());
    return true;
}

bool FileExists(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool ReadFirstLine(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    char buf[256] = {};
    DWORD rd = 0;
    std::string all;
    while (ReadFile(h, buf, sizeof(buf) - 1, &rd, nullptr) && rd > 0) {
        all.append(buf, rd);
        if (all.size() >= 4096) break;
    }
    CloseHandle(h);
    const size_t nl = all.find_first_of("\r\n");
    out = all.substr(0, nl == std::string::npos ? all.size() : nl);
    return true;
}

bool WriteText(const std::wstring& path, const std::string& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD w = 0;
    const bool ok =
        WriteFile(h, text.data(), (DWORD)text.size(), &w, nullptr) != FALSE;
    CloseHandle(h);
    return ok;
}

// 写新载荷 + 字节数/CRC 复核。旧文件已由调用方先删（顺序契约见
// rescue_guard.h::RescuePayloadDeploySteps）；不备份、不用临时文件、不改名。
bool WriteAndVerify(const std::wstring& src, const std::wstring& dst,
                    std::string& log) {
    if (!FileExists(src)) {
        log += "source missing: " + std::string(src.begin(), src.end()) + "\n";
        return false;
    }
    SetFileAttributesW(dst.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
        log += "write failed err=" + std::to_string(GetLastError()) + "\n";
        return false;
    }
    unsigned long long ssz = 0, dsz = 0;
    uint32_t a = 0, b = 0;
    if (!FileSizeOf(src, ssz) || !FileSizeOf(dst, dsz) || ssz != dsz ||
        !FileCrc32(src, a) || !FileCrc32(dst, b) || a != b) {
        log += "verify FAIL " + std::string(dst.begin(), dst.end()) + "\n";
        return false;
    }
    log += "payload written: " + std::string(dst.begin(), dst.end()) + "\n";
    return true;
}

// 删除单个旧载荷文件；不存在也算成功。日志带释放字节。
bool RemoveOldPayload(const std::wstring& path, std::string& log,
                      bool& removedAny) {
    unsigned long long sz = 0;
    if (!FileSizeOf(path, sz))
        return true;  // 不存在
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!DeleteFileW(path.c_str())) {
        log += "remove old payload FAIL: " +
               std::string(path.begin(), path.end()) +
               " err=" + std::to_string(GetLastError()) + "\n";
        return false;
    }
    removedAny = true;
    char line[128];
    snprintf(line, sizeof(line), "old payload removed: %.1f MB\n",
             sz / 1048576.0);
    log += line;
    return true;
}

// 目录树字节数（只读；用于"清理释放了多少空间"的日志）。
unsigned long long TreeBytes(const std::wstring& path) {
    unsigned long long total = 0;
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW((path + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            total += TreeBytes(path + L"\\" + n);
        else
            total += ((unsigned long long)fd.nFileSizeHigh << 32) |
                     (unsigned long long)fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}

bool RemoveTreeLocal(const std::wstring& path) {
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW((path + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            std::wstring full = path + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                RemoveTreeLocal(full);
            } else {
                SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(full.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryW(path.c_str()) != 0;
}

}  // namespace

bool ReadEspRescuePayload(const std::wstring& espRoot, EspRescuePayload& out,
                          bool deepCrc) {
    out = EspRescuePayload{};
    if (espRoot.empty()) return false;
    const std::wstring dir = espRoot + L"EFI\\ZJRESTORE";

    std::string b;
    if (ReadFirstLine(dir + L"\\rescue-build.txt", b)) {
        out.hasBuild = true;
        out.build = NormalizeRescueBuild(b);
    }

    const std::wstring ir = dir + L"\\initramfs-zjrestore.cpio.gz";
    if (FileExists(ir)) {
        out.hasInitramfs = true;
        FileSizeOf(ir, out.initramfsSize);
        // 快路径不读本体（17.4MB）；只有明确要求深查时才算 CRC。
        if (deepCrc) out.crcKnown = FileCrc32(ir, out.initramfsCrc);
    }
    out.hasKernel = FileExists(dir + L"\\vmlinuz-zjrestore.efi") ||
                    FileExists(dir + L"\\vmlinuz-zjrestore");
    return true;
}

unsigned long long CleanupLegacyRescueBackups(const std::wstring& espRoot,
                                              std::string& log) {
    const std::wstring dir = espRoot + L"EFI\\ZJRESTORE";
    unsigned long long freed = 0;
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW((dir + L"\\backup-*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        std::wstring name = fd.cFileName;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (!IsRescueBackupDirName(name)) continue;
        const std::wstring full = dir + L"\\" + name;
        const unsigned long long bytes = TreeBytes(full);
        std::string an(name.begin(), name.end());
        if (RemoveTreeLocal(full)) {
            freed += bytes;
            char line[256];
            snprintf(line, sizeof(line),
                     "legacy payload backup removed: %s (%.1f MB)\n",
                     an.c_str(), bytes / 1048576.0);
            log += line;
        } else {
            log += "legacy payload backup remove FAIL: " + an + "\n";
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (freed) {
        char line[128];
        snprintf(line, sizeof(line),
                 "legacy payload backup cleanup freed %.1f MB\n",
                 freed / 1048576.0);
        log += line;
    }
    return freed;
}

void WriteFirmwareBootSnapshot(const std::wstring& espRoot, std::string& log) {
    const std::wstring dir = espRoot + L"EFI\\ZJRESTORE";
    std::vector<std::string> lines;
    UefiBootOrderSummary(lines);
    std::string txt;
    for (const auto& ln : lines) txt += ln + "\r\n";
    if (!WriteText(dir + L"\\firmware-boot.txt", txt))
        log += "firmware boot snapshot FAIL\n";
}

bool RefreshEspRescuePayload(const std::wstring& espRoot,
                             const std::wstring& assetDir, std::string& log,
                             std::string& msg) {
    const std::wstring dir = espRoot + L"EFI\\ZJRESTORE";
    EspRescuePayload esp;
    ReadEspRescuePayload(espRoot, esp);  // 快路径：版本戳 + 大小，不读本体

    const std::wstring biIr =
        assetDir + L"\\bootfiles\\initramfs-zjrestore.cpio.gz";
    unsigned long long biSize = 0;
    const bool biOk = FileSizeOf(biIr, biSize);

    const RescuePayloadState state = ClassifyRescuePayload(
        esp.build, SYSRECOVER_VERSION, esp.hasInitramfs || esp.hasKernel,
        /*espCrcKnown=*/false, 0u, 0u, esp.initramfsSize, biSize);

    char head[320] = {};
    snprintf(head, sizeof(head),
             "ESP rescue payload: build=%s initramfs=%llu (builtin=%s %llu)\n",
             esp.build.empty() ? "(none)" : esp.build.c_str(),
             esp.initramfsSize, SYSRECOVER_VERSION, biSize);
    log += head;

    if (!RescuePayloadMustNotBeReused(state)) {
        log += "rescue payload matches builtin (keep)\n";
        // 历史遗留的载荷备份也顺手清掉（不占空间就不留）。
        CleanupLegacyRescueBackups(espRoot, log);
        return true;
    }

    if (!biOk) {
        msg = RescuePayloadWriteFailedMessage();
        log += Tr("内置 initramfs 缺失，无法写入\n");
        log += msg + "\n";
        return false;
    }

    log += "ESP rescue payload stale -> remove old, then write new (no backup)\n";
    CleanupLegacyRescueBackups(espRoot, log);
    WriteFirmwareBootSnapshot(espRoot, log);

    // 本次要刷新的文件集（不新建文件；Secure Boot 壳文件只在 ESP 上已存在时刷新）。
    struct Target {
        std::wstring src, dst;
    };
    std::vector<Target> targets;
    if (esp.hasInitramfs)
        targets.push_back({biIr, dir + L"\\initramfs-zjrestore.cpio.gz"});

    const std::wstring biKernel = assetDir + L"\\bootfiles\\vmlinuz-zjrestore";
    if (FileExists(dir + L"\\vmlinuz-zjrestore.efi"))
        targets.push_back({biKernel, dir + L"\\vmlinuz-zjrestore.efi"});
    if (FileExists(dir + L"\\vmlinuz-zjrestore"))
        targets.push_back({biKernel, dir + L"\\vmlinuz-zjrestore"});

    struct Chain {
        const wchar_t* dst;
        const wchar_t* src;
    };
    const SbMode sb = CurrentSbMode();
    Chain chain[] = {
        {L"shimx64.efi", L"\\bootfiles\\sb\\shimx64.efi"},
        {L"grub.cfg", L"\\bootfiles\\sb\\grub.cfg"},
        {L"efiloader.efi", L"\\bootfiles\\sb\\efiloader.efi"},
        {L"zjrestore-uki.efi", L"\\bootfiles\\sb\\zjrestore-uki.efi"},
    };
    for (const auto& c : chain) {
        if (!FileExists(dir + L"\\" + c.dst)) continue;
        if (!FileExists(assetDir + c.src)) continue;
        targets.push_back({assetDir + c.src, dir + L"\\" + c.dst});
    }
    if (FileExists(dir + L"\\grubx64.efi")) {
        const wchar_t* src = (sb == SbMode::Shim)
                                 ? L"\\bootfiles\\sb\\zjrestore-uki.efi"
                                 : L"\\bootfiles\\sb\\grubx64.efi";
        if (FileExists(assetDir + src))
            targets.push_back({assetDir + src, dir + L"\\grubx64.efi"});
    }

    // 顺序契约：remove-old → write-new → verify（见 RescuePayloadDeploySteps）。
    bool removedAny = false;
    bool ok = true;
    for (const auto& t : targets) {
        if (!RemoveOldPayload(t.dst, log, removedAny)) {
            msg = RescuePayloadWriteFailedMessage();
            log += msg + "\n";
            // 若已删过别的文件，ESP 上已不完整 → 摘掉指向残骸的启动项。
            if (MustRemoveBootEntryAfterFailure(removedAny)) {
                RemoveUefiBootEntry(log, /*temporary=*/false);
                log += "boot entry removed (payload missing)\n";
            }
            return false;
        }
    }
    if (!removedAny) {
        // 一个旧文件都没删 → 说明 ESP 上已无可刷新载荷；别写残骸，直接失败。
        msg = RescuePayloadWriteFailedMessage();
        log += "no existing rescue payload to refresh\n";
        log += msg + "\n";
        // 自报版本说旧、盘上却没有载荷 → 已有启动项本身就在指向空。
        RemoveUefiBootEntry(log, /*temporary=*/false);
        log += "boot entry removed (payload missing)\n";
        return false;
    }
    for (const auto& t : targets)
        ok = WriteAndVerify(t.src, t.dst, log) && ok;

    if (!ok) {
        msg = RescuePayloadWriteFailedMessage();
        log += msg + "\n";
        // 载荷不完整 → 不能留下指向残骸的启动项。
        RemoveUefiBootEntry(log, /*temporary=*/false);
        log += "boot entry removed (payload missing)\n";
        return false;
    }

    // 最后才更新自报版本。写失败不摘启动项：载荷本身已写全并通过校验，
    // 只是"自报版本"没写成 —— 下次检测会把它判成 UnreadableBuild 再重写一次，
    // 不影响本次进救援。
    const std::string vb = std::string(SYSRECOVER_VERSION) + "\n";
    if (!WriteText(dir + L"\\rescue-build.txt", vb))
        log += "write rescue-build.txt FAIL (payload verified; next install rewrites)\n";
    return true;
}

}  // namespace sysrecover
