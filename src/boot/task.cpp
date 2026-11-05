// 还原任务暂存实现。
#include "task.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "../common/version.h"

namespace sysrecover {
namespace {

std::string W2U(const std::wstring& w) {
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0,
                                nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, 0);
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr,
                            nullptr);
    return s;
}

// JSON 字符串转义（反斜杠/引号），供 json 侧写 Windows 路径。
std::string JsonEsc(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '\\' || c == '"')
            out += '\\';
        out += c;
    }
    return out;
}

bool WriteTextFile(const std::wstring& path, const std::string& utf8) {
    HANDLE h =
        CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    BOOL ok = ::WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written,
                          nullptr);
    CloseHandle(h);
    return ok && written == utf8.size();
}

// M14：读回校验用（conf 写完后回读，确认关键键真的落盘）。
bool ReadTextFile(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD size = GetFileSize(h, nullptr);
    out.assign(size, '\0');
    DWORD got = 0;
    const BOOL ok = ReadFile(h, out.data(), size, &got, nullptr) != 0;
    CloseHandle(h);
    out.resize(ok ? got : 0);
    return ok;
}

bool HasKeyLine(const std::string& text, const char* key) {
    if (text.compare(0, strlen(key), key) == 0)
        return true;
    const std::string pat = std::string("\n") + key;
    return text.find(pat) != std::string::npos;
}

}  // namespace

// 任务 sidecar（key=value，Linux restore.sh 读取）。软件目录与目标分区根
// 各写一份：日志写目标根、conf 写软件目录，二者可能不在同一分区，Linux 侧
// 需能就近取到（PIT-035）。
// M14（1.0）：契约/日志不再用 8192 固定缓冲 + snprintf —— 0.9.96 从不检查
// snprintf 返回值，超长路径会把后面的 contract_version/keep_old/target_guid
// 键截掉，Linux 侧就可能按默认策略选错目标。改用 std::string 逐步追加，
// 从根上消除截断（单字段长度上限由调用方/系统路径限制决定）。
namespace {
void Kv(std::string& s, const char* key, const std::string& v) {
    s += key;
    s += v;
    s += '\n';
}
std::string Num(long long n) {
    char b[32];
    snprintf(b, sizeof(b), "%lld", n);
    return b;
}
std::string NumU(unsigned long long n) {
    char b[32];
    snprintf(b, sizeof(b), "%llu", n);
    return b;
}
}  // namespace

std::string BuildTaskConf(const RestoreTask& t) {
    std::string s;
    Kv(s, "action=", t.action.empty() ? "restore" : t.action);
    Kv(s, "pt_type=", t.ptType);
    Kv(s, "image_part_guid=", W2U(t.imagePartGuid));
    Kv(s, "image_rel_path=", W2U(t.imageRelPath));
    Kv(s, "image_path=", W2U(t.imagePath));
    Kv(s, "image_index=", Num(t.imageIndex));
    Kv(s, "image_size=", NumU(t.imageSize));
    Kv(s, "reparse_scan=", Num(t.reparseScan));
    Kv(s, "efs_scan=", Num(t.efsScan));
    Kv(s, "esp_index=", Num(t.espIndex));
    Kv(s, "ea_index=", Num(t.eaIndex));
    Kv(s, "esp_fix_dir=", W2U(t.espFixDir));
    Kv(s, "ea_fix_dir=", W2U(t.eaFixDir));
    Kv(s, "target_guid=", W2U(t.targetGuid));
    Kv(s, "target_offset=", NumU(t.targetOffset));
    Kv(s, "target_size=", NumU(t.targetSize));
    Kv(s, "target_disk_serial=", W2U(t.targetDiskSerial));
    Kv(s, "repair_boot=", Num(t.repairBoot ? 1 : 0));
    Kv(s, "partition_count=", NumU(t.partCount));
    Kv(s, "software_dir=", W2U(t.softwareDir));
    Kv(s, "engine=", t.engine.empty() ? "block" : t.engine);
    Kv(s, "keep_old=", Num(t.keepOld ? 1 : 0));
    Kv(s, "target_mode=", t.targetMode.empty() ? "format" : t.targetMode);
    Kv(s, "new_computer_name=", W2U(t.computerName));
    Kv(s, "contract_version=", Num(SYSRECOVER_CONTRACT_VERSION));
    return s;
}

std::string BuildTaskJson(const RestoreTask& t) {
    std::string s;
    s += "{\"schema\":1,\"action\":\"";
    s += (t.action.empty() ? "restore" : t.action);
    s += "\",\"pt_type\":\"";
    s += t.ptType;
    s += "\",\"image_path\":\"";
    s += JsonEsc(W2U(t.imagePath));
    s += "\",\"image_rel_path\":\"";
    s += W2U(t.imageRelPath);
    s += "\",\"image_index\":";
    s += Num(t.imageIndex);
    s += ",\"image_size\":";
    s += NumU(t.imageSize);
    s += ",\"reparse_scan\":";
    s += Num(t.reparseScan);
    s += ",\"efs_scan\":";
    s += Num(t.efsScan);
    s += ",\"esp_index\":";
    s += Num(t.espIndex);
    s += ",\"ea_index\":";
    s += Num(t.eaIndex);
    s += ",\"esp_fix_dir\":\"";
    s += JsonEsc(W2U(t.espFixDir));
    s += "\",\"ea_fix_dir\":\"";
    s += JsonEsc(W2U(t.eaFixDir));
    s += "\",\"target_disk\":";
    s += Num(t.targetDisk);
    s += ",\"target_part\":";
    s += Num(t.targetPart);
    s += ",\"target_offset\":";
    s += NumU(t.targetOffset);
    s += ",\"target_size\":";
    s += NumU(t.targetSize);
    s += ",\"repair_boot\":";
    s += (t.repairBoot ? "true" : "false");
    s += ",\"partition_count\":";
    s += NumU(t.partCount);
    s += ",\"target_mode\":\"";
    s += (t.targetMode.empty() ? "format" : t.targetMode);
    s += "\",\"new_computer_name\":\"";
    s += W2U(t.computerName);
    s += "\"}\n";
    return s;
}

std::string BuildRestoreLogText(const RestoreTask& t,
                                const std::string& isoTimestamp) {
    std::string s;
    Kv(s, "action=", t.action.empty() ? "restore" : t.action);
    Kv(s, "log_time=", isoTimestamp);
    Kv(s, "software_version=", SYSRECOVER_VERSION);
    Kv(s, "software_path=", W2U(t.softwarePath));
    Kv(s, "target_disk_name=", W2U(t.targetDiskName));
    Kv(s, "target_disk_serial=", W2U(t.targetDiskSerial));
    Kv(s, "target_disk_size=", NumU(t.targetDiskSize));
    Kv(s, "target_part_offset=", NumU(t.targetOffset));
    Kv(s, "target_part_size=", NumU(t.targetSize));
    Kv(s, "target_fs=", W2U(t.targetFs));
    Kv(s, "target_vol_label=", W2U(t.targetVolLabel));
    Kv(s, "image_path=", W2U(t.imagePath));
    Kv(s, "image_index=", Num(t.imageIndex));
    Kv(s, "image_size=", NumU(t.imageSize));
    Kv(s, "reparse_scan=", Num(t.reparseScan));
    Kv(s, "efs_scan=", Num(t.efsScan));
    Kv(s, "esp_index=", Num(t.espIndex));
    Kv(s, "ea_index=", Num(t.eaIndex));
    Kv(s, "esp_fix_dir=", W2U(t.espFixDir));
    Kv(s, "ea_fix_dir=", W2U(t.eaFixDir));
    Kv(s, "repair_boot=", Num(t.repairBoot ? 1 : 0));
    Kv(s, "pt_type=", t.ptType);
    Kv(s, "engine=", t.engine.empty() ? "block" : t.engine);
    Kv(s, "keep_old=", Num(t.keepOld ? 1 : 0));
    Kv(s, "contract_version=", Num(SYSRECOVER_CONTRACT_VERSION));
    return s;
}

bool WriteRestoreTask(const std::wstring& dir, const RestoreTask& t,
                      std::string& log) {
    // sidecar（key=value，供 Linux restore.sh）
    // 注意：image_path 为 restore.sh 第 7 步定位镜像分区的必需键（Phase 5 补）。
    const std::string conf = BuildTaskConf(t);
    if (!WriteTextFile(dir + L"\\restore-task.conf", conf)) {
        log += "write restore-task.conf FAIL\n";
        return false;
    }
    // M14：回读校验关键键。0.9.96 的 8192 固定缓冲一旦截断，Linux 侧会拿不到
    // contract_version/keep_old/target_guid，从而按默认值选错目标盘。
    std::string back;
    if (!ReadTextFile(dir + L"\\restore-task.conf", back) ||
        !HasKeyLine(back, "contract_version=") ||
        !HasKeyLine(back, "keep_old=") || !HasKeyLine(back, "target_guid=") ||
        !HasKeyLine(back, "target_disk_serial=")) {
        log += "restore-task.conf verify FAIL (keys missing/truncated)\n";
        return false;
    }
    // json（同内容，供 Windows 侧/GUI 读取）
    if (!WriteTextFile(dir + L"\\restore-task.json", BuildTaskJson(t))) {
        log += "write restore-task.json FAIL\n";
        return false;
    }
    log += "restore-task written\n";
    return true;
}

bool WriteRestoreLog(wchar_t letter, const RestoreTask& t, std::string& log) {
    wchar_t root[4] = {letter, L':', L'\\', 0};
    // 1) 清除旧 _zjresy*.log（对齐旧 C# ClearOldRestoreLogs）
    WIN32_FIND_DATAW fd;
    std::wstring pattern = std::wstring(root) + L"_zjresy*.log";
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            DeleteFileW((std::wstring(root) + fd.cFileName).c_str());
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
    // 2) 写新日志（文件名含日时分，对齐旧 C# 命名）
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[64];
    swprintf(name, 64, L"_zjresy%02u%02u%02u.log", st.wDay, st.wHour,
             st.wMinute);
    char timeBuf[40];
    snprintf(timeBuf, sizeof(timeBuf), "%04u-%02u-%02uT%02u:%02u:%02u",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    if (!WriteTextFile(std::wstring(root) + name,
                       BuildRestoreLogText(t, timeBuf))) {
        log += "write _zjresy log FAIL\n";
        return false;
    }
    // 同分区根并写一份 conf：Linux 侧在日志所在分区即可就近取到，避免
    // 「日志在 C:、conf 在 D:\软件目录」导致 S99zjrestore 找不到任务（PIT-035）。
    WriteTextFile(std::wstring(root) + L"restore-task.conf", BuildTaskConf(t));
    log += "restore log written to ";
    log += W2U(std::wstring(root) + name);
    log += "\n";
    return true;
}

// 清掉所有固定盘/可移动盘**根目录**上的旧契约（用户 2026-10-04 规格：只留唯一
// 一份）。背景：救援层 init 会挑"第一个发现 _zjresy 的分区"；别处残留（拷日志、
// 旧测试）可能把目标指错（参照机日志实证 /dev/sda1 与契约矛盾）。不碰
// <盘>\ZJRESTORE\（部署目录）与软件目录（写新契约会覆盖副契约）。
int CleanupStrayContracts(std::string& log) {
    int n = 0;
    DWORD mask = GetLogicalDrives();
    for (wchar_t c = L'A'; c <= L'Z'; ++c) {
        if (!(mask & (1u << (c - L'A'))))
            continue;
        wchar_t root[4] = {c, L':', L'\\', 0};
        DWORD t = GetDriveTypeW(root);
        if (t != DRIVE_FIXED && t != DRIVE_REMOVABLE)
            continue;
        WIN32_FIND_DATAW fd = {};
        HANDLE h = FindFirstFileW(
            (std::wstring(root) + L"_zjresy*.log").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                std::wstring p = std::wstring(root) + fd.cFileName;
                SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                if (DeleteFileW(p.c_str()))
                    ++n;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        for (const wchar_t* nm : {L"restore-task.conf", L"restore-task.json"}) {
            std::wstring p = std::wstring(root) + nm;
            SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (DeleteFileW(p.c_str()))
                ++n;
        }
    }
    log += "stray contracts cleaned: " + std::to_string(n) + "\n";
    return n;
}

std::string TaskConfGet(const std::string& confText, const std::string& key) {
    const std::string want = key + "=";
    size_t pos = 0;
    while (pos <= confText.size()) {
        size_t e = confText.find('\n', pos);
        std::string line = confText.substr(
            pos, e == std::string::npos ? std::string::npos : e - pos);
        pos = (e == std::string::npos) ? confText.size() + 1 : e + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;
        if (line.compare(0, want.size(), want) == 0)
            return line.substr(want.size());
    }
    return {};
}

}  // namespace sysrecover
