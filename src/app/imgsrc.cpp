#include "app/imgsrc.h"

#include "common/i18n.h"
#include "common/logger.h"

#include <windows.h>

#include <map>
#include <mutex>

namespace sysrecover {
namespace {

std::mutex g_mu;
std::map<std::wstring, std::wstring> g_passwords;  // path → 口令（仅内存）
std::map<std::wstring, std::wstring> g_temps;      // .zje 路径 → 临时明文

std::wstring Lower(const std::wstring& s) {
    std::wstring r = s;
    for (auto& c : r)
        c = (wchar_t)towlower(c);
    return r;
}

std::wstring PasswordFor(const std::wstring& path) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_passwords.find(Lower(path));
    return it == g_passwords.end() ? std::wstring() : it->second;
}

}  // namespace

void SetImagePassword(const std::wstring& path, const std::wstring& pwd) {
    std::lock_guard<std::mutex> lk(g_mu);
    const std::wstring k = Lower(path);
    if (pwd.empty())
        g_passwords.erase(k);
    else
        g_passwords[k] = pwd;
}

std::wstring GetImagePassword(const std::wstring& path) {
    return PasswordFor(path);
}

void ClearImagePasswords() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_passwords.clear();
}

bool ImageIsEncrypted(const std::wstring& path) {
    return ZjeIsFile(path);
}

int ImageProbe(const std::wstring& path, std::wstring& why) {
    why.clear();
    if (!ImageIsEncrypted(path)) {
        WimEngine eng;
        return eng.Probe(path, why);
    }
    const std::wstring pwd = PasswordFor(path);
    if (pwd.empty()) {
        why = Tr(L"这是加密镜像，需要先输入密码");
        return -1;
    }
    ZjeInfo info;
    std::vector<ZjeImage> imgs;
    std::wstring err;
    const int rc = ZjeReadMeta(path, pwd, &info, &imgs, err);
    if (rc != 0) {
        why = err;
        return -1;
    }
    if (imgs.empty()) {
        why = Tr(L"镜像里没有任何子镜像");
        return -1;
    }
    return 0;
}

// H4（1.0）：快速结构预检（格式化前的强制前置；毫秒级）。
int ImageQuickPrecheck(const std::wstring& path, std::wstring& why) {
    why.clear();
    if (!ImageIsEncrypted(path)) {
        WimEngine eng;
        return eng.QuickPrecheck(path, why);
    }
    // 加密容器：meta 已由 ZjeReadMeta 校验（含 HMAC）；这里再核对正文是否被截断。
    const std::wstring pwd = PasswordFor(path);
    if (pwd.empty()) {
        why = Tr(L"这是加密镜像，需要先输入密码");
        return -1;
    }
    ZjeInfo info;
    std::vector<ZjeImage> imgs;
    std::wstring err;
    if (ZjeReadMeta(path, pwd, &info, &imgs, err) != 0) {
        why = err;
        return -1;
    }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        why = Tr(L"无法打开镜像文件");
        return -1;
    }
    LARGE_INTEGER fsz{};
    GetFileSizeEx(h, &fsz);
    CloseHandle(h);
    const unsigned long long fileSize =
        static_cast<unsigned long long>(fsz.QuadPart);
    if (info.bodyOffset > fileSize || info.plainSize == 0) {
        why = Tr(L"加密镜像头声明异常");
        return -1;
    }
    // 正文允许比明文略大（分块填充），但绝不能小于明文长度。
    if (fileSize - info.bodyOffset < info.plainSize) {
        why = Tr(L"加密镜像正文被截断（文件比声明的短）");
        return -1;
    }
    return 0;
}

int ImageList(const std::wstring& path, std::vector<ImageDesc>& out) {
    out.clear();
    if (!ImageIsEncrypted(path)) {
        WimEngine eng;
        return eng.ListImages(path, out);
    }
    const std::wstring pwd = PasswordFor(path);
    if (pwd.empty())
        return -1;
    ZjeInfo info;
    std::vector<ZjeImage> imgs;
    std::wstring err;
    if (ZjeReadMeta(path, pwd, &info, &imgs, err) != 0)
        return -1;
    for (const auto& z : imgs) {
        ImageDesc d;
        d.index = z.index;
        d.name = z.name;
        d.description = z.description;
        d.sizeBytes = z.sizeBytes;
        d.creationTime = z.creationTime;
        out.push_back(std::move(d));
    }
    return out.empty() ? -1 : 0;
}

int ImageContentSize(const std::wstring& path, int index,
                     unsigned long long* bytes) {
    if (!bytes)
        return -1;
    *bytes = 0;
    if (!ImageIsEncrypted(path)) {
        WimEngine eng;
        return eng.ImageSize(path, index, bytes);
    }
    const std::wstring pwd = PasswordFor(path);
    if (pwd.empty())
        return -1;
    ZjeInfo info;
    std::vector<ZjeImage> imgs;
    std::wstring err;
    if (ZjeReadMeta(path, pwd, &info, &imgs, err) != 0)
        return -1;
    for (const auto& z : imgs) {
        if (z.index == (index < 1 ? 1 : index)) {
            *bytes = z.sizeBytes;
            return 0;
        }
    }
    return -1;
}

int ImageHasWinload(const std::wstring& path, int index, bool* exists) {
    if (exists)
        *exists = false;
    if (!ImageIsEncrypted(path)) {
        WimEngine eng;
        return eng.ImagePathExists(path, index, L"\\Windows\\system32\\winload.exe",
                                   exists);
    }
    const std::wstring pwd = PasswordFor(path);
    if (pwd.empty())
        return -1;
    ZjeInfo info;
    std::vector<ZjeImage> imgs;
    std::wstring err;
    if (ZjeReadMeta(path, pwd, &info, &imgs, err) != 0)
        return -1;
    for (const auto& z : imgs) {
        if (z.index == (index < 1 ? 1 : index)) {
            if (exists)
                *exists = z.hasWinload;
            return 0;
        }
    }
    return -1;
}

// 2026-11-06 实机：安装源类镜像（install.esd/wim）里没有目标系统的注册表
// （\Windows\System32\config\SYSTEM），还原后必然 0xc000000f。这里只查
// **第一个映像的目录表**（wimlib 元数据，秒级），不靠文件名/大小猜。
// 加密容器（.zje，本机自产、不可能是安装源）不查询 → 返回 -1，不阻断。
int ImageHasSystemHive(const std::wstring& path, int index, bool* exists) {
    if (exists)
        *exists = false;
    if (ImageIsEncrypted(path))
        return -1;
    WimEngine eng;
    return eng.ImagePathExists(path, index, L"\\Windows\\System32\\config\\SYSTEM",
                               exists);
}

std::wstring ResolveImagePlain(const std::wstring& path, ProgressFn progress,
                               std::wstring& err, bool* temp) {
    err.clear();
    if (temp)
        *temp = false;
    if (!ImageIsEncrypted(path))
        return path;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_temps.find(Lower(path));
        if (it != g_temps.end() &&
            GetFileAttributesW(it->second.c_str()) != INVALID_FILE_ATTRIBUTES)
            return it->second;
    }
    const std::wstring pwd = PasswordFor(path);
    if (pwd.empty()) {
        err = Tr(L"这是加密镜像，需要先输入密码");
        return {};
    }
    const std::wstring plain = ZjeTempPlainPath(path);
    CryptProgress cp = [&](unsigned long long done, unsigned long long total) {
        if (!progress)
            return false;
        int pct = total ? (int)(done * 100 / total) : -1;
        return progress(pct, "decrypt");
    };
    std::wstring derr;
    LogInfo("ZJE: decrypting image for read access");
    if (ZjeDecryptFile(path, plain, pwd, cp, derr) != 0) {
        err = derr;
        int n = WideCharToMultiByte(CP_UTF8, 0, derr.c_str(), -1, nullptr, 0,
                                    nullptr, nullptr);
        std::string u8(n > 0 ? n - 1 : 0, 0);
        if (n > 0)
            WideCharToMultiByte(CP_UTF8, 0, derr.c_str(), -1, &u8[0], n, nullptr,
                                nullptr);
        LogError("ZJE decrypt failed: " + u8);
        // 取消（err=已取消）也要把半成品清掉 —— ZjeDecryptFile 已删 .tmp
        return {};
    }
    if (temp)
        *temp = true;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_temps[Lower(path)] = plain;
    }
    return plain;
}

void ReleaseImagePlain(const std::wstring& path) {
    std::wstring tmp;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_temps.find(Lower(path));
        if (it == g_temps.end())
            return;
        tmp = it->second;
        g_temps.erase(it);
    }
    if (!tmp.empty())
        DeleteFileW(tmp.c_str());
}

}  // namespace sysrecover
