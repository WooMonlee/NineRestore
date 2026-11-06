// B9：从 src/gui/main_form.cpp 机械搬入，判定规则与注释原样保留（行为不变）。
#include "request_precheck.h"

#include <windows.h>

namespace sysrecover {

bool LooksLikeImagePath(const std::wstring& p) {
    if (p.size() < 4) return false;
    size_t n = p.size();
    auto eq = [&](const wchar_t* ext) {
        for (int i = 0; i < 4; ++i) {
            wchar_t a = p[n - 4 + i];
            if (a >= L'A' && a <= L'Z') a = (wchar_t)(a - L'A' + L'a');
            if (a != ext[i]) return false;
        }
        return true;
    };
    return eq(L".wim") || eq(L".esd") || eq(L".swm") || eq(L".zje");
}

// ⚠️ 目录不是镜像：UNC 共享根（\\server\share\）在输入过程中会先变成合法目录，
// 若当成镜像去开，会同步阻塞并触发文本同步/光标跳动
//（2026-10-21 VM 实测：输入汉字跑到最前面）。
bool ImagePathReady(const std::wstring& p) {
    const DWORD a = ::GetFileAttributesW(p.c_str());
    if (a != INVALID_FILE_ATTRIBUTES)
        return (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
    return LooksLikeImagePath(p);
}

}  // namespace sysrecover
