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

bool CopyAndVerify(const std::wstring& src, const std::wstring& dst,
                   std::string& log) {
    if (!FileExists(src)) {
        log += "source missing: " + std::string(src.begin(), src.end()) + "\n";
        return false;
    }
    SetFileAttributesW(dst.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
        log += "copy failed err=" + std::to_string(GetLastError()) + "\n";
        return false;
    }
    uint32_t a = 0, b = 0;
    if (!FileCrc32(src, a) || !FileCrc32(dst, b) || a != b) {
        log += "verify failed: " + std::string(dst.begin(), dst.end()) + "\n";
        return false;
    }
    log += "rewritten " + std::string(dst.begin(), dst.end()) + "\n";
    return true;
}

std::wstring BackupDirFor(const std::wstring& efiDir, const std::string& tag) {
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    char stamp[32] = {};
    snprintf(stamp, sizeof(stamp), "%04u%02u%02u-%02u%02u%02u", st.wYear,
             st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::wstring name = L"backup-";
    name.append(tag.begin(), tag.end());
    name += L"-" + std::wstring(stamp, stamp + strlen(stamp));
    std::wstring dir = efiDir + L"\\" + name;
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
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

}  // namespace

bool ReadEspRescuePayload(const std::wstring& espRoot, EspRescuePayload& out) {
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
        out.crcKnown = FileCrc32(ir, out.initramfsCrc);
    }
    out.hasKernel = FileExists(dir + L"\\vmlinuz-zjrestore.efi") ||
                    FileExists(dir + L"\\vmlinuz-zjrestore");
    return true;
}

bool BackupEspRescuePayload(const std::wstring& espRoot, const std::string& tag,
                            std::string& log) {
    const std::wstring efiDir = espRoot + L"EFI\\ZJRESTORE";
    const std::wstring bak = BackupDirFor(efiDir, tag);
    bool ok = true;
    unsigned n = 0;

    WIN32_FIND_DATAW fd = {};
    HANDLE hFind = FindFirstFileW((efiDir + L"\\*").c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::wstring src = efiDir + L"\\" + name;
            const std::wstring dst = bak + L"\\" + name;
            SetFileAttributesW(src.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
                ++n;
            } else {
                ok = false;
                log += "backup FAIL " + std::string(name.begin(), name.end()) +
                       " err=" + std::to_string(GetLastError()) + "\n";
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
    log += "backup dir: " + std::string(bak.begin(), bak.end()) + " (" +
           std::to_string(n) + " files)\n";

    // Boot####/BootOrder：固件启动项快照（文本，判定"写入前指向谁"）。
    std::vector<std::string> lines;
    UefiBootOrderSummary(lines);
    std::string txt;
    for (const auto& ln : lines) txt += ln + "\r\n";
    if (!WriteText(bak + L"\\firmware-boot.txt", txt))
        log += "firmware boot snapshot FAIL\n";
    return ok;
}

bool RefreshEspRescuePayload(const std::wstring& espRoot,
                             const std::wstring& assetDir, std::string& log,
                             std::string& msg) {
    EspRescuePayload esp;
    ReadEspRescuePayload(espRoot, esp);

    const std::wstring biIr = assetDir + L"\\bootfiles\\initramfs-zjrestore.cpio.gz";
    unsigned long long biSize = 0;
    uint32_t biCrc = 0;
    const bool biOk = FileSizeOf(biIr, biSize) && FileCrc32(biIr, biCrc);

    const RescuePayloadState state = ClassifyRescuePayload(
        esp.build, SYSRECOVER_VERSION, esp.hasInitramfs || esp.hasKernel,
        biOk && esp.crcKnown, esp.initramfsCrc, biCrc, esp.initramfsSize,
        biSize);

    char head[320] = {};
    snprintf(head, sizeof(head),
             "ESP rescue payload: build=%s initramfs=%llu (builtin=%s %llu)\n",
             esp.build.empty() ? "(none)" : esp.build.c_str(),
             esp.initramfsSize, SYSRECOVER_VERSION, biSize);
    log += head;

    if (DecideRescuePayloadAction(state, true) == RescuePayloadAction::Keep) {
        log += "rescue payload matches builtin (keep)\n";
        return true;
    }

    log += "ESP rescue payload stale -> rewrite\n";
    BackupEspRescuePayload(espRoot, "pre-refresh", log);

    const std::wstring dir = espRoot + L"EFI\\ZJRESTORE";
    bool ok = true;
    if (esp.hasInitramfs)
        ok = CopyAndVerify(biIr, dir + L"\\initramfs-zjrestore.cpio.gz", log) && ok;

    const std::wstring biKernel = assetDir + L"\\bootfiles\\vmlinuz-zjrestore";
    if (FileExists(dir + L"\\vmlinuz-zjrestore.efi"))
        ok = CopyAndVerify(biKernel, dir + L"\\vmlinuz-zjrestore.efi", log) && ok;
    if (FileExists(dir + L"\\vmlinuz-zjrestore"))
        ok = CopyAndVerify(biKernel, dir + L"\\vmlinuz-zjrestore", log) && ok;

    // Secure Boot 链的壳文件只在 ESP 上已存在时刷新（不新建）。
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
        ok = CopyAndVerify(assetDir + c.src, dir + L"\\" + c.dst, log) && ok;
    }
    if (FileExists(dir + L"\\grubx64.efi")) {
        const wchar_t* src = (sb == SbMode::Shim)
                                 ? L"\\bootfiles\\sb\\zjrestore-uki.efi"
                                 : L"\\bootfiles\\sb\\grubx64.efi";
        if (FileExists(assetDir + src))
            ok = CopyAndVerify(assetDir + src, dir + L"\\grubx64.efi", log) && ok;
    }

    // 最后才更新自报版本；写失败即 fail-closed（盘上仍是旧 code 的自报）。
    const std::string vb = std::string(SYSRECOVER_VERSION) + "\n";
    if (!WriteText(dir + L"\\rescue-build.txt", vb)) {
        ok = false;
        log += "write rescue-build.txt FAIL\n";
    }

    if (!ok) {
        msg = RescuePayloadFailMessage(esp.build, SYSRECOVER_VERSION);
        log += msg + "\n";
        log += "refreshed files left in place; rescue payload NOT updated in full\n";
    }
    return ok;
}

}  // namespace sysrecover
