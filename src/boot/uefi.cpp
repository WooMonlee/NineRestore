// UEFI 固件启动项（Boot####）+ BootOrder —— 从 Windows 直接写 NVRAM。
// 依据：UEFI 规范 §3 Boot Manager（Boot#### 的 OptionalData 作为该镜像的
// LoadOptions 传递）；内核 efi-stub 的 efi_convert_cmdline() 按 efi_char16_t*
// 读取，故命令行必须是 UTF-16LE。Rufus 的 EFICreateNewEntry 为同款实现。
#include "../common/i18n.h"
#include "../common/logger.h"
#include "uefi.h"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "bcd.h"
#include "bcd_parse.h"
#include "bootnames.h"
#include "../common/process.h"
#include "../common/relocate.h"  // LogBaseDir()：清理前确认没有待执行的暂存任务
#include "../common/version.h"
#include "../common/zip.h"  // Crc32（部署后校验，A6）
#include "rescue_guard.h"   // N17：ESP 旧载荷自证与"先删后写"无备份替换

// ── MOK/UKI 备选线（PIT-062）──────────────────────────────────────
// 「固件启动项 → shimx64.efi → 我们签名的 UKI」这条路（需要一次性 MOK 注册）
// 已不再是默认：现在默认走 **Canonical 签名链**（PIT-066 变体 D，零注册）。
// 按用户要求：**代码留在仓库里，但默认不编译**，不占用产品体积；日后若
// Canonical 的签名链被收紧，把这里改成 1 重新编译即可启用。
//   相关资产（也在仓库里，不随包分发）：bootfiles/sb/{shimx64.efi, mmx64.efi,
//   fbx64.efi, zj-mok.cer} + 构建脚本 tools/build-uki.py + keys/zj-mok.*
#define ZJ_ENABLE_MOK_PATH 0

namespace sysrecover {
namespace {

// 文件 CRC32 + 部署后校验（A6）：救援文件必须与 bootfiles 源一致 —— 防旧版
// 残留/半截拷贝导致"旧救援配新契约"混用（用户 2026-10-04 要求杜绝）。
bool FileCrc32(const std::wstring& path, uint32_t& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    std::vector<char> buf;
    char tmp[1 << 20];
    DWORD rd = 0;
    while (ReadFile(h, tmp, sizeof(tmp), &rd, nullptr) && rd > 0)
        buf.insert(buf.end(), tmp, tmp + rd);
    CloseHandle(h);
    out = Crc32(buf.data(), buf.size());
    return true;
}

// EFI_GLOBAL_VARIABLE 命名空间。
const wchar_t kGlobalGuid[] = L"{8BE4DF61-93CA-11D2-AA0D-00E098032B8C}";

// 条目描述 = 身份标识（ASCII；固件启动菜单字体未必含中文，PIT-054）。
// 常驻菜单与日常还原临时项必须用不同描述，首启清理只认临时项。
const wchar_t* EntryDesc(bool temporary) {
    return temporary ? kTempEntryDesc : kMenuEntryDesc;
}

const wchar_t kKernelPath[] = L"\\EFI\\ZJRESTORE\\vmlinuz-zjrestore.efi";

// 内核命令行（= Boot#### 的 OptionalData，UTF-16LE）。zjre=1 是我们自己的标记。
const wchar_t kCmdLine[] =
    L"initrd=\\EFI\\ZJRESTORE\\initramfs-zjrestore.cpio.gz console=tty0 "
    L"nvme_core.io_timeout=1 zjre=1";

#pragma pack(push, 1)
struct EfiDirNode {      // 描述符结束节点
    BYTE type;           // 0x7f
    BYTE subType;        // 0xff
    WORD length;         // 4
};
#pragma pack(pop)

typedef BOOL(WINAPI* FnSetFwEx)(LPCWSTR, LPCWSTR, PVOID, DWORD, DWORD);
FnSetFwEx g_setEx = nullptr;

// Win8+ 的 Ex 版可**显式指定变量属性**，我们固定 NON_VOLATILE|BOOTSERVICE|
// RUNTIME(0x7)，保证启动项非易失（重启后仍在）。没有 Ex（Win7）时回退普通版。
void ResolveSetEx() {
    if (g_setEx)
        return;
    HMODULE h = GetModuleHandleW(L"kernel32.dll");
    if (!h)
        h = LoadLibraryW(L"kernel32.dll");
    if (!h)
        return;
    void* p = reinterpret_cast<void*>(
        GetProcAddress(h, "SetFirmwareEnvironmentVariableExW"));
    g_setEx = reinterpret_cast<FnSetFwEx>(p);
}

BOOL SetVarW(LPCWSTR name, LPCWSTR guid, PVOID data, DWORD size) {
    ResolveSetEx();
    SetLastError(0);
    if (g_setEx)
        return g_setEx(name, guid, data, size, 0x7);
    return SetFirmwareEnvironmentVariableW(name, guid, data, size);
}

// 删固件变量（= Set 一个 0 长度的值）。
bool DeleteVarW(const std::wstring& name) {
    SetLastError(0);
    return SetVarW(name.c_str(), kGlobalGuid, nullptr, 0) != FALSE;
}

// Boot#### 的 HD(...) 节点是不是指向"这块 ESP"（起止 LBA + 分区 GUID 三者全等）。
// 用于安装/升级去重与失败回滚：只删"同一块盘同一分区"的多余条目，别的硬盘/
// 别的 ESP 上的同类条目一律不碰。判据本体在 uefi.h::UefiEntryTargetsEsp（可单测）。
bool OptionHdMatches(const std::vector<BYTE>& b, const GUID& sig,
                     unsigned long long partStart, unsigned long long partSize) {
    return UefiEntryTargetsEsp(b, sig, partStart, partSize);
}

// 写固件变量需要 SeSystemEnvironmentPrivilege（管理员默认持有但**未启用**）。
bool EnableEnvPrivilege(std::string& log) {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        log += "OpenProcessToken failed\n";
        return false;
    }
    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, SE_SYSTEM_ENVIRONMENT_NAME, &luid)) {
        log += "LookupPrivilegeValue(SeSystemEnvironment) failed\n";
        CloseHandle(tok);
        return false;
    }
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    SetLastError(0);
    BOOL ok = AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    DWORD err = GetLastError();
    CloseHandle(tok);
    if (!ok || err == ERROR_NOT_ALL_ASSIGNED) {
        log += "AdjustTokenPrivileges(SeSystemEnvironment) failed err=" +
               std::to_string(err) + "\n";
        return false;
    }
    return true;
}

std::wstring VarName(int num) {
    wchar_t n[10];
    swprintf(n, 10, L"Boot%04X", num);
    return n;
}

bool ReadVar(const std::wstring& name, std::vector<BYTE>& out) {
    DWORD cap = 8192;
    out.resize(cap);
    SetLastError(0);
    DWORD len = GetFirmwareEnvironmentVariableW(name.c_str(), kGlobalGuid, out.data(), cap);
    if (len == 0) {
        out.clear();
        return false;
    }
    out.resize(len);
    return true;
}

// 解析 EFI_LOAD_OPTION：返回描述与 FilePathList（供判归属）。
bool ParseOption(const std::vector<BYTE>& b, std::wstring& desc,
                 std::wstring& devPath) {
    if (b.size() < 8)
        return false;
    WORD fpll = 0;
    memcpy(&fpll, &b[4], 2);
    size_t off = 6;
    std::wstring d;
    while (off + 2 <= b.size()) {
        wchar_t c;
        memcpy(&c, &b[off], 2);
        off += 2;
        if (c == 0)
            break;
        d.push_back(c);
    }
    if (off + fpll > b.size())
        return false;
    // 设备路径里可打印的 ASCII 足够判 ZJRESTORE 归属
    std::wstring p;
    for (size_t i = off; i < off + fpll && i + 1 < b.size(); ++i) {
        wchar_t c = (wchar_t)b[i];
        if (c >= 32 && c < 127)
            p.push_back(c);
    }
    desc = d;
    devPath = p;
    return true;
}

// PIT-216：固件里的路径**大小写随机器**（实测这台机是
// \EFI\MICROSOFT\BOOT\BOOTMGFW.EFI 全大写）—— 所有路径比较必须忽略大小写，
// 否则"已经有 Windows Boot Manager"会被判成"没有"，于是又新建一条，
// 而且新建后校验也失败（见下），最终 BootOrder 没被置顶。
bool ContainsNoCase(const std::wstring& hay, const wchar_t* needle) {
    if (!needle || !*needle) return false;
    const std::wstring n = needle;
    auto it = std::search(
        hay.begin(), hay.end(), n.begin(), n.end(),
        [](wchar_t a, wchar_t b) { return towupper(a) == towupper(b); });
    return it != hay.end();
}

namespace {
// 条目种类：1 = 常驻菜单，2 = 一次性临时项，0 = 不是我们认识的名字。
// PIT-246：旧版本生成过 SysRecover / SysRecovery Environment /
// "NineRestore Recovery Environment" 等描述；升级时必须识别成"同一份载荷"，
// 否则 FindOurEntry 找不到旧项 → 新建一条 → 固件菜单里同时出现新旧两个名字。
int EntryKindOf(const std::wstring& desc) {
    if (desc == kMenuEntryDesc) return 1;
    if (desc == kTempEntryDesc) return 2;
    for (const wchar_t* d : kLegacyPermanentDescs)
        if (desc == d) return 1;
    for (const wchar_t* d : kLegacyTempDescs)
        if (desc == d) return 2;
    return 0;
}

bool IsOurs(const std::vector<BYTE>& b, int kind) {
    std::wstring desc, dev;
    if (!ParseOption(b, desc, dev))
        return false;
    // 必须同时满足：描述是我们（含历史品牌）的，且设备路径指向我们的目录。
    // 只按描述名匹配会误伤用户自建的同名项；只按路径匹配会把临时项当常驻项。
    return EntryKindOf(desc) == kind && ContainsNoCase(dev, L"ZJRESTORE");
}
}  // namespace

// 找我们已存在的条目；找到返回条目号，否则 -1。
// 同名（当前品牌）优先；找不到再回退到历史品牌名，保证升级路径只更新不新增。
int FindOurEntry(bool temporary) {
    const int kind = temporary ? 2 : 1;
    const wchar_t* expect = EntryDesc(temporary);
    std::vector<BYTE> order;
    if (!ReadVar(L"BootOrder", order))
        return -1;
    int legacy = -1;
    int n = (int)(order.size() / 2);
    for (int i = 0; i < n; ++i) {
        WORD v = 0;
        memcpy(&v, &order[i * 2], 2);
        std::vector<BYTE> b;
        if (!ReadVar(VarName(v), b))
            continue;
        if (!IsOurs(b, kind))
            continue;
        std::wstring desc, dev;
        if (ParseOption(b, desc, dev) && desc == expect)
            return v;  // 当前品牌名：直接复用（描述会被重写成最新名字）
        if (legacy < 0)
            legacy = v;  // 历史品牌名：升级用，装完后再清理重复项
    }
    return legacy;
}

namespace {
// 删除指定 Boot####：先从 BootOrder 摘掉，再删变量，最后清 BootNext。
// （原 RemoveUefiBootEntry 的 1/2/3 步；抽出来供"去重"复用）
// 返回 true = 回读确认 BootOrder 不再列出它、且 Boot#### 变量确实读不到了。
// PIT-250：只看 SetVarW 返回值不够 —— 安全软件可能"返回成功但写入被吞"。
bool RemoveEntryNum(int num) {
    std::vector<BYTE> order;
    bool orderOk = false;
    if (ReadVar(L"BootOrder", order)) {
        std::vector<WORD> list;
        for (size_t i = 0; i + 1 < order.size(); i += 2) {
            WORD v = 0;
            memcpy(&v, &order[i], 2);
            if (v != (WORD)num)
                list.push_back(v);
        }
        std::vector<BYTE> nb(list.size() * 2);
        for (size_t i = 0; i < list.size(); ++i)
            memcpy(&nb[i * 2], &list[i], 2);
        orderOk = SetVarW(L"BootOrder", kGlobalGuid, nb.data(),
                          (DWORD)nb.size()) != FALSE;
    }
    bool optOk =
        SetVarW(VarName(num).c_str(), kGlobalGuid, nullptr, 0) != FALSE;
    // 回读①：Boot#### 变量必须真的消失。
    std::vector<BYTE> chk;
    if (ReadVar(VarName(num), chk))
        optOk = false;
    // 回读②：BootOrder 里也不能再有这个号（否则留下悬空引用）。
    std::vector<BYTE> ord2;
    if (ReadVar(L"BootOrder", ord2)) {
        orderOk = true;
        for (size_t i = 0; i + 1 < ord2.size(); i += 2) {
            WORD v = 0;
            memcpy(&v, &ord2[i], 2);
            if (v == (WORD)num) {
                orderOk = false;
                break;
            }
        }
    } else {
        orderOk = false;
    }
    WORD next = 0;
    DWORD len = GetFirmwareEnvironmentVariableW(L"BootNext", kGlobalGuid, &next,
                                                sizeof(next));
    if (len == sizeof(next) && next == (WORD)num)
        SetVarW(L"BootNext", kGlobalGuid, nullptr, 0);
    return orderOk && optOk;
}

// 只删"当前这块 ESP"上的同种类条目（安装/升级失败回滚用）。
// PIT-251（2026-11-08）：安装/升级的失败清理原先走 RemoveUefiBootEntry 的
// "全盘按描述+路径"删除 —— 双 ESP 机器上会顺带摘掉另一块 ESP 的可用菜单。
// 判据与去重一致：HD 节点起止 LBA + GPT 分区 GUID 三者全等才算同一 ESP。
bool RemoveOurEntriesOnEsp(const GUID& sig, unsigned long long start,
                           unsigned long long size, bool temporary,
                           std::string& log) {
    const int kind = temporary ? 2 : 1;
    bool allOk = true;
    int removed = 0;
    for (int guard = 0; guard < 8; ++guard) {
        std::vector<BYTE> order;
        if (!ReadVar(L"BootOrder", order))
            break;
        int hit = -1;
        for (size_t i = 0; i + 1 < order.size(); i += 2) {
            WORD v = 0;
            memcpy(&v, &order[i], 2);
            std::vector<BYTE> b;
            if (ReadVar(VarName(v), b) && IsOurs(b, kind) &&
                OptionHdMatches(b, sig, start, size)) {
                hit = (int)v;
                break;
            }
        }
        if (hit < 0)
            break;
        if (!RemoveEntryNum(hit))
            allOk = false;
        ++removed;
    }
    if (removed > 0) {
        log += "firmware boot entry removed on this ESP x" +
               std::to_string(removed) + "\n";
    }
    return allOk;
}
}  // namespace

int FindFreeEntry() {
    BYTE tmp[8];
    for (int i = 0; i <= 0xFFFF; ++i) {
        SetLastError(0);
        if (GetFirmwareEnvironmentVariableW(VarName(i).c_str(), kGlobalGuid, tmp, sizeof(tmp)) == 0 &&
            GetLastError() == ERROR_ENVVAR_NOT_FOUND)
            return i;
    }
    return -1;
}

// ESP 卷的 GPT 信息（构造短格式 HardDrive 节点用）。
bool QueryEspGpt(const std::wstring& espRoot, GUID& sig, DWORD& partNum,
                 unsigned long long& start, unsigned long long& size,
                 std::string& log) {
    std::wstring dev = L"\\\\.\\" + espRoot.substr(0, 2);
    HANDLE h = CreateFileW(dev.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        log += "open ESP volume failed err=" + std::to_string(GetLastError()) +
               "\n";
        return false;
    }
    PARTITION_INFORMATION_EX pi{};
    DWORD ret = 0;
    if (!DeviceIoControl(h, IOCTL_DISK_GET_PARTITION_INFO_EX, nullptr, 0, &pi,
                         sizeof(pi), &ret, nullptr)) {
        log += "IOCTL_DISK_GET_PARTITION_INFO_EX failed err=" +
               std::to_string(GetLastError()) + "\n";
        CloseHandle(h);
        return false;
    }
    if (pi.PartitionStyle != PARTITION_STYLE_GPT) {
        log += "ESP is not a GPT partition\n";
        CloseHandle(h);
        return false;
    }
    DWORD bps = 512;
    DISK_GEOMETRY_EX geo{};
    if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &geo,
                        sizeof(geo), &ret, nullptr) &&
        geo.Geometry.BytesPerSector)
        bps = geo.Geometry.BytesPerSector;
    CloseHandle(h);
    sig = pi.Gpt.PartitionId;
    partNum = pi.PartitionNumber;
    start = pi.StartingOffset.QuadPart / bps;
    size = pi.PartitionLength.QuadPart / bps;
    return true;
}

bool CopyOne(const std::wstring& src, const std::wstring& dst,
             std::string& log) {
    // 直接写（调用方已先删旧载荷；不备份、不写临时文件、不改名 —— 用户
    // 2026-11-07 口径）。这里只清属性，保证能创建/覆盖目标文件。
    SetFileAttributesW(dst.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (CopyFileW(src.c_str(), dst.c_str(), FALSE))
        return true;
    char s[512] = {};
    WideCharToMultiByte(CP_UTF8, 0, dst.c_str(), -1, s, sizeof(s), nullptr,
                        nullptr);
    log += std::string("copy ") + s + " failed err=" +
           std::to_string(GetLastError()) + "\n";
    return false;
}

// 源文件大小（不存在返回 0），供 ESP 空间检查用。
unsigned long long FileSize(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    CloseHandle(h);
    return (unsigned long long)sz.QuadPart;
}

// 文件/目录是否存在。
bool PathExists(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool DirExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// 宽 → UTF-8（bcd_parse 的判定用的是窄串）。
std::string Narrow(const std::wstring& w) {
    if (w.empty())
        return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        return std::string();
    std::string s((size_t)n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr,
                        nullptr);
    return s;
}

// 目录里是否存在匹配 pattern 的文件（如 Fonts\*_boot.ttf）。
bool HasFileMatching(const std::wstring& dir, const wchar_t* pattern) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    FindClose(h);
    return true;
}

// 递归删除文件/目录（先清属性，否则只读/隐藏的删不掉）。
bool RemoveTree(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES)
        return true;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        return DeleteFileW(path.c_str()) != 0;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((path + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 ||
                wcscmp(fd.cFileName, L"..") == 0)
                continue;
            RemoveTree(path + L"\\" + fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    return RemoveDirectoryW(path.c_str()) != 0;
}

}  // namespace

// P6：读固件 db，看它信任哪张微软 UEFI CA（只做字符串扫描 —— 证书的 CN 在 DER 里
// 是明文）。**必须定义在匿名命名空间之外**（要对外链接给 CLI 的 diag 用）。
int FirmwareTrustedUefiCas() {
    std::string dummy;
    EnableEnvPrivilege(dummy);  // 读固件变量需要 SeSystemEnvironmentPrivilege
    std::vector<BYTE> buf(64 * 1024);
    SetLastError(0);
    DWORD len = GetFirmwareEnvironmentVariableW(
        L"db", kGlobalGuid, buf.data(), static_cast<DWORD>(buf.size()));
    if (len == 0)
        return 0;  // 读不到（非 UEFI / 无权限 / 固件不给）
    std::string s(reinterpret_cast<const char*>(buf.data()), len);
    int mask = 0;
    if (s.find("Microsoft Corporation UEFI CA 2011") != std::string::npos)
        mask |= kFirmwareCa2011;
    if (s.find("Microsoft UEFI CA 2023") != std::string::npos ||
        s.find("Windows UEFI CA 2023") != std::string::npos)
        mask |= kFirmwareCa2023;
    return mask;
}

static bool BuildAndWrite(int num, const GUID& sig, DWORD partNum,
                          unsigned long long start, unsigned long long size,
                          const std::wstring& targetPath, const wchar_t* cmdLine,
                          const wchar_t* entryDesc, std::string& log) {
    UefiHdNode hd{};
    hd.type = 4;
    hd.subType = 1;
    hd.length = sizeof(UefiHdNode);
    hd.partNumber = partNum;
    hd.partStart = start;
    hd.partSize = size;
    hd.signature = sig;
    hd.mbrType = 2;
    hd.sigType = 2;

    size_t klen = targetPath.size();
    WORD fpLen = (WORD)(4 + (klen + 1) * 2);
    std::vector<BYTE> fp(fpLen);
    fp[0] = 4;  // Media
    fp[1] = 4;  // FilePath
    memcpy(&fp[2], &fpLen, 2);
    memcpy(&fp[4], targetPath.c_str(), (klen + 1) * 2);

    EfiDirNode end{0x7f, 0xff, 4};

    DWORD fpll =
        (DWORD)(sizeof(UefiHdNode) + fpLen + sizeof(EfiDirNode));
    size_t descBytes = (wcslen(entryDesc) + 1) * 2;
    size_t cmdBytes = cmdLine ? (wcslen(cmdLine) + 1) * 2 : 0;

    std::vector<BYTE> buf(6 + descBytes + fpll + cmdBytes);
    DWORD attr = 1;  // LOAD_OPTION_ACTIVE
    memcpy(&buf[0], &attr, 4);
    memcpy(&buf[4], &fpll, 2);
    size_t off = 6;
    memcpy(&buf[off], entryDesc, descBytes);
    off += descBytes;
    memcpy(&buf[off], &hd, sizeof(hd));
    off += sizeof(hd);
    memcpy(&buf[off], fp.data(), fpLen);
    off += fpLen;
    memcpy(&buf[off], &end, sizeof(end));
    off += sizeof(end);
    if (cmdBytes)
        memcpy(&buf[off], cmdLine, cmdBytes);

    std::wstring name = VarName(num);
    SetLastError(0);
    if (!SetVarW(name.c_str(), kGlobalGuid, buf.data(), (DWORD)buf.size())) {
        log += "write " + std::string(name.begin(), name.end()) +
               " failed err=" + std::to_string(GetLastError()) + "\n";
        return false;
    }
    // 回读校验：**用我们自己写进去的 targetPath 比**（大小写不敏感）。
    // 不能再用 IsOurs（它写死要求路径含 ZJRESTORE）—— 这条路径也用来建
    // "Windows Boot Manager"条目，用 ZJRESTORE 校验必然失败（PIT-216）。
    std::vector<BYTE> back;
    bool verified = false;
    if (ReadVar(name, back)) {
        std::wstring d2, dev2;
        if (ParseOption(back, d2, dev2))
            verified = (d2 == entryDesc) && ContainsNoCase(dev2, targetPath.c_str());
    }
    if (!verified) {
        log += "verify entry failed\n";
        return false;
    }
    return true;
}

bool InstallUefiBootEntry(const std::wstring& espRoot,
                          const std::wstring& exeDir, std::string& log,
                          bool temporary) {
    if (!EnableEnvPrivilege(log))
        return false;

    // 0) 清掉同类旧残留（只清本次这一类）：BCD 里的 bootapp 条目
    //    + 指向它的 bootsequence。临时项绝不能删常驻菜单的 GUID。
    {
        std::string out;
        const wchar_t* guid = temporary ? TemporaryGuid() : RecoveryGuid();
        RunProcess(SysToolPath(L"bcdedit.exe"),
                   L"/delete " + std::wstring(guid) + L" /f", out);
        RunProcess(SysToolPath(L"bcdedit.exe"),
                   L"/deletevalue {bootmgr} bootsequence", out);
    }

    // 1) 先确定"这块 ESP"的身份：安装/升级的去重、失败回滚共用同一判据
    //   （起止 LBA + GPT 分区 GUID），不再按"描述 + 路径"跨 ESP 误删。
    GUID espSig{};
    DWORD espPart = 0;
    unsigned long long espStart = 0, espSize = 0;
    const bool espIdOk =
        QueryEspGpt(espRoot, espSig, espPart, espStart, espSize, log);
    auto removeOurEntries = [&](bool temp) {
        if (espIdOk)
            return RemoveOurEntriesOnEsp(espSig, espStart, espSize, temp, log);
        // 拿不到 GPT 身份时退回旧的全盘删除（0.9/1.0 RC2 行为，保底）。
        log += "ESP identity unavailable: removing our entries globally\n";
        return RemoveUefiBootEntry(log, temp);
    };

    // 2) 救援文件 → ESP 的 EFI\ZJRESTORE 目录（注意：C++ 注释行尾不能是反斜杠，
    //    否则会行继续、把下一行代码吞掉）
    std::wstring efiDir = espRoot + L"EFI";
    std::wstring dir = efiDir + L"\\ZJRESTORE";
    CreateDirectoryW(efiDir.c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    // （B，2026-10-18）这里原先 RemoveTree(dir) 全清再全拷，每次安装/暂存都把
    // ~28MB 载荷重写一遍（慢盘上拖出"无响应"窗）。现在改为：文件集构建后按
    // "定点清 stale + keep 未变化"处理，见下面的 cleanup / changed 逻辑。

    // N17（2026-11-07）：先读 ESP 上救援环境**自报**的 build 与 initramfs 大小，
    // 与内置载荷比对。0.9.92 现场就是"程序说装好了，盘上跑的是旧码"：旧 dist 的
    // 源载荷与旧盘上文件相同 → keep，没人发现。这里把"到底是哪版"变成日志里的
    // 确定事实。判定走**快路径**（版本戳 + 大小），不读 17.4MB 本体算哈希；
    // 写后才做 CRC 深查（用户 2026-11-07：检测不必每次全读）。
    const ULONGLONG tDetect = GetTickCount64();
    EspRescuePayload prior;
    ReadEspRescuePayload(espRoot, prior);  // 快路径：deepCrc 默认 false
    const std::wstring builtinIr =
        exeDir + L"\\bootfiles\\initramfs-zjrestore.cpio.gz";
    const unsigned long long builtinIrSize = FileSize(builtinIr);
    const RescuePayloadState priorState = ClassifyRescuePayload(
        prior.build, SYSRECOVER_VERSION, prior.hasInitramfs || prior.hasKernel,
        /*espCrcKnown=*/false, 0u, 0u, prior.initramfsSize, builtinIrSize);
    {
        char lb[320] = {};
        snprintf(lb, sizeof(lb),
                 "ESP rescue payload: build=%s initramfs=%llu "
                 "(builtin=%s %llu)\n",
                 prior.build.empty() ? "(none)" : prior.build.c_str(),
                 prior.initramfsSize, SYSRECOVER_VERSION, builtinIrSize);
        log += lb;
    }
    // ESP 空间寸土寸金（用户 2026-11-07）：**不备份旧载荷**。历史遗留的
    // `backup-*` 目录在这里清掉（日志写清单与释放字节），再写一份几 KB 的固件
    // 启动序快照。旧载荷"删旧 → 写新"在下面文件集确定后执行（顺序契约见
    // rescue_guard.h::RescuePayloadDeploySteps）。
    CleanupLegacyRescueBackups(espRoot, log);
    if (RescuePayloadMustNotBeReused(priorState)) {
        log += "ESP rescue payload stale -> remove old, then write new (no backup)\n";
        WriteFirmwareBootSnapshot(espRoot, log);
    }

    // Secure Boot 开着时，固件只加载**微软签名**的镜像 → 我们的内核不能直接当
    // 启动目标。两种走法（PIT-063 对比实验，ZJ_SB_MODE 切换）：
    //   shim    ：固件启动项 → shimx64.efi（微软签名）→ grubx64.efi(= 我们签名的
    //             UKI)。标准机制，但首次要在 MokManager 注册一次公钥。
    //   bootapp ：BCD bootapp → efiloader.efi → 我们的 UKI。零用户交互，但依赖
    //             `nointegritychecks`（关掉 bootmgr 的完整性校验）。
    SbMode mode = CurrentSbMode();
    std::string mlog = std::string("secure boot mode: ") + SbModeName(mode);
    mlog += "\n";
    log += mlog;

    std::wstring targetPath = kKernelPath;
    const wchar_t* cmdLine = kCmdLine;
    // N8（2026-11-07）：命令行带上入口类型，救援层据此区分 menu / temp，diag 可读。
    std::wstring cmdWithKind;
    if (cmdLine) {
        cmdWithKind = cmdLine;
        cmdWithKind += temporary ? L" zjentry=temp" : L" zjentry=menu";
        cmdLine = cmdWithKind.c_str();
    }
    const wchar_t* entryDesc = EntryDesc(temporary);
    bool useFirmwareEntry = true;

    // 要写进 ESP 的文件（exeDir 相对路径 → ESP 里的文件名）
    std::vector<std::pair<std::wstring, std::wstring>> files;
#if ZJ_ENABLE_MOK_PATH
    if (mode == SbMode::Shim) {
        files = {
            {L"\\bootfiles\\sb\\shimx64.efi", L"shimx64.efi"},
            {L"\\bootfiles\\sb\\mmx64.efi", L"mmx64.efi"},
            {L"\\bootfiles\\sb\\fbx64.efi", L"fbx64.efi"},
            {L"\\bootfiles\\sb\\zjrestore-uki.efi", L"grubx64.efi"},
            {L"\\bootfiles\\sb\\zj-mok.cer", L"zj-mok.cer"},
        };
        // shim 的默认二级就是同目录的 grubx64.efi；命令行在 UKI 里，无需 OptionalData
        targetPath = L"\\EFI\\ZJRESTORE\\shimx64.efi";
        cmdLine = nullptr;
    } else
#endif
    if (mode == SbMode::Grub) {
        // shim（**微软双签：CA2011 + CA2023**）→ Debian 签名的 GRUB（shim 内嵌同一把
        // 证书，**无需 MOK 注册**）→ grub.cfg 用普通 `linux`/`initrd` 加载我们的内核。
        // 2026-09-23 换链（备选 B）：Ubuntu → Debian。原因：Ubuntu 的 shim 目前只带
        // CA2011 单签，**2026 新出厂只信 CA2023 的固件起不来**；Debian 的是双签。
        // 不再需要 mmx64/fbx64 —— 那是 MOK 备选线的资产（本项目 grub 模式用不到）。
        files = {
            {L"\\bootfiles\\sb\\shimx64.efi", L"shimx64.efi"},
            {L"\\bootfiles\\sb\\grubx64.efi", L"grubx64.efi"},
            {L"\\bootfiles\\sb\\grub.cfg", L"grub.cfg"},
            {L"\\bootfiles\\vmlinuz-zjrestore", L"vmlinuz-zjrestore"},
            {L"\\bootfiles\\initramfs-zjrestore.cpio.gz",
             L"initramfs-zjrestore.cpio.gz"},
        };
        targetPath = L"\\EFI\\ZJRESTORE\\shimx64.efi";
        cmdLine = nullptr;
    } else if (mode == SbMode::Bootapp) {
        // bootmgr 加载 efiloader，efiloader 再加载我们的 UKI（命令行/initrd 都在
        // UKI 自己的节里）。不需要固件启动项。
        files = {
            {L"\\bootfiles\\sb\\efiloader.efi", L"efiloader.efi"},
            {L"\\bootfiles\\sb\\zjrestore-uki.efi", L"zjrestore-uki.efi"},
        };
        useFirmwareEntry = false;
    } else {
        files = {
            {L"\\bootfiles\\vmlinuz-zjrestore",
             L"vmlinuz-zjrestore.efi"},
            {L"\\bootfiles\\initramfs-zjrestore.cpio.gz",
             L"initramfs-zjrestore.cpio.gz"},
        };
        // PIT-195（2026-10-31）：**暂存**用的启动项改指一份独立的内核副本
        // `vmlinuz-zjtemp.efi`。原因：单次启动一旦把启动项置顶（不依赖固件是否
        // 认 BootNext），万一救援层跑不起来就必须能"自己失效"，否则每次开机都
        // 往救援里撞 = 死循环。救援层重启前删掉这份副本即可——常驻菜单用的
        // `vmlinuz-zjrestore.efi` 不受影响。
        if (temporary) {
            files.push_back({L"\\bootfiles\\vmlinuz-zjrestore",
                             L"vmlinuz-zjtemp.efi"});
            targetPath = L"\\EFI\\ZJRESTORE\\vmlinuz-zjtemp.efi";
        }
    }

    // 定点清 stale：不同 Secure Boot 走法部署的文件集不同（直启内核 / shim 一套 /
    // bootapp 的 UKI + efiloader），换走法时不清理会越装越多、把 ESP 塞满
    //（实测 "ESP space: need 49.3MB, free 31.9MB"）。原先直接 RemoveTree 全清；
    // 现在只删"不属于本次文件集"的条目 —— 上次装过的文件本次可 keep 复用。
    // payloadRemoved：本函数是否已删过 ESP 上的救援载荷（失败时要摘启动项）。
    bool payloadRemoved = false;
    {
        std::vector<std::wstring> keepNames;
        for (const auto& f : files)
            keepNames.push_back(f.second);
        keepNames.push_back(L"rescue-build.txt");
        keepNames.push_back(L"firmware-boot.txt");  // NVRAM 启动序快照（KB 级）
        // `backup-*` 已由 CleanupLegacyRescueBackups 全量清掉（不再保留载荷备份）；
        // 这里只删"不属于本次文件集"的条目。`.pre.bak`（写前 NVRAM 原始值）保留。
        WIN32_FIND_DATAW fd;
        HANDLE hFind = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                std::wstring name = fd.cFileName;
                if (name == L"." || name == L"..")
                    continue;
                bool keep = false;
                for (const auto& k : keepNames)
                    if (name == k) {
                        keep = true;
                        break;
                    }
                if (!keep && !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                    name.size() > 8 &&
                    name.compare(name.size() - 8, 8, L".pre.bak") == 0)
                    keep = true;  // Boot####.pre.bak / bootorder.pre.bak
                if (keep)
                    continue;
                std::wstring full = dir + L"\\" + name;
                std::string an(name.begin(), name.end());
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    if (RemoveTree(full)) {
                        log += "stale dir removed: " + an + "\n";
                        payloadRemoved = true;
                    } else {
                        log += "stale dir remove FAIL: " + an + "\n";
                    }
                } else {
                    SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
                    if (DeleteFileW(full.c_str())) {
                        log += "stale removed: " + an + "\n";
                        payloadRemoved = true;
                    } else {
                        log += "stale remove FAIL: " + an + " (err=" +
                               std::to_string(GetLastError()) + ")\n";
                    }
                }
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }
    }

    // B（2026-10-18）：逐文件决定 keep（内容与源一致 → 跳过写盘）还是 todo（要
    // 复制）。源 CRC 在这里算一遍并存起来，复制后校验直接复用 → 源只读一次。
    // keep 判定本身就是"源 vs 落盘"的完整比对，等价于原先的复制后校验且更强
    //（连副本都不用重读）。
    struct DeployItem {
        std::wstring src, dst;
        bool needCopy = false;
        unsigned long long srcSize = 0;
        unsigned long long dstSize = 0;
    };
    std::vector<DeployItem> items;
    std::vector<uint32_t> srcCrc(files.size(), 0);
    std::vector<char> srcOk(files.size(), 0);
    unsigned long long needFull = 0;  // 先删后写：要写入的总量
    unsigned long long reclaim = 0;   // 删旧文件可退回的字节数
    for (size_t i = 0; i < files.size(); ++i) {
        std::wstring src = exeDir + files[i].first;
        std::wstring dst = dir + L"\\" + files[i].second;
        srcOk[i] = FileCrc32(src, srcCrc[i]) ? 1 : 0;
        uint32_t dc = 0;
        bool same = srcOk[i] && FileCrc32(dst, dc) && dc == srcCrc[i];
        std::string an(files[i].second.begin(), files[i].second.end());
        if (same) {
            log += "keep " + an + " (unchanged)\n";
            items.push_back({src, dst, false, 0, 0});
        } else {
            const unsigned long long ssz = FileSize(src);
            unsigned long long dsz = 0;
            WIN32_FILE_ATTRIBUTE_DATA fad = {};
            if (GetFileAttributesExW(dst.c_str(), GetFileExInfoStandard, &fad))
                dsz = ((unsigned long long)fad.nFileSizeHigh << 32) |
                      (unsigned long long)fad.nFileSizeLow;
            // 先删后写（用户 2026-11-07）：旧文件腾出的空间可立即用于新文件，
            // 所以按"全量"算 need，同时把 reclaim（旧文件大小）计入可用空间。
            needFull += ssz;
            reclaim += dsz;
            items.push_back({src, dst, true, ssz, dsz});
        }
    }

    // 空间检查：ESP 一般只有 100MB，Windows 自己的引导文件已占 ~40MB，而我们要塞
    // ~52MB（内核+initramfs，或签名版 UKI + shim）。装不下时**提前算清并报错**，
    // 而不是删掉旧载荷、写到一半失败。可用空间 = free + reclaim（先删后写）。
    // 余量 16MB（2026-09-29 由 1MB 提高）：`bcdboot` 刷新该分区上的
    // `EFI\Microsoft\Boot` 目录（bootmgfw 1.5MB + bootmgr.efi 1.5MB + Fonts
    // 4.5MB + Resources/MUI + BCD）实测约 8.6MB；留 16MB 才能保证"我们装完之后
    // bcdboot 还写得进"（docs/15）。
    ULARGE_INTEGER freeBytes{};
    if (needFull > 0 && GetDiskFreeSpaceExW(espRoot.c_str(), &freeBytes, nullptr,
                                            nullptr)) {
        const unsigned long long kMargin = 16ull << 20;  // 16MB：给 bcdboot 留位
        const unsigned long long netNeed =
            needFull > reclaim ? needFull - reclaim : 0;
        char line[320];
        snprintf(line, sizeof(line),
                 "ESP space: need(full) %.1f MB - reclaim %.1f MB (net %.1f MB)"
                 " + margin %.0f MB, free %.1f MB\n",
                 needFull / 1048576.0, reclaim / 1048576.0,
                 netNeed / 1048576.0, kMargin / 1048576.0,
                 freeBytes.QuadPart / 1048576.0);
        log += line;
        if (freeBytes.QuadPart + reclaim < needFull + kMargin) {
            // 文案把"净增量"和"给 bcdboot 的 16MB 余量"都写清，别再出现
            // "需要 29.5 / 可用 29.4"这种看起来只差 0.1MB 的误导。
            // L1：数值先格式化到小缓冲，再用固定文本拼接（译文不再当格式串）。
            char needM[32], marginM[32], freeM[32];
            snprintf(needM, sizeof(needM), "%.1f", netNeed / 1048576.0);
            snprintf(marginM, sizeof(marginM), "%.0f", kMargin / 1048576.0);
            snprintf(freeM, sizeof(freeM), "%.1f",
                     freeBytes.QuadPart / 1048576.0);
            std::string msg = Tr("ESP 空间不足：还需 ");
            msg += needM;
            msg += Tr("MB，另需预留 ");
            msg += marginM;
            msg += Tr("MB 给 bcdboot，可用 ");
            msg += freeM;
            msg += Tr("MB。请清理 ESP 上的其它文件，或换用更小的 initramfs。\n");
            snprintf(line, sizeof(line), "%s", msg.c_str());
            log += line;
            // 写满/只读时也要让用户看到"旧 vs 新"两个版本，而不是只有一串 MB 数字：
            // 与 CopyOne/verify 失败分支保持同一 fail-closed 文案。
            if (RescuePayloadMustNotBeReused(priorState))
                log += RescuePayloadFailMessage(prior.build, SYSRECOVER_VERSION) +
                       "\n";
            // 若上面的定点清理已经删过旧载荷文件，此刻 ESP 上已不完整 →
            // 不能留下指向残骸的启动项（用户 2026-11-07 要求）。
            if (MustRemoveBootEntryAfterFailure(payloadRemoved))
                removeOurEntries(temporary);
            log += RescuePayloadWriteFailedMessage() + "\n";
            return false;
        }
    }

    // 顺序契约：detect（上面已做）→ remove-old → write-new → verify → install-entry。
    // 先删旧载荷腾空间（旧载荷本身有问题，不备份、不用临时文件）；删不掉就停手，
    // 此时旧文件仍在盘上，不算残骸。
    for (const auto& it : items) {
        if (!it.needCopy || it.dstSize == 0)
            continue;
        SetFileAttributesW(it.dst.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (!DeleteFileW(it.dst.c_str())) {
            log += "remove old payload FAIL: " +
                   std::string(it.dst.begin(), it.dst.end()) +
                   " err=" + std::to_string(GetLastError()) + "\n";
            break;  // 删不掉就停手：旧文件仍在，后续不再写
        }
        payloadRemoved = true;
        char rb[96];
        snprintf(rb, sizeof(rb), "old payload removed: %.1f MB\n",
                 it.dstSize / 1048576.0);
        log += rb;
    }
    bool copied = true;
    for (const auto& it : items) {
        if (!it.needCopy)
            continue;
        if (!CopyOne(it.src, it.dst, log)) {
            copied = false;
            break;
        }
    }
    if (!copied) {
        log += "write UEFI rescue files FAIL\n";
        log += RescuePayloadWriteFailedMessage() + "\n";
        // 旧载荷已删 → 不能留下指向残骸的启动项（用户 2026-11-07 要求）。
        if (MustRemoveBootEntryAfterFailure(payloadRemoved))
            removeOurEntries(temporary);
        if (RescuePayloadMustNotBeReused(priorState))
            log += RescuePayloadFailMessage(prior.build, SYSRECOVER_VERSION) +
                   "\n";
        return false;
    }
    // A6：部署后逐文件 CRC 校验（源 vs ESP 落盘），不一致即失败；并写明文构建戳。
    // 源 CRC 上面已算过（srcOk/srcCrc）→ 这里目标只读回一次；源当时读不出才重读
    // 源。keep 的文件已比对过 CRC，无需再验。
    for (size_t i = 0; i < items.size(); ++i) {
        if (!items[i].needCopy)
            continue;
        uint32_t a = srcCrc[i], b = 0;
        bool srcReadable = srcOk[i] || FileCrc32(items[i].src, a);
        if (!srcReadable || !FileCrc32(items[i].dst, b) || a != b) {
            log += "verify FAIL " +
                   std::string(items[i].dst.begin(), items[i].dst.end()) + "\n";
            log += "verify deployed rescue FAIL\n";
            log += RescuePayloadWriteFailedMessage() + "\n";
            if (MustRemoveBootEntryAfterFailure(payloadRemoved))
                removeOurEntries(temporary);
            if (RescuePayloadMustNotBeReused(priorState))
                log += RescuePayloadFailMessage(prior.build,
                                                SYSRECOVER_VERSION) + "\n";
            return false;
        }
    }
    {
        std::string vb = std::string(SYSRECOVER_VERSION) + "\n";
        HANDLE hv = CreateFileW((dir + L"\\rescue-build.txt").c_str(),
                                GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hv != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            WriteFile(hv, vb.data(), (DWORD)vb.size(), &w, nullptr);
            CloseHandle(hv);
        }
    }
    if (RescuePayloadMustNotBeReused(priorState)) {
        // 耗时不作门禁（用户 2026-11-07：不为 1-3 秒牺牲正确性），仅记录实测值备查。
        char tb[128];
        snprintf(tb, sizeof(tb),
                 "rescue payload replace (detect+remove+write+verify): %llu ms\n",
                 (unsigned long long)(GetTickCount64() - tDetect));
        log += tb;
    }
    if (mode == SbMode::None)
        SetFileAttributesW((dir + L"\\vmlinuz-zjrestore.efi").c_str(),
                           FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
    // PIT-195：暂存用的临时内核必须在**挂载期间**确认落盘（复核阶段 ESP 已卸载，
    // 看不到文件 —— 这是 0.9.69 首次实机的回归点）。
    if (temporary && mode == SbMode::None) {
        const std::wstring tmpK = dir + L"\\vmlinuz-zjtemp.efi";
        const DWORD ta = GetFileAttributesW(tmpK.c_str());
        if (ta == INVALID_FILE_ATTRIBUTES || (ta & FILE_ATTRIBUTE_DIRECTORY)) {
            log += "temporary kernel NOT deployed: vmlinuz-zjtemp.efi\n";
            return false;
        }
        SetFileAttributesW(tmpK.c_str(), FILE_ATTRIBUTE_HIDDEN |
                                             FILE_ATTRIBUTE_SYSTEM);
        log += "temporary kernel ready (vmlinuz-zjtemp.efi)\n";
    }

    // ── bootapp 走法：BCD bootapp 条目（bootmgr → efiloader → 我们的 UKI）──
    // 不需要固件启动项；单次启动用 BCD `bootsequence`（与 BIOS 路径同法）。
    if (!useFirmwareEntry) {
        std::wstring guid = temporary ? TemporaryGuid() : RecoveryGuid();
        std::string out;
        RunProcess(SysToolPath(L"bcdedit.exe"), L"/delete " + guid + L" /f",
                   out);  // 幂等：先删同类旧条目
        int rc = RunProcess(SysToolPath(L"bcdedit.exe"),
                            L"/create " + guid + L" /d \"" +
                                std::wstring(entryDesc) +
                                L"\" /application bootapp",
                            out);
        log += out;
        if (rc != 0) {
            log += "bcdedit /create bootapp FAILED rc=" + std::to_string(rc) +
                   "\n";
            return false;
        }
        bool ok = true;
        struct {
            const wchar_t* args;
        } const sets[] = {
            {L"device boot"},
            {L"path \\EFI\\ZJRESTORE\\efiloader.efi"},
            {L"nointegritychecks true"},  // 关掉 bootmgr 对 bootapp 的完整性校验
            {L"loadoptions \\EFI\\ZJRESTORE\\zjrestore-uki.efi"},
        };
        for (const auto& s : sets) {
            std::string o2;
            int r2 = RunProcess(SysToolPath(L"bcdedit.exe"),
                                L"/set " + guid + L" " + s.args, o2);
            if (r2 != 0) {
                log += std::string("bcdedit /set ") + " FAILED: " + o2 + "\n";
                ok = false;
            }
        }
        RunProcess(SysToolPath(L"bcdedit.exe"),
                   L"/displayorder " + guid + L" /addlast", out);
        if (!ok)
            return false;
        // 单次启动（写后读回校验，失败重试一次；安全软件可能假放行，
        // 见 bcd.h::BcdSetBootsequence 注释）
        if (!BcdSetBootsequence(guid, log)) {
            log += "set bcd bootsequence FAILED (readback mismatch)\n";
            return false;
        }
        // 清掉可能残留的**同类**固件启动项（临时项不得动常驻菜单）
        removeOurEntries(temporary);
        log += "bootapp ready: bootmgr -> efiloader -> zjrestore-uki.efi\n";
        return true;
    }

    // 3) 固件启动项（ESP 身份已在步骤 1 取得）
    if (!espIdOk) {
        log += "ESP GPT identity unavailable; refuse to write Boot####\n";
        return false;
    }
    int num = FindOurEntry(temporary);
    if (num < 0)
        num = FindFreeEntry();
    if (num < 0) {
        log += "no free Boot#### slot\n";
        return false;
    }
    // N17：写前备份将被覆盖的 Boot####（原始 NVRAM 字节）—— 载荷本身不备份
    //（先删后写、直接替换），这里只给启动项留一份可回退的原始值。
    {
        char nm[32] = {};
        snprintf(nm, sizeof(nm), "Boot%04X", num);
        const std::wstring bootNameW(nm, nm + strlen(nm));
        std::vector<BYTE> oldv;
        if (ReadVar(bootNameW, oldv) && !oldv.empty()) {
            const std::wstring bak =
                espRoot + L"EFI\\ZJRESTORE\\" + bootNameW + L".pre.bak";
            HANDLE hb = CreateFileW(bak.c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
            if (hb != INVALID_HANDLE_VALUE) {
                DWORD wr = 0;
                WriteFile(hb, oldv.data(), (DWORD)oldv.size(), &wr, nullptr);
                CloseHandle(hb);
                log += std::string(nm) + ".pre.bak saved\n";
            }
        }
    }
    if (!BuildAndWrite(num, espSig, espPart, espStart, espSize, targetPath,
                       cmdLine, entryDesc, log))
        return false;

    // PIT-246 / PIT-251：升级时同种类可能还有历史品牌名的残留项（SysRecover /
    // SysRecovery Environment / 旧 "NineRestore Recovery Environment"）。
    // 保留刚写好/复用的 num；其余项必须**同时**满足"是我们这一类"且
    // "HD 起止 LBA + GPT 分区 GUID 与当前 ESP 全等"才删 —— 别的 ESP 上的
    // NineRestore 菜单不属于本次安装，不能连带删掉。
    {
        std::vector<BYTE> dupOrder;
        if (ReadVar(L"BootOrder", dupOrder)) {
            const int kind = temporary ? 2 : 1;
            for (size_t i = 0; i + 1 < dupOrder.size(); i += 2) {
                WORD v = 0;
                memcpy(&v, &dupOrder[i], 2);
                if ((int)v == num)
                    continue;
                std::vector<BYTE> b;
                if (!ReadVar(VarName(v), b) || !IsOurs(b, kind))
                    continue;
                if (!OptionHdMatches(b, espSig, espStart, espSize)) {
                    char nm2[32] = {};
                    snprintf(nm2, sizeof(nm2), "Boot%04X", (int)v);
                    log += std::string("duplicate on another ESP kept: ") +
                           nm2 + "\n";
                    continue;
                }
                // PIT-250：RemoveEntryNum 内部已回读 BootOrder + Boot####；
                // 只看 SetVarW 返回值时"安全软件吞写"会在日志里谎报已删。
                const bool gone = RemoveEntryNum((int)v);
                char dup[80];
                if (!gone) {
                    snprintf(dup, sizeof(dup),
                             "duplicate Boot%04X delete FAILED (read-back mismatch)\n",
                             (int)v);
                } else {
                    snprintf(dup, sizeof(dup), "removed duplicate Boot%04X\n",
                             (int)v);
                }
                log += dup;
            }
        }
    }

    // 3) 挂进 BootOrder（**末尾**：默认仍进 Windows，固件启动菜单里能选到）
    std::vector<BYTE> order;
    const bool orderRead = ReadVar(L"BootOrder", order);
    if (!orderRead) {
        // PIT-195：原实现把"读失败"当空表 → 会把 BootOrder 覆盖成只剩我们一项，
        // 其它系统从启动菜单里消失（可能"无法开机"）。读失败直接失败，让上层
        // fail-closed。
        log += "read BootOrder failed err=" + std::to_string(GetLastError()) +
               " (refuse to overwrite)\n";
        return false;
    }
    // N17：写前备份原始 BootOrder（不只是暂存项的 bootorder.bak）。
    {
        const std::wstring bak = espRoot + L"EFI\\ZJRESTORE\\bootorder.pre.bak";
        HANDLE hb = CreateFileW(bak.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hb != INVALID_HANDLE_VALUE) {
            DWORD wr = 0;
            WriteFile(hb, order.data(), (DWORD)order.size(), &wr, nullptr);
            CloseHandle(hb);
            log += "bootorder.pre.bak saved (" + std::to_string(order.size()) +
                   "B)\n";
        }
    }
    int n = (int)(order.size() / 2);
    std::vector<WORD> list(n);
    bool present = false;
    for (int i = 0; i < n; ++i) {
        memcpy(&list[i], &order[i * 2], 2);
        if (list[i] == (WORD)num)
            present = true;
    }
    if (!present)
        list.push_back((WORD)num);
    // PIT-195：**暂存项置顶**（不依赖固件是否支持 BootNext —— PIT-194 现场
    // 固件就把 BootNext 忽略了）。原顺序存到 ESP，救援/新系统首启后还原。
    if (temporary) {
        std::vector<WORD> front;
        front.push_back((WORD)num);
        for (WORD v : list)
            if (v != (WORD)num)
                front.push_back(v);
        list.swap(front);
    }
    std::vector<BYTE> nb(list.size() * 2);
    for (size_t i = 0; i < list.size(); ++i)
        memcpy(&nb[i * 2], &list[i], 2);
    SetLastError(0);
    if (!SetVarW(L"BootOrder", kGlobalGuid, nb.data(), (DWORD)nb.size())) {
        log += "set BootOrder failed err=" + std::to_string(GetLastError()) +
               "\n";
        return false;
    }
    if (temporary) {
        // 原顺序（不含本项）留档：新系统首启的清理助手据此还原。
        std::vector<BYTE> orig(order.size() > 0 ? order.size() : 2);
        size_t w = 0;
        for (int i = 0; i < n; ++i) {
            WORD v = 0;
            memcpy(&v, &order[i * 2], 2);
            if (v == (WORD)num)
                continue;
            memcpy(&orig[w], &order[i * 2], 2);
            w += 2;
        }
        orig.resize(w);
        const std::wstring bak = espRoot + L"EFI\\ZJRESTORE\\bootorder.bak";
        HANDLE hb = CreateFileW(bak.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hb != INVALID_HANDLE_VALUE) {
            DWORD wr = 0;
            WriteFile(hb, orig.data(), (DWORD)orig.size(), &wr, nullptr);
            CloseHandle(hb);
            log += "bootorder.bak saved (" + std::to_string(orig.size()) +
                   "B)\n";
        }
        // PIT-209：把条目号留给 Linux 救援层 —— 它救完/失败重启前要能把这
        // 条**指向已删内核**的 Boot#### 从 NVRAM 里删掉，否则固件每次开机
        // 都先去加载一个不存在的文件（有的固件就此卡住/反复重启）。
        {
            char name[32];
            snprintf(name, sizeof(name), "Boot%04X", num);
            const std::wstring mp = espRoot + L"EFI\\ZJRESTORE\\temp-entry.txt";
            HANDLE hm = CreateFileW(mp.c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
            if (hm != INVALID_HANDLE_VALUE) {
                DWORD wr = 0;
                WriteFile(hm, name, (DWORD)strlen(name), &wr, nullptr);
                CloseHandle(hm);
                log += std::string("temp entry marker: ") + name + "\n";
            }
        }
        log += "staged entry Boot" + std::to_string(num) +
               " promoted to BootOrder[0]\n";
    }
    // N8（2026-11-07）：把入口类型落盘。grub/shim 模式没有逐条目的 OptionalData，
    // 救援层读不到 zjentry=，改读这个文件；两者都会写进 entry-started 与 status。
    {
        const std::wstring ek = espRoot + L"EFI\\ZJRESTORE\\entry-kind.txt";
        HANDLE hk = CreateFileW(ek.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hk != INVALID_HANDLE_VALUE) {
            const char* kind = temporary ? "temp\n" : "menu\n";
            DWORD wr = 0;
            WriteFile(hk, kind, (DWORD)strlen(kind), &wr, nullptr);
            CloseHandle(hk);
            log += std::string("entry-kind.txt = ") +
                   (temporary ? "temp" : "menu") + "\n";
        }
    }
    char line[160];
    snprintf(line, sizeof(line),
             "firmware boot entry Boot%04X installed (desc=%ls, optional=%u chars)\n",
             num, entryDesc, cmdLine ? (unsigned)wcslen(cmdLine) : 0u);
    log += line;

    // Secure Boot 且密钥**还没注册**时：把下一次启动指到我们的条目，这样重启就
    // 直接进 MokManager 让用户注册（否则普通重启只进 Windows，用户根本看不到
    // 注册界面）。注册完成后 BootNext 已消耗，之后正常进 Windows。
#if ZJ_ENABLE_MOK_PATH
    if (mode == SbMode::Shim && !IsMokEnrolled(exeDir)) {
        WORD v = (WORD)num;
        SetLastError(0);
        if (SetVarW(L"BootNext", kGlobalGuid, &v, sizeof(v)))
            log += "BootNext set -> next boot enters MokManager to enroll "
                   "zj-mok.cer\n";
        else
            log += "set BootNext failed err=" +
                   std::to_string(GetLastError()) + "\n";
    }
#endif
    return true;
}

bool UefiBootEntryExists(std::string& detail) {
    int num = FindOurEntry(false);
    if (num < 0) {
        detail = Tr("未安装（固件启动项不存在）");
        return false;
    }
    // L1：Boot#### 的编号单独格式化后再拼接（固定 %04X 格式串）。
    char numText[16];
    snprintf(numText, sizeof(numText), "%04X", num);
    detail = Tr("已安装 Boot");
    detail += numText;
    return true;
}

bool RemoveUefiBootEntry(std::string& log, bool temporary) {
    if (!EnableEnvPrivilege(log))
        return false;
    // PIT-246：循环删到没有为止 —— 升级前可能同时存在
    // SysRecover / SysRecovery Environment / NineRestore 多条指向同一份载荷的
    // 残留项；只删一条会留下"看得见但已失效"的旧名字。
    int removed = 0;
    bool allOk = true;
    for (int guard = 0; guard < 8; ++guard) {
        int num = FindOurEntry(temporary);
        if (num < 0)
            break;
        if (!RemoveEntryNum(num))
            allOk = false;
        ++removed;
    }
    // PIT-250：RemoveEntryNum 回读确认了 BootOrder+Boot#### 两处；"安全软件吞写"
    // 时循环会空转 8 次仍报"已删除"。这里再按 FindOurEntry 复查一遍（防止
    // 删除只改了变量、BootOrder 还列着）。
    const int left = FindOurEntry(temporary);
    if (left >= 0) {
        char lb[64];
        snprintf(lb, sizeof(lb),
                 "firmware boot entry removal INCOMPLETE: Boot%04X still present\n",
                 left);
        log += lb;
        return false;
    }
    if (!allOk) {
        log += "firmware boot entry removal INCOMPLETE: BootOrder/Boot#### write not confirmed\n";
        return false;
    }
    if (removed == 0) {
        log += temporary ? "no temporary firmware entry to remove\n"
                         : "no firmware entry to remove\n";
        return true;
    }
    log += "firmware boot entry removed x" + std::to_string(removed) +
           " (verified)\n";
    return true;
}

bool UefiTempBootStateOk(std::string& detail) {
    if (!EnableEnvPrivilege(detail)) return false;
    const int num = FindOurEntry(true);
    if (num < 0) {
        detail = "temporary Boot#### missing (not in BootOrder)";
        return false;
    }
    // 只查固件变量。**不要**在这里读 ESP 文件：调用点在 UnmountEsp 之后，
    // 路径必然"不存在"（PIT-195 回归：0.9.69 首次实机就栽在这）。
    // 临时内核是否落盘由 InstallUefiBootEntry 挂载期间保证（CRC + 显式检查）。
    // BootNext 指向它，**或**它在 BootOrder 首位（不赌固件认 BootNext）
    WORD next = 0;
    const DWORD len = GetFirmwareEnvironmentVariableW(L"BootNext", kGlobalGuid,
                                                      &next, sizeof(next));
    const bool nextOk = (len == sizeof(next) && next == (WORD)num);
    bool firstOk = false;
    std::vector<BYTE> order;
    if (ReadVar(L"BootOrder", order) && order.size() >= 2) {
        WORD first = 0;
        memcpy(&first, &order[0], 2);
        firstOk = (first == (WORD)num);
    }
    char buf[160];
    if (!nextOk && !firstOk) {
        snprintf(buf, sizeof(buf),
                 "Boot%04X neither BootNext nor BootOrder[0]", num);
        detail = buf;
        return false;
    }
    snprintf(buf, sizeof(buf), "Boot%04X ok (%s%s%s)", num,
             nextOk ? "BootNext" : "", (nextOk && firstOk) ? "+" : "",
             firstOk ? "BootOrder[0]" : "");
    detail = buf;
    return true;
}

// 清理指向"临时内核已不存在"的 SysRecover 启动项（PIT-195）：
// 场景：以前暂存过、救援没跑成、救援层已把 vmlinuz-zjtemp.efi 删掉 →
// 固件里留一个永远加载失败、还顶在 BootOrder 首位的项（每次开机白等几秒）。
bool CleanupStaleUefiEntries(std::string& log) {
    // ★ PIT-209：这个函数会把"临时启动项"整条删掉 —— 如果此刻正有一个
    //   **待执行**的暂存还原（重启后要进救援），删掉它 = 用户白重启一次。
    //   以前只靠调用方自觉（注释这么写），现在自己守一道门。
    const std::wstring pend = LogBaseDir() + L"\\logs\\pending-restore.txt";
    if (GetFileAttributesW(pend.c_str()) != INVALID_FILE_ATTRIBUTES) {
        log += "stale cleanup skipped: pending staged restore exists\n";
        return false;
    }
    if (!EnableEnvPrivilege(log)) return false;
    int removed = 0;
    for (int guard = 0; guard < 8; ++guard) {
        const int num = FindOurEntry(true);
        if (num < 0)
            break;
        // 有临时内核 → 说明是"待执行"，不能清
        // （这里只能看 ESP 文件：外部调用前已确认没有待执行任务）
        std::vector<BYTE> b;
        if (!ReadVar(VarName(num), b))
            break;
        std::wstring desc, dev;
        ParseOption(b, desc, dev);
        // PIT-246：不再要求 desc == 当前名字 —— 历史品牌名也在清理范围内；
        // FindOurEntry(true) 已保证种类是"临时项"且路径在 ZJRESTORE 下。
        if (!RemoveUefiBootEntry(log, true)) {
            log += "stale cleanup INCOMPLETE (firmware entry still present)\n";
            break;
        }
        ++removed;
    }
    if (removed > 0)
        log += "stale restore entry removed x" + std::to_string(removed) +
               "\n";
    return removed > 0;
}

bool SetUefiBootNext(std::string& log, bool temporary) {
    if (!EnableEnvPrivilege(log))
        return false;
    int num = FindOurEntry(temporary);
    if (num < 0) {
        log += temporary ? "SetUefiBootNext: temporary entry not installed\n"
                         : "SetUefiBootNext: entry not installed\n";
        return false;
    }
    WORD v = (WORD)num;
    // 写后读回校验 + 重试一次（用户 2026-10-27 需求）：安全软件可能吞掉固件变量
    // 写入（返回成功但 BootNext 没落盘）→ 重启后照旧进原系统、用户毫无提示。
    for (int attempt = 1; attempt <= 2; ++attempt) {
        SetLastError(0);
        if (!SetVarW(L"BootNext", kGlobalGuid, &v, sizeof(v))) {
            log += "set BootNext failed err=" +
                   std::to_string(GetLastError()) + " (attempt " +
                   std::to_string(attempt) + ")\n";
            continue;
        }
        WORD back = 0xFFFF;
        DWORD len = GetFirmwareEnvironmentVariableW(L"BootNext", kGlobalGuid,
                                                    &back, sizeof(back));
        if (len == sizeof(back) && back == v) {
            char line[64];
            snprintf(line, sizeof(line), "BootNext = Boot%04X (verified)\n", num);
            log += line;
            return true;
        }
        log += "BootNext verify FAILED (wanted Boot" + std::to_string(num) +
               ", got " +
               (len == sizeof(back)
                    ? "Boot" + std::to_string((int)back)
                    : "len=" + std::to_string((int)len)) +
               ") attempt " + std::to_string(attempt) + "\n";
    }
    return false;
}

bool EnsureWindowsBootEntry(const std::wstring& espRoot, std::string& log,
                            bool front) {
    // 兜底 UEFI 引导（2026-10-27）：没有 bcdboot 时固件里没有 "Windows Boot
    // Manager"，这里自己写；已存在就只确保它在 BootOrder 里，不重复建条目。
    // PIT-208：front=true 要 **提到 BootOrder 第一位**。还原/修引导之后必须这样，
    //   否则固件可能先启第三方恢复工具、或已删内核的陈旧条目 —— 表现就是
    //   "还原成功、开机却反复重启进不了系统"。
    if (!EnableEnvPrivilege(log))
        return false;
    GUID sig{};
    DWORD partNum = 0;
    unsigned long long start = 0, size = 0;
    if (!QueryEspGpt(espRoot, sig, partNum, start, size, log)) {
        log += "windows boot entry: ESP GPT info unavailable\n";
        return false;
    }
    const wchar_t* kDesc = L"Windows Boot Manager";
    const std::wstring kPath = L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi";

    // 1) 找"同名（描述）+ 设备路径含 bootmgfw"的已有条目，复用并按需挪到最前
    int found = -1;
    std::vector<BYTE> order;
    ReadVar(L"BootOrder", order);
    const int n = (int)(order.size() / 2);
    for (int i = 0; i < n; ++i) {
        WORD v = 0;
        memcpy(&v, &order[i * 2], 2);
        std::vector<BYTE> b;
        if (!ReadVar(VarName(v), b))
            continue;
        std::wstring d, dev;
        if (!ParseOption(b, d, dev))
            continue;
        if (d == kDesc && ContainsNoCase(dev, L"bootmgfw.efi")) {
            found = v;
            break;
        }
    }

    WORD v = 0;
    if (found >= 0) {
        v = (WORD)found;
        char line[112];
        snprintf(line, sizeof(line),
                 "windows boot entry: exists Boot%04X (%s)\n", found,
                 front ? "move to front" : "kept");
        log += line;
    } else {
        // 2) 新建一条：磁盘格式 HardDrive + FilePath（与救援项同一套写法）
        const int num = FindFreeEntry();
        if (num < 0) {
            log += "windows boot entry: no free Boot#### slot\n";
            return false;
        }
        if (!BuildAndWrite(num, sig, partNum, start, size, kPath, nullptr, kDesc,
                           log)) {
            log += "windows boot entry: write failed\n";
            return false;
        }
        v = (WORD)num;
        char line[96];
        snprintf(line, sizeof(line), "windows boot entry: Boot%04X created\n",
                 num);
        log += line;
    }

    // 2b) PIT-209：清掉**同一块 ESP 上**重复的「Windows Boot Manager」条目
    //     （装过多次/固件自己加过）—— 固件启动菜单里会出现好几个同名项，
    //     还有一个可能指向旧 ESP。只删"描述 + bootmgfw 路径 + 同一分区 HD 节点"
    //     三者全等的；别的硬盘上的条目一律不碰。
    {
        std::vector<BYTE> ord0;
        if (ReadVar(L"BootOrder", ord0)) {
            int dup = 0;
            for (size_t i = 0; i + 1 < ord0.size(); i += 2) {
                WORD w = 0;
                memcpy(&w, &ord0[i], 2);
                if (w == v) continue;
                std::vector<BYTE> b;
                if (!ReadVar(VarName(w), b)) continue;
                std::wstring d, dev;
                if (!ParseOption(b, d, dev)) continue;
                if (d != kDesc) continue;
                if (!ContainsNoCase(dev, L"bootmgfw.efi")) continue;
                if (!OptionHdMatches(b, sig, start, size)) continue;
                if (DeleteVarW(VarName(w))) {
                    ++dup;
                    log += "windows boot entry: duplicate Boot" +
                           std::to_string(w) + " removed\n";
                }
            }
            if (dup > 0)
                log += "windows boot entry: removed " + std::to_string(dup) +
                       " duplicate(s)\n";
        }
    }

    // 3) 写 BootOrder：front=true 放第一位；否则只在"原本没有"时补到末尾
    std::vector<BYTE> cur;
    ReadVar(L"BootOrder", cur);
    std::vector<WORD> ids;
    bool present = false;
    for (size_t i = 0; i + 1 < cur.size(); i += 2) {
        WORD w = 0;
        memcpy(&w, &cur[i], 2);
        if (w == v) {
            if (present)
                continue;  // 去重
            present = true;
            if (!front)
                ids.push_back(w);  // 非 front：保持原位
            continue;
        }
        ids.push_back(w);
    }
    if (front)
        ids.insert(ids.begin(), v);
    else if (!present)
        ids.push_back(v);

    std::vector<BYTE> ord;
    ord.reserve(ids.size() * 2);
    for (WORD w : ids) {
        ord.push_back((BYTE)(w & 0xFF));
        ord.push_back((BYTE)(w >> 8));
    }
    SetLastError(0);
    if (!SetVarW(L"BootOrder", kGlobalGuid, ord.data(), (DWORD)ord.size())) {
        log += "windows boot entry: BootOrder write failed err=" +
               std::to_string(GetLastError()) + "\n";
        return false;
    }
    // 写后回读：安全软件可能"返回成功但没落盘"（用户 2026-10-27 反馈过）
    std::vector<BYTE> back;
    bool inOrder = false, first = false;
    if (ReadVar(L"BootOrder", back)) {
        for (size_t i = 0; i + 1 < back.size(); i += 2) {
            WORD w = 0;
            memcpy(&w, &back[i], 2);
            if (w == v) {
                inOrder = true;
                first = (i == 0);
                break;
            }
        }
    }
    char line[160];
    snprintf(line, sizeof(line),
             "windows boot entry: Boot%04X %s (order_verified=%d first=%d)\n", v,
             found >= 0 ? "reused" : "created", (int)inOrder, (int)first);
    log += line;
    if (front && !first) {
        log += "windows boot entry: could not move to front of BootOrder\n";
        return false;
    }
    return inOrder;
}

void UefiBootOrderSummary(std::vector<std::string>& lines) {
    // PIT-208："开机到底先启动谁"只能看 BootOrder，不能再靠猜。
    // 2026-11-02 现场（还原成功却反复重启）缺的就是这份快照。
    std::string elog;
    if (!EnableEnvPrivilege(elog))
        return;
    std::vector<BYTE> order;
    if (!ReadVar(L"BootOrder", order))
        return;
    const int n = (int)(order.size() / 2);
    for (int i = 0; i < n; ++i) {
        WORD v = 0;
        memcpy(&v, &order[i * 2], 2);
        char head[96];
        std::vector<BYTE> b;
        if (!ReadVar(VarName(v), b)) {
            snprintf(head, sizeof(head), "Boot%04X <missing>", v);
            lines.push_back(head);
            continue;
        }
        std::wstring d, dev;
        if (!ParseOption(b, d, dev)) {
            snprintf(head, sizeof(head), "Boot%04X <unparsable>", v);
            lines.push_back(head);
            continue;
        }
        std::string dev8 = Narrow(dev);
        // dev 里含 HD(...) 节点的原始字节（可打印的会变成乱码），
        // 从第一个反斜杠起才是真正的文件路径。
        const size_t bs = dev8.find('\\');
        if (bs != std::string::npos && bs > 0)
            dev8 = dev8.substr(bs);
        if (dev8.size() > 140)
            dev8 = dev8.substr(0, 137) + "...";
        const std::string desc = Narrow(d);
        snprintf(head, sizeof(head), "Boot%04X \"%s\" ", v, desc.c_str());
        lines.push_back(std::string(head) + dev8);
    }
}

bool IsSecureBootEnabled() {
    // 固件全局命名空间里的 `SecureBoot`：1 = 开启（User Mode），0 = 关闭。
    BYTE v = 0;
    DWORD len = GetFirmwareEnvironmentVariableW(L"SecureBoot", kGlobalGuid, &v,
                                                sizeof(v));
    return len >= 1 && v != 0;
}

bool IsMokEnrolled(const std::wstring& exeDir) {
    // MOK 有独立命名空间；MokListRT 是给运行时读的副本（含已注册的证书 DER）。
    const wchar_t kMokGuid[] = L"{605DAB50-E046-4300-ABB6-3DDBB810D214}";
    std::vector<BYTE> mok(64 * 1024);
    DWORD len = GetFirmwareEnvironmentVariableW(L"MokListRT", kMokGuid,
                                                mok.data(),
                                                (DWORD)mok.size());
    if (len == 0)
        return false;
    mok.resize(len);
    // 读我们的公钥证书（DER），在 MokListRT 里找这段字节
    HANDLE h = CreateFileW((exeDir + L"\\bootfiles\\sb\\zj-mok.cer").c_str(),
                           GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    std::vector<BYTE> cert(4096);
    DWORD got = 0;
    ReadFile(h, cert.data(), (DWORD)cert.size(), &got, nullptr);
    CloseHandle(h);
    if (got == 0)
        return false;
    cert.resize(got);
    return std::search(mok.begin(), mok.end(), cert.begin(), cert.end()) !=
           mok.end();
}

namespace {
// 读引导走法开关文件 <exeDir>\sb-mode.txt（内容：shim / grub / bootapp）。
// **文件优先于环境变量**：UAC 提权/双击启动时环境变量不一定继承得到
// （实测踩过：设了 ZJ_SB_MODE=grub，日志里却还是 "secure boot mode: shim"）。
std::wstring ReadSbModeFile() {
    wchar_t p[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, p, MAX_PATH))
        return L"";
    std::wstring s = p;
    size_t pos = s.find_last_of(L"\\/");
    if (pos == std::wstring::npos)
        return L"";
    std::wstring file = s.substr(0, pos) + L"\\sb-mode.txt";
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return L"";
    char buf[64] = {};
    DWORD got = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(h);
    std::wstring w;
    for (DWORD i = 0; i < got; ++i) {
        char c = buf[i];
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
            if (!w.empty())
                break;
            continue;
        }
        w.push_back((wchar_t)tolower((unsigned char)c));
    }
    return w;
}
}  // namespace

const char* SbModeName(SbMode m) {
    switch (m) {
        case SbMode::Shim: return "shim";
        case SbMode::Grub: return "grub";
        case SbMode::Bootapp: return "bootapp";
        default: return "none";
    }
}

SbMode CurrentSbMode() {
    if (!IsSecureBootEnabled())
        return SbMode::None;
    // 模式来源：<exeDir>\sb-mode.txt（优先）→ 环境变量 ZJ_SB_MODE
    std::wstring m = ReadSbModeFile();
    if (m.empty()) {
        wchar_t buf[32] = {};
        if (GetEnvironmentVariableW(L"ZJ_SB_MODE", buf, 32) > 0) {
            for (const wchar_t* q = buf; *q; ++q)
                m.push_back((wchar_t)towlower(*q));
        }
    }
    if (m == L"bootapp")
        return SbMode::Bootapp;  // 实验用：SB 开启时走不通（PIT-065）
#if ZJ_ENABLE_MOK_PATH
    if (m == L"shim")
        return SbMode::Shim;  // 备选线：我们签名的 UKI（需一次性 MOK 注册）
#else
#endif
    // 默认走 grub：**借 Canonical 的签名链**（微软签名的 shim → Canonical 签名的
    // GRUB → Canonical 签名的内核 + 我们的 initramfs）→ 零注册、零成本、无绕过，
    // 实测 QEMU 全链通过（PIT-066 变体 D）。
    return SbMode::Grub;
}

// ── 引导层健康检查（docs/15 · P2/P3）──────────────────────────────
// 起因：`bcdboot` 的返回码曾被丢弃（静默失败），且没人校验产物；ESP 一旦写残，
// 还原后就是"选择操作系统"空菜单 / 各种 0xc00000xx。这三个函数把"写没写成"
// 变成可判定的事实，并让调用方 fail-closed。
bool TargetBcdTemplateOk(wchar_t targetLetter, std::string& detail) {
    if (!targetLetter) {
        detail = "target letter is empty";
        return false;
    }
    std::wstring tpl = std::wstring(1, targetLetter) +
                       L":\\Windows\\System32\\Config\\BCD-Template";
    auto usable = [](const std::wstring& p) {
        return PathExists(p) && FileSize(p) > 0;
    };
    if (usable(tpl)) return true;
    // WOW64 重定向（PIT-193，2026-10-31 用户本机实测）：32 位进程访问
    // `<系统盘>:\Windows\System32\...` 会被系统改写成 `SysWOW64`，
    // 而 `SysWOW64\Config\BCD-Template` **不存在** → 明明该文件在
    // `System32\Config\` 也会被判成"缺模板"，整单暂存被回滚。
    // 32 位进程另有 `%windir%\Sysnative` 指向原生 System32，用它再看一次。
    if (sizeof(void*) == 4) {
        wchar_t win[MAX_PATH] = {};
        if (GetWindowsDirectoryW(win, MAX_PATH)) {
            const std::wstring nat =
                std::wstring(win) + L"\\Sysnative\\Config\\BCD-Template";
            if (usable(nat)) {
                LogInfo("BCD-Template found via Sysnative (32-bit process, "
                        "WOW64 redirection bypassed)");
                return true;
            }
        }
    }
    detail = PathExists(tpl)
                 ? "empty \\Windows\\System32\\Config\\BCD-Template"
                 : "missing \\Windows\\System32\\Config\\BCD-Template";
    return false;
}

bool EspHasRoomForBcdboot(const std::wstring& espRoot, std::string& detail) {
    // bcdboot 刷新 \EFI\Microsoft\Boot\ 实测约 8.6MB（bootmgfw + bootmgr.efi +
    // Fonts 4.5MB + Resources + MUI + BCD）；要求 ≥ 24MB 留够余量。
    const unsigned long long kNeed = 24ull << 20;
    ULARGE_INTEGER freeBytes{};
    if (!GetDiskFreeSpaceExW(espRoot.c_str(), &freeBytes, nullptr, nullptr)) {
        detail = "cannot query free space on ESP";
        return false;
    }
    char line[160];
    snprintf(line, sizeof(line), "ESP free for bcdboot: %.1f MB (need %.0f MB)",
             freeBytes.QuadPart / 1048576.0, kNeed / 1048576.0);
    detail = line;
    return freeBytes.QuadPart >= kNeed;
}

bool VerifyEspBcd(const std::wstring& espRoot, const std::wstring& srcWinDir,
                  std::string& detail) {
    // ① BCD 文件在不在
    std::wstring bcd = espRoot + L"EFI\\Microsoft\\Boot\\BCD";
    if (!PathExists(bcd)) {
        detail = Tr("ESP 上没有 \\EFI\\Microsoft\\Boot\\BCD");
        return false;
    }
    // ② bcdedit 能不能读出"有 displayorder + 有 winload 条目 + 默认项指向本分区"
    // 0.9.33：精简 PE（如 NewBee）没有 bcdedit.exe，但 bcdboot 可用；此时
    // 仍做文件级检查，避免把能启动的 PE 因缺少枚举工具误判成失败。
    std::string out;
    const std::wstring bcdedit = SysToolPath(L"bcdedit.exe");
    if (GetFileAttributesW(bcdedit.c_str()) == INVALID_FILE_ATTRIBUTES) {
        detail = "bcdedit.exe missing; ESP BCD file-level check only";
        LogWarn("ESP BCD verify: bcdedit.exe missing, skip enum check");
    } else {
        // 统一产物断言（PIT-208/209）：displayorder 非空 + 有 winload +
        // **默认项指向本次还原的分区** + 加载器必须是 winload.efi。
        // 以前只查"有条目"，指向别处（甚至指向已删除卷）的孤儿条目也能过，
        // 于是跳过 bcdboot，机器就起不来（2026-11-02 客户机）。
        std::string vwhy;
        if (!VerifyBcdStore(bcd, srcWinDir, /*uefi=*/true, vwhy)) {
            detail = Tr("ESP 上的 BCD 不可用：") + vwhy;
            return false;
        }
    }
    // ③ 硬项：bootmgr 实体 + 固件兜底路径（bcdboot 必写；缺 = 真坏）
    struct Item {
        const wchar_t* path;   // 相对 ESP 根
        const char* name;      // 报错用（ASCII）
    };
    const Item kItems[] = {
        {L"EFI\\Microsoft\\Boot\\bootmgfw.efi", "bootmgfw.efi"},
        {L"EFI\\Boot\\bootx64.efi", "EFI\\Boot\\bootx64.efi"},
    };
    for (const auto& it : kItems) {
        if (!PathExists(espRoot + it.path)) {
            detail = std::string(Tr("ESP 上缺少引导文件：")) + it.name;
            return false;
        }
    }
    // ④ 软项（精简系统容错，PIT-126）：ESP 缺 → 看**源**里有没有
    //    （源也缺 = 精简镜像的既成事实，机器本来就能开机 → 放行 + note；
    //      源有而 ESP 没有 = bcdboot "写一半" → 仍 fail-closed）。
    std::wstring bootDir = espRoot + L"EFI\\Microsoft\\Boot";
    const bool haveSrc = !srcWinDir.empty();
    std::string notes;
    auto checkSoft = [&](bool espHas, bool srcHas, const char* srcName,
                         const std::string& failMsg) -> bool {
        switch (EspSoftItemVerdict(espHas, haveSrc && srcHas)) {
            case SoftItemVerdict::Ok:
                return true;
            case SoftItemVerdict::Fail:
                detail = failMsg;
                return false;
            case SoftItemVerdict::SkipSourceMissing:
                notes += std::string(" [lite: source lacks ") + srcName + "]";
                return true;
        }
        return true;
    };
    {
        bool espHas = PathExists(bootDir + L"\\Resources\\bootres.dll");
        bool srcHas =
            haveSrc && PathExists(srcWinDir + L"\\Boot\\Resources\\bootres.dll");
        if (!checkSoft(espHas, srcHas, "Resources\\bootres.dll",
                       Tr("ESP 上缺少引导资源：Resources\\bootres.dll")))
            return false;
    }
    {
        bool espHas = HasFileMatching(bootDir + L"\\Fonts", L"*_boot.ttf");
        bool srcHas =
            haveSrc && HasFileMatching(srcWinDir + L"\\Boot\\Fonts", L"*_boot.ttf");
        if (!checkSoft(espHas, srcHas, "Boot\\Fonts\\*_boot.ttf",
                       Tr("ESP 上缺少引导字体（Fonts\\*_boot.ttf）")))
            return false;
    }
    {
        bool espHas = DirExists(bootDir + L"\\zh-CN") ||
                      DirExists(bootDir + L"\\en-US");
        bool srcHas =
            haveSrc && (DirExists(srcWinDir + L"\\Boot\\Resources\\zh-CN") ||
                        DirExists(srcWinDir + L"\\Boot\\Resources\\en-US"));
        if (!checkSoft(espHas, srcHas, "Boot\\Resources\\<lang>",
                       Tr("ESP 上缺少引导语言资源目录（zh-CN / en-US）")))
            return false;
    }
    detail = std::string("ok") + notes;
    return true;
}

}  // namespace sysrecover